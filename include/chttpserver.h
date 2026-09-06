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

#pragma once

#include <chttp.h>
#include <clogger.h>
#include <common.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * @file chttpserver.h
 * @brief HTTP/1.1 server backed by event_loop (a persistent, multi-threaded
 *        epoll reactor) with routing, middleware, sub-routers, TLS, and
 *        optional streaming-body dispatch.
 *
 * ### Quick start
 *
 * @code
 * void hello(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
 *     (void)req; (void)ctx;
 *     chttpsvr_resp_write_str(resp, "Hello, world!");
 * }
 *
 * int main(void) {
 *     chttpsvr_construct(srv, logger);
 *     chttpsvr_register_handler(srv, CHTTP_GET, "/hello", hello, NULL);
 *     chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
 *     chttpsvr_start(srv, &cfg);
 *     chttpsvr_engine_wait();
 *     chttpsvr_destroy(srv);
 * }
 * @endcode
 *
 * ### Threading model
 *
 * Routing happens as soon as a request's headers are parsed; before any
 * body byte is read.  An unmatched route is rejected immediately, without
 * ever reading the body it's about to discard.  A matched route (whether
 * registered with chttpsvr_register_handler (buffered body) or
 * chttpsvr_register_streaming_handler (streaming body)) is handed to the
 * server's own ctpool worker thread pool right away, regardless of body
 * size.  The worker thread reads the request body itself, batch by batch,
 * directly off the socket; the reactor thread's job on any request is
 * therefore O(1) and it is never blocked reading a large or slow body,
 * let alone by user handler code.
 *
 * Each server owns one ctpool, created when chttpsvr_start() is called.  The
 * pool size is governed by chttpsvr_config_t.worker_thread_count and
 * chttpsvr_config_t.worker_queue_capacity.  If the task queue is full when a
 * request arrives, the server returns 503 Service Unavailable immediately.
 *
 * Streaming handlers call chttpsvr_req_read() to pull each body batch as it
 * arrives, before the rest of the body has necessarily even reached the
 * server.  Buffered handlers receive the complete body in memory via
 * chttpsvr_req_body() once the worker has finished reading it in full.  Both
 * handler types run on worker threads and may block; chttpsvr_req_read()
 * itself blocks the calling worker (never the reactor) until data arrives,
 * EOF, an error, chttpsvr_config_t.stream_read_timeout_ms elapses, or (if
 * set) chttpsvr_config_t.max_body_read_duration_ms elapses.
 *
 * ### Engine lifecycle (implicit)
 *
 * The shared event_loop reactor (the "engine") is started automatically on
 * the first chttpsvr_start() call and stopped automatically when the last
 * running server is destroyed.  There is no need to call engine lifecycle
 * functions manually.
 *
 * To install a custom logger for engine-level events (TLS handshake
 * failures, listen-socket bind failures, idle-timeout closures) before the
 * first server starts, call chttpsvr_set_engine_logger().
 *
 * To block the calling thread until the engine exits (e.g. after an external
 * shutdown signal such as SIGTERM triggers chttpsvr_engine_stop() from a
 * signal handler), call chttpsvr_engine_wait().
 *
 * A request handler wanting to shut its own server down from within itself
 * (e.g. an admin/shutdown endpoint) should call chttpsvr_engine_stop(),
 * which is safe there by design; calling chttpsvr_destroy() or
 * chttpsvr_stop()+chttpsvr_start() directly on the server currently running
 * that handler is refused/fatal instead (see their own doc comments).
 * chttpsvr_engine_wait() is refused/fatal there too, for the same reason:
 * such a handler should call chttpsvr_engine_stop() and simply return, not
 * also wait for the drain it just triggered to finish on the same thread.
 *
 * Multiple servers may run concurrently; each listens on its own port and
 * has its own routes, middleware, worker pool, and clog handle.
 *
 * Typical single-server pattern:
 * @code
 *   chttpsvr_construct(srv, logger);
 *   chttpsvr_register_handler(srv, CHTTP_GET, "/hello", hello, NULL);
 *   chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
 *   chttpsvr_start(srv, &cfg);
 *   chttpsvr_engine_wait();  // blocks until signal / chttpsvr_engine_stop()
 *   chttpsvr_destroy(srv);
 * @endcode
 *
 * ### Middleware chain limit
 *
 * Each router (the root router for global middleware, and each sub-router)
 * caps its own middleware chain at 32 entries.  The limit is enforced at
 * registration time: chttpsvr_use / chttpsvr_router_use return
 * ccol_not_permitted (without adding the entry) on the call that would exceed
 * the 32-entry cap for that specific router.
 *
 * Separately, dispatch time also enforces a combined cap of 32 for the
 * effective chain of a given request (global middleware + the matched
 * router's own middleware).  Because each side of that sum can independently
 * hold up to 32 entries, the combined count can still exceed 32 even though
 * neither router individually hit its own registration-time cap; when this
 * happens every request through the affected router receives a 500 response.
 *
 * ### Route registration order and shadowing
 *
 * Routes and routers are evaluated in registration order.  The root router
 * (routes registered directly with chttpsvr_register_handler /
 * chttpsvr_register_streaming_handler) is always checked first, before any
 * sub-router.  A root-level route whose path AND method both match wins
 * outright, shadowing a same-path sub-router route completely.  If a
 * root-level route matches the same path but a DIFFERENT method, it does
 * not shadow the sub-router route: every other root-level route is still
 * tried for a same-path, same-method match first, and only once the whole
 * root router has been exhausted does matching fall through to the
 * sub-router, where a route whose own method matches the request is served
 * normally.  (This mirrors how two same-router routes registered for the
 * same path under different methods already behave; see README.md's routing
 * section for the 405-vs-404 mechanism this shares.)  To avoid unintentional
 * shadowing, do not register
 * root-level routes whose paths overlap with a sub-router's prefix +
 * pattern combination for the same method.
 *
 * ### Route patterns
 *
 * Exact paths:   /health  /api/v1/status
 * Named params:  /users/{id}/posts/{postId}
 *
 * ### TLS
 *
 * Set chttpsvr_config_t.tls to a pointer to a chttp_tls_config_t with
 * cert_path and key_path.  Requires -lssl -lcrypto at link time.
 * Pass NULL for plaintext HTTP.
 */

/* ========================================================================== */
/*                         OPAQUE HANDLES                                     */
/* ========================================================================== */

/** @brief Opaque HTTP server structure. */
typedef struct chttpserver chttpserver;

/**
 * @brief Server handle.
 *
 * chttpsvr is an opaque VALUE handle (a packed {slot index, generation}
 * pair), not a pointer; it must never be cast to/from void*, compared via
 * a pointer cast, or otherwise treated as an address. Compare it directly
 * against CHTTPSVR_INVALID (or use it in a truthiness check; CHTTPSVR_INVALID
 * is 0, so `if (!srv)` works too). Internally, every use of a chttpsvr is
 * resolved through a
 * library-owned slot table before the underlying server object is touched:
 * a handle whose slot has since been freed (or reused for an unrelated,
 * later server) is always detected, rather than silently dereferencing
 * freed or wrong-object memory. See chttpsvr_destroy's own doc comment for
 * what happens when a stale handle reaches it specifically.
 */
typedef uint64_t chttpsvr;

/** @brief Sentinel value for "no server"; the chttpsvr analogue of NULL. */
#define CHTTPSVR_INVALID ((chttpsvr)0)

/** @brief Opaque route group (sub-router) handle. */
typedef struct chttpsvr_router chttpsvr_router;

/** @brief Opaque per-request context (read via chttpsvr_req_* functions). */
typedef struct chttpsvr_req chttpsvr_req;

/**
 * @brief Opaque per-request response accumulator (written via
 *        chttpsvr_resp_* functions).
 */
typedef struct chttpsvr_resp chttpsvr_resp;

/* ========================================================================== */
/*                         CALLBACK TYPES                                     */
/* ========================================================================== */

/**
 * @brief HTTP request handler.
 *
 * Runs on a ctpool worker thread owned by the server; may block.
 *
 * @param req  Request object; valid only for the duration of the call.
 * @param resp Response accumulator; populated by the handler and flushed when
 *             the handler returns.
 * @param ctx  Per-route opaque context set at registration time.
 */
typedef void (*chttpsvr_handler_fn)(chttpsvr_req *req, chttpsvr_resp *resp,
                                    void *ctx);

/**
 * @brief Continuation function passed to each middleware.
 *
 * Call to advance to the next middleware in the chain (or to the final
 * handler if no more middleware remain).  Calling it zero times short-circuits
 * the chain.  Calling it more than once from the same invocation skips
 * whatever step the first call already invoked and instead advances the
 * chain a second time, invoking the step after that one (the final handler
 * itself, invoked repeatedly, only if this call is already the last
 * middleware in the chain); this is never a useful pattern and must be
 * avoided.
 */
typedef void (*chttpsvr_next_fn)(chttpsvr_req *req, chttpsvr_resp *resp);

/**
 * @brief Middleware function.
 *
 * Runs before the final handler (and, if global, before sub-router
 * middleware too).  Call next(req, resp) to pass control to the next handler.
 *
 * Runs on a ctpool worker thread; may block.
 *
 * @param req   Request object.
 * @param resp  Response accumulator.
 * @param ctx   Per-middleware opaque context set at chttpsvr_use time.
 * @param next  Call to proceed to the next handler in the chain.
 */
typedef void (*chttpsvr_middleware_fn)(chttpsvr_req *req, chttpsvr_resp *resp,
                                       void *ctx, chttpsvr_next_fn next);

/* ========================================================================== */
/*                         SERVER CONFIGURATION                               */
/* ========================================================================== */

/**
 * @brief Pass as worker_queue_capacity to configure an unbounded task queue
 *        with no 503 back-pressure.
 *
 * Use with caution: an unbounded queue can grow without limit under sustained
 * overload.  In most production deployments a finite capacity with graceful
 * 503 rejection is preferable.
 */
#define CHTTPSVR_QUEUE_UNBOUNDED ((size_t)-1)

/**
 * @brief Server startup configuration.
 *
 * Fill in the fields you want to override and pass a pointer to
 * chttpsvr_start().  Use CHTTPSVR_CONFIG_DEFAULT as a starting point.
 */
typedef struct chttpsvr_config {
  /** Bind address; NULL or "0.0.0.0" binds all interfaces. Alternatively,
   *  a value of the form "unix://path/to/socket" binds a Unix domain socket
   *  at that path instead of a TCP listener; port is then ignored (may be
   *  0). A stale socket file already at that path is removed automatically
   *  before binding. A given chttpsvr instance listens on either TCP or a
   *  Unix socket, never both; an application wanting both creates two
   *  chttpsvr instances (both share the same process-wide reactor already,
   *  at no extra cost). */
  const char *host;
  /** Listening port (default 8080). Ignored when host is a "unix://" path. */
  uint16_t port;
  /** Max request body in bytes (default 4 MiB); 0 = unlimited. A buffered
   *  route whose body exceeds this is rejected with 413 before the handler
   *  ever runs; a streaming route's handler is always invoked, and
   *  chttpsvr_req_read() returns -1 with chttpsvr_req_stream_error() ==
   *  ccol_msg_too_large once the limit is crossed. Either way the
   *  connection is closed after the resulting response (Connection: close)
   *  rather than kept alive, since the excess body bytes beyond the limit
   *  are discarded, not drained. */
  size_t max_body_size;
  /** Per-connection read timeout in ms; 0 = disabled (no idle timeout).
   *  Used as the idle-timeout sweep's threshold when idle_timeout_ms is 0;
   *  only applies while a connection is idle between requests (waiting for
   *  the next pipelined/keep-alive request's headers), not while a request
   *  is actively being handled by a worker thread (see
   *  max_body_read_duration_ms for that). */
  long read_timeout_ms;
  /** Keep-alive idle timeout in ms; 0 = same as read_timeout_ms.
   *  When non-zero, overrides read_timeout_ms as the idle-timeout sweep's
   *  threshold. */
  long idle_timeout_ms;
  /** Bounds how long a worker thread will wait for the *next* batch of body
   *  bytes while reading a request body (buffered or streaming), in ms.
   *  0 = wait indefinitely (bounded only by read_timeout_ms/idle_timeout_ms,
   *  which reset on any connection activity and so do not protect against a
   *  client that trickles bytes slowly enough to always beat them).
   *  Default 30000 (30 s). On expiry, chttpsvr_req_read() returns -1 and
   *  chttpsvr_req_stream_error() reports ccol_timed_out; buffered routes
   *  respond 408 automatically.
   *
   *  Setting this (and/or max_body_read_duration_ms) to 0 does not turn an
   *  otherwise-unresponsive connection into a permanent liability: shutting
   *  down the server (chttpsvr_stop() immediately followed by
   *  chttpsvr_start(), chttpsvr_destroy(), or chttpsvr_engine_stop()) still
   *  completes in bounded time even if a worker thread is genuinely blocked
   *  reading such a connection's body, by forcibly closing that connection
   *  once its own bounded, graceful wait for in-flight requests is
   *  exhausted. This only ever happens as part of that shutdown sequence,
   *  never merely because a peer is slow; a request that is still making
   *  progress is never affected by it. */
  unsigned stream_read_timeout_ms;
  /** Bounds the *total* wall-clock time a worker thread will spend reading
   *  one request's body (buffered or streaming), in ms; 0 = no limit.
   *  Unlike stream_read_timeout_ms (which only bounds each individual gap
   *  between batches, and so never fires against a client that trickles a
   *  byte or two just before every gap expires), this caps the sum of all
   *  such waits for a single request; closing that trickle-forever loophole,
   *  which would otherwise let a handful of slow connections pin the entire
   *  worker pool indefinitely. On expiry, chttpsvr_req_read() returns -1 and
   *  chttpsvr_req_stream_error() reports ccol_timed_out (the same outcome as
   *  a stream_read_timeout_ms expiry); buffered routes respond 408
   *  automatically. Default 0 (disabled) so existing deployments are
   *  unaffected until this is explicitly opted into. */
  unsigned max_body_read_duration_ms;
  /** Bounds how long a worker thread will wait, per write(2)-equivalent
   *  call, while sending one response to a slow-reading client, in ms; 0 =
   *  use stream_read_timeout_ms's value. Bounds how long a slow-reading
   *  peer can hold a worker thread during response send; a worker pool is
   *  a small, explicitly-sized resource for handler work, and this closes
   *  the same class of gap stream_read_timeout_ms closes for the read
   *  side. Setting this to 0 has the same bounded-shutdown property
   *  stream_read_timeout_ms's own doc comment describes: it does not make
   *  server shutdown wait forever on a stalled peer either. */
  unsigned response_write_timeout_ms;
  /** Bounds the *total* wall-clock time a worker thread will spend sending
   *  one response, in ms; 0 = no limit. Unlike response_write_timeout_ms
   *  (which only bounds each individual write(2)-equivalent call, and so
   *  never fires against a client that reads a byte or two just before
   *  every such call's own timeout expires), this caps the sum of all such
   *  waits for a single response; closing the identical trickle-forever
   *  loophole max_body_read_duration_ms closes on the read side, so a
   *  handful of slow-reading connections cannot pin the entire worker pool
   *  indefinitely by trickling reads of an otherwise large response. On
   *  expiry the response send fails and the connection is closed (the
   *  client sees a truncated response or a reset, having already received
   *  as much as it read before the deadline). Default 0 (disabled) so
   *  existing deployments are unaffected until this is explicitly opted
   *  into.
   *
   *  A courtesy rejection response (404/405/413/500/501/503, generated
   *  internally rather than by a handler) and the "Expect: 100-continue"
   *  interim "100 Continue" line are both additionally always bounded by a
   *  small internal ceiling (currently 2 seconds) regardless of this
   *  setting's own value, including 0/disabled: both are always a fixed,
   *  small shape (no body, or a single 25-byte status line) with no
   *  legitimate reason to ever need longer, and a slow-reading peer must
   *  never be able to hold a worker thread on one indefinitely just because
   *  the operator left this knob at its own default for their handler-
   *  controlled responses. A value set here smaller than that internal
   *  ceiling still applies in full; only a value of 0 or larger than the
   *  ceiling is narrowed for these two internally-generated writes
   *  specifically. */
  unsigned max_response_write_duration_ms;
  /** Maximum combined size, in bytes, of a request's header block (request
   *  line + all header lines). 0 = use the library's built-in default
   *  (64 KiB). A request whose headers exceed this is rejected (the
   *  connection is closed; the client sees a reset/EOF rather than a
   *  well-formed error response, since the header block itself couldn't be
   *  parsed far enough to know how to respond). */
  size_t max_header_bytes;
  /** Maximum number of simultaneously open connections across this
   *  listener; 0 = unlimited. Once at capacity, new connections are simply
   *  left pending in the kernel's own listen backlog (accept(2) is not
   *  called again for this listener until a connection closes and frees a
   *  slot) rather than accepted and immediately rejected. Resumption is not
   *  instantaneous: it is noticed by a periodic sweep, with a worst case of
   *  about one second between a slot freeing and the listener resuming. */
  size_t max_connections;
  /** TLS config; NULL = plaintext. */
  const chttp_tls_config_t *tls;
  /** Number of worker threads in the server-owned ctpool (default: CPU count).
   *  Pass 0 to use the CPU count. */
  int worker_thread_count;
  /** Capacity of the worker task queue.
   *  0                       = library default (1024 * worker_thread_count).
   *  CHTTPSVR_QUEUE_UNBOUNDED = no limit (never returns 503 due to overflow).
   *  Any other value         = exact bounded capacity; 503 is returned when
   *                            the queue is full. */
  size_t worker_queue_capacity;
  /** Enable SO_KEEPALIVE on every accepted TCP connection (default: off,
   *  matching this library's historical behavior). Lets the OS detect and
   *  close a connection whose peer has gone silently unreachable (e.g. a
   *  pulled network cable) using the kernel's own keepalive probe interval,
   *  independent of and in addition to idle_timeout_ms (which only measures
   *  local inactivity, not peer reachability). No effect on a Unix domain
   *  socket listener. */
  bool enable_keepalive;
  /** Set SO_REUSEPORT on the listening socket (default: off). Lets more
   *  than one process (or, within one process, more than one chttpsvr
   *  instance) bind the same host:port simultaneously, with the kernel
   *  load-balancing accepted connections across them. This library does
   *  not itself coordinate multiple processes; this knob only controls
   *  whether the OS-level prerequisite for an application to do so
   * itself is set. No effect on a Unix domain socket listener. */
  bool enable_reuseport;
  /** Set IPV6_V6ONLY on the listening socket when it ends up binding an
   *  IPv6 address (default: off, i.e. leave the OS default, which on Linux
   *  is a dual-stack socket that also accepts IPv4-mapped connections
   *  unless already restricted elsewhere, e.g. by /proc/sys/net/ipv6/
   *  bindv6only). Set this to true for an IPv6-only listener that must
   *  never silently also accept IPv4 traffic. No effect on an IPv4 or
   *  Unix domain socket listener. */
  bool ipv6_only;
} chttpsvr_config_t;

/**
 * @brief Sensible defaults for chttpsvr_config_t.
 *
 * Port 8080, all interfaces, 4 MiB body limit, no TLS,
 * CPU-count worker threads, library-default queue capacity.
 */
#define CHTTPSVR_CONFIG_DEFAULT              \
  ((chttpsvr_config_t){                      \
      .host = "0.0.0.0",                     \
      .port = 8080,                          \
      .max_body_size = (4U * 1024U * 1024U), \
      .read_timeout_ms = 0,                  \
      .idle_timeout_ms = 0,                  \
      .stream_read_timeout_ms = 30000,       \
      .max_body_read_duration_ms = 0,        \
      .response_write_timeout_ms = 0,        \
      .max_response_write_duration_ms = 0,   \
      .max_header_bytes = 0,                 \
      .max_connections = 0,                  \
      .tls = NULL,                           \
      .worker_thread_count = 0,              \
      .worker_queue_capacity = 0,            \
      .enable_keepalive = false,             \
      .enable_reuseport = false,             \
      .ipv6_only = false,                    \
  })

/* ========================================================================== */
/*                         ENGINE LOGGER                                      */
/* ========================================================================== */

/**
 * @brief Install a custom logger for the shared event_loop engine's own
 *        diagnostics.
 *
 * The engine logger captures the reactor's own diagnostics: TLS handshake
 * failures, listen-socket bind failures, and idle-timeout sweep closures.
 * It is separate from the per-server logger passed to create_chttpsvr().
 *
 * Call this before the first chttpsvr_start() if you want a custom engine
 * logger. If no logger has been installed when the engine first starts, a
 * fallback logger (fd 2, level CLOG_FATAL) is installed automatically,
 * mirroring create_chttpsvr's own internal stderr/FATAL-only logger; since
 * this engine's own diagnostics (idle-timeout closures, TLS handshake
 * failures, listen-socket setup failures) are all logged below
 * CLOG_FATAL, that fallback logger's min_level filters every one of them
 * out, so it is silent in practice unless this function is used to
 * install a more verbose one.
 *
 * Internally this function derives a logger from cl via clog_derive() and
 * adds the field component=http-server-engine.  The previously registered
 * engine logger (if any) is closed.  The caller retains ownership of cl and
 * must keep it alive for as long as any server may be running.
 *
 * @param cl  Parent logger to derive from; must not be CLOG_INVALID.
 * @return ccol_success or ccol_invalid_args (cl is CLOG_INVALID).
 */
ccol_retval_t chttpsvr_set_engine_logger(clog cl);

/* ========================================================================== */
/*                    ENGINE MEMORY MANAGEMENT                                */
/* ========================================================================== */

/**
 * @brief Install custom memory management procs for the shared event_loop
 *        engine's own internal allocations.
 *
 * By default, the event_loop reactor shared by every chttpsvr instance in
 * this process (see the module notes on the shared engine) allocates its
 * own memory (the registration table, per-connection dispatch state, and
 * so on) using the default allocator. Calling this function with a
 * non-NULL *mp* redirects all of that to the supplied procs instead,
 * exactly like the memory management procs accepted by every other module
 * in this library. Passing NULL reverts to the default behavior.
 *
 * This is independent of the allocator each individual chttpsvr instance
 * uses for its own connections/requests (configured via create_chttpsvr_mp,
 * following the usual _mp convention); this function only affects the one
 * shared reactor's own construction.
 *
 * Unlike chttpsvr_set_engine_logger(), which can be swapped at any time,
 * this function may only be called before the first chttpsvr_start() in
 * the process: once the reactor has allocated memory with one set of
 * procs, swapping to a different malloc/free pairing would corrupt the
 * heap. It may be called again after the reactor has fully stopped
 * (chttpsvr_engine_wait() has returned), before the next chttpsvr_start().
 *
 * @param mp  Custom memory management procs, or NULL to revert to the
 *            default. If non-NULL, all four function pointers must be set.
 * @return ccol_success, ccol_invalid_args (mp is non-NULL but has a NULL
 *         function pointer), or ccol_not_permitted (the engine is already
 *         running; stop it first).
 */
ccol_retval_t chttpsvr_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp);

