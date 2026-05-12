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
 * sub-router.  A root-level route that matches the same path as a sub-router's
 * route will therefore always win, shadowing the sub-router route completely.
 * To avoid unintentional shadowing, do not register root-level routes whose
 * paths overlap with a sub-router's prefix + pattern combination.
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

/** @brief Server handle (pointer to opaque struct). */
typedef chttpserver *chttpsvr;

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
 * the chain; calling it more than once will invoke the tail of the chain
 * multiple times and must be avoided.
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
  /** Max request body in bytes (default 4 MiB). A buffered route whose body
   *  exceeds this is rejected with 413 before the handler ever runs; a
   *  streaming route's handler is always invoked, and chttpsvr_req_read()
   *  returns -1 with chttpsvr_req_stream_error() == ccol_msg_too_large once
   *  the limit is crossed. Either way the connection is closed after the
   *  resulting response (Connection: close) rather than kept alive, since
   *  the excess body bytes beyond the limit are discarded, not drained. */
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
   *  respond 408 automatically. */
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
   *  side. */
  unsigned response_write_timeout_ms;
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
   *  slot) rather than accepted and immediately rejected. */
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
 * logger. If no logger has been installed when the engine first starts,
 * diagnostics at those points are simply skipped; there is no default
 * logger to fall back to (unlike create_chttpsvr's own internal
 * stderr/FATAL-only logger).
 *
 * Internally this function derives a logger from cl via clog_derive() and
 * adds the field component=http-engine.  The previously registered engine
 * logger (if any) is closed.  The caller retains ownership of cl and must
 * keep it alive for as long as any server may be running.
 *
 * @param cl  Parent logger to derive from; must not be NULL.
 * @return ccol_success or ccol_invalid_args (cl is NULL).
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
 * shared reactor itself has fully exited -- that final teardown runs on a
 * separate reaper thread. If the calling code needs a deterministic guarantee
 * that the engine has fully exited (e.g. right before process exit, so an
 * engine-installed logger via chttpsvr_set_engine_logger() is not still
 * reachable), call chttpsvr_engine_wait() explicitly after chttpsvr_destroy().
 */
void chttpsvr_engine_wait(void);

/**
 * @brief Signal the shared event_loop engine to stop.
 *
 * Non-blocking and async-signal-safe: safe to call from a signal handler.
 * The engine drains in-flight requests and exits; chttpsvr_engine_wait() can
 * be used to block until that drain completes.
 *
 * The library does not install any signal handlers.  Applications are
 * responsible for wiring this function (or chttpsvr_stop()) into whatever
 * signal or shutdown mechanism they use.
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
 *   - cl == NULL:  an internal logger writing only FATAL messages to stderr
 *                  is created.
 *   - cl != NULL:  a new logger is derived from cl (clog_derive()) with the
 *                  field component=http-server set on it.  The caller's cl
 *                  is left untouched and remains owned by the caller.
 * Either way, the resulting server-owned logger is closed automatically by
 * chttpsvr_destroy().
 *
 * @param mprocs   Custom allocator, or NULL for malloc/free.
 * @param cl       Parent logger to derive this server's logger from, or NULL
 *                 to use an internal stderr/FATAL-only logger.
 * @param err_str  Optional: receives a static error string on failure.
 * @return New server handle, or NULL on failure.
 */
chttpsvr create_chttpsvr_mp(ccol_memmgmt_procs_t *mprocs, clog cl,
                            char **err_str);

/**
 * @brief Create an HTTP server with the default allocator.
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
 */
void __chttpsvr_destroy(chttpsvr srv);

/**
 * @brief RAII cleanup helper (used with _ccol_destructor).
 */
static inline __attribute__((always_inline)) void ___chttpsvr_destroy(
    chttpsvr *pp) {
  if (pp && *pp) {
    __chttpsvr_destroy(*pp);
    *pp = NULL;
  }
}

/**
 * @brief Destroy an HTTP server and set the handle to NULL.
 *
 * Stops the server if it is running, drains all in-flight requests, and
 * if this is the last running server, stops the shared engine.
 * Must not be called concurrently with other operations on the same handle.
 */
#define chttpsvr_destroy(name)  \
  do {                          \
    __chttpsvr_destroy((name)); \
    (name) = NULL;              \
  } while (0)

