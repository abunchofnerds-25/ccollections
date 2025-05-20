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
 * @brief HTTP/1.1 server backed by facil.io with routing, middleware,
 *        sub-routers, TLS, and optional streaming-body dispatch.
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
 * Every request -- whether registered with chttpsvr_register_handler
 * (buffered body) or chttpsvr_register_streaming_handler (streaming body) --
 * is dispatched off the facil.io reactor thread and run inside the server's
 * own ctpool worker thread pool.  Reactor threads are never blocked by user
 * handler code.
 *
 * Each server owns one ctpool, created when chttpsvr_start() is called.  The
 * pool size is governed by chttpsvr_config_t.worker_thread_count and
 * chttpsvr_config_t.worker_queue_capacity.  If the task queue is full when a
 * request arrives, the server returns 503 Service Unavailable immediately.
 *
 * Streaming handlers may call chttpsvr_req_read() to pull body bytes
 * incrementally.  Buffered handlers receive the complete body in memory via
 * chttpsvr_req_body().  Both handler types run on worker threads and may block.
 *
 * ### Engine lifecycle (implicit)
 *
 * The shared facil.io event loop (the "engine") is started automatically on
 * the first chttpsvr_start() call and stopped automatically when the last
 * running server is destroyed.  There is no need to call engine lifecycle
 * functions manually.
 *
 * To install a custom logger for engine-level events (TLS init, epoll/kqueue
 * internals, etc.) before the first server starts, call
 * chttpsvr_set_engine_logger().
 *
 * To block the calling thread until the engine exits (e.g. after an external
 * shutdown signal such as SIGTERM handled by fio_stop()), call
 * chttpsvr_engine_wait().
 *
 * Multiple servers may run concurrently -- each listens on its own port and
 * has its own routes, middleware, worker pool, and clog handle.
 *
 * Typical single-server pattern:
 * @code
 *   chttpsvr_construct(srv, logger);
 *   chttpsvr_register_handler(srv, CHTTP_GET, "/hello", hello, NULL);
 *   chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
 *   chttpsvr_start(srv, &cfg);
 *   chttpsvr_engine_wait();  // blocks until signal / fio_stop()
 *   chttpsvr_destroy(srv);
 * @endcode
 *
 * ### Middleware chain limit
 *
 * The combined middleware chain per request (global middleware + the matched
 * router's own middleware) is capped at 32 entries.  Exceeding this limit
 * causes every request through the affected router to receive a 500 response.
 * The limit is enforced at dispatch time, not at registration time:
 * chttpsvr_use and chttpsvr_router_use always return ccol_success regardless
 * of how many entries have already been registered.
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
  /** Bind address; NULL or "0.0.0.0" binds all interfaces. */
  const char *host;
  /** Listening port (default 8080). */
  uint16_t port;
  /** Max request body in bytes before 413 is returned (default 4 MiB). */
  size_t max_body_size;
  /** Per-connection read timeout in ms; 0 = facil.io default (~40 s).
   *  Used as the facil.io connection timeout when idle_timeout_ms is 0. */
  long read_timeout_ms;
  /** Keep-alive idle timeout in ms; 0 = same as read_timeout_ms.
   *  When non-zero, overrides read_timeout_ms as the facil.io connection
   *  timeout.  Both fields map to the single timeout parameter exposed by
   *  the underlying facil.io http_listen call. */
  long idle_timeout_ms;
  /** TLS config; NULL = plaintext. */
  const chttp_tls_config_t *tls;
  /** Number of worker threads in the server-owned ctpool (default: CPU count).
   *  Pass 0 to use the CPU count. */
  int worker_thread_count;
  /** Capacity of the worker task queue.
   *  0                       = library default (256 * worker_thread_count).
   *  CHTTPSVR_QUEUE_UNBOUNDED = no limit (never returns 503 due to overflow).
   *  Any other value         = exact bounded capacity; 503 is returned when
   *                            the queue is full. */
  size_t worker_queue_capacity;
} chttpsvr_config_t;

/**
 * @brief Sensible defaults for chttpsvr_config_t.
 *
 * Port 8080, all interfaces, 4 MiB body limit, no TLS,
 * CPU-count worker threads, library-default queue capacity.
 */
