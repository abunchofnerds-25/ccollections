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

#include <stdint.h>
#include <sys/types.h>

#include "chttp.h"
#include "clogger.h"
#include "common.h"

/* Every declaration from here to the end of this header is part of the
 * public Application Binary Interface (ABI) of libccollections, and the
 * shared library exports all of them. Because the library is built with
 * -fvisibility=hidden, a function or object that is not in one of these
 * blocks stays internal to the library and is absent from its dynamic
 * symbol table, so no symbol of the same name in the application can
 * interpose it or collide with it. */
#pragma GCC visibility push(default)

/**
 * @file chttpserver.h
 * @brief HTTP/1.1 server that runs on ccol_event_loop (a persistent,
 * multi-threaded epoll reactor), with routes, middleware,
 *        sub-routers, TLS, and optional dispatch of a streaming body.
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
 *     chttpsvr_construct(srv, CLOG_INVALID);
 *     chttpsvr_register_handler(srv, CHTTP_GET, "/hello", hello, NULL);
 *     chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
 *     if (chttpsvr_start(srv, &cfg) != ccol_success) {
 *         chttpsvr_destroy(srv);
 *         chttpsvr_engine_wait();  // wait until the engine stops completely
 *         return 1;
 *     }
 *     chttpsvr_engine_wait();
 *     chttpsvr_destroy(srv);
 * }
 * @endcode
 *
 * ### Threading model
 *
 * The server routes a request as soon as it parses the headers of that
 * request, before it reads any body byte.  It rejects an unmatched route
 * immediately, without reading the body that it is about to discard, and it
 * hands a matched route to a thread pool right away, whatever the body size.
 * The work of the reactor thread on any request is therefore O(1), and
 * neither a large or slow body nor the handler code of the caller ever
 * blocks the reactor thread.
 *
 * A route that chttpsvr_register_handler registers (buffered body) runs on
 * the worker pool of the server, a ctpool that chttpsvr_start() creates and
 * whose size the fields chttpsvr_config_t.worker_thread_count and
 * chttpsvr_config_t.worker_queue_capacity control.  A worker reads the body
 * directly off the socket and never waits for a slow client: a body that
 * stops arriving parks its connection with no thread, and any free worker
 * continues it once more of the body is ready.  The handler runs once the
 * whole body is in memory, and chttpsvr_req_body() gives it.  A response
 * that meets a full socket parks in the same way after the handler returns,
 * so the thread that runs a handler can differ from the one that began to
 * read its body.  When the task queue is full at the moment a request
 * arrives, the server immediately returns 503 Service Unavailable, with
 * Retry-After.
 *
 * A route that chttpsvr_register_streaming_handler registers (streaming body)
 * runs on a second pool, the streaming pool, which the server creates at the
 * first streaming request and which chttpsvr_config_t.streaming_thread_count
 * sizes.  A streaming handler calls chttpsvr_req_read() to pull each body
 * batch as it arrives, without waiting for the rest of the body to reach the
 * server.  chttpsvr_req_read() blocks the streaming thread that calls it,
 * never the reactor or a worker, until one of these happens: data arrives,
 * the stream reaches EOF, an error happens,
 * chttpsvr_config_t.stream_read_timeout_us expires,
 * chttpsvr_config_t.max_body_read_duration_us expires if the caller set it,
 * or the average rate of the body falls below
 * chttpsvr_config_t.min_transfer_rate_bps.  A slow streaming client can
 * therefore occupy at most the streaming pool, never the threads of buffered
 * routes.  A streaming request that finds no free streaming thread waits,
 * with no thread, in a bounded queue with a deadline.
 *
 * Both handler types, and every middleware, can block.  A handler of either
 * kind counts as a thread of its own server: the calls that it must not make
 * on that server, such as chttpsvr_destroy() or a restart, are refused or
 * fatal on both pools alike.
 *
 * ### Request validation
 *
 * The server refuses a malformed or unsupported request before any route,
 * middleware or handler sees it, and closes the connection after the
 * answer:
 *   - 505 HTTP Version Not Supported for a request line whose version is
 *     not HTTP/1.x, such as HTTP/0.9, HTTP/2.0 or HTTP/3.0. A higher minor
 *     version of HTTP/1 is served as HTTP/1.1 (RFC 9110 SS2.5).
 *   - 400 for a Host header outside the uri-host [ ":" port ] grammar of
 *     RFC 9112 SS3.2: a reg-name of unreserved, sub-delims and
 *     percent-encoded bytes, an IPv4 address, or a bracketed IP literal.
 *     A '/', '@', '?', '#' or whitespace in the value is refused, as are an
 *     empty host and a port that is not all digits.
 *   - 501 Not Implemented for a Transfer-Encoding that names a coding other
 *     than chunked, such as "gzip, chunked", because the server decodes no
 *     other coding, and 400 for one that does not end in chunked.
 *   - 400 for a request line or header line longer than 8 KiB, for more
 *     than 100 header lines, and for a header block above
 *     chttpsvr_config_t.max_header_bytes.
 * The chunk framing of a body is judged as the body arrives: its chunk
 * extensions (which the server checks and then drops) and the rest of its
 * framing may exceed 16 bytes plus twice the data of each chunk by 16 KiB in
 * all. A body past that bound is malformed, so a buffered route answers 400,
 * and chttpsvr_req_read() of a streaming route fails with
 * ccol_http_transfer_aborted.
 *
 * An HTTP/1.0 request that carries "Expect: 100-continue" gets no interim
 * 100 response (RFC 9110 SS10.1.1); its client sends the body at once.
 *
 * A path and its {param} values are decoded, so a decoded segment can hold
 * '/' (from %2F), and the server does not merge or remove "." and ".."
 * segments. See "Route precedence" below before you use either as a file
 * name.
 *
 * ### Engine lifecycle (implicit)
 *
 * The first chttpsvr_start() call starts the shared ccol_event_loop reactor
 * (the "engine") automatically, and the destruction of the last running
 * server stops it automatically, so you do not need to call the engine
 * lifecycle functions yourself.
 *
 * Call chttpsvr_set_engine_logger() to install a custom logger for
 * engine-level events (TLS handshake failures, listen-socket bind failures
 * and idle-timeout closures).  It takes effect at once, and it also serves
 * every later start of the engine.
 *
 * Call chttpsvr_engine_wait() to block the calling thread until the engine
 * exits, for example when an external shutdown signal such as SIGTERM makes
 * a signal handler call chttpsvr_engine_stop().
 *
 * A request handler, for example an admin or shutdown endpoint, can shut its
 * own server down from inside itself by calling chttpsvr_engine_stop(),
 * which is safe there by design.  A direct call to chttpsvr_destroy(), or to
 * chttpsvr_stop() and then chttpsvr_start(), on the server that runs that
 * handler is refused or fatal instead (see the doc comment of each one), and
 * so is chttpsvr_engine_wait(), for the same reason.  Such a handler calls
 * chttpsvr_engine_stop() and then returns; it must not also wait on the same
 * thread for the drain that it started.
 *
 * More than one server can run at the same time, each one listening on its
 * own port and with its own routes, middleware, worker pool, and clog
 * handle.
 *
 * Typical single-server pattern:
 * @code
 *   chttpsvr_construct(srv, CLOG_INVALID);
 *   chttpsvr_register_handler(srv, CHTTP_GET, "/hello", hello, NULL);
 *   chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
 *   if (chttpsvr_start(srv, &cfg) != ccol_success) {
 *     chttpsvr_destroy(srv);
 *     chttpsvr_engine_wait();  // wait until the engine stops completely
 *     return 1;
 *   }
 *   chttpsvr_engine_wait();  // blocks until signal / chttpsvr_engine_stop()
 *   chttpsvr_destroy(srv);
 * @endcode
 *
 * ### Middleware chain limit
 *
 * Each router, the root router (which holds the global middleware) and each
 * sub-router alike, caps its own middleware chain at 32 entries.  The
 * library applies the limit at registration time: chttpsvr_use and
 * chttpsvr_router_use return ccol_not_permitted, and add no entry, on the
 * call that would go above the cap of 32 entries for that one router.
 *
 * Dispatch time applies a separate combined cap of 32 for the effective chain
 * of one request, which is the global middleware plus the middleware of the
 * router that matched.  Because each side of that sum can hold up to 32
 * entries on its own, the combined count can go above 32 while neither
 * router reaches its own cap at registration time, and every request
 * through the affected router then gets a 500 response.
 *
 * ### Route precedence
 *
 * The server tries the routers in order of how specific the mount prefix is,
 * most specific first: every sub-router first, in descending order of how
 * many path segments its prefix has, and the root router last, because the
 * prefix of the root router matches every path.  The root router holds the
 * routes that chttpsvr_register_handler and
 * chttpsvr_register_streaming_handler register directly.  Two mounts with
 * the same segment count are either disjoint literals, of which at most one
 * can match one path, or the same prefix registered twice, in which case the
 * server tries the earlier registration first.  Precedence therefore depends
 * only on the prefixes and the patterns that you register, never on the
 * order of registration, and a root-level pattern of any breadth can never
 * hide the routes of a sub-router or skip the middleware of that
 * sub-router.
 *
 * A mounted prefix owns every path under it.  A path is under a prefix when
 * its decoded form (the one that chttpsvr_req_path() reports) equals the
 * prefix or continues it with a '/': "/admin", "/admin/x/y", "/%61dmin/x"
 * and "/admin%2Fx" are all under "/admin", but "/administrator" is not.  The
 * most specific mount that owns the path handles the request alone, so no
 * route of a less specific mount, and no root-level route, can answer it.
 * When none of the routes of the owner matches, the owner answers 404 (or
 * 405 when a route of the owner matches the path but not the method), and
 * the middleware of the owner runs for that rejection as it runs for its
 * routes.  A guard in the middleware of a mount therefore covers every path
 * under the mount, whatever any other router registers.  A mount nested
 * under another mount, such as "/admin/deep" under "/admin", owns the paths
 * under itself, so the middleware of "/admin" does not run for them; give
 * the nested mount a guard of its own when it needs one.  Two sub-routers on
 * the identical prefix share that prefix: the routes of both can answer, the
 * earlier registration first, and the earlier one answers a 404 or a 405.
 * A path under no mount goes to the root router.
 *
 * Inside one router, the server tries the routes in the order of their
 * registration.  A 405 response has an Allow header that lists each method
 * that a route of the router (or routers) that answers accepts for the
 * path, plus HEAD where a CHTTP_GET route serves the path.  See
 * chttpsvr_register_handler(3) and doc/chttpserver.md.
 *
 * Route matching works on the raw segments of the path, each one decoded on
 * its own.  A decoded segment, and therefore a {param} value, can hold '/'
 * (from %2F), and it can be "." or ".." (from %2E): the server does not merge
 * or remove dot segments.  A path whose own prefix is spelled with %2F, such
 * as "/admin%2Fx", is under "/admin", but no route of that mount can match it,
 * so the mount answers it with a 404.  Never use a path or a parameter value
 * as a file name without checking it.
 *
 * A route registered for CHTTP_GET also serves HEAD requests for the same
 * path, as RFC 9110 SS9.3.2 needs.  The handler runs exactly as it does for
 * GET, and the server sends the response without its body but with the
 * content-length of the body that the handler wrote.  A handler that writes
 * no body for HEAD can set that content-length itself with
 * chttpsvr_resp_set_header().  A route registered explicitly for CHTTP_HEAD or
 * CHTTP_ANY on that path wins over this fallback, whatever order you
 * register the two in.
 *
 * ### Route patterns
 *
 * Exact paths:   /health  /api/v1/status
 * Named params:  /users/{id}/posts/{postId}
 *
 * ### TLS
 *
 * Set chttpsvr_config_t.tls to a pointer to a chttp_tls_config_t that has
 * cert_path and key_path (TLS needs -lssl -lcrypto at link time), or pass
 * NULL for plaintext HTTP.
 *
 * A ca_bundle_path beside them turns on mutual TLS: the handshake of a
 * client that presents no certificate, or one that does not verify against
 * that bundle, fails. client_cert_optional relaxes the first rule, and
 * chttpsvr_req_peer_cert_verified() then tells a handler which kind of
 * client it serves. chttpsvr_req_peer_cert_der(),
 * chttpsvr_req_peer_cert_sha256() and chttpsvr_req_peer_cert_subject() tell
 * it which client authenticated. CRLs in the bundle are enforced; see
 * chttp_tls_config_t.
 */

/* ========================================================================== */
/*                         OPAQUE HANDLES                                     */
/* ========================================================================== */

/** @brief Opaque HTTP server structure. */
typedef struct chttpserver chttpserver;

/**
 * @brief Server handle.
 *
 * chttpsvr is an opaque VALUE handle, not a pointer: it packs a slot index
 * and a generation into one value. Never cast it to or from void*, never
 * compare it with a pointer cast, and never treat it as an address; compare
 * it directly against CHTTPSVR_INVALID. You can also use it in a truth
 * check, because CHTTPSVR_INVALID is 0, so `if (!srv)` works. Before it
 * touches the server struct, the library resolves every use of a chttpsvr
 * through a slot table that it owns, so it always detects a handle whose
 * slot it freed, or whose slot it reused for a different, later server, and
 * never dereferences freed memory or the memory of the wrong server. See the
 * doc comment of chttpsvr_destroy for what happens when a stale handle
 * reaches that function.
 */
typedef uint64_t chttpsvr;

/** @brief Sentinel value for "no server": the chttpsvr equivalent of
 *         NULL. */