/* ========================================================================== */
/*                    ENGINE REACTOR THREAD COUNT                             */
/* ========================================================================== */

/**
 * @brief Configure how many OS threads the shared event_loop engine devotes
 *        to its own polling and dispatch.
 *
 * By default (never having called this function, or having called it with
 * num_threads == 0), the shared reactor uses exactly 1 thread: a single
 * dedicated thread that both polls (epoll_wait) and runs every dispatch
 * callback (header parsing, TLS handshake stepping) inline, with no
 * separate dispatch worker pool at all. Calling this function with a
 * num_threads > 1 spins up that many OS threads instead, following
 * event_loop_create_with_mprocs's own num_reactor_threads semantics
 * (cthreadcomm.h): one dedicated polling thread plus (num_threads - 1)
 * separate dispatch worker threads that actually run callbacks.
 *
 * The default is 1, not an auto-detected CPU count, because it performs
 * better for the common case:
 *
 *   - Plain HTTP, connections reused (typical browser/API-client
 *     traffic): a single thread gives the best throughput. There is
 *     essentially no CPU-bound work in the dispatch phase for plain HTTP
 *     (a fast header parse), so spreading it across threads only adds
 *     hand-off overhead with nothing to parallelize.
 *   - TLS, connections reused (typical HTTPS traffic once a client's
 *     connection pooling is accounted for): a genuine trade, not a clean
 *     win either way. A single thread gives lower throughput but a
 *     clearly better and more consistent p99 latency than multiple
 *     dispatch threads. Most of a TLS connection's requests hit the same
 *     cheap steady-state path plain HTTP does; only the connection's own
 *     handshake pays the expensive part, and that cost is amortized
 *     across however many requests the connection goes on to serve.
 *   - TLS with no connection reuse at all (every request pays a brand-new
 *     handshake; a deliberately extreme case, not typical traffic):
 *     multiple dispatch threads give both higher throughput and lower p99
 *     latency, since a TLS handshake's asymmetric-crypto cost (the
 *     server's private-key operation) is genuine CPU-bound work that
 *     benefits from being spread across cores when there is enough of it.
 *
 * The scenario where a larger num_threads is worth its cost is
 * specifically sustained *connection churn* combined with TLS: many
 * distinct clients each opening a connection for only one or a few
 * requests before it closes, so a large fraction of total traffic pays
 * the handshake's CPU cost rather than amortizing it away. This is real
 * for some deployments (a public API absorbing many one-off anonymous
 * clients, an IoT/device gateway where unreliable networks cause frequent
 * reconnects, a webhook receiver called by many different external
 * services) but is the less common shape overall: most HTTP client
 * software (browsers, and most serious HTTP client libraries, including
 * this library's own chttpclient) pools and reuses connections
 * specifically to avoid paying handshake cost repeatedly, and a server
 * sitting behind a load balancer or reverse proxy often never sees raw
 * handshake churn from the public internet at all. A deployment that
 * knows its own traffic is churn-heavy should raise num_threads
 * accordingly; this function exists for exactly that override.
 *
 * Like chttpsvr_set_engine_mem_mgmt_procs, this configures a value baked
 * into the reactor at construction time: it may only be called before the
 * first chttpsvr_start() in the process, or again after the engine has
 * fully stopped (chttpsvr_engine_wait() has returned), before the next
 * chttpsvr_start().
 *
 * @param num_threads  Desired reactor OS thread count, or 0 to restore the
 *                      default (1).
 * @return ccol_success, or ccol_not_permitted (the engine is already
 *         running; stop it first).
 */