/* ========================================================================== */
/*                         LIFECYCLE MACROS                                   */
/* ========================================================================== */

/** @brief Declare an uninitialised server variable. */
#define chttpsvr_declare(name) chttpsvr name

/** @brief Declare a server variable with automatic destruction on scope exit.
 */
#define chttpsvr_declare_scoped(name) \
  chttpsvr name _ccol_destructor(___chttpsvr_destroy) = NULL

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
  chttpsvr name = NULL;                                 \
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
#define chttpsvr_construct_scoped(name, cl)                   \
  chttpsvr name _ccol_destructor(___chttpsvr_destroy) = NULL; \
  do {                                                        \
    char *_chs_err = NULL;                                    \
    (name) = create_chttpsvr((cl), &_chs_err);                \
    if (!(name)) {                                            \
      fatal_err("chttpsvr_construct_scoped('%s'): %s", #name, \
                _chs_err ? _chs_err : "unknown error");       \
    }                                                         \
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
 * Returns immediately; use chttpsvr_engine_wait() to block on engine exit.
 *
 * @param srv  Server handle.
 * @param cfg  Startup configuration.  If NULL, CHTTPSVR_CONFIG_DEFAULT is used.
 * @return ccol_success            Listener bound and registered.
 *         ccol_invalid_args       srv is NULL or cfg->port is 0.
 *         ccol_not_permitted      The server is already running (stop it first
 *                                 with chttpsvr_stop() before restarting).
 *         ccol_not_enough_memory  Internal allocation failed.
 *         ccol_unexpected_failure The listener socket could not be bound, or
 *                                 the engine thread could not be started.
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
 *                 trailing slash are rejected.
 * @param fn       Handler function.
 * @param ctx      Opaque user data passed to fn.
 * @return ccol_success, ccol_invalid_args (srv/pattern/fn NULL; pattern does
 *         not start with '/'; or pattern contains consecutive or trailing
 *         slashes), or ccol_not_enough_memory.
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
 *                trailing slash are rejected.
 * @param fn      Handler function (runs in ctpool worker thread).
 * @param ctx     Opaque user data passed to fn.
 * @return ccol_success, ccol_invalid_args (srv/pattern/fn NULL; pattern does
 *         not start with '/'; or pattern contains consecutive or trailing
 *         slashes), or ccol_not_enough_memory.
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
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_not_permitted if the root router's middleware chain is already
 *         at the 32-entry cap (see "Middleware chain limit" above).
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
 *
 * IMPORTANT; the "/" prefix edge case:
 *   A prefix of "/" undergoes no trailing-slash removal (the strip loop only
 *   runs when the prefix length exceeds one character), so it is stored as-is
 *   with prefix_len = 1.  The matcher checks path[prefix_len], i.e. path[1],
 *   which must be either '/' or '\0'.  For the root path "/" the character at
 *   index 1 is '\0', so the router is entered.  For any other path such as
 *   "/foo", path[1] is 'f', which fails the check, so the router is skipped
 *   entirely.  All routes registered on a "/" sub-router are therefore only
 *   reachable from the exact path "/".
 *
 *   If you want a catch-all sub-router that receives every request, register
 *   routes directly on the server with chttpsvr_register_handler /
 * chttpsvr_register_streaming_handler instead of using a sub-router with prefix
 * "/".
 *
 * @param srv     Server handle (must not be NULL).
 * @param prefix  Path prefix (e.g. "/api/v1"); must be non-NULL and start
 *                with '/'.  Consecutive slashes (e.g. "//api" or "/a//b") are
 *                rejected.  A trailing slash, if present, is stripped
 *                automatically so "/api/v1" and "/api/v1/" are equivalent.
 *                The prefix "/" results in a sub-router that only matches the
 *                exact root path "/"; see the note above.
 * @return Sub-router pointer (owned by srv), or NULL when srv is NULL,
 *         prefix is NULL, prefix does not start with '/', prefix contains
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
 * @param fn       Handler function.
 * @param ctx      Opaque user data passed to fn.
 * @return ccol_success, ccol_invalid_args (router/pattern/fn NULL; pattern
 *         does not start with '/'; or pattern contains consecutive or trailing
 *         slashes), or ccol_not_enough_memory.
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
 * @param fn      Handler function (runs in ctpool worker thread).
 * @param ctx     Opaque user data passed to fn.
 * @return ccol_success, ccol_invalid_args (router/pattern/fn NULL; pattern
 *         does not start with '/'; or pattern contains consecutive or trailing
 *         slashes), or ccol_not_enough_memory.
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
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_not_permitted if this router's middleware chain is already at
 *         the 32-entry cap (see "Middleware chain limit" above).
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
 * @return Pointer to the value string, or NULL if not present.
 *
 * Lifetime note: the returned pointer is valid for the duration of the handler
 * call only.  Do NOT store it beyond the handler's return; it points into
 * the pre-copied header array owned by the request context, which is released
 * when the request is torn down.
 */
const char *chttpsvr_req_header(const chttpsvr_req *req, const char *name);

/**
 * @brief Return the request body for buffered routes.
 *
 * @param req      Request handle.
 * @param len_out  If non-NULL, receives the body length in bytes.
 * @return Pointer to the body bytes (not NUL-terminated), or NULL if empty.
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
 * Returns -1 for hard errors (req is NULL, buf is NULL with buflen > 0, or
 * the handler was registered with chttpsvr_register_handler rather than
 * chttpsvr_register_streaming_handler) as well as for a broken connection,
 * an exceeded stream_read_timeout_ms or max_body_read_duration_ms, or a body
 * that exceeds max_body_size mid-stream; call chttpsvr_req_stream_error()
 * immediately afterward to distinguish these.
 *
 * @param req     Request handle (must be from a
 * chttpsvr_register_streaming_handler route).
 * @param buf     Destination buffer.  May be NULL only when buflen is 0.
 * @param buflen  Buffer capacity in bytes.  Passing 0 is a valid no-op.
 * @return Number of bytes read (>0), 0 at EOF or for buflen==0, -1 on error.
 */
ssize_t chttpsvr_req_read(chttpsvr_req *req, void *buf, size_t buflen);

/**
 * @brief Report why the most recent chttpsvr_req_read() call returned -1.
 *
 * Meaningful only immediately after a -1 return from chttpsvr_req_read();
 * otherwise the result is unspecified (there may be no error to report).
 *
 * @param req  Request handle.
 * @return ccol_timed_out (stream_read_timeout_ms or
 *         max_body_read_duration_ms elapsed),
 *         ccol_msg_too_large (max_body_size exceeded mid-stream),
 *         ccol_http_transfer_aborted (connection closed or malformed
 *         framing), ccol_success (no error recorded), or
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
 * @param val_out  On success, receives a pointer to the value string.
 * @return ccol_success            Exactly one value found; *val_out is set.
 *         ccol_key_not_found     Key is absent.
 *         ccol_not_permitted     Key has multiple values.
 *         ccol_not_enough_memory Query-string parse buffer could not be
 * allocated. ccol_invalid_args      req or key is NULL.
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
 * @brief Return true if chttpsvr_req_query ran out of memory while building
 *        the result array for this request.
 *
 * chttpsvr_req_query returns (NULL, count=0) both when the requested key is
 * absent AND when the internal result array could not be grown due to OOM.
 * After any NULL return from chttpsvr_req_query, call this function to
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
 * ccol_invalid_args without modifying the response.
 *
 * @param resp   Response handle (must not be NULL).
 * @param name   Header name (must not be NULL).
 * @param value  Header value (must not be NULL).
 * @return ccol_success, ccol_invalid_args (resp/name/value is NULL), or
 *         ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_resp_set_header(chttpsvr_resp *resp, const char *name,
                                       const char *value);

/**
 * @brief Append bytes to the response body.
 *
 * Multiple calls accumulate.  The complete buffer is flushed when the handler
 * returns.
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
 */
ccol_retval_t chttpsvr_resp_write_str(chttpsvr_resp *resp, const char *str);

/**
 * @brief Format and append a printf-style string to the response body.
 *
 * Equivalent to formatting `format`/`...` with printf semantics and passing
 * the result to chttpsvr_resp_write_str(), without an intermediate
 * caller-visible allocation.
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
 * @param resp  Response handle.
 * @param json  JSON data (must be non-NULL).
 * @param len   Length of json in bytes (must be > 0).
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_resp_write_json(chttpsvr_resp *resp, const char *json,
                                       size_t len);