#define CHTTPSVR_INVALID ((chttpsvr)0)

/** @brief Opaque route group (sub-router) handle. */
typedef struct chttpsvr_router chttpsvr_router;

/** @brief Opaque context for one request, which the chttpsvr_req_* functions
 *         read. */
typedef struct chttpsvr_req chttpsvr_req;

/**
 * @brief Opaque response accumulator for one request, which the
 *        chttpsvr_resp_* functions write to.
 */
typedef struct chttpsvr_resp chttpsvr_resp;

/* ========================================================================== */
/*                         CALLBACK TYPES                                     */
/* ========================================================================== */

/**
 * @brief HTTP request handler.
 *
 * It runs on a thread of a pool that the server owns (the worker pool for a
 * buffered route, and the streaming pool for a streaming route), and it can
 * block.
 *
 * @param req  Request object, valid only for the length of the call.
 * @param resp Response accumulator. The handler fills it, and the server
 *             flushes it after the handler returns.
 * @param ctx  Opaque context for this route, which you set at registration
 *             time.
 */
typedef void (*chttpsvr_handler_fn)(chttpsvr_req *req, chttpsvr_resp *resp,
                                    void *ctx);

/**
 * @brief Continuation function that the server gives to each middleware.
 *
 * Call it to move to the next middleware in the chain, or to the final
 * handler when no more middleware remain; a middleware that never calls it
 * stops the chain there. Do not call it more than once from one middleware
 * call: a second call skips the step that the first call already ran and
 * moves the chain forward once more, to the step after that one, and when
 * this middleware is already the last one in the chain, the second call
 * runs the final handler again. This pattern is never useful.
 */
typedef void (*chttpsvr_next_fn)(chttpsvr_req *req, chttpsvr_resp *resp);

/**
 * @brief Middleware function.
 *
 * It runs before the final handler, and a global middleware also runs
 * before every sub-router middleware. Call next(req, resp) to pass control
 * to the next handler.
 *
 * It runs on a ctpool worker thread, and it can block.
 *
 * @param req   Request object.
 * @param resp  Response accumulator.
 * @param ctx   Opaque context for this middleware, which you set in the call
 *              to chttpsvr_use.
 * @param next  Call it to go to the next handler in the chain.
 */
typedef void (*chttpsvr_middleware_fn)(chttpsvr_req *req, chttpsvr_resp *resp,
                                       void *ctx, chttpsvr_next_fn next);

/* ========================================================================== */
/*                         SERVER CONFIGURATION                               */
/* ========================================================================== */

/**
 * @brief Pass this as worker_queue_capacity to get an unbounded task queue
 *        with no 503 back-pressure.
 *
 * Be careful with this value: an unbounded queue can grow without a limit
 * under a long overload, and most production deployments are better with a
 * finite capacity and a graceful 503 rejection.
 */
#define CHTTPSVR_QUEUE_UNBOUNDED ((size_t)-1)

/**
 * @brief Pass this as min_transfer_rate_bps to turn the minimum transfer rate
 *        off.
 *
 * With the floor off, only the per-gap and total limits bound a client that
 * sends its body, or reads its response, very slowly.
 */
#define CHTTPSVR_NO_RATE_FLOOR ((unsigned)-1)

/**
 * @brief Pass this as max_partial_body_memory to hold no limit on the memory
 *        that the bodies of buffered requests hold.
 */
#define CHTTPSVR_NO_MEMORY_CAP ((size_t)-1)

/**
 * @brief Pass this as body_memory_wait_timeout_us or as
 *        streaming_queue_timeout_us to let a request wait with no deadline.
 */
#define CHTTPSVR_NO_DEADLINE UINT64_MAX

/**
 * @brief Pass this as streaming_thread_count to run streaming handlers on
 *        the worker pool, beside buffered handlers, with no pool of their
 *        own.
 */
#define CHTTPSVR_STREAMING_POOL_OFF (-1)

/**
 * @brief Server startup configuration.
 *
 * Start from CHTTPSVR_CONFIG_DEFAULT, fill in the fields that you want to
 * change, then pass a pointer to chttpsvr_start().
 *
 * Every timeout and duration field is a uint64_t count of microseconds,
 * whose name ends in _us. The server keeps each one in whole milliseconds,
 * rounding a value up to the next millisecond, so a value above 0 never
 * reads as 0. A value longer than the server can keep saturates to the
 * longest finite limit that it keeps for that field (never shorter than 24
 * days) instead of turning into 0 or into "no limit".
 * CHTTPSVR_NO_DEADLINE, in the two fields that accept it, is the one value
 * that means no limit.
 */