ccol_retval_t chttpsvr_set_engine_num_reactor_threads(size_t num_threads);

/* ========================================================================== */
/*                         ENGINE WAIT                                        */
/* ========================================================================== */

/**
 * @brief Block until the shared event_loop engine's reactor threads exit.
 *
 * Returns immediately if the engine was never started or has already stopped.
 *
 * Use this as an escape hatch when you want to keep the calling thread alive
 * until a shutdown signal triggers engine exit, without destroying the server
 * handle first.
 *
 * Note: destroying the last running server (via chttpsvr_destroy) synchronously
 * quiesces that server (stops listening, drains in-flight requests, closes
 * connections, releases its engine reference) but does NOT block until the
 * shared reactor itself has fully exited; that final teardown runs on a
 * separate reaper thread. If the calling code needs a deterministic guarantee
 * that the engine has fully exited (e.g. right before process exit, so an
 * engine-installed logger via chttpsvr_set_engine_logger() is not still
 * reachable), call chttpsvr_engine_wait() explicitly after chttpsvr_destroy().
 *
 * Calling this from within a request handler or middleware currently running
 * on ANY server's own worker pool is fatal, immediately (rather than hanging):
 * draining that server's worker pool can never complete while this exact
 * in-flight request is itself blocked waiting for the engine to finish
 * exiting, deadlocking the entire shared engine's shutdown, not just one
 * server. An admin/shutdown endpoint that wants to fully drain before
 * responding should call chttpsvr_engine_stop() and return normally instead;
 * chttpsvr_engine_wait() must be called from a separate thread.
 */