#define CHTTPSVR_CONFIG_DEFAULT                  \
  ((chttpsvr_config_t){                          \
      .host = "0.0.0.0",                         \
      .port = 8080,                              \
      .max_body_size = (4U * 1024U * 1024U),     \
      .read_timeout_ms = 0,                      \
      .idle_timeout_ms = 0,                      \
      .tls = NULL,                               \
      .worker_thread_count = 0,                  \
      .worker_queue_capacity = 0,                \
  })

/* ========================================================================== */
/*                         ENGINE LOGGER                                      */
/* ========================================================================== */

/**
 * @brief Install a custom logger for facil.io engine-level events.
 *
 * The engine logger captures low-level facil.io diagnostics (epoll/kqueue
 * initialisation, TLS handshakes, internal memory events, etc.).  It is
 * separate from the per-server logger passed to create_chttpsvr().
 *
 * Call this before the first chttpsvr_start() if you want a custom engine
 * logger.  If no logger has been installed when the engine first starts, a
 * minimal default logger writing only FATAL messages to stderr is created
 * automatically.
 *
 * Internally this function derives a logger from cl via clog_derive(), adds
 * the field component=http-engine, and registers the derived logger with
 * facil.io.  The previously registered engine logger (if any) is closed.
 * The caller retains ownership of cl and must keep it alive for as long as
 * any server may be running.
 *
 * @param cl  Parent logger to derive from; must not be NULL.
 * @return ccol_success or ccol_invalid_args (cl is NULL).
 */
ccol_retval_t chttpsvr_set_engine_logger(clog cl);

/* ========================================================================== */
/*                         ENGINE WAIT                                        */
/* ========================================================================== */

/**
 * @brief Block until the facil.io engine thread exits.
 *
 * Returns immediately if the engine was never started or has already stopped.
 *
 * Use this as an escape hatch when you want to keep the calling thread alive
 * until a shutdown signal triggers engine exit, without destroying the server
 * handle first.
 *
 * Note: destroying the last running server (via chttpsvr_destroy) also blocks
 * until the engine exits; calling chttpsvr_engine_wait() separately is only
 * necessary when you need to perform teardown between the engine stop and the
 * server destruction.
 */
void chttpsvr_engine_wait(void);

/**
 * @brief Signal the facil.io engine to stop.
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
 * @param mprocs   Custom allocator, or NULL for malloc/free.
 * @param cl       Logger for this server's request/routing events.  Must not
 *                 be NULL; pass a derived logger at a suppressed level to
 *                 silence output.
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
 * @brief Internal destroy -- use chttpsvr_destroy macro instead.
 *
 * Stops the server if it is running, drains the server's worker pool, and
 * if this is the last running server, stops the facil.io engine and waits
 * for it to exit.  Frees all owned resources including sub-routers, routes,
 * and the server-owned ctpool.
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
 * @param cl    Logger for this server's request/routing events.
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
 * @param cl    Logger for this server's request/routing events.
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
 * the first chttpsvr_start() call in the process, the shared facil.io event
 * loop is started automatically in a background thread.  Subsequent calls
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
 * The reactor thread is never blocked: the request is paused and the handler
 * is dispatched to the server's ctpool.  Inside the handler use
 * chttpsvr_req_read() to pull body bytes incrementally.
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
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory.
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
 * IMPORTANT -- the "/" prefix edge case:
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
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory.
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
 * call only.  Do NOT store it beyond the handler's return -- it points into
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
 * Reads up to buflen bytes of the request body into buf.  Successive calls
 * advance the read position.
 *
 * If buflen is 0 the function returns 0 immediately regardless of buf (the
 * call is a no-op, consistent with POSIX read(2) semantics).
 *
 * Returns -1 only for hard errors: req is NULL, buf is NULL with buflen > 0,
 * or the handler was registered with chttpsvr_register_handler (buffered)
 * rather than chttpsvr_register_streaming_handler.
 *
 * @param req     Request handle (must be from a
 * chttpsvr_register_streaming_handler route).
 * @param buf     Destination buffer.  May be NULL only when buflen is 0.
 * @param buflen  Buffer capacity in bytes.  Passing 0 is a valid no-op.
 * @return Number of bytes read (>0), 0 at EOF or for buflen==0, -1 on error.
 */
ssize_t chttpsvr_req_read(chttpsvr_req *req, void *buf, size_t buflen);

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