typedef struct chttpsvr_config {
  /** Bind address, with rules that follow the listener of the Go net
   *  package.
   *
   *  NULL or "" listens on every interface with one dual-stack socket on
   *  [::] (IPV6_V6ONLY off), which takes IPv6 clients and, through
   *  v4-mapped addresses, IPv4 clients too; on a host without IPv6 it
   *  listens on 0.0.0.0 instead. ipv6_only turns it into an IPv6-only
   *  listener.
   *
   *  A literal IPv4 or IPv6 address listens on exactly that address:
   *  "0.0.0.0" on every IPv4 interface, "::" on every IPv6 interface
   *  (which the system may also make dual-stack; see ipv6_only), and
   *  "127.0.0.1" on the IPv4 loopback alone.
   *
   *  An address in brackets, as in "[::1]" or "[127.0.0.1]", is the
   *  address inside the brackets; a name in brackets fails the start.
   *
   *  A host name listens on its first IPv4 address when it has one, and on
   *  its first IPv6 address otherwise; with ipv6_only, on its first IPv6
   *  address when it has one, and on its first IPv4 address otherwise.
   *  "localhost" therefore listens on 127.0.0.1, whatever order the
   *  resolver gives its addresses in. The server tries that one address
   *  alone: when it cannot bind it, for example because another socket
   *  holds that port on 127.0.0.1, chttpsvr_start() fails, and the server
   *  never listens on another address of the name in its place. A name that
   *  does not resolve also fails the start, and the engine logger reports
   *  the reason that the resolver gave.
   *
   *  A value of the form "unix://path/to/socket" binds a Unix domain socket
   *  at that path instead of a TCP listener, and the server then ignores
   *  port, which may be 0. A socket file at the path that no process listens
   *  on is replaced. The start fails, and leaves the file alone, when the
   *  path names anything that is not a socket, or a socket that a live
   *  server listens on. chttpsvr_stop() and the destroy remove the socket
   *  file only while the path names the socket that this server created,
   *  and they reach it through the directory that held it at the start, so
   *  a later chdir() does not matter. One chttpsvr instance listens on TCP
   *  or on a Unix socket, never on both; an application that wants both
   *  creates two chttpsvr instances, which share the one reactor of the
   *  process at no extra cost. */
  const char *host;
  /** Port to listen on (default 8080), which the server ignores when host is
   *  a "unix://" path. */
  uint16_t port;
  /** Maximum request body in bytes (default 4 MiB), where 0 means unlimited.
   *  The server rejects a buffered route whose body is above this with 413,
   *  before the handler runs. It always calls the handler of a streaming
   *  route, and there chttpsvr_req_read() returns -1 and
   *  chttpsvr_req_stream_error() gives ccol_msg_too_large after the body
   *  crosses the limit. In both cases the server closes the connection after
   *  the response (Connection: close) instead of keeping it alive, because
   *  it does not read the body bytes above the limit. That close is a
   *  lingering close; see chttpsvr_start(). */
  size_t max_body_size;
  /** Read timeout for each connection, in microseconds (default 60000000,
   *  that is 60 s).
   *  0 turns the idle timeout off.
   *  The idle-timeout sweep uses this value as its threshold when
   *  idle_timeout_us is 0. It applies only while a connection is idle
   *  between requests, that is, while it waits for the headers of the next
   *  pipelined or keep-alive request, and not while a worker thread handles
   *  a request (see max_body_read_duration_us for that case).
   *
   *  A finite value bounds a connection that opens, sends a part of a header
   *  block or nothing at all, and then goes quiet. With the timeout off,
   *  such a connection holds its slot until the peer closes it, so a small
   *  number of sockets can occupy the listener forever. Set this to 0 only
   *  when something else in the deployment, such as the timeout of a
   *  reverse proxy, already bounds the lifetime of an idle connection.
   *
   *  Because this value measures the GAP between one piece of activity and
   *  the next, it does not bound a peer that keeps sending, as slowly as it
   *  wants, and never finishes its header block; max_header_read_duration_us
   *  bounds that peer. */
  uint64_t read_timeout_us;
  /** Keep-alive idle timeout in microseconds (default 60000000, that is
   *  60 s).
   *  0 means the same value as read_timeout_us, and a value above 0
   *  overrides read_timeout_us as the threshold of the idle-timeout
   *  sweep. */
  uint64_t idle_timeout_us;
  /** This bounds the gap between one batch of body bytes and the *next*
   *  while the server reads a request body, in microseconds, for a buffered
   *  body and for a streaming body. For a buffered body no thread waits: the
   *  connection parks, and the periodic sweep, or the worker that resumes
   *  it, applies this limit.
   *  0 means that the server waits forever, bounded only by read_timeout_us
   *  and idle_timeout_us, which reset on any connection activity and so do
   *  not protect you against a client that sends its bytes slowly enough to
   *  always beat them.
   *  The default is 30000000 (30 s). After this time expires,
   *  chttpsvr_req_read() returns -1 and chttpsvr_req_stream_error() gives
   *  ccol_timed_out, and a buffered route answers 408 automatically.
   *
   *  A value of 0 here, or in max_body_read_duration_us, does not make an
   *  unresponsive connection a permanent problem: a shutdown of the server
   *  (a call to chttpsvr_destroy() or to chttpsvr_engine_stop()) completes
   *  in bounded time, even when a streaming handler really is blocked while
   *  it reads the body of such a connection, while a restart with
   *  chttpsvr_stop() and chttpsvr_start() waits for nothing and cuts nothing
   *  off. The server gets this bound by a forced close of that connection
   *  after its own bounded and graceful wait for the in-flight requests
   *  ends. This happens only as a part of that shutdown sequence, never
   *  merely because a peer is slow, and it never affects a request that
   *  makes progress. */
  uint64_t stream_read_timeout_us;
  /** This bounds the *total* wall-clock time that the server spends to read
   *  the body of one request, in microseconds, for a buffered body and for a
   *  streaming body; 0 means no limit. stream_read_timeout_us bounds only
   *  each single gap between two batches, so it never fires against a
   *  client that sends a byte or two just before each gap expires; this
   *  field closes that loophole by capping the sum of all such waits for
   *  one request. After this time expires, chttpsvr_req_read() returns -1
   *  and chttpsvr_req_stream_error() gives ccol_timed_out, the same result
   *  as an expiry of stream_read_timeout_us, and a buffered route answers
   *  408 automatically. The default is 300000000 (5 minutes), which is large
   *  enough for an upload of max_body_size bytes (4 MiB by default) over a
   *  link so slow that the transfer itself is the bottleneck. Set it to 0 to
   *  turn the limit off, or lower for a deployment whose bodies are small
   *  and whose clients are close. */
  uint64_t max_body_read_duration_us;
  /** This bounds the gap between two writes that make progress while the
   *  server sends one response to a client that reads slowly, in
   *  microseconds; 0 means that the server uses the value of
   *  stream_read_timeout_us. No thread waits for such a client: the
   *  connection parks, and the periodic sweep, or the worker that resumes
   *  it, applies this limit. This field closes on the write side the same
   *  class of gap that stream_read_timeout_us closes on the read side. A
   *  value of 0 here keeps the bounded-shutdown property that the doc
   *  comment of stream_read_timeout_us describes, so it does not make a
   *  server shutdown wait forever on a stalled peer.
   *
   *  Progress is the client taking bytes of the response: a write that the
   *  kernel accepts, and also the send queue of the socket shrinking, as the
   *  sweep sees it about once a second. A client that reads a large response
   *  steadily therefore never meets this limit, however large a send buffer
   *  the kernel gives the socket. The sweep interval is the precision of the
   *  limit: a client that stops reading is cut between one and two sweep
   *  intervals after the limit passes. */
  uint64_t response_write_timeout_us;
  /** This bounds the *total* wall-clock time that the server spends to send
   *  one response, in microseconds; 0 means no limit.
   *  response_write_timeout_us bounds only each single call equivalent to
   *  write(2), so it never fires against a client that reads a byte or two
   *  just before the timeout of each such call expires; this field caps the
   *  sum of all such waits for one response, closing on the write side the
   *  same loophole that max_body_read_duration_us closes on the read side.
   *  After this time expires, the send of the response fails and the server
   *  closes the connection, so the client sees a truncated response or a
   *  reset and keeps whatever it read before the deadline. The default is
   *  300000000 (5 minutes), the write-side equivalent of the default of
   *  max_body_read_duration_us, for the same reasons. Set it to 0 to turn
   *  the limit off.
   *
   *  A small internal ceiling of 2 seconds always bounds two more writes,
   *  whatever value you set here, including a value of 0: a courtesy
   *  rejection response (400, 404, 405, 408, 413, 500, 501, 503 or 505) that
   *  the server generates itself instead of a handler, and the interim
   *  "100 Continue" line for "Expect: 100-continue". Both have a fixed,
   *  small shape (no body, or one status line of 25 bytes) and have no
   *  legitimate reason to need longer. Neither ever holds a thread while it
   *  waits for the peer: a write that meets a full socket parks the
   *  connection, as a response of a handler does, and the ceiling bounds how
   *  long it may stay parked. The one exception is the interim line of a
   *  streaming route, which chttpsvr_req_read() writes on the thread of the
   *  handler, which waits for the client there as it waits for the body;
   *  the ceiling bounds that wait. A value here below that internal ceiling
   *  applies in full: the server narrows only a value of 0, or a value above
   *  the ceiling, and only for these two internal writes. */
  uint64_t max_response_write_duration_us;
  /** This bounds the *total* wall-clock time that one connection can spend
   *  in the reactor-owned phase of one request, in microseconds. That phase
   *  is the TLS handshake, where there is one, plus the read of the whole
   *  header block of that request. 0 means no limit, and the default is
   *  30000000 (30 s).
   *
   *  read_timeout_us and idle_timeout_us measure the gap between one piece
   *  of activity and the next, and every byte that arrives resets them; this
   *  field caps the sum instead. It bounds a peer that opens a connection
   *  and then sends its request headers, or its handshake, one byte at a
   *  time, forever, without ever completing them. Such a peer never comes
   *  near max_header_bytes, because it never finishes the block at all, and
   *  it resets the gap timer on every byte, while each connection that it
   *  holds costs a file descriptor, an epoll registration and one buffer.
   *  Once enough of them reach max_connections, the listener stops accepting
   *  anything. This field is the header-phase equivalent of
   *  max_body_read_duration_us, which closes the same loophole for the body
   *  phase.
   *
   *  The budget is for each request, not for each connection. It starts at
   *  the first TLS handshake step for a connection that has one, and at the
   *  first byte of the header block of the request for one that does not.
   *  The next request of a keep-alive connection starts a fresh budget when
   *  its own first byte arrives, so time that the connection spends idle
   *  between two requests is never charged to either one. After the budget
   *  expires, the server closes the connection and sends no response,
   *  because the header block was never complete enough to answer. The
   *  expiry is noticed by the same periodic sweep that read_timeout_us and
   *  idle_timeout_us use, which runs once a second and closes every
   *  connection that expired, however many there are, so a connection that
   *  is past its deadline lives up to about a second longer than that
   *  deadline.
   *
   *  The default is generous next to any legitimate client: a browser or an
   *  API client sends its header block immediately after it connects, and
   *  even a full TLS handshake plus several kilobytes of headers over a
   *  lossy link with high latency takes only seconds. Raise it for a
   *  deployment whose clients really are that slow, and set it to 0 to turn
   *  the limit off when something else, such as a reverse proxy in front of
   *  this server, already bounds the same phase. */
  uint64_t max_header_read_duration_us;
  /** Maximum combined size, in bytes, of the header block of one request
   *  (the request line plus all the header lines). 0 means that the server
   *  uses the built-in default of the library (64 KiB). The server rejects a
   *  request whose headers are above this by answering 400 Bad Request and
   *  then closing the connection, as it does for every other malformed
   *  request. The answer is best effort: a peer that is still streaming its
   *  oversized header block when the server closes can meet a reset before
   *  it reads anything.
   *
   *  Two fixed limits apply beside this one, whatever its value, and a
   *  request past either gets the same 400: the request line and each header
   *  line must end within 8 KiB, CRLF included, and a request holds at most
   *  100 header lines. A chunked body's chunk-size lines and trailer lines
   *  have the same 8 KiB bound, and its trailer lines count against the 100
   *  and against this byte limit. */
  size_t max_header_bytes;
  /** Maximum number of connections open at the same time on this listener
   *  (default 16384), where 0 means unlimited. A finite value bounds how
   *  many file descriptors one listener can hold, so a peer that opens
   *  connections faster than the idle sweep frees them cannot use up the
   *  descriptor limit of the process and take down with it every other
   *  listener and every outbound socket that the application opens. Raise
   *  it for a deployment that really serves more connections at the same
   *  time than this, and raise RLIMIT_NOFILE to match.
   *  At capacity, new connections stay pending in the listen backlog of the
   *  kernel instead of being accepted and then rejected: the server does not
   *  call accept(2) again for this listener until a connection closes and
   *  frees a slot. The listener does not resume at once, because a periodic
   *  sweep notices the free slot, so the worst case is about one second
   *  between the free of a slot and the resume of the listener. */
  size_t max_connections;
  /** TLS configuration. NULL means plaintext. */
  const chttp_tls_config_t *tls;
  /** Number of worker threads in the ctpool that the server owns. The
   *  default is the CPU count, which you also get by passing 0. */
  int worker_thread_count;
  /** Capacity of the worker task queue.
   *  0                       = the library default (1024 * worker_thread_
   *                            count).
   *  CHTTPSVR_QUEUE_UNBOUNDED = no limit. An overflow never gives a 503.
   *  Any other value         = the exact bounded capacity. The server gives
   *                            a 503 when the queue is full. */
  size_t worker_queue_capacity;
  /** Turn SO_KEEPALIVE on for every TCP connection that the server accepts
   *  (default: off), so that the OS, with its own keepalive probe interval,
   *  finds and closes a connection whose peer became silently unreachable,
   *  for example after somebody pulls a network cable. This is separate
   *  from idle_timeout_us and adds to it, because idle_timeout_us measures
   *  only local inactivity, not whether the peer is reachable. It has no
   *  effect on a Unix domain socket listener. */
  bool enable_keepalive;
  /** Set SO_REUSEPORT on the socket that listens (default: off), which lets
   *  more than one process, or more than one chttpsvr instance in one
   *  process, bind the same host and port at the same time, while the
   *  kernel load-balances the accepted connections across them. This
   *  library does not coordinate more than one process itself: this setting
   *  only controls whether the OS-level prerequisite is in place, so that an
   *  application can do that coordination itself. It has no effect on a Unix
   *  domain socket listener. */
  bool enable_reuseport;
  /** Set IPV6_V6ONLY on the socket that listens, when that socket binds an
   *  IPv6 address (default: off). With host NULL or "", off means a
   *  dual-stack listener whatever the system default. With an IPv6 address
   *  or name, off keeps the OS default: on Linux a dual-stack socket, which
   *  also accepts IPv4-mapped connections, unless something such as
   *  /proc/sys/net/ipv6/bindv6only restricts it, and on FreeBSD an
   *  IPv6-only socket, unless the sysctl net.inet6.ip6.v6only is 0. Set this
   *  to true for an IPv6-only listener that must never also accept IPv4
   *  traffic; a host name then listens on its IPv6 address when it has one.
   *  It has no effect on an IPv4 listener or on a Unix domain socket
   *  listener. */
  bool ipv6_only;
  /** The minimum average rate, in bytes per second, at which a client must
   *  send the body of a request and read the response to it. 0 selects the
   *  default of 240, and CHTTPSVR_NO_RATE_FLOOR turns the floor off.
   *
   *  The server measures the rate over the whole phase, from the moment it
   *  starts to read the body, or to write the response, to now, and judges
   *  it only once min_transfer_rate_grace_us of that phase has passed. A
   *  body that falls below the floor gets 408 Request Timeout and the server
   *  then closes the connection, while a response that falls below it is
   *  cut off and the connection is closed. A streaming handler sees the
   *  floor as a failed chttpsvr_req_read(), with chttpsvr_req_stream_error()
   *  giving ccol_timed_out. Time that the server itself holds a request back
   *  (a wait for body memory, or a place in the streaming queue) never
   *  counts against the client. The floor adds to stream_read_timeout_us,
   *  max_body_read_duration_us, response_write_timeout_us and
   *  max_response_write_duration_us instead of replacing them. Because a
   *  connection that waits for the client holds no worker thread, the floor
   *  bounds the memory and the descriptor that such a connection keeps, not
   *  a thread. */
  unsigned min_transfer_rate_bps;
  /** How long a body read or a response write may run before
   *  min_transfer_rate_bps applies to it, in microseconds. 0 selects the
   *  default of 5000000 (5 s). */
  uint64_t min_transfer_rate_grace_us;
  /** The most memory, in bytes, that the bodies of buffered requests may
   *  hold together on this server, from the first byte that the server
   *  reads until the handler returns. 0 selects the default of 256 MiB, and
   *  CHTTPSVR_NO_MEMORY_CAP turns the limit off.
   *
   *  A reservation covers the memory that the buffer of a body really holds,
   *  and it lasts exactly as long as that buffer: while the body arrives,
   *  while the middleware and the handler run, and until the handler
   *  returns, when the server frees the body before the response goes out.
   *  A request that is refused, cut off or closed frees its body and its
   *  reservation at once, so a response that waits for a slow reader holds
   *  no body.
   *
   *  A body that a Content-Length frames reserves its whole declared length
   *  before the server reads a byte of it, and its buffer then takes exactly
   *  that length. When that does not fit, the request waits, holding no
   *  thread, and the server reads nothing from it and sends no "100
   *  Continue" until memory frees. A chunked body reserves its buffer as the
   *  buffer grows, before the read that needs it; because a growth holds the
   *  old buffer and the new one together until the copy ends, it reserves
   *  both for that time, and it waits in the same way when that does not
   *  fit. Waiting requests are admitted in the order in which they began to
   *  wait. When every body that holds memory is itself waiting for more, the
   *  oldest waiter goes ahead past the limit, one request at a time, so the
   *  limit can be exceeded by less than twice max_body_size (the body of
   *  that request and, while its buffer grows, the old buffer). A body whose
   *  handler runs is not waiting, so a handler that does not return keeps
   *  its body counted, and a request that a handler waits for, and whose
   *  body does not fit beside the bodies that running handlers hold, waits
   *  until body_memory_wait_timeout_us answers it with 503. A body that
   *  arrives complete together with its headers, and every streaming route,
   *  never reserves anything. */
  size_t max_partial_body_memory;
  /** How long a request may wait for body memory, in microseconds, where 0
   *  selects the default of 30000000 (30 s). After that time the server
   *  answers 503 Service Unavailable with a Retry-After header and closes
   *  the connection. CHTTPSVR_NO_DEADLINE lets a request wait for as long as
   *  it takes. */
  uint64_t body_memory_wait_timeout_us;
  /** The number of threads of the pool that runs streaming handlers, which
   *  chttpsvr_register_streaming_handler() and chttpsvr_router_on_stream()
   *  register. 0 selects the resolved worker_thread_count, and
   *  CHTTPSVR_STREAMING_POOL_OFF runs streaming handlers on the worker pool
   *  instead, beside buffered ones.
   *
   *  A streaming handler reads its body on its own thread, at the pace of
   *  the client, so a pool of its own keeps slow streaming clients from
   *  taking the threads that serve buffered routes. The server creates the
   *  pool at the first streaming request that it receives, so a server
   *  without streaming traffic starts no thread for it. */
  int streaming_thread_count;
  /** How many streaming requests may wait for a free streaming thread. 0
   *  selects four times the streaming thread count, and
   *  CHTTPSVR_QUEUE_UNBOUNDED holds no limit. A streaming request that finds
   *  the queue full gets 503 Service Unavailable with a Retry-After header
   *  at once, and the server closes the connection. A waiting request holds
   *  no thread. */
  size_t streaming_queue_capacity;
  /** How long a streaming request may wait in that queue, in microseconds,
   *  where 0 selects the default of 5000000 (5 s). A request that waits
   *  longer than that gets 503 Service Unavailable with a Retry-After
   *  header, and the connection is closed. CHTTPSVR_NO_DEADLINE lets a
   *  request wait for as long as it takes. */
  uint64_t streaming_queue_timeout_us;
} chttpsvr_config_t;

/**
 * @brief Reasonable defaults for chttpsvr_config_t.
 *
 * Port 8080, every interface through one dual-stack listener (host NULL;
 * set host to "0.0.0.0" for IPv4 alone), a body limit of 4 MiB, no TLS, one
 * worker thread for each CPU, and the queue capacity of the library
 * default.
 *
 * Every timeout and every resource limit here is finite, so a server that
 * starts from these defaults is already bounded against two kinds of peer:
 * one that opens a connection and then stalls (a slowloris), and one that
 * sends a body, or reads a response, one byte at a time. The doc comment of
 * each field above describes its own limit, and you can change each one on
 * its own. For the fields up to ipv6_only, a value of 0 keeps its
 * documented meaning of "off" or "unlimited". The fields from
 * min_transfer_rate_bps to streaming_queue_timeout_us read 0 as "use the
 * default", so that a configuration built with {0} or with designated
 * initializers keeps these defenses; each of them turns off only through
 * its own named constant.
 *
 * A worker thread never waits on a slow client for a buffered route: when
 * the body of a request stops arriving, or the client stops reading the
 * response, the server parks the connection with no thread and resumes it
 * on any free worker once the socket is ready again.
 */
#define CHTTPSVR_CONFIG_DEFAULT                          \
  ((chttpsvr_config_t){                                  \
      .host = NULL,                                      \
      .port = 8080,                                      \
      .max_body_size = (4U * 1024U * 1024U),             \
      .read_timeout_us = 60000000,                       \
      .idle_timeout_us = 60000000,                       \
      .stream_read_timeout_us = 30000000,                \
      .max_body_read_duration_us = 300000000,            \
      .response_write_timeout_us = 0,                    \
      .max_response_write_duration_us = 300000000,       \
      .max_header_read_duration_us = 30000000,           \
      .max_header_bytes = 0,                             \
      .max_connections = 16384,                          \
      .tls = NULL,                                       \
      .worker_thread_count = 0,                          \
      .worker_queue_capacity = 0,                        \
      .enable_keepalive = false,                         \
      .enable_reuseport = false,                         \
      .ipv6_only = false,                                \
      .min_transfer_rate_bps = 240,                      \
      .min_transfer_rate_grace_us = 5000000,             \
      .max_partial_body_memory = (256U * 1024U * 1024U), \
      .body_memory_wait_timeout_us = 30000000,           \
      .streaming_thread_count = 0,                       \
      .streaming_queue_capacity = 0,                     \
      .streaming_queue_timeout_us = 5000000,             \
  })