void chttpsvr_engine_wait(void);

/**
 * @brief Signal the shared event_loop engine to stop.
 *
 * Non-blocking and async-signal-safe: safe to call from a signal handler.
 * The engine drains in-flight requests and exits; chttpsvr_engine_wait() can
 * be used to block until that drain completes.
 *
 * Safe to call more than once, including while an earlier call's own drain
 * is still in progress (e.g. two SIGTERMs delivered moments apart, or a
 * defensive extra call from application shutdown code): a repeated or
 * overlapping call is a no-op, never a second, redundant teardown attempt.
 *
 * The library does not install any signal handlers.  Applications are
 * responsible for wiring this function into whatever signal or shutdown
 * mechanism they use.  This is the only function in this module documented
 * as async-signal-safe; chttpsvr_stop() is not (see its own doc comment)
 * and must not be called directly from a signal handler.
 */
void chttpsvr_engine_stop(void);

/* ========================================================================== */
/*                         CONSTRUCTORS                                       */
/* ========================================================================== */

/**
 * @brief Create an HTTP server with a custom allocator.
 *
 * The server is idle until chttpsvr_start() is called.  Routes and middleware
 * may be registered at any time before or after serving.
 *
 * The server always manages its own logger, distinct from any handle passed
 * in by the caller:
 *   - cl == CLOG_INVALID: an internal logger writing only FATAL messages to
 *                  stderr is created.
 *   - cl != CLOG_INVALID: a new logger is derived from cl (clog_derive())
 *                  with the field component=http-server set on it.  The
 *                  caller's cl is left untouched and remains owned by the
 *                  caller.
 * Either way, the resulting server-owned logger is closed automatically by
 * chttpsvr_destroy().
 *
 * @param mprocs   Custom allocator, or NULL for malloc/free.
 * @param cl       Parent logger to derive this server's logger from, or NULL
 *                 to use an internal stderr/FATAL-only logger.
 * @param err_str  Optional: receives a static error string on failure.
 * @return New server handle, or CHTTPSVR_INVALID on failure.
 */
chttpsvr create_chttpsvr_mp(ccol_memmgmt_procs_t *mprocs, clog cl,
                            char **err_str);

/**
 * @brief Create an HTTP server with the default allocator.
 *
 * @return New server handle, or CHTTPSVR_INVALID on failure.
 */
static inline __attribute__((always_inline)) chttpsvr
create_chttpsvr(clog cl, char **err_str) {
  return create_chttpsvr_mp(NULL, cl, err_str);
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/**
 * @brief Internal destroy; use chttpsvr_destroy macro instead.
 *
 * Stops the server if it is running, drains the server's worker pool, and
 * if this is the last running server, stops the shared event_loop engine and
 * waits for it to exit.  Frees all owned resources including sub-routers,
 * routes, and the server-owned ctpool.
 *
 * srv must be a currently-live handle (one returned by create_chttpsvr/_mp
 * and not yet destroyed). A stale handle (one that has already been
 * destroyed, whether by an earlier, completed call to this same function,
 * or concurrently, by another thread racing this one right now), a forged
 * value, or garbage is a fatal error: this function calls fatal_err()
 * (abort()/SIGABRT), rather than risking a use-after-free or double-free,
 * for both a purely sequential double-destroy and a temporally-overlapping
 * concurrent one. CHTTPSVR_INVALID (0) is the one exception and remains a
 * silent no-op, matching chttpsvr_destroy's own "destroy NULLs the handle"
 * idiom.
 *
 * Calling this from within a request handler or middleware currently
 * running on srv's own worker pool (i.e. destroying the very server a
 * handler is executing for, from inside that same handler) is the
 * identical fatal misuse: this thread's own in-flight request can never
 * finish while it is itself blocked here waiting to destroy the server it
 * belongs to, and this function's own worker pool teardown could otherwise
 * never make progress either way. Detected immediately and reported the
 * same way as a stale handle, rather than hanging. To shut a server down
 * from within one of its own handlers, either destroy it from a different
 * thread, or call chttpsvr_engine_stop() (safe to call from within a
 * handler; see its own doc comment) if a full engine shutdown is the goal.
 *
 * Safe to call concurrently with a chttpsvr_engine_stop() that is still
 * force-stopping this same server on its own background reaper thread
 * (e.g. a signal handler triggering chttpsvr_engine_stop() while an
 * application thread independently calls this function on the same
 * handle): whichever of the two finishes tearing srv down first, the other
 * waits for that teardown to fully complete before proceeding, rather than
 * racing it.
 */
void __chttpsvr_destroy(chttpsvr srv);

/**
 * @brief RAII cleanup helper (used with _ccol_destructor).
 *
 * Safe to call on an already-CHTTPSVR_INVALID *pp (a no-op); calling it on a
 * stale, non-CHTTPSVR_INVALID handle that was already destroyed some other
 * way is the same fatal misuse __chttpsvr_destroy itself documents.
 */
static inline __attribute__((always_inline)) void ___chttpsvr_destroy(
    chttpsvr *pp) {
  if (pp && *pp) {
    __chttpsvr_destroy(*pp);
    *pp = CHTTPSVR_INVALID;
  }
}

/**
 * @brief Destroy an HTTP server and set the handle to CHTTPSVR_INVALID.
 *
 * Stops the server if it is running, drains all in-flight requests, and
 * if this is the last running server, stops the shared engine. Calling
 * this a second time on an independently-held copy of the same handle
 * value (whether concurrently, or later, after the first call has already
 * completed), or calling it from within one of this server's own request
 * handlers/middleware, is a fatal error; see __chttpsvr_destroy's own doc
 * comment.
 */
#define chttpsvr_destroy(name)  \
  do {                          \
    __chttpsvr_destroy((name)); \
    (name) = CHTTPSVR_INVALID;  \
  } while (0)

/* ========================================================================== */
/*                         LIFECYCLE MACROS                                   */
/* ========================================================================== */

/** @brief Declare an uninitialised server variable. */
#define chttpsvr_declare(name) chttpsvr name

/** @brief Declare a server variable with automatic destruction on scope exit.
 */
#define chttpsvr_declare_scoped(name) \
  chttpsvr name _ccol_destructor(___chttpsvr_destroy) = CHTTPSVR_INVALID

/**
 * @brief Declare and initialise a server; fatal_err on failure.
 *
 * @param name  Variable name for the server handle.
 * @param cl    Parent logger to derive this server's logger from, or NULL to
 *              use an internal stderr/FATAL-only logger.  See
 *              create_chttpsvr_mp() for details.
 *
 * Example:
 * @code
 * chttpsvr_construct(srv, logger);
 * chttpsvr_register_handler(srv, CHTTP_GET, "/hello", my_handler, NULL);
 * chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
 * cfg.port = 9000;
 * chttpsvr_start(srv, &cfg);
 * chttpsvr_engine_wait();
 * chttpsvr_destroy(srv);
 * @endcode
 */
#define chttpsvr_construct(name, cl)                    \
  chttpsvr name = CHTTPSVR_INVALID;                     \
  do {                                                  \
    char *_chs_err = NULL;                              \
    (name) = create_chttpsvr((cl), &_chs_err);          \
    if (!(name)) {                                      \
      fatal_err("chttpsvr_construct('%s'): %s", #name,  \
                _chs_err ? _chs_err : "unknown error"); \
    }                                                   \
  } while (0)

/**
 * @brief Declare, initialise, and auto-destroy on scope exit; fatal_err on
 *        failure.
 *
 * @param name  Variable name for the server handle.
 * @param cl    Parent logger to derive this server's logger from, or NULL to
 *              use an internal stderr/FATAL-only logger.  See
 *              create_chttpsvr_mp() for details.
 */
#define chttpsvr_construct_scoped(name, cl)                               \
  chttpsvr name _ccol_destructor(___chttpsvr_destroy) = CHTTPSVR_INVALID; \
  do {                                                                    \
    char *_chs_err = NULL;                                                \
    (name) = create_chttpsvr((cl), &_chs_err);                            \
    if (!(name)) {                                                        \
      fatal_err("chttpsvr_construct_scoped('%s'): %s", #name,             \
                _chs_err ? _chs_err : "unknown error");                   \
    }                                                                     \
  } while (0)

/* ========================================================================== */
/*                         SERVER LIFECYCLE                                   */
/* ========================================================================== */

/**
 * @brief Register this server's listener on the shared engine.
 *
 * Binds the listening socket and begins accepting connections.  If this is
 * the first chttpsvr_start() call in the process, the shared event_loop
 * reactor is started automatically in background threads.  Subsequent calls
 * for additional servers reuse the already-running engine.
 *
 * The server creates its own ctpool (worker_thread_count threads,
 * worker_queue_capacity queue depth) at this point.  If the server was
 * previously started and stopped, the old pool is drained and replaced.
 *
 * Restarting srv (chttpsvr_stop() followed by this function) is safe even
 * when a different thread's chttpsvr_stop() call on the same handle is
 * still in flight: this function waits for that call's own teardown of the
 * old listener to fully finish before binding a new one on the same
 * host:port, rather than racing a bind() against a socket the old call has
 * not yet closed.
 *
 * This function is also safe to race against a concurrent, engine-wide
 * teardown (chttpsvr_engine_stop(), or the shared reactor's own graceful
 * shutdown when the last other server referencing it is destroyed): it
 * transparently retries until that teardown has finished, rather than
 * racing it.
 *
 * Returns immediately; use chttpsvr_engine_wait() to block on engine exit.
 *
 * Calling this from within one of srv's own request handlers/middleware to
 * restart the very server that handler is running on (typically preceded
 * by that same handler calling chttpsvr_stop()) is refused with
 * ccol_not_permitted rather than attempted: this thread's own in-flight
 * request can never finish while it is itself blocked draining that exact
 * request, so the restart could never safely proceed. Refusing it leaves
 * srv in a perfectly safe, recoverable state (its previous configuration,
 * or, if chttpsvr_stop() already ran, simply stopped); a later,
 * legitimate call to this function from a different thread still restarts
 * it normally.
 *
 * @param srv  Server handle.
 * @param cfg  Startup configuration.  If NULL, CHTTPSVR_CONFIG_DEFAULT is used.
 * @return ccol_success            Listener bound and registered.
 *         ccol_invalid_args       srv is CHTTPSVR_INVALID, a stale/already-
 *                                 destroyed handle, cfg->port is 0 (unless
 *                                 cfg->host is a "unix://" path, where port
 *                                 is ignored), or cfg->tls is set without
 *                                 both cert_path and key_path also set (a
 *                                 server certificate and its private key
 *                                 are a pair: neither one alone, nor
 *                                 ca_bundle_path by itself, nor an
 *                                 otherwise-empty chttp_tls_config_t such as
 *                                 CHTTP_TLS_DEFAULT, is ever valid
 *                                 server-side TLS configuration).
 *         ccol_not_permitted      The server is already running (stop it first
 *                                 with chttpsvr_stop() before restarting), or
 *                                 this call was made from within one of
 *                                 srv's own request handlers/middleware.
 *         ccol_not_enough_memory  Internal allocation failed, or the shared
 *                                 engine (its reactor threads or its own
 *                                 diagnostics logger) could not be created.
 *         ccol_unexpected_failure The listener socket could not be bound,
 *                                 the listener could not be registered with
 *                                 the shared reactor, the shared idle-timeout
 *                                 sweep thread could not be started, or
 *                                 cfg->tls was set but its certificate/key
 *                                 pair or ca_bundle_path could not be loaded.
 */
ccol_retval_t chttpsvr_start(chttpsvr srv, const chttpsvr_config_t *cfg);

/**
 * @brief Close this server's listener socket.
 *
 * Stops accepting new connections on this server's port.  In-flight requests
 * that are already running continue until they complete; the engine and any
 * other registered servers keep running.
 *
 * To wait for all in-flight requests on this server to drain before freeing
 * resources, call chttpsvr_destroy() (which waits internally).
 *
 * A silent no-op if srv is CHTTPSVR_INVALID or a stale/already-destroyed
 * handle.
 *
 * Safe to call concurrently with a chttpsvr_start() call restarting the same
 * handle on a different thread: the restart waits for this call's own
 * teardown of the old listener to fully finish before binding a new one,
 * rather than racing it.
 *
 * Synchronous and NOT async-signal-safe: unlike chttpsvr_engine_stop(), do
 * not call this function directly from a signal handler (it takes internal
 * locks that the interrupted thread could already be holding, e.g. inside
 * chttpsvr_start()/chttpsvr_destroy(), which can self-deadlock the calling
 * thread). For a signal-driven shutdown hook, use chttpsvr_engine_stop()
 * instead; it is documented as async-signal-safe specifically because its
 * implementation defers all such work off of the interrupted thread.
 */
void chttpsvr_stop(chttpsvr srv);

/* ========================================================================== */
/*                         ROUTE REGISTRATION (SERVER)                        */
/* ========================================================================== */

/**
 * @brief Register a buffered-body route on the server.
 *
 * The complete request body (if any) is buffered before the handler is
 * called.  The handler runs on a ctpool worker thread and may block.
 *
 * @param srv      Server handle.
 * @param method   HTTP method to match, or CHTTP_ANY to match every method.
 *                 When CHTTP_ANY is used the handler receives all methods on
 *                 the pattern; call chttpsvr_req_method() inside the handler
 *                 to branch on the actual method.  Registration order still
 *                 governs priority: a method-specific route registered before
 *                 a CHTTP_ANY route on the same pattern wins for its method,
 *                 while the CHTTP_ANY route catches every other method.
 * @param pattern  URL pattern; must start with '/'.  Supports named
 *                 parameters: /users/{id}.  Consecutive slashes and a
 *                 trailing slash are rejected.  Each {name} must be unique
 *                 within the pattern; reusing the same name twice (e.g.
 *                 /a/{id}/b/{id}) is rejected.
 * @param fn       Handler function.
 * @param ctx      Opaque user data passed to fn.
 * @return ccol_success, ccol_invalid_args (srv is CHTTPSVR_INVALID, a
 *         stale/already-destroyed handle, or pattern/fn is NULL; pattern
 *         does not start with '/'; pattern contains consecutive or
 *         trailing slashes; a {name} segment is malformed (missing closing
 *         '}') or contains a character outside [A-Za-z0-9_]; or the same
 *         {name} is used more than once), or ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_register_handler(chttpsvr srv, chttp_method_t method,
                                        const char *pattern,
                                        chttpsvr_handler_fn fn, void *ctx);

/**
 * @brief Register a streaming-body route on the server.
 *
 * The reactor thread is never blocked: routing happens as soon as headers
 * are parsed, before any body byte is read, and the handler is dispatched to
 * the server's ctpool right away regardless of body size.  Inside the
 * handler use chttpsvr_req_read() to pull each body batch as it actually
 * arrives on the socket; read live by the worker thread, not pre-buffered.
 *
 * If the server's task queue is full when a request arrives, the server
 * returns 503 Service Unavailable immediately.
 *
 * @param srv     Server handle.
 * @param method  HTTP method to match, or CHTTP_ANY to match every method.
 *                See chttpsvr_register_handler for the CHTTP_ANY semantics.
 * @param pattern URL pattern; must start with '/'.  Supports named
 *                parameters: /users/{id}.  Consecutive slashes and a
 *                trailing slash are rejected.  Each {name} must be unique
 *                within the pattern; reusing the same name twice (e.g.
 *                /a/{id}/b/{id}) is rejected.
 * @param fn      Handler function (runs in ctpool worker thread).
 * @param ctx     Opaque user data passed to fn.
 * @return ccol_success, ccol_invalid_args (srv is CHTTPSVR_INVALID, a
 *         stale/already-destroyed handle, or pattern/fn is NULL; pattern
 *         does not start with '/'; pattern contains consecutive or
 *         trailing slashes; a {name} segment is malformed (missing closing
 *         '}') or contains a character outside [A-Za-z0-9_]; or the same
 *         {name} is used more than once), or ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_register_streaming_handler(chttpsvr srv,
                                                  chttp_method_t method,
                                                  const char *pattern,
                                                  chttpsvr_handler_fn fn,
                                                  void *ctx);

/**
 * @brief Add global middleware (runs before every route handler).
 *
 * Middleware is executed in registration order.  All global middleware runs
 * before sub-router middleware and before the final handler.
 *
 * @param srv  Server handle.
 * @param fn   Middleware function.
 * @param ctx  Opaque user data passed to fn.
 * @return ccol_success, ccol_invalid_args (srv is CHTTPSVR_INVALID, a
 *         stale/already-destroyed handle, or fn is NULL), ccol_not_enough_
 *         memory, or ccol_not_permitted if the root router's middleware
 *         chain is already at the 32-entry cap (see "Middleware chain
 *         limit" above).
 */
ccol_retval_t chttpsvr_use(chttpsvr srv, chttpsvr_middleware_fn fn, void *ctx);

/* ========================================================================== */
/*                         SUB-ROUTERS                                        */
/* ========================================================================== */

/**
 * @brief Create a sub-router scoped to a path prefix.
 *
 * Routes registered on the sub-router are matched only when the request path
 * starts with prefix.  The sub-router's own middleware runs after global
 * middleware and before the final route handler.
 *
 * Sub-router lifetime is tied to the server; do not free the returned pointer.
 * Every chttpsvr_router_on / chttpsvr_router_on_stream / chttpsvr_router_use
 * call against a given sub-router validates that the owning server has not
 * been destroyed (including concurrently, from another thread) before
 * touching it, exactly like every other mutating call in this API that takes
 * a chttpsvr handle directly; it is therefore safe to hold a sub-router
 * pointer across a chttpsvr_stop()/chttpsvr_start() restart, and a
 * registration call racing a concurrent chttpsvr_destroy() of the owning
 * server simply returns ccol_invalid_args rather than touching freed memory.
 *
 * IMPORTANT; the "/" prefix edge case:
 *   A prefix of "/" undergoes no trailing-slash removal (the strip loop only
 *   runs when the prefix length exceeds one character), so it is stored as-is
 *   with prefix_len = 1.  A "/" sub-router matches only the exact request
 *   path "/"; any other path, including one starting with a second slash
 *   (e.g. "//foo"), does not match and falls through to the next router.
 *   All routes registered on a "/" sub-router are therefore only reachable
 *   from the exact path "/".
 *
 *   If you want a catch-all sub-router that receives every request, register
 *   routes directly on the server with chttpsvr_register_handler /
 *   chttpsvr_register_streaming_handler instead of using a sub-router with
 *   prefix "/".
 *
 * @param srv     Server handle (must not be CHTTPSVR_INVALID).
 * @param prefix  Path prefix (e.g. "/api/v1"); must be non-NULL and start
 *                with '/'.  Consecutive slashes (e.g. "//api" or "/a//b") are
 *                rejected.  A trailing slash, if present, is stripped
 *                automatically so "/api/v1" and "/api/v1/" are equivalent.
 *                The prefix "/" results in a sub-router that only matches the
 *                exact root path "/"; see the note above.
 * @return Sub-router pointer (owned by srv), or NULL when srv is
 *         CHTTPSVR_INVALID, a stale/already-destroyed handle, prefix is
 *         NULL, prefix does not start with '/', prefix contains
 *         consecutive slashes, or on OOM.
 */
chttpsvr_router *chttpsvr_subrouter(chttpsvr srv, const char *prefix);

/**
 * @brief Register a buffered-body route on a sub-router.
 *
 * The effective match path is prefix + pattern (e.g. "/api/v1" + "/users/{id}"
 * matches "/api/v1/users/42").
 *
 * @param router   Sub-router returned by chttpsvr_subrouter().
 * @param method   HTTP method to match, or CHTTP_ANY to match every method.
 *                 See chttpsvr_register_handler for the CHTTP_ANY semantics.
 * @param pattern  URL pattern relative to the prefix; must start with '/'.
 *                 Consecutive slashes and a trailing slash are rejected.
 *                 Each {name} must be unique within the pattern; reusing the
 *                 same name twice (e.g. /a/{id}/b/{id}) is rejected.
 * @param fn       Handler function.
 * @param ctx      Opaque user data passed to fn.
 * @return ccol_success, ccol_invalid_args (router/pattern/fn NULL; pattern
 *         does not start with '/'; pattern contains consecutive or trailing
 *         slashes; a {name} segment is malformed (missing closing '}') or
 *         contains a character outside [A-Za-z0-9_]; the same {name} is used
 *         more than once; or router's owning server has since been
 *         destroyed), or ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_router_on(chttpsvr_router *router, chttp_method_t method,
                                 const char *pattern, chttpsvr_handler_fn fn,
                                 void *ctx);

/**
 * @brief Register a streaming-body route on a sub-router.
 *
 * @param router  Sub-router.
 * @param method  HTTP method to match, or CHTTP_ANY to match every method.
 *                See chttpsvr_register_handler for the CHTTP_ANY semantics.
 * @param pattern URL pattern relative to the prefix; must start with '/'.
 *                Consecutive slashes and a trailing slash are rejected.
 *                Each {name} must be unique within the pattern; reusing the
 *                same name twice (e.g. /a/{id}/b/{id}) is rejected.
 * @param fn      Handler function (runs in ctpool worker thread).
 * @param ctx     Opaque user data passed to fn.
 * @return ccol_success, ccol_invalid_args (router/pattern/fn NULL; pattern
 *         does not start with '/'; pattern contains consecutive or trailing
 *         slashes; a {name} segment is malformed (missing closing '}') or
 *         contains a character outside [A-Za-z0-9_]; the same {name} is used
 *         more than once; or router's owning server has since been
 *         destroyed), or ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_router_on_stream(chttpsvr_router *router,
                                        chttp_method_t method,
                                        const char *pattern,
                                        chttpsvr_handler_fn fn, void *ctx);

/**
 * @brief Add middleware to a sub-router.
 *
 * Runs after global middleware and before the sub-router's own handlers.
 *
 * @param router  Sub-router.
 * @param fn      Middleware function.
 * @param ctx     Opaque user data.
 * @return ccol_success, ccol_invalid_args (router/fn NULL, or router's
 *         owning server has since been destroyed), ccol_not_enough_memory,
 *         or ccol_not_permitted if this router's middleware chain is already
 *         at the 32-entry cap (see "Middleware chain limit" above).
 */
ccol_retval_t chttpsvr_router_use(chttpsvr_router *router,
                                  chttpsvr_middleware_fn fn, void *ctx);

/* ========================================================================== */
/*                         REQUEST API                                        */
/* ========================================================================== */

/**
 * @brief Return the HTTP method of the request.
 *
 * Returns CHTTP_GET when req is NULL (unlike other request accessors, which
 * return NULL, this function cannot signal an error through the return type).
 * Always one of the seven concrete chttp_method_t constants (never CHTTP_ANY,
 * and never any other value): a request whose method this server does not
 * recognize is rejected with 501 before any handler runs, so req always
 * belongs to a request whose method is one of the seven.
 */
chttp_method_t chttpsvr_req_method(const chttpsvr_req *req);

/**
 * @brief Return the URL-decoded request path (NUL-terminated).
 *
 * The returned pointer is valid for the lifetime of the request.
 */
const char *chttpsvr_req_path(const chttpsvr_req *req);

/**
 * @brief Return a request header value by name (case-insensitive).
 *
 * @param req   Request handle.
 * @param name  Header name (e.g. "Content-Type").
 * @return Pointer to the value string, or NULL if not present. If the
 *         client sent the same header name more than once, the LAST
 *         occurrence (in wire order) is returned.
 *
 * Lifetime note: the returned pointer is valid for the duration of the handler
 * call only.  Do NOT store it beyond the handler's return; it points into
 * the pre-copied header array owned by the request context, which is released
 * when the request is torn down.
 *
 * A chunked request body's trailer fields (RFC 7230 SS4.1.2) are indexed
 * exactly like any other header and become retrievable through this same
 * function once they have actually been parsed; there is no separate
 * trailer-specific accessor. For a buffered route this is transparent: the
 * handler is only ever invoked once the whole body, trailers included, has
 * already been read, so a trailer field is always retrievable from the very
 * start of the handler. For a streaming route, a trailer field becomes
 * retrievable only once the handler's own chttpsvr_req_read() calls have
 * drained the body all the way to its natural end (a return of 0);
 * querying it any earlier returns NULL, indistinguishable from the field
 * never having been sent at all.
 */
const char *chttpsvr_req_header(const chttpsvr_req *req, const char *name);

/**
 * @brief Return the request body for buffered routes.
 *
 * @param req      Request handle.
 * @param len_out  If non-NULL, receives the body length in bytes.
 * @return Pointer to the body bytes (not NUL-terminated), or NULL if empty
 *         or if req's own route is a streaming route (use
 *         chttpsvr_req_read() there instead; this function always returns
 *         NULL/0 for it, never a partial or stale view of the body).
 *         Valid for the lifetime of the request.
 */
const void *chttpsvr_req_body(const chttpsvr_req *req, size_t *len_out);

/**
 * @brief Read body bytes for streaming routes (pull-reader model).
 *
 * Reads up to buflen bytes of the request body into buf, batch by batch, as
 * they actually arrive on the connection; this call reads the socket
 * itself (via the worker thread executing the handler, never the reactor
 * thread) and blocks the calling worker until at least one byte is
 * available, the body ends, an error occurs, stream_read_timeout_ms
 * (chttpsvr_config_t) elapses without new data, or the request's total body
 * read time exceeds max_body_read_duration_ms (chttpsvr_config_t), if set.
 *
 * If buflen is 0 the function returns 0 immediately regardless of buf (the
 * call is a no-op, consistent with POSIX read(2) semantics).
 *
 * Returns -1 both for caller misuse (req is NULL, buf is NULL with
 * buflen > 0, or the handler was registered with chttpsvr_register_handler
 * rather than chttpsvr_register_streaming_handler) and for a genuine,
 * data-dependent transfer error (a broken connection, an exceeded
 * stream_read_timeout_ms or max_body_read_duration_ms, or a body that
 * exceeds max_body_size mid-stream). chttpsvr_req_stream_error(), called
 * immediately afterward, distinguishes only the latter, data-dependent
 * group (plus a NULL/invalid req itself); it reports ccol_success, not a
 * distinct code, for the buf-is-NULL and non-streaming-route misuse cases,
 * since both are static programming errors a caller can only have made by
 * violating this function's own documented preconditions (reachable on
 * every single call to the offending code, not intermittently) rather
 * than a condition needing runtime diagnosis.
 *
 * If the request carried "Expect: 100-continue" AND actually has a body to
 * receive (a Content-Length or chunked Transfer-Encoding was present), the
 * very first call to this function (for that request) writes the interim
 * "100 Continue" response before attempting to read anything, telling the
 * client it may go ahead and upload the body. A streaming handler that
 * instead rejects a request outright (bad auth, unacceptable Content-Type,
 * ...) by writing a final response without ever calling this function skips
 * that interim response entirely: the client sees the real rejection
 * directly, exactly as if it had never sent "Expect: 100-continue" and is
 * not asked to upload a body the server was never going to read. A request
 * that carries "Expect: 100-continue" but has no body at all never receives
 * the interim response either, for the identical reason: there is nothing
 * to invite the client to upload.
 *
 * @param req     Request handle (must be from a
 * chttpsvr_register_streaming_handler route).
 * @param buf     Destination buffer.  May be NULL only when buflen is 0.
 * @param buflen  Buffer capacity in bytes.  Passing 0 is a valid no-op.
 * @return Number of bytes read (>0), 0 at EOF or for buflen==0, -1 on error.
 */
ssize_t chttpsvr_req_read(chttpsvr_req *req, void *buf, size_t buflen);

/**
 * @brief Report why the most recent chttpsvr_req_read() call returned -1,
 *        for the subset of causes that need runtime diagnosis.
 *
 * Meaningful only immediately after a -1 return from chttpsvr_req_read();
 * otherwise the result is unspecified (there may be no error to report).
 *
 * Distinguishes only chttpsvr_req_read()'s genuine, data-dependent transfer
 * errors (see that function's own doc comment). It returns ccol_success,
 * indistinguishable from "no error at all," when the -1 was instead caused
 * by caller misuse (buf was NULL with buflen > 0, or the request's route
 * was not registered with chttpsvr_register_streaming_handler); both are
 * static programming errors, reachable on every call to the offending code
 * rather than only sometimes, so a caller hitting one is expected to find
 * and fix it during ordinary testing rather than diagnose it at runtime.
 *
 * @param req  Request handle.
 * @return ccol_timed_out (stream_read_timeout_ms or
 *         max_body_read_duration_ms elapsed),
 *         ccol_msg_too_large (max_body_size exceeded mid-stream),
 *         ccol_not_enough_memory (an internal allocation failed while
 *         growing the body buffer; not a client-caused error),
 *         ccol_http_transfer_aborted (connection closed or malformed
 *         framing), ccol_success (no error recorded, OR the -1 was actually
 *         caused by one of the caller-misuse cases above), or
 *         ccol_unexpected_failure (req or its handle is invalid).
 */
ccol_retval_t chttpsvr_req_stream_error(const chttpsvr_req *req);

/**
 * @brief Return a named path parameter extracted from the URL pattern.
 *
 * For route /users/{id}, calling chttpsvr_req_param(req, "id") on a request
 * to /users/42 returns "42" (URL-decoded).
 *
 * @param req   Request handle.
 * @param name  Parameter name (without braces).
 * @return Pointer to the value string, or NULL if the name is not a parameter
 *         in the matched route.  Valid for the lifetime of the request.
 */
const char *chttpsvr_req_param(const chttpsvr_req *req, const char *name);

/**
 * @brief Return all values for a query parameter key (multi-value support).
 *
 * For ?foo=1&foo=2, chttpsvr_req_query(req, "foo", &n) returns {"1","2"} and
 * sets *n = 2.
 *
 * @param req        Request handle.
 * @param key        Query parameter name (URL-decoded comparison).
 * @param count_out  If non-NULL, receives the number of values found.
 * @return Pointer to a NULL-terminated array of NUL-terminated value strings,
 *         or NULL if the key is absent.  The string values pointed to by the
 *         array elements are valid for the lifetime of the request.  The array
 *         pointer itself is a shared scratch buffer: it is only valid until the
 *         next call to chttpsvr_req_query on the same request.  Copy any
 *         values you need before making a subsequent query call.
 *
 * NOTE: this function cannot distinguish an OOM failure (either during
 * query-string parsing or when growing the result array) from a genuine
 * key-absent result; both produce (NULL, count=0).  Use
 * chttpsvr_req_query_oom() after a NULL return to tell these two cases apart,
 * or use chttpsvr_req_query_one for single-valued keys.
 */
const char **chttpsvr_req_query(chttpsvr_req *req, const char *key,
                                size_t *count_out);

/**
 * @brief Return the single value for a query parameter key.
 *
 * Fails if the key has multiple values or is absent.
 *
 * @param req      Request handle.
 * @param key      Query parameter name.
 * @param val_out  On success, receives a pointer to the value string; reset
 *                 to NULL (if non-NULL itself) on every failure return.
 * @return ccol_success            Exactly one value found; *val_out is set.
 *         ccol_key_not_found      Key is absent.
 *         ccol_not_permitted      Key has multiple values.
 *         ccol_not_enough_memory  Query-string parse buffer could not be
 *                                 allocated.
 *         ccol_invalid_args       req or key is NULL.
 */
ccol_retval_t chttpsvr_req_query_one(chttpsvr_req *req, const char *key,
                                     const char **val_out);

/**
 * @brief Return the raw (URL-encoded) query string, or NULL if absent.
 *
 * E.g. for /search?q=hello%20world&page=2 this returns
 * "q=hello%20world&page=2".
 */
const char *chttpsvr_req_raw_query(const chttpsvr_req *req);

/**
 * @brief Return true if a query-string allocation for this request has
 *        failed under OOM: either chttpsvr_req_query's own result-array
 *        growth, or the request's one-time, shared query-string parse that
 *        chttpsvr_req_query and chttpsvr_req_query_one each lazily trigger
 *        on first use. chttpsvr_req_raw_query never triggers this parse (it
 *        returns the raw string captured directly from the request line),
 *        so it has no bearing on this flag.
 *
 * chttpsvr_req_query returns (NULL, count=0) both when the requested key is
 * absent AND when either of the above allocations failed under OOM. After
 * any NULL return from chttpsvr_req_query, call this function to
 * distinguish the two cases.
 *
 * The flag is latching: once set it stays true for the lifetime of the
 * request regardless of subsequent query calls.
 *
 * Returns false when req is NULL.
 */
bool chttpsvr_req_query_oom(const chttpsvr_req *req);

/* ========================================================================== */
/*                         RESPONSE API                                       */
/* ========================================================================== */

/**
 * @brief Set the HTTP response status code (default: 200).
 *
 * status_code is stored verbatim, with no validation here; a value outside
 * the valid HTTP status-code range 100-999 is silently sent as 500 on the
 * wire instead, at actual response-send time.
 *
 * Setting status_code to a 1xx, 204, or 304 value silences whatever body
 * was already (or is later) written via chttpsvr_resp_write / _write_str /
 * _printf / _write_json, regardless of call order; see chttpsvr_resp_write's
 * own doc comment.
 */
void chttpsvr_resp_set_status(chttpsvr_resp *resp, int status_code);

/**
 * @brief Set a response header.
 *
 * If a header with the same name already exists (case-insensitive comparison
 * per RFC 7230) the stored value is replaced in-place and the function returns
 * immediately.  The stored header name retains the casing from the FIRST call;
 * the casing supplied by subsequent calls for the same name is ignored.
 * Because HTTP/1.1 header names are case-insensitive this does not affect
 * wire-level correctness, but callers who inspect the stored names directly
 * should be aware of this behaviour.
 *
 * All three arguments must be non-NULL; passing NULL for any returns
 * ccol_invalid_args without modifying the response. name must also be a
 * non-empty RFC 7230 SS3.2.6 tchar-only token: every byte must be an ASCII
 * letter, digit, or one of "!#$%&'*+-.^_`|~". A zero-length name has no
 * valid on-the-wire representation; a name containing any other byte (e.g.
 * a space or a literal ':') is not itself a CRLF-injection vector but
 * still produces a structurally malformed wire line a strict downstream
 * parser could misread (e.g. "X Foo: bar" as a name puts
 * "X Foo:bar:baz\r\n" on the wire, not a genuine two-field split).
 *
 * value must not contain a CR or LF byte; both name and value are written
 * verbatim onto the wire with no further escaping, so an embedded CR/LF in
 * value would let a caller that reflects request-controlled data (a query
 * parameter, a path parameter, an echoed request header) into a response
 * header inject arbitrary extra header lines or split the response in two
 * on behalf of whoever controls that data. Rejected with ccol_invalid_args
 * rather than silently stripped or truncated.
 *
 * "Connection" and "Content-Length" are accepted (name/value are still
 * validated and stored like any other header) but never sent verbatim: the
 * server always decides and emits its own Connection header, reflecting
 * whether the connection is actually kept open afterward, and always
 * computes and emits its own Content-Length header from the response body
 * actually written via chttpsvr_resp_write / _write_str / _printf /
 * _write_json, since both decisions drive real wire framing and a
 * caller-supplied value could otherwise silently disagree with what the
 * server actually does, desynchronizing a kept-alive connection's framing
 * for whichever request follows.
 *
 * "Transfer-Encoding" is rejected outright with ccol_invalid_args instead:
 * this server never transfer-codes a response body, so honoring a
 * caller-set Transfer-Encoding header is impossible, and letting one reach
 * the wire would pair it with this function's own auto-computed
 * Content-Length header over a body that was never actually transfer-coded:
 * an ambiguous framing an intermediary that honors Transfer-Encoding
 * over Content-Length (RFC 7230 SS3.3.3) could misparse, the same class of
 * response-splitting hazard the Connection/Content-Length handling above
 * exists to prevent. Mirrors chttp_request_set_header's identical
 * Transfer-Encoding rejection on the client side (chttpclient.h).
 *
 * @param resp   Response handle (must not be NULL).
 * @param name   Header name (must not be NULL or empty, must contain only
 *               RFC 7230 tchar bytes, and must not be "Transfer-Encoding").
 * @param value  Header value (must not be NULL, must not contain CR/LF).
 * @return ccol_success, ccol_invalid_args (resp/name/value is NULL; name is
 *         empty or contains a byte outside the RFC 7230 tchar set; value
 *         contains a CR or LF byte; or name is "Transfer-Encoding",
 *         case-insensitive), or ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_resp_set_header(chttpsvr_resp *resp, const char *name,
                                       const char *value);

/**
 * @brief Append bytes to the response body.
 *
 * Multiple calls accumulate.  The complete buffer is flushed when the handler
 * returns.
 *
 * The accumulated body is silently never written to the wire, regardless of
 * how much was appended here, if the response's final status code (whatever
 * chttpsvr_resp_set_status last set by the time the handler returns) is a
 * 1xx, 204, or 304: RFC 9110 6.4.1/15.2.1/15.4.5 forbid all three from ever
 * carrying a body. This function itself still returns ccol_success for a
 * genuinely successful append; nothing in this API reports an error for the
 * body having been discarded this way, so a handler that writes diagnostic
 * or informational body content and only later (e.g. via a cache-validation
 * middleware) has the status downgraded to one of these codes will not see
 * that content reach the client, with no error signal anywhere to catch it.
 * A 1xx or 204 additionally never gets an auto-injected Content-Length
 * header at all; a 304, unlike those two, still reports one matching the
 * length of the body that was written here, even though the body bytes
 * themselves are withheld the same way.
 *
 * @param resp  Response handle.
 * @param data  Data to write.
 * @param len   Number of bytes.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_resp_write(chttpsvr_resp *resp, const void *data,
                                  size_t len);

/**
 * @brief Append a NUL-terminated string to the response body.
 *
 * See chttpsvr_resp_write's own doc comment for the 1xx/204/304
 * body-discard interaction, which applies identically here.
 */
ccol_retval_t chttpsvr_resp_write_str(chttpsvr_resp *resp, const char *str);

/**
 * @brief Format and append a printf-style string to the response body.
 *
 * Equivalent to formatting `format`/`...` with printf semantics and passing
 * the result to chttpsvr_resp_write_str(), without an intermediate
 * caller-visible allocation. See chttpsvr_resp_write's own doc comment for
 * the 1xx/204/304 body-discard interaction, which applies identically here.
 *
 * @param resp    Response handle (must not be NULL).
 * @param fmt  printf-style format string (must not be NULL).
 * @return ccol_success, ccol_invalid_args (resp/format is NULL, or the
 *         underlying vsnprintf encoding fails), or ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_resp_printf(chttpsvr_resp *resp, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * @brief Append a JSON body and set Content-Type to application/json.
 *
 * Appends len bytes of json to the response body and then sets the
 * content-type response header to "application/json".  The body is written
 * first: an OOM on the body allocation leaves the response completely
 * unchanged (no header is set, no bytes are buffered).  If the body write
 * succeeds but the subsequent header set fails with OOM, the body bytes are
 * already buffered; the caller receives ccol_not_enough_memory but the
 * response body is partially written.  This is an unlikely degenerate case
 * documented here for completeness.
 *
 * Returns ccol_invalid_args when resp or json is NULL, or when len is 0
 * (a zero-length JSON body with Content-Type set is semantically invalid;
 * the function rejects it rather than silently setting the header with no
 * body bytes).
 *
 * See chttpsvr_resp_write's own doc comment for the 1xx/204/304
 * body-discard interaction, which applies identically here.
 *
 * @param resp  Response handle.
 * @param json  JSON data (must be non-NULL).
 * @param len   Length of json in bytes (must be > 0).
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_resp_write_json(chttpsvr_resp *resp, const char *json,
                                       size_t len);