/* ========================================================================== */
/*                         ENGINE LOGGER                                      */
/* ========================================================================== */

/**
 * @brief Install a custom logger for the shared ccol_event_loop engine's own
 *        diagnostics.
 *
 * The engine logger captures the diagnostics of the reactor itself (TLS
 * handshake failures, listen-socket bind failures, and closures from the
 * idle-timeout sweep). It is separate from the logger of each server, the
 * one that you pass to ccol_create_chttpsvr().
 *
 * You can call this function at any time: before the first chttpsvr_start(),
 * while the engine runs, and between a full stop of the engine and the next
 * start. The logger serves the engine that runs at the time, if one does,
 * and every engine that a later chttpsvr_start() brings up, until another
 * call of this function replaces it. An engine that starts with no such
 * logger in place installs a fallback logger of its own instead, which
 * writes to fd 2 at level CLOG_FATAL, the same way as the internal
 * stderr-only and FATAL-only logger of ccol_create_chttpsvr. Because the
 * engine logs all of its own diagnostics below CLOG_FATAL, the min_level of
 * that fallback logger filters every one of them out, so the fallback
 * logger is silent in practice. Use this function to install a logger that
 * says more.
 *
 * This function derives a logger from cl with clog_derive() and adds the
 * field component=http-server-engine.  It closes the logger that an earlier
 * call derived, and the fallback logger of a running engine, when there is
 * one.  The library owns the derived logger and closes it when another call
 * replaces it, or at the exit of the process or the unload of the library
 * when no engine runs then. The caller keeps ownership of cl and may close
 * cl whenever it likes: the derived logger is a handle of its own, and it
 * keeps the destination of cl open as long as it lives.
 *
 * @param cl  Parent logger to derive from. It must not be CLOG_INVALID.
 * @return ccol_success, ccol_invalid_args when cl is CLOG_INVALID, or
 *         ccol_not_enough_memory when the derived logger cannot be
 *         allocated.
 */
ccol_retval_t chttpsvr_set_engine_logger(clog cl);

/* ========================================================================== */
/*                    ENGINE MEMORY MANAGEMENT                                */
/* ========================================================================== */

/**
 * @brief Install custom memory management procs for the shared ccol_event_loop
 *        engine's own internal allocations.
 *
 * Every chttpsvr instance in this process shares one ccol_event_loop
 * reactor (see the module notes on the shared engine). By default, that
 * reactor allocates its own memory (the registration table, the dispatch
 * state of each connection, and other internal data) with the default
 * allocator. A call to this function with an *mp* that is not NULL sends all
 * of that to the procs that you give instead, in the same way as the memory
 * management procs that every other module in this library accepts, and a
 * call with NULL goes back to the default behavior.
 *
 * Each chttpsvr instance uses its own allocator for its own connections and
 * requests, which you set with ccol_create_chttpsvr_mp, under the usual _mp
 * convention. This function is separate from that allocator and affects
 * only the construction of the one shared reactor.
 *
 * While you can swap the logger of chttpsvr_set_engine_logger() at any
 * time, you can call this function only before the first chttpsvr_start()
 * in the process, because after the reactor allocates memory with one set
 * of procs, a swap to a different malloc and free pair corrupts the heap.
 * You can call it again after the reactor stops completely, that is, after
 * chttpsvr_engine_wait() returns, and before the next chttpsvr_start().
 *
 * @param mp  Custom memory management procs, or NULL to go back to the
 *            default. All four function pointers must be set when mp is not
 *            NULL.
 * @return ccol_success; ccol_invalid_args when mp is not NULL but has a NULL
 *         function pointer; or ccol_not_permitted when the engine already
 *         runs, in which case stop the engine first.
 */
ccol_retval_t chttpsvr_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp);

/* ========================================================================== */
/*                    ENGINE REACTOR THREAD COUNT                             */
/* ========================================================================== */

/**
 * @brief Configure how many OS threads the shared ccol_event_loop engine
 * devotes to its own polling and dispatch.
 *
 * The shared reactor uses exactly 1 thread by default, which is what you get
 * when you never call this function, or when you call it with num_threads
 * == 0. That one dedicated thread both polls with epoll_wait and runs every
 * dispatch callback (which parses headers and steps the TLS handshake)
 * inline, with no separate dispatch worker pool at all. A call to this
 * function with a num_threads above 1 starts that many OS threads instead,
 * following the num_reactor_threads rules of
 * ccol_event_loop_create_with_mprocs (cthreadcomm.h): one dedicated polling
 * thread, plus (num_threads - 1) separate dispatch worker threads that run
 * the callbacks.
 *
 * The default is 1, instead of a CPU count that the library detects,
 * because 1 is faster for the common case:
 *
 *   - Plain HTTP with reused connections, which is the typical traffic of a
 *     browser or an API client: one thread gives the best throughput.
 *     Because a header parse is fast, the dispatch phase of plain HTTP has
 *     almost no CPU-bound work in it, so more threads only add the cost of
 *     a hand-off, with nothing to run in parallel.
 *   - TLS with reused connections, which is the typical HTTPS traffic once
 *     you count the connection pool of a client: this is a real trade, not
 *     a clean win either way. One thread gives lower throughput, but a
 *     clearly better and steadier p99 latency than more than one dispatch
 *     thread. Most requests on a TLS connection take the same cheap
 *     steady-state path as plain HTTP; only the handshake of the connection
 *     pays the expensive part, and that cost spreads across every request
 *     that the connection then serves.
 *   - TLS with no connection reuse at all, where every request pays a new
 *     handshake: more than one dispatch thread gives both higher throughput
 *     and lower p99 latency. This is a deliberately extreme case, not
 *     typical traffic. The asymmetric-crypto cost of a TLS handshake is the
 *     private-key operation of the server, which is real CPU-bound work and
 *     gains from a spread across cores when there is enough of it.
 *
 * A larger num_threads is worth its cost in one case: a steady *churn of
 * connections* together with TLS, where many separate clients each open a
 * connection for only one request, or a few, before it closes, so a large
 * part of the total traffic pays the CPU cost of a handshake instead of
 * spreading it out. This is real for some deployments, such as a public API
 * that takes many one-off anonymous clients, a gateway for IoT devices
 * where unreliable networks cause frequent reconnects, and a webhook
 * receiver that many different external services call. But it is the less
 * common shape overall: most HTTP client software (browsers, most serious
 * HTTP client libraries, and the chttpclient of this library) pools and
 * reuses connections exactly to avoid a repeated handshake cost, and a
 * server behind a load balancer or a reverse proxy often never sees raw
 * handshake churn from the public internet at all. A deployment that knows
 * that its own traffic has a lot of churn should raise num_threads, and
 * this function exists for that override.
 *
 * This function sets a value that the reactor takes at construction time,
 * the same way as chttpsvr_set_engine_mem_mgmt_procs, so you can call it
 * only before the first chttpsvr_start() in the process, or again after the
 * engine stops completely, that is, after chttpsvr_engine_wait() returns,
 * and before the next chttpsvr_start().
 *
 * @param num_threads  The OS thread count that you want for the reactor, or
 *                      0 to go back to the default of 1.
 * @return ccol_success, or ccol_not_permitted when the engine already runs,
 *         in which case stop the engine first.
 */
ccol_retval_t chttpsvr_set_engine_num_reactor_threads(size_t num_threads);

/* ========================================================================== */
/*                         ENGINE WAIT                                        */
/* ========================================================================== */

/**
 * @brief Block until the shared ccol_event_loop engine's reactor threads exit.
 *
 * It returns at once when the engine never started, or when the engine
 * already stopped.
 *
 * Use it as an escape hatch when you want to keep the calling thread alive
 * until a shutdown signal makes the engine exit, without destroying the
 * server handle first.
 *
 * Note: a call to chttpsvr_destroy on the last running server quiesces that
 * server synchronously (it stops the listener, drains the in-flight
 * requests, closes the connections and frees the engine reference of that
 * server), but it does NOT block until the shared reactor itself exits
 * completely, because that final teardown runs on a separate reaper thread.
 * Call chttpsvr_engine_wait() explicitly after chttpsvr_destroy() when the
 * calling code needs a firm guarantee that the engine exited completely,
 * for example just before the process exits.
 *
 * Do not call this function from a request handler or a middleware that runs
 * on the worker pool of ANY server: such a call is fatal at once instead of
 * hanging. The drain of the worker pool of that server can never finish
 * while this one in-flight request itself waits here for the engine to
 * exit, which deadlocks the shutdown of the whole shared engine, not only of
 * one server. An admin or shutdown endpoint that wants the engine drained
 * calls chttpsvr_engine_stop() and then returns normally; call
 * chttpsvr_engine_wait() from a separate thread.
 */
void chttpsvr_engine_wait(void);

/**
 * @brief Signal the shared ccol_event_loop engine to stop.
 *
 * It does not block, and it is async-signal-safe, so you can call it from a
 * signal handler. The engine drains the in-flight requests and exits; use
 * chttpsvr_engine_wait() to block until that drain finishes.
 *
 * You can call it more than once, including while the drain of an earlier
 * call still runs, as happens with two SIGTERMs that arrive moments apart,
 * or with an extra defensive call from the shutdown code of an application.
 * A repeated or overlapping call does nothing and is never a second
 * teardown.
 *
 * The library installs no signal handlers, so an application must connect
 * this function to whatever signal or shutdown mechanism it uses.  This is
 * the only function in this module that is documented as
 * async-signal-safe: chttpsvr_stop() is not, and you must not call it
 * directly from a signal handler (see the doc comment of that function).
 */
void chttpsvr_engine_stop(void);

/* ========================================================================== */
/*                         CONSTRUCTORS                                       */
/* ========================================================================== */

/**
 * @brief Create an HTTP server with a custom allocator.
 *
 * The server is idle until you call chttpsvr_start().  You can register
 * routes and middleware at any time, before or after the server serves
 * traffic.
 *
 * The server always manages its own logger, separate from any handle that
 * the caller gives:
 *   - cl == CLOG_INVALID: the server creates an internal logger that writes
 *                  only FATAL messages to stderr.
 *   - cl != CLOG_INVALID: the server derives a new logger from cl with
 *                  clog_derive(), and sets the field component=http-server
 *                  on it.  The cl of the caller does not change, and the
 *                  caller keeps ownership of it.
 * In both cases, chttpsvr_destroy() closes the logger of the server
 * automatically.
 *
 * @param mprocs   Custom allocator, or NULL for malloc and free.
 * @param cl       Parent logger to derive the logger of this server from, or
 *                 CLOG_INVALID to use an internal logger that writes only FATAL
 *                 messages to stderr.
 * @param err_str  Optional. On a failure it receives a static error string.
 * @return The new server handle, or CHTTPSVR_INVALID on a failure.
 */
chttpsvr ccol_create_chttpsvr_mp(ccol_memmgmt_procs_t *mprocs, clog cl,
                                 char **err_str);

/**
 * @brief Create an HTTP server with the default allocator.
 *
 * @return The new server handle, or CHTTPSVR_INVALID on a failure.
 */
static inline __attribute__((always_inline)) chttpsvr
ccol_create_chttpsvr(clog cl, char **err_str) {
  return ccol_create_chttpsvr_mp(NULL, cl, err_str);
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/**
 * @brief Internal destroy. Use the chttpsvr_destroy macro instead.
 *
 * It stops the server when the server runs, drains the worker pool of the
 * server, closes every connection that remains, and frees the reference of
 * this server to the shared ccol_event_loop engine, together with every
 * resource that the server owns, including the sub-routers, the routes, and
 * the ctpool that the server owns.
 *
 * Every guarantee of this function covers srv alone: after it returns, the
 * listener of srv is closed, the connections of srv are closed, and the
 * worker pool of srv is drained. When srv held the last engine reference,
 * this function only STARTS the teardown of the shared engine, which
 * destroys the reactor, stops the idle sweep, tears down the server registry
 * of the process, and closes the fallback engine logger, and which runs to
 * its end on a separate reaper thread, after this function returns. Call
 * chttpsvr_engine_wait() after this function whenever that final teardown
 * must finish before the next step:
 *   - before you return from main(), so that the reaper does not still run
 *     library code while the process tears itself down;
 *   - before you unload this library from an image that dlopen() loaded;
 *   - before you tear down a custom allocator that
 *     chttpsvr_set_engine_mem_mgmt_procs() installed, or make it invalid in
 *     another way, because the reaper frees the memory of the reactor and of
 *     the fallback engine logger through that allocator.
 *
 * The reaper thread itself needs none of that: an application that destroys
 * its last server and then calls neither chttpsvr_engine_wait() nor a later
 * chttpsvr_start() leaves no unjoined thread behind, and no thread stack
 * mapping.
 *
 * srv must be a live handle, which comes from ccol_create_chttpsvr or
 * ccol_create_chttpsvr_mp and which nothing has destroyed yet. A stale
 * handle is a fatal error, and so are a forged value and garbage. A handle
 * is stale when an earlier, completed call to this same function destroyed
 * it, and also when another thread destroys it at this moment, in a race
 * with this call. This function then calls ccol_fatal_err(), which is
 * abort() and SIGABRT, instead of a use-after-free or a double-free; this
 * covers both a purely sequential double-destroy and a concurrent one that
 * overlaps in time. CHTTPSVR_INVALID (0) is the one exception and stays a
 * silent no-op, which matches the idiom of chttpsvr_destroy, where the
 * destroy sets the handle to the invalid value.
 *
 * Do not call this function from a request handler or a middleware that runs
 * on the worker pool of srv: such a call destroys the very server that the
 * handler runs for, from inside that same handler, and it is the same fatal
 * misuse. The in-flight request of this thread can never finish while the
 * thread itself blocks here to destroy the server that it belongs to, so
 * the worker pool teardown of this function could never make progress
 * either way. The library detects this at once and reports it the same way
 * as a stale handle, instead of hanging. To shut a server down from inside
 * one of its own handlers, destroy it from a different thread, or call
 * chttpsvr_engine_stop() when you want a full engine shutdown, which is
 * safe from inside a handler (see its own doc comment).
 *
 * You can call this function at the same time as a chttpsvr_engine_stop()
 * that still force-stops this same server on its own background reaper
 * thread, for example when a signal handler calls chttpsvr_engine_stop()
 * while an application thread calls this function on the same handle. One
 * of the two finishes the teardown of srv first, and the other one then
 * waits for that teardown to finish completely instead of racing it.
 */
void __chttpsvr_destroy(chttpsvr srv);

/**
 * @brief RAII cleanup helper. Use it with _ccol_destructor.
 *
 * It is safe on a *pp that is already CHTTPSVR_INVALID, where it does
 * nothing. A call on a stale handle that is not CHTTPSVR_INVALID and that
 * something else already destroyed is the same fatal misuse that
 * __chttpsvr_destroy documents.
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
 * It stops the server when the server runs, drains every in-flight request,
 * and frees the reference of this server to the shared engine, which starts
 * the asynchronous teardown of the engine when this was the last reference.
 * Call chttpsvr_engine_wait() after it when that teardown must finish
 * before the next step; see the doc comment of __chttpsvr_destroy. A second
 * call on a separate copy of the same handle value is a fatal error, both
 * for a concurrent second call and for a later one, after the first call
 * completes, and so is a call from inside a request handler or a middleware
 * of this server. See the doc comment of __chttpsvr_destroy.
 *
 * @note The macro evaluates name exactly once, and name must be a
 * modifiable lvalue, such as a variable or an element of an array
 */
#define chttpsvr_destroy(name) \
  _ccol_chttpsvr_destroy_impl( \
      name, _ccol_uniq(__ccol_chttpsvr_destroy_slot, __COUNTER__))

/* Internal: the body of chttpsvr_destroy. Because slot is a name from
 * _ccol_uniq(), the macro nests inside the argument of another destroy macro
 * and stays -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_chttpsvr_destroy_impl(name, slot) \
  do {                                          \
    __typeof__(name) *slot = &(name);           \
    __chttpsvr_destroy(*slot);                  \
    *slot = CHTTPSVR_INVALID;                   \
  } while (0)

/* ========================================================================== */
/*                         LIFECYCLE MACROS                                   */
/* ========================================================================== */

/** @brief Declare a server variable that is not initialized. */
#define chttpsvr_declare(name) chttpsvr name

/** @brief Declare a server variable that the compiler destroys automatically
 *         at the end of the scope.
 */
#define chttpsvr_declare_scoped(name) \
  chttpsvr name _ccol_destructor(___chttpsvr_destroy) = CHTTPSVR_INVALID

/**
 * @brief Declare and initialize a server, calling ccol_fatal_err on a
 *        failure.
 *
 * @param name  The variable name for the server handle.
 * @param cl    Parent logger to derive the logger of this server from, or
 *              CLOG_INVALID to use an internal logger that writes only FATAL
 *              messages to stderr.  See ccol_create_chttpsvr_mp() for the
 *              details.
 *
 * Example:
 * @code
 * chttpsvr_construct(srv, CLOG_INVALID);
 * chttpsvr_register_handler(srv, CHTTP_GET, "/hello", my_handler, NULL);
 * chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
 * cfg.port = 9000;
 * if (chttpsvr_start(srv, &cfg) != ccol_success) {
 *   chttpsvr_destroy(srv);
 *   chttpsvr_engine_wait();  // wait until the engine stops completely
 *   return 1;
 * }
 * chttpsvr_engine_wait();
 * chttpsvr_destroy(srv);
 * @endcode
 */
#define chttpsvr_construct(name, cl)                         \
  chttpsvr name = CHTTPSVR_INVALID;                          \
  do {                                                       \
    char *_chs_err = NULL;                                   \
    (name) = ccol_create_chttpsvr((cl), &_chs_err);          \
    if (!(name)) {                                           \
      ccol_fatal_err("chttpsvr_construct('%s'): %s", #name,  \
                     _chs_err ? _chs_err : "unknown error"); \
    }                                                        \
  } while (0)

/**
 * @brief Declare a server, initialize it, and destroy it automatically at
 *        the end of the scope, calling ccol_fatal_err on a failure.
 *
 * @param name  The variable name for the server handle.
 * @param cl    Parent logger to derive the logger of this server from, or
 *              CLOG_INVALID to use an internal logger that writes only FATAL
 *              messages to stderr.  See ccol_create_chttpsvr_mp() for the
 *              details.
 */
#define chttpsvr_construct_scoped(name, cl)                               \
  chttpsvr name _ccol_destructor(___chttpsvr_destroy) = CHTTPSVR_INVALID; \
  do {                                                                    \
    char *_chs_err = NULL;                                                \
    (name) = ccol_create_chttpsvr((cl), &_chs_err);                       \
    if (!(name)) {                                                        \
      ccol_fatal_err("chttpsvr_construct_scoped('%s'): %s", #name,        \
                     _chs_err ? _chs_err : "unknown error");              \
    }                                                                     \
  } while (0)

/* ========================================================================== */
/*                         SERVER LIFECYCLE                                   */
/* ========================================================================== */

/**
 * @brief Register this server's listener on the shared engine.
 *
 * It binds the socket that listens, and starts to accept connections.  When
 * this is the first chttpsvr_start() call in the process, the library
 * starts the shared ccol_event_loop reactor automatically, in background
 * threads, and a later call for another server reuses the engine that
 * already runs.
 *
 * The server creates its own ctpool at this point, with
 * worker_thread_count threads and a queue depth of worker_queue_capacity.
 * When the server ran before, the new pools replace the old ones at once
 * while the old ones drain in the background: a request of the previous
 * run that still runs finishes normally, with its own response, and this
 * call does not wait for it, while chttpsvr_destroy() does.
 *
 * Every TCP connection that the server accepts gets TCP_NODELAY, and it keeps
 * the socket buffer sizes of the kernel, so that the buffer autotuning of
 * Linux (tcp(7)) stays on for it.  A connection on a Unix domain socket
 * listener gets both socket buffers raised to at least 128 KiB, because such
 * a socket has no autotuning.
 *
 * When the server closes a connection after a complete response while the
 * client can still be sending (the unread body of a request that the server
 * refused, a body that a handler left unread, or pipelined bytes), it uses
 * a lingering close. A plain close would make the kernel answer those bytes
 * with a reset, which can destroy the response before the client reads it,
 * so the server instead sends a TLS close_notify on a TLS connection, shuts
 * down its side for writing, and reads and discards what the client still
 * sends, for at most 2 seconds, before it closes. A client that sends the
 * whole body of a refused upload before it reads the response therefore
 * gets the response too, as long as it finishes sending within that time:
 * it reads the whole response and then an orderly end of the stream. A
 * lingering connection holds no thread, and it counts against
 * max_connections until it closes.
 *
 * The library leaves the disposition of SIGPIPE to the application. No write
 * of the server raises that signal, because every write to a connection (a
 * TLS record and the close_notify of a TLS teardown included) is a send(2)
 * or a sendmsg(2) with MSG_NOSIGNAL. A client that closes or resets its
 * connection while the server writes a response therefore ends that one
 * connection and never the process, whatever disposition of SIGPIPE the
 * application has chosen.
 *
 * A restart of srv is a call to chttpsvr_stop() and then a call to this
 * function, which applies cfg at once: the new listener binds before this
 * call returns, while a request that the previous run still serves goes on
 * to its end in the background, and a keep-alive connection that stayed
 * open across the stop is served under the new configuration from its next
 * request on. A restart is safe even while a chttpsvr_stop() call on the
 * same handle from a different thread is still in flight: this function
 * waits for the teardown of the old listener in that call to finish
 * completely and then binds a new listener on the same host and port,
 * instead of racing a bind() against a socket that the old call did not
 * close yet.
 *
 * This function is also safe in a race with a concurrent engine-wide
 * teardown, which comes from chttpsvr_engine_stop(), or from the graceful
 * shutdown of the shared reactor after the destruction of the last other
 * server that references it. This function retries transparently until
 * that teardown finishes instead of racing it.
 *
 * It returns at once; use chttpsvr_engine_wait() to block until the engine
 * exits.
 *
 * Do not call this function from a request handler or a middleware of srv to
 * restart the very server that the handler runs on (the same handler
 * usually calls chttpsvr_stop() first). The library refuses such a call with
 * ccol_not_permitted, without trying it, and the refusal leaves srv in a
 * safe state that you can recover from: the configuration that srv had
 * before, or a stopped server when chttpsvr_stop() already ran. A later,
 * legitimate call to this function from a different thread restarts it
 * normally.
 *
 * @param srv  Server handle.
 * @param cfg  Startup configuration.  The server uses
 *             CHTTPSVR_CONFIG_DEFAULT when this is NULL.
 * @return ccol_success            The listener is bound and registered.
 *         ccol_invalid_args       srv is CHTTPSVR_INVALID, or a stale handle
 *                                 that something already destroyed; or
 *                                 cfg->port is 0, unless cfg->host is a
 *                                 "unix://" path, where the server ignores
 *                                 the port; or cfg->tls is set but
 *                                 cert_path and key_path are not both set.
 *                                 A server certificate and its private key
 *                                 are a pair, and neither one alone is ever
 *                                 a valid server-side TLS configuration;
 *                                 nor is ca_bundle_path alone, nor an
 *                                 otherwise empty chttp_tls_config_t such as
 *                                 CHTTP_TLS_DEFAULT.
 *         ccol_not_permitted      The server already runs (stop it with
 *                                 chttpsvr_stop() before you restart it),
 *                                 or this call came from a request handler
 *                                 or a middleware of srv.
 *         ccol_not_enough_memory  An internal allocation failed, or the
 *                                 library could not create the shared
 *                                 engine, that is, its reactor threads or
 *                                 its own diagnostics logger.
 *         ccol_unexpected_failure The library could not bind the listener
 *                                 socket (among other reasons, because the
 *                                 address that cfg->host names is in use,
 *                                 because cfg->host does not resolve or is
 *                                 malformed, which the engine logger
 *                                 reports with its reason, or because a
 *                                 "unix://" path names a file that is not a
 *                                 socket, or a socket that a live server
 *                                 listens on), could not register the listener
 *                                 with the shared reactor, or could not
 *                                 start the shared idle-timeout sweep
 *                                 thread; or cfg->tls was set, and the
 *                                 library could not load its certificate
 *                                 and key pair or its ca_bundle_path; or
 *                                 the process already holds
 *                                 PTHREAD_KEYS_MAX thread-specific keys, so
 *                                 the library could not create the one that
 *                                 marks the worker threads of a server (a
 *                                 later call tries again).
 */
ccol_retval_t chttpsvr_start(chttpsvr srv, const chttpsvr_config_t *cfg);

/**
 * @brief Close this server's listener socket.
 *
 * It stops accepting new connections on the port of this server, but does
 * not close the connections that the server already accepted: an in-flight
 * request that already runs continues until it completes, and a keep-alive
 * connection stays open and goes on to be served, request after request,
 * until its client closes it or the idle timeout closes it, as it would on
 * a running server.  The engine and every other registered server keep
 * running.
 *
 * Call chttpsvr_destroy(), which waits internally, to close every
 * connection and to wait for every in-flight request on this server to
 * drain before you free the resources.
 *
 * It does nothing, and says nothing, when srv is CHTTPSVR_INVALID or a stale
 * handle that something already destroyed.
 *
 * You can call it at the same time as a chttpsvr_start() call that restarts
 * the same handle on a different thread: the restart waits for the teardown
 * of the old listener in this call to finish completely, and then binds a
 * new one, instead of racing it.
 *
 * It is synchronous, and it is NOT async-signal-safe, so do not call this
 * function directly from a signal handler; use chttpsvr_engine_stop()
 * there instead. This function takes internal locks that the interrupted
 * thread can already hold, for example inside chttpsvr_start() or
 * chttpsvr_destroy(), so the calling thread can deadlock itself. Use
 * chttpsvr_engine_stop() for a shutdown hook that a signal drives: that
 * function is documented as async-signal-safe, because it moves all of this
 * work off the interrupted thread.
 */
void chttpsvr_stop(chttpsvr srv);

/* ========================================================================== */
/*                         ROUTE REGISTRATION (SERVER)                        */
/* ========================================================================== */

/**
 * @brief Register a buffered-body route on the server.
 *
 * The server buffers the complete request body, when there is one, before it
 * calls the handler, which runs on a ctpool worker thread and can block.
 *
 * @param srv      Server handle.
 * @param method   The HTTP method to match, or CHTTP_ANY to match every
 *                 method.  With CHTTP_ANY the handler gets every method on
 *                 the pattern; call chttpsvr_req_method() inside the
 *                 handler to branch on the real method.  Registration order
 *                 still controls the priority: a route for one method that
 *                 you register before a CHTTP_ANY route on the same pattern
 *                 wins for its own method, and the CHTTP_ANY route then
 *                 catches every other method.
 * @param pattern  The URL pattern, which must start with '/' and supports
 *                 named parameters, for example /users/{id}.  The library
 *                 rejects two slashes together and a slash at the end.  Each
 *                 {name} must be unique inside the pattern, so the library
 *                 rejects the same name twice, for example /a/{id}/b/{id}.
 * @param fn       The handler function.
 * @param ctx      Opaque user data that the server gives to fn.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory. The
 *         library gives ccol_invalid_args when srv is CHTTPSVR_INVALID or a
 *         stale handle that something already destroyed, and also when
 *         pattern or fn is NULL, when pattern does not start with '/',
 *         when pattern has two slashes together or a slash at the end, when
 *         a {name} segment is malformed because it has no closing '}', when
 *         a {name} segment has a character outside [A-Za-z0-9_], or when the
 *         same {name} appears more than once.
 */
ccol_retval_t chttpsvr_register_handler(chttpsvr srv, chttp_method_t method,
                                        const char *pattern,
                                        chttpsvr_handler_fn fn, void *ctx);

/**
 * @brief Register a streaming-body route on the server.
 *
 * Nothing ever blocks the reactor thread.  The server routes the request as
 * soon as it parses the headers, before it reads any body byte, and
 * dispatches the handler right away, whatever the body size, to the
 * streaming pool of the server.  That pool is separate from the worker pool
 * of buffered routes, so a slow streaming client never holds a thread that
 * a buffered route needs.  Use chttpsvr_req_read() inside the handler to
 * pull each body batch as it arrives on the socket: the streaming thread
 * reads it live, instead of the server buffering it first.
 *
 * A streaming request that finds every streaming thread busy waits, with no
 * thread, in a queue of chttpsvr_config_t.streaming_queue_capacity entries.
 * The server immediately returns 503 Service Unavailable, with Retry-After,
 * when that queue is full, and when a request waits in it longer than
 * chttpsvr_config_t.streaming_queue_timeout_us; the handler never runs for
 * such a request.  With chttpsvr_config_t.streaming_thread_count set to
 * CHTTPSVR_STREAMING_POOL_OFF, the handler runs on the worker pool instead,
 * and the 503 of a full worker queue applies.
 *
 * @param srv     Server handle.
 * @param method  The HTTP method to match, or CHTTP_ANY to match every
 *                method.  See chttpsvr_register_handler for the rules of
 *                CHTTP_ANY.
 * @param pattern The URL pattern, which must start with '/' and supports
 *                named parameters, for example /users/{id}.  The library
 *                rejects two slashes together and a slash at the end.  Each
 *                {name} must be unique inside the pattern, so the library
 *                rejects the same name twice, for example /a/{id}/b/{id}.
 * @param fn      The handler function, which runs on a thread of the
 *                streaming pool.
 * @param ctx     Opaque user data that the server gives to fn.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory. The
 *         library gives ccol_invalid_args when srv is CHTTPSVR_INVALID or a
 *         stale handle that something already destroyed, and also when
 *         pattern or fn is NULL, when pattern does not start with '/',
 *         when pattern has two slashes together or a slash at the end, when
 *         a {name} segment is malformed because it has no closing '}', when
 *         a {name} segment has a character outside [A-Za-z0-9_], or when the
 *         same {name} appears more than once.
 */
ccol_retval_t chttpsvr_register_streaming_handler(chttpsvr srv,
                                                  chttp_method_t method,
                                                  const char *pattern,
                                                  chttpsvr_handler_fn fn,
                                                  void *ctx);

/**
 * @brief Add global middleware (runs before every route handler).
 *
 * The server runs the middleware in registration order.  Every global
 * middleware runs before every sub-router middleware, and before the final
 * handler.
 *
 * The global middleware also runs for a request that this server rejects
 * before any handler could run, so a rate limiter, a ban list for each
 * peer, or an access log that you build as a middleware sees that traffic
 * instead of the server silently leaving it out.  Three rejections run the
 * chain before their response goes out: a 404 when no route matched, a 405
 * when no route has that method, and a 413 when the declared Content-Length
 * is above max_body_size.  For a 404 or a 405 of a path under a mounted
 * prefix, the chain is the global one plus the chain of the sub-router that
 * owns the path (see "Route precedence" at the top of this file); for a 404
 * or a 405 of any other path it is the global chain alone.  Because the
 * server decides a 413 after a route already matched, a 413 runs the full
 * effective chain of that request, including the sub-router middleware.
 * The server never reaches the final handler in any of these cases, so a
 * chain that runs to its end sends the rejection response as it stands,
 * while a middleware that does not call next answers the request itself,
 * exactly as it does for a matched route.  Such a request has a method, a
 * path, headers and a query string to read, but no path parameters,
 * because no pattern captured any, and the server never reads its body, so
 * chttpsvr_req_param() returns NULL and chttpsvr_req_read() returns -1.
 *
 * The send treats a response that a middleware makes for such a request as
 * an internally generated rejection, so the small unconditional ceiling on
 * the write duration that max_response_write_duration_us describes applies
 * to it. Keep such a response small, the way a rejection response is.
 *
 * Three rejections deliberately run no middleware: a 500, which is an
 * allocation failure of this server, where application code that allocates
 * again is the wrong answer to memory pressure; a 503, where the worker
 * pool is already full, so more application work is exactly what must not
 * start; and a 501 or a 505, for a method token that the server does not
 * recognize, a transfer coding that it does not implement, or an HTTP
 * version whose major number is not 1, which the server decides from the
 * request line or from the framing headers, before it routes the request,
 * so there is no routed request to observe. A rejection that the server
 * sends inline, which happens when the rejection thread pool of the server
 * is saturated or shuts down, also runs no middleware.
 *
 * @param srv  Server handle.
 * @param fn   The middleware function.
 * @param ctx  Opaque user data that the server gives to fn.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_not_permitted. The library gives ccol_invalid_args when srv
 *         is CHTTPSVR_INVALID, when srv is a stale handle that something
 *         already destroyed, or when fn is NULL, and ccol_not_permitted when
 *         the middleware chain of the root router is already at the cap of
 *         32 entries (see "Middleware chain limit" above).
 */
ccol_retval_t chttpsvr_use(chttpsvr srv, chttpsvr_middleware_fn fn, void *ctx);

/* ========================================================================== */
/*                         SUB-ROUTERS                                        */
/* ========================================================================== */

/**
 * @brief Create a sub-router scoped to a path prefix.
 *
 * A route that you register on the sub-router matches only when the request
 * path is under prefix.  The sub-router owns every path under prefix unless
 * a more specific mount owns it: a request for such a path reaches the routes
 * of this sub-router or gets a 404 or a 405 from it, and never reaches a
 * root-level route.  The middleware of the sub-router runs after the global
 * middleware, and before the final route handler, or before that 404 or 405.
 * See "Route precedence" at the top of this file.
 *
 * The lifetime of a sub-router follows the server, so do not free the
 * pointer that this function gives you.  Every chttpsvr_router_on,
 * chttpsvr_router_on_stream and chttpsvr_router_use call on a sub-router
 * first checks that nothing destroyed the server that owns it, a check that
 * also covers a concurrent destroy from another thread, as does every other
 * call in this API that changes state and takes a chttpsvr handle directly.
 * It is therefore safe to keep a sub-router pointer across a restart with
 * chttpsvr_stop() and chttpsvr_start(), and a registration call that races
 * a concurrent chttpsvr_destroy() of the owning server returns
 * ccol_invalid_args and touches no freed memory.
 *
 * IMPORTANT: the special case of the "/" prefix:
 *   The library removes no slash from the end of a prefix of "/", because
 *   the strip loop runs only for a prefix longer than one character, so it
 *   stores the prefix as it is, with prefix_len = 1.  A "/" sub-router
 *   owns only the exact request path "/": no other path is under it, and
 *   the server routes every other path, including a path that starts with
 *   a second slash, for example "//foo", as if the "/" sub-router did not
 *   exist.  Every route on a "/" sub-router is therefore reachable only
 *   from the exact path "/".
 *
 *   That makes this prefix a trap for a sub-router that carries its own
 *   middleware: an authenticator or a rate limiter on a "/" sub-router does
 *   not refuse everything outside "/", because a root-level route, or
 *   another sub-router, serves any other path normally, and the middleware
 *   of the "/" sub-router does not run for it.
 *
 *   Register a middleware globally with chttpsvr_use() when you want it to
 *   run for every request, whatever the path, and register routes directly
 *   on the server with chttpsvr_register_handler or
 *   chttpsvr_register_streaming_handler when you want a catch-all that gets
 *   every request. Do not use a sub-router with the prefix "/" for either
 *   one.
 *
 * @param srv     Server handle. It must not be CHTTPSVR_INVALID.
 * @param prefix  The path prefix, for example "/api/v1", which must not be
 *                NULL and must start with '/'.  The library rejects two
 *                slashes together, for example "//api" or "/a//b", and
 *                removes a slash at the end automatically, so "/api/v1" and
 *                "/api/v1/" are the same.  The prefix "/" gives you a
 *                sub-router that matches only the exact root path "/" (see
 *                the note above).
 * @return A sub-router pointer that srv owns, or NULL. It gives NULL when
 *         srv is CHTTPSVR_INVALID, when srv is a stale handle that something
 *         already destroyed, when prefix is NULL, when prefix does not start
 *         with '/', when prefix has two slashes together, or when the
 *         library runs out of memory.
 */
chttpsvr_router *chttpsvr_subrouter(chttpsvr srv, const char *prefix);

/**
 * @brief Register a buffered-body route on a sub-router.
 *
 * The effective match path is prefix plus pattern: for example, "/api/v1"
 * plus "/users/{id}" matches "/api/v1/users/42".
 *
 * @param router   The sub-router that chttpsvr_subrouter() gave you.
 * @param method   The HTTP method to match, or CHTTP_ANY to match every
 *                 method.  See chttpsvr_register_handler for the rules of
 *                 CHTTP_ANY.
 * @param pattern  The URL pattern, relative to the prefix, which must start
 *                 with '/'.  The library rejects two slashes together and a
 *                 slash at the end.  Each {name} must be unique inside the
 *                 pattern, so the library rejects the same name twice, for
 *                 example /a/{id}/b/{id}.
 * @param fn       The handler function.
 * @param ctx      Opaque user data that the server gives to fn.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory. The
 *         library gives ccol_invalid_args when router, pattern or fn is
 *         NULL, when pattern does not start with '/', when pattern has two
 *         slashes together or a slash at the end, when a {name} segment is
 *         malformed because it has no closing '}', when a {name} segment has
 *         a character outside [A-Za-z0-9_], when the same {name} appears
 *         more than once, or when something destroyed the server that owns
 *         router.
 */
ccol_retval_t chttpsvr_router_on(chttpsvr_router *router, chttp_method_t method,
                                 const char *pattern, chttpsvr_handler_fn fn,
                                 void *ctx);

/**
 * @brief Register a streaming-body route on a sub-router.
 *
 * @param router  The sub-router.
 * @param method  The HTTP method to match, or CHTTP_ANY to match every
 *                method.  See chttpsvr_register_handler for the rules of
 *                CHTTP_ANY.
 * @param pattern The URL pattern, relative to the prefix, which must start
 *                with '/'.  The library rejects two slashes together and a
 *                slash at the end.  Each {name} must be unique inside the
 *                pattern, so the library rejects the same name twice, for
 *                example /a/{id}/b/{id}.
 * @param fn      The handler function, which runs on a thread of the
 *                streaming pool.
 * @param ctx     Opaque user data that the server gives to fn.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory. The
 *         library gives ccol_invalid_args when router, pattern or fn is
 *         NULL, when pattern does not start with '/', when pattern has two
 *         slashes together or a slash at the end, when a {name} segment is
 *         malformed because it has no closing '}', when a {name} segment has
 *         a character outside [A-Za-z0-9_], when the same {name} appears
 *         more than once, or when something destroyed the server that owns
 *         router.
 */
ccol_retval_t chttpsvr_router_on_stream(chttpsvr_router *router,
                                        chttp_method_t method,
                                        const char *pattern,
                                        chttpsvr_handler_fn fn, void *ctx);

/**
 * @brief Add middleware to a sub-router.
 *
 * It runs after the global middleware and before the handlers of the
 * sub-router.
 *
 * It also runs for a request that the server routes to this sub-router and
 * then rejects with 413 before its handler could run, and for the 404 or the
 * 405 of a path that this sub-router owns and that none of its routes
 * matches.  See chttpsvr_use for the full rule, and for the rejections that
 * run no middleware at all.
 *
 * @param router  The sub-router.
 * @param fn      The middleware function.
 * @param ctx     Opaque user data.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_not_permitted. The library gives ccol_invalid_args when
 *         router or fn is NULL, or when something destroyed the server that
 *         owns router, and ccol_not_permitted when the middleware chain of
 *         this router is already at the cap of 32 entries (see "Middleware
 *         chain limit" above).
 */
ccol_retval_t chttpsvr_router_use(chttpsvr_router *router,
                                  chttpsvr_middleware_fn fn, void *ctx);

/* ========================================================================== */
/*                         REQUEST API                                        */
/* ========================================================================== */

/**
 * @brief Return the HTTP method of the request.
 *
 * It returns CHTTP_GET when req is NULL: the other request accessors return
 * NULL there, but this function cannot report an error through its return
 * type.
 * The result is always one of the seven real chttp_method_t constants, never
 * CHTTP_ANY and never any other value, because the server rejects a request
 * whose method it does not recognize with 501, before any handler runs, so
 * req always belongs to a request whose method is one of the seven.
 */
chttp_method_t chttpsvr_req_method(const chttpsvr_req *req);

/**
 * @brief Return the URL-decoded request path, which ends with a NUL byte.
 *
 * The library decodes the percent-encoded sequences under the path rules of
 * RFC 3986, so the string that it gives back is the path that it matched a
 * route pattern against.
 *
 * A middleware that runs for a rejected request (see chttpsvr_use()) can
 * reach a path whose percent-encoding is malformed, which is itself one of
 * the reasons why no route could match the path. Such a path has no decoded
 * form, so this function gives it back exactly as it arrived, in its
 * encoded form, instead of giving back nothing at all: an access log that
 * leaves out the one request most worth a record is worse than one that
 * records exactly what the peer sent.
 *
 * The path always begins with "/". The server answers a request whose
 * target is neither an origin-form path nor an absolute-form http or https
 * URL with 400 Bad Request, before any middleware runs. The one exception is
 * "OPTIONS *", which the server answers itself with 200 and a Content-Length
 * of 0, exactly as Go's net/http server does, with no middleware and no
 * handler; up to 4 KiB of its body are read and discarded, and a larger
 * body closes the connection after the 200. Any other method with the
 * target "*" gets 400 and a close. For an absolute-form target the path is
 * the path of that URL, and an empty one is "/".
 *
 * The pointer stays valid for the lifetime of the request.
 */
const char *chttpsvr_req_path(const chttpsvr_req *req);

/**
 * @brief Report whether the client of this request presented a TLS
 *        certificate that verified against the ca_bundle_path of the server.
 *
 * It gives true only when the connection is TLS, the client presented a
 * certificate during the handshake, and that certificate verified, and
 * false for a plaintext connection, for a server with no ca_bundle_path,
 * for a client that presented no certificate, and when req is NULL.
 *
 * With the default configuration a server that has a ca_bundle_path accepts
 * no client without such a certificate, so every request there reports
 * true. With client_cert_optional set (chttp_tls_config_t) a client that
 * presents nothing is accepted too, and this function is how a handler or a
 * middleware tells it apart from a verified one. A client whose certificate
 * does not verify never reaches a handler in either mode.
 *
 * The answer belongs to the connection, so every request on one keep-alive
 * connection reports the same value, and a resumed TLS session reports the
 * answer of the handshake that established it.
 */
bool chttpsvr_req_peer_cert_verified(const chttpsvr_req *req);

/** The length, in bytes, of the digest that chttpsvr_req_peer_cert_sha256()
 *  writes. */
#define CHTTPSVR_PEER_CERT_SHA256_LEN 32

/**
 * @brief Give the DER encoding of the client certificate of this request.
 *
 * It gives the certificate only when chttpsvr_req_peer_cert_verified()
 * reports true for req: the leaf certificate that the client presented and
 * that verified against the ca_bundle_path of the server. It gives NULL, and
 * 0 in *len_out, in every case where that function reports false, and also
 * when an allocation fails; a caller that needs to tell the two apart asks
 * chttpsvr_req_peer_cert_verified().
 *
 * Use it to read any field of the certificate with a library of your
 * choice, for example d2i_X509() of OpenSSL, or to compare the whole
 * certificate with a pinned one.
 *
 * @param req      Request handle.
 * @param len_out  Optional. It receives the byte length of the encoding.
 * @return A pointer to the bytes, which stay valid for the lifetime of the
 *         request and which you must not free.
 */
const void *chttpsvr_req_peer_cert_der(const chttpsvr_req *req,
                                       size_t *len_out);

/**
 * @brief Write the SHA-256 fingerprint of the client certificate of this
 *        request.
 *
 * The fingerprint is the SHA-256 digest of the DER encoding that
 * chttpsvr_req_peer_cert_der() gives, the same value that
 * `openssl x509 -noout -fingerprint -sha256` prints. It identifies one
 * certificate exactly, so a handler can compare it with a list of allowed
 * clients.
 *
 * @param req  Request handle.
 * @param out  A buffer of CHTTPSVR_PEER_CERT_SHA256_LEN bytes, which every
 *             failure with a buffer leaves all zeroes.
 * @return ccol_success            out holds the fingerprint.
 *         ccol_key_not_found      chttpsvr_req_peer_cert_verified() reports
 *                                 false for req: no client certificate
 *                                 verified on this connection.
 *         ccol_not_enough_memory  An allocation failed.
 *         ccol_invalid_args       req or out is NULL.
 */
ccol_retval_t chttpsvr_req_peer_cert_sha256(
    const chttpsvr_req *req, unsigned char out[CHTTPSVR_PEER_CERT_SHA256_LEN]);

/**
 * @brief Give the subject of the client certificate of this request as an
 *        RFC 2253 distinguished name.
 *
 * The name lists its attributes from the most specific to the least, for
 * example "CN=alice,OU=staff,O=Example". Every byte above 0x7F, every control
 * character and every character that RFC 2253 reserves is escaped with a
 * backslash, so the string is printable ASCII and a name that a client
 * crafted cannot pose as another one. Compare it as a whole string: a
 * substring of a subject can come from any attribute.
 *
 * It gives the subject only when chttpsvr_req_peer_cert_verified() reports
 * true for req, and NULL in every case where that function reports false,
 * and also when an allocation fails.
 *
 * @param req  Request handle.
 * @return A NUL-terminated string that stays valid for the lifetime of the
 *         request and that you must not free.
 */
const char *chttpsvr_req_peer_cert_subject(const chttpsvr_req *req);

/**
 * @brief Return the value of a request header by name, with a name
 *        comparison that ignores case.
 *
 * @param req   Request handle.
 * @param name  The header name, for example "Content-Type".
 * @return A pointer to the value string, or NULL when the header is absent.
 *         When the client sent the same header name more than once, the
 *         function gives the LAST value, in wire order.
 *
 * Note on the lifetime: the pointer stays valid only for the length of the
 * handler call, so do NOT keep it after the handler returns. It points into
 * the copied header array that the request context owns, which the server
 * frees when it tears the request down.
 *
 * The trailer fields of a chunked request body (RFC 7230 SS4.1.2) are NOT
 * request headers, and this function never gives one back: it answers only
 * from the header block of the request, so a trailer field of any name
 * reads as absent. Trailers arrive after the server routes the request and
 * after the middleware runs, and on a buffered route also after the server
 * already read the header block. An index over trailers next to headers
 * would therefore let a client add a trailer that displaces a header that
 * an upstream proxy set (such as X-Forwarded-For, or the identity header of
 * an authentication gateway) on a request that the server already committed
 * to. This server has no trailer accessor at all; a deployment that needs
 * trailer semantics should terminate the trailers in front of this server.
 */
const char *chttpsvr_req_header(const chttpsvr_req *req, const char *name);

/**
 * @brief Return the request body of a buffered route.
 *
 * @param req      Request handle.
 * @param len_out  When this is not NULL, it receives the body length in
 *                 bytes.
 * @return A pointer to the body bytes, which do not end with a NUL byte, or
 *         NULL when the body is empty or when the route of req is a
 *         streaming route. Use chttpsvr_req_read() for a streaming route:
 *         this function always gives NULL and a length of 0 for such a
 *         route, and never a partial or stale view of the body.
 *         The pointer stays valid for the lifetime of the request.
 */
const void *chttpsvr_req_body(const chttpsvr_req *req, size_t *len_out);

/**
 * @brief Read body bytes on a streaming route, in a pull-reader model.
 *
 * It reads up to buflen bytes of the request body into buf, batch by batch,
 * as they arrive on the connection.  This call reads the socket itself, on
 * the streaming thread that runs the handler, never on the reactor thread,
 * and it blocks that thread until one of these
 * happens: at least one byte is available, the body ends, an error happens,
 * stream_read_timeout_us (chttpsvr_config_t) expires with no new data, the
 * total body read time of the request goes above max_body_read_duration_us
 * (chttpsvr_config_t) when you set that field, or the average rate of the
 * body since the first call falls below min_transfer_rate_bps
 * (chttpsvr_config_t) once min_transfer_rate_grace_us has passed. Each of
 * the last three makes chttpsvr_req_stream_error() give ccol_timed_out.
 *
 * The function returns 0 at once when buflen is 0, whatever buf holds, and
 * the call then does nothing, which matches the rules of POSIX read(2).
 *
 * It returns -1 for two groups of cause. The first group is caller misuse:
 * req is NULL, buf is NULL while buflen is above 0, or
 * chttpsvr_register_handler registered the handler instead of
 * chttpsvr_register_streaming_handler. The second group is a real transfer
 * error that depends on the data: a broken connection, an expired
 * stream_read_timeout_us or max_body_read_duration_us, or a body that goes
 * above max_body_size in mid-stream. Call chttpsvr_req_stream_error()
 * immediately after this function to tell the second group apart; that
 * function also reports a req that is NULL or invalid. It gives
 * ccol_success, not a separate code, for the misuse case where buf is NULL
 * and for the one where the route is not a streaming route, because both
 * of those are static programming errors: a caller can make them only by a
 * breach of the documented preconditions of this function, and they happen
 * on every single call to the code at fault, not sometimes, so they need no
 * diagnosis at run time.
 *
 * When the request carried "Expect: 100-continue" and really has a body to
 * receive (a Content-Length or a chunked Transfer-Encoding), the first call
 * to this function for that request writes the interim "100 Continue"
 * response before it tries to read anything, which tells the client to go
 * ahead and upload the body. A streaming handler can instead reject a
 * request outright, for example for bad authentication or an unacceptable
 * Content-Type, by writing a final response and never calling this
 * function. The server then skips that interim response completely, so the
 * client sees the real rejection directly, exactly as it does when it never
 * sends "Expect: 100-continue", and is not asked to upload a body that the
 * server was never going to read. A request that carries "Expect:
 * 100-continue" but has no body at all also gets no interim response, for
 * the same reason: there is nothing to invite the client to upload.
 *
 * @param req     Request handle, which must come from a route that
 *                chttpsvr_register_streaming_handler registered.
 * @param buf     The destination buffer, which may be NULL only when buflen
 *                is 0.
 * @param buflen  The buffer capacity in bytes.  A value of 0 is valid, and
 *                the call then does nothing.
 * A call returns the body bytes that the connection has on hand, up to
 * buflen, and waits only when it has none. It reads the socket at most once
 * for that, and one read brings at most 8 KiB of body, so a call can return
 * far fewer bytes than buflen while more of the body is still on its way.
 * As with read(2), only a return of 0 means that the body ended.
 *
 * @return The number of bytes that it read, which is above 0; 0 at EOF and
 *         for a buflen of 0; or -1 on an error.
 */
ssize_t chttpsvr_req_read(chttpsvr_req *req, void *buf, size_t buflen);

/**
 * @brief Report why the last chttpsvr_req_read() call returned -1, covering
 *        only the causes that need a diagnosis at run time.
 *
 * It has a meaning only immediately after chttpsvr_req_read() returns -1;
 * the result is unspecified at any other time, because there may be no
 * error to report.
 *
 * It tells apart only the real transfer errors of chttpsvr_req_read() that
 * depend on the data (see the doc comment of that function). When caller
 * misuse caused the -1 instead, it returns ccol_success, which you cannot
 * tell apart from "no error at all". Two cases of misuse cause it: buf was
 * NULL while buflen was above 0, or chttpsvr_register_streaming_handler did
 * not register the route of the request. Both are static programming errors
 * that happen on every call to the code at fault, not only sometimes, so a
 * caller that hits one is expected to find it and fix it in ordinary
 * testing instead of diagnosing it at run time.
 *
 * @param req  Request handle.
 * @return One of six values. ccol_timed_out means that
 *         stream_read_timeout_us or max_body_read_duration_us expired, or
 *         that the rate of the body fell below min_transfer_rate_bps.
 *         ccol_msg_too_large means that the body went above max_body_size in
 *         mid-stream. ccol_not_enough_memory means that an internal
 *         allocation failed while the server grew the body buffer, an error
 *         that the client did not cause. ccol_http_transfer_aborted means
 *         that the connection closed, or that the framing was malformed.
 *         ccol_success means that the library recorded no error, OR that one
 *         of the misuse cases above caused the -1. ccol_unexpected_failure
 *         means that req or its handle is invalid.
 */
ccol_retval_t chttpsvr_req_stream_error(const chttpsvr_req *req);

/**
 * @brief Return a named path parameter that the server took from the URL
 *        pattern.
 *
 * For example, with the route /users/{id}, a call to
 * chttpsvr_req_param(req, "id") on a request for /users/42 gives "42". The
 * library URL-decodes the value.
 *
 * @param req   Request handle.
 * @param name  The parameter name, without the braces.
 * @return A pointer to the value string, or NULL when the name is not a
 *         parameter of the route that matched.  The pointer stays valid for
 *         the lifetime of the request.
 */
const char *chttpsvr_req_param(const chttpsvr_req *req, const char *name);

/**
 * @brief Return every value for one query parameter key, including a key
 *        that has more than one value.
 *
 * For ?foo=1&foo=2, chttpsvr_req_query(req, "foo", &n) gives {"1","2"} and
 * sets *n to 2.
 *
 * @param req        Request handle.
 * @param key        The query parameter name, in its decoded form. The
 *                   library URL-decodes each key of the query string and
 *                   compares it with key as it stands, as url.Values of Go
 *                   does: "a b" finds both a+b and a%20b, and "a%20b"
 *                   finds neither.
 * @param count_out  When this is not NULL, it receives the number of values
 *                   that the function found.
 * @return A pointer to an array of value strings, or NULL when the key is
 *         absent. The array ends with a NULL, and each string ends with a
 *         NUL byte.  The value strings that the array elements point to stay
 *         valid for the lifetime of the request, but the array pointer
 *         itself is a shared scratch buffer that stays valid only until the
 *         next call to chttpsvr_req_query on the same request, so copy the
 *         values that you need before you make another query call.
 *
 * NOTE: this function cannot tell an out-of-memory failure, which can
 * happen while the library parses the query string or grows the result
 * array, apart from a real "key is absent" result: both give NULL and a
 * count of 0.  Call chttpsvr_req_query_oom() after a NULL return to tell
 * the two cases apart, or use chttpsvr_req_query_one for a key that has one
 * value.
 */
const char **chttpsvr_req_query(chttpsvr_req *req, const char *key,
                                size_t *count_out);

/**
 * @brief Return the one value of a query parameter key.
 *
 * It fails when the key has more than one value, and when the key is absent.
 *
 * @param req      Request handle.
 * @param key      The query parameter name, in its decoded form; it is
 *                 matched as chttpsvr_req_query() matches it.
 * @param val_out  On success it receives a pointer to the value string, and
 *                 on every failure return the function sets it to NULL,
 *                 when val_out itself is not NULL.
 * @return ccol_success            The function found exactly one value and
 *                                 set *val_out.
 *         ccol_key_not_found      The key is absent.
 *         ccol_not_permitted      The key has more than one value.
 *         ccol_not_enough_memory  The library could not allocate the parse
 *                                 buffer for the query string.
 *         ccol_invalid_args       req or key is NULL.
 */
ccol_retval_t chttpsvr_req_query_one(chttpsvr_req *req, const char *key,
                                     const char **val_out);

/**
 * @brief Return the raw, URL-encoded query string, or NULL when it is
 *        absent.
 *
 * For example, for /search?q=hello%20world&page=2 this function gives
 * "q=hello%20world&page=2".
 */
const char *chttpsvr_req_raw_query(const chttpsvr_req *req);

/**
 * @brief Return true when an allocation for the query string of this request
 *        failed because the library ran out of memory.
 *
 * Two allocations can fail this way: the growth of the result array inside
 * chttpsvr_req_query, and the one-time, shared parse of the query string
 * for this request, which chttpsvr_req_query and chttpsvr_req_query_one
 * each start lazily, on their first use. chttpsvr_req_raw_query never
 * starts that parse, because it gives back the raw string that the server
 * took from the request line, so it has no effect on this flag.
 *
 * chttpsvr_req_query gives NULL and a count of 0 in two cases: the key is
 * absent, or one of the two allocations above failed out of memory. Call
 * this function after any NULL return from chttpsvr_req_query to tell the
 * two cases apart.
 *
 * The flag latches: once the library sets it, it stays true for the
 * lifetime of the request, whatever query calls follow.
 *
 * It returns false when req is NULL.
 */
bool chttpsvr_req_query_oom(const chttpsvr_req *req);

/* ========================================================================== */
/*                         RESPONSE API                                       */
/* ========================================================================== */

/**
 * @brief Set the status code of the HTTP response (the default is 200).
 *
 * The library stores status_code exactly as you give it and checks nothing
 * here, but when it sends the response, it silently puts 500 on the wire
 * instead for a value outside the valid HTTP status-code range of 100 to
 * 999.
 *
 * A status_code of 1xx, 204 or 304 drops whatever body chttpsvr_resp_write,
 * chttpsvr_resp_write_str, chttpsvr_resp_printf or chttpsvr_resp_write_json
 * already wrote, or writes later, whatever the order of the calls. See the
 * doc comment of chttpsvr_resp_write.
 */
void chttpsvr_resp_set_status(chttpsvr_resp *resp, int status_code);

/**
 * @brief Set a response header, replacing every field of the same name.
 *
 * When fields with the same name already exist, the first one takes the new
 * value in place and keeps its position, and the others go, so the response
 * carries exactly one field of that name. The name comparison ignores case,
 * as RFC 7230 needs.  The stored header name keeps the letter case of the
 * FIRST call, and the function ignores the letter case that a later call
 * gives for the same name; because HTTP/1.1 header names ignore case, this
 * does not affect what goes on the wire. Use chttpsvr_resp_add_header()
 * instead to send one more field of a name, such as a second Set-Cookie.
 *
 * The fields go on the wire in the order of the calls that first set each
 * of them.
 *
 * All three arguments must not be NULL: a NULL in any of them gives
 * ccol_invalid_args and changes nothing in the response. name must also be
 * a token that is not empty and that holds only tchar bytes, as RFC 7230
 * SS3.2.6 defines them, so every byte must be an ASCII letter, an ASCII
 * digit, or one of "!#$%&'*+-.^_`|~". A name of zero length has no valid
 * form on the wire. A name with any other byte in it, for example a space
 * or a literal ':', is not itself a way to inject a CRLF, but it makes a
 * structurally malformed wire line that a strict parser downstream can read
 * wrongly: for example, the name "X Foo: bar" puts "X Foo:bar:baz\r\n" on
 * the wire, which is not a real split into two fields.
 *
 * value must not hold a CR byte or an LF byte.  The server writes both name
 * and value onto the wire exactly as they are, with no further escaping, so
 * a CR or LF inside value would let a caller inject extra header lines, or
 * split the response in two, for whoever controls the data that the caller
 * reflects (as when it puts a query parameter, a path parameter or an
 * echoed request header into a response header). The function rejects a CR
 * or LF with ccol_invalid_args instead of silently stripping it or
 * truncating the value.
 *
 * The function accepts "Connection" and checks and stores the name and the
 * value the same way as for any other header, but the server never sends
 * it: the server always decides its own Connection header and emits that,
 * and the header shows whether the server really keeps the connection open
 * afterwards.
 *
 * "Content-Length" must be one or more ASCII digits whose number fits in a
 * signed 64-bit integer; any other value gives ccol_invalid_args. Whether
 * the server sends it depends on the response:
 *
 * - A response that sends a body carries the length of the body that
 *   chttpsvr_resp_write, chttpsvr_resp_write_str, chttpsvr_resp_printf or
 *   chttpsvr_resp_write_json really wrote, never the value that you set,
 *   because that length is the framing of the response on the wire, and a
 *   value that disagrees with it would desynchronize a kept-alive
 *   connection for whichever request comes next.
 * - A 304, and a response to HEAD for which the handler wrote no body, send
 *   no body whatever the field says, and carry the Content-Length that you
 *   set, which is the length that a 200 to a GET would carry (RFC 9110
 *   SS8.6), or none at all when you set none.
 * - A 1xx and a 204 never carry one.
 *
 * This is the behaviour of Go's net/http server.
 *
 * A Date that the caller sets is sent as given and replaces the Date that
 * the server otherwise adds, so the response carries exactly one. Without
 * one from the caller, every final response carries the current time in
 * UTC, in the IMF-fixdate form of RFC 9110 SS5.6.7, such as "Sun, 06 Nov
 * 1994 08:49:37 GMT", while a 1xx response carries none. That is the rule
 * of Go's net/http server. A repeated call with the same name in any letter
 * case, through this function or through chttpsvr_resp_add_header(),
 * replaces the value, so no call sequence can produce two Date lines.
 *
 * The function rejects "Transfer-Encoding" outright with ccol_invalid_args.
 * This server never transfer-codes a response body, so it cannot obey a
 * Transfer-Encoding header that a caller sets, and such a header on the
 * wire would also sit next to the Content-Length header that this function
 * computes, over a body that nothing transfer-coded. That framing is
 * ambiguous: an intermediary that obeys Transfer-Encoding over
 * Content-Length (RFC 7230 SS3.3.3) can parse it wrongly, which is the same
 * class of response-splitting hazard that the handling of Connection and
 * Content-Length above prevents. chttp_request_set_header rejects
 * Transfer-Encoding in the same way on the client side (chttpclient.h).
 *
 * @param resp   Response handle. It must not be NULL.
 * @param name   The header name, which must not be NULL or empty, must hold
 *               only RFC 7230 tchar bytes, and must not be
 *               "Transfer-Encoding".
 * @param value  The header value, which must not be NULL and must hold no
 *               CR byte and no LF byte.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory. The
 *         library gives ccol_invalid_args when resp, name or value is NULL,
 *         when name is empty, when name holds a byte outside the RFC 7230
 *         tchar set, when value holds a CR byte or an LF byte, when name is
 *         "Transfer-Encoding" in any letter case, or when name is
 *         "Content-Length" and value is not a number as described above. A
 *         failed call changes nothing in the response.
 */
ccol_retval_t chttpsvr_resp_set_header(chttpsvr_resp *resp, const char *name,
                                       const char *value);

/**
 * @brief Add one more response header field, after every field that is
 *        already set.
 *
 * Unlike chttpsvr_resp_set_header(), this function keeps the fields of the
 * same name that already exist, so a response can carry, for example, two
 * Set-Cookie lines; each call puts one "name:value" line on the wire, in
 * the order of the calls.
 *
 * It checks name and value exactly as chttpsvr_resp_set_header() does, with
 * the same results. Three names carry one field at most in any response
 * ("Connection", "Content-Length" and "Date"), and for those three this
 * function does what chttpsvr_resp_set_header() does and replaces the
 * value; see chttpsvr_resp_set_header() for how the server treats each of
 * them.
 *
 * @param resp   Response handle. It must not be NULL.
 * @param name   The header name, under the rules of
 *               chttpsvr_resp_set_header().
 * @param value  The header value, under the rules of
 *               chttpsvr_resp_set_header().
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory, in the
 *         cases that chttpsvr_resp_set_header() lists. A failed call
 *         changes nothing in the response.
 */
ccol_retval_t chttpsvr_resp_add_header(chttpsvr_resp *resp, const char *name,
                                       const char *value);

/**
 * @brief Append bytes to the response body.
 *
 * More than one call adds to the same body, and the server flushes the
 * complete buffer after the handler returns.
 *
 * The server silently never writes the collected body to the wire, whatever
 * amount you add here, when the final status code of the response (the one
 * that the last chttpsvr_resp_set_status call set by the time the handler
 * returns) is a 1xx, 204 or 304, because RFC 9110 6.4.1, 15.2.1 and 15.4.5
 * forbid all three of these codes from ever carrying a body. This function
 * itself returns ccol_success for an append that really worked, and nothing
 * in this API reports an error for a body that the server dropped this
 * way. For example, when a handler writes diagnostic or informational body
 * content and a cache-validation middleware then lowers its status to one
 * of these codes, that content never reaches the client, and no error
 * signal anywhere catches this. None of the three gets an automatic
 * Content-Length header: a 304 carries the Content-Length that the handler
 * set with chttpsvr_resp_set_header(), and none otherwise; see that
 * function.
 *
 * @param resp  Response handle.
 * @param data  The data to write.
 * @param len   The number of bytes.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_resp_write(chttpsvr_resp *resp, const void *data,
                                  size_t len);

/**
 * @brief Append a string that ends with a NUL byte to the response body.
 *
 * The server drops the body for a 1xx, a 204 and a 304 here in exactly the
 * same way; see the doc comment of chttpsvr_resp_write.
 */
ccol_retval_t chttpsvr_resp_write_str(chttpsvr_resp *resp, const char *str);

/**
 * @brief Format and append a printf-style string to the response body.
 *
 * This is the same as a format of `format` and `...` under the printf rules,
 * followed by a call to chttpsvr_resp_write_str() with the result, with no
 * allocation in between that the caller can see. The server drops the body
 * for a 1xx, a 204 and a 304 here in exactly the same way; see the doc
 * comment of chttpsvr_resp_write.
 *
 * @param resp    Response handle, which must not be NULL.
 * @param fmt  The format string, in the printf style, which must not be
 *             NULL.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory. The
 *         library gives ccol_invalid_args when resp or format is NULL, and
 *         when the vsnprintf encoding under it fails.
 */
ccol_retval_t chttpsvr_resp_printf(chttpsvr_resp *resp, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * @brief Append a JSON body and set Content-Type to application/json.
 *
 * It appends len bytes of json to the response body, and then sets the
 * content-type response header to "application/json".  Because it writes
 * the body first, the response does not change at all when the body
 * allocation runs out of memory: the function sets no header and buffers no
 * bytes.  When the body write works and the header set after it then runs
 * out of memory, the body bytes are already in the buffer, so the caller
 * gets ccol_not_enough_memory with a partly written response body. This
 * case is unlikely, and this comment documents it only to be complete.
 *
 * The function gives ccol_invalid_args when resp or json is NULL, and when
 * len is 0: a JSON body of zero length with the Content-Type header set has
 * no valid meaning, so the function rejects it instead of silently setting
 * the header with no body bytes.
 *
 * The server drops the body for a 1xx, a 204 and a 304 here in exactly the
 * same way; see the doc comment of chttpsvr_resp_write.
 *
 * @param resp  Response handle.
 * @param json  The JSON data, which must not be NULL.
 * @param len   The length of json in bytes, which must be above 0.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_resp_write_json(chttpsvr_resp *resp, const char *json,
                                       size_t len);

#pragma GCC visibility pop
