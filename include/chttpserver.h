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

/* Every declaration from here to the end of this header belongs to the
 * public Application Binary Interface (ABI) of libccollections, and the
 * shared library exports all of them. The library is built with
 * -fvisibility=hidden, so a function or object outside these blocks stays
 * internal and is absent from the dynamic symbol table, and no symbol of
 * the same name in your application can interpose it or collide with it. */
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
 * The server routes a request as soon as it has parsed its headers, before
 * it reads any body byte.  It rejects an unmatched route at once, without
 * reading a body it would only discard, and it hands a matched route to a
 * thread pool immediately, whatever the body size.  So the reactor thread
 * does O(1) work per request, and neither a large or slow body nor your
 * handler code ever blocks it.
 *
 * A route registered with chttpsvr_register_handler (buffered body) runs on
 * the server's worker pool, a ctpool that chttpsvr_start() creates and that
 * chttpsvr_config_t.worker_thread_count and
 * chttpsvr_config_t.worker_queue_capacity size.  A worker reads the body
 * straight off the socket and never waits for a slow client: a body that
 * stops arriving parks its connection with no thread, and any free worker
 * picks it up again once more of the body is ready.  The handler runs once
 * the whole body is in memory, and chttpsvr_req_body() gives it to you.  A
 * response that meets a full socket parks the same way after the handler
 * returns, so the thread that runs a handler may not be the one that began
 * reading its body.  If the task queue is full when a request arrives, the
 * server answers 503 Service Unavailable with Retry-After at once.
 *
 * A route registered with chttpsvr_register_streaming_handler (streaming
 * body) runs on a second pool, the streaming pool, which the server creates
 * on the first streaming request and which
 * chttpsvr_config_t.streaming_thread_count sizes.  A streaming handler calls
 * chttpsvr_req_read() to pull each body batch as it arrives, without
 * waiting for the rest of the body to reach the server.
 * chttpsvr_req_read() blocks the streaming thread that calls it, never the
 * reactor or a worker, until one of these happens: data arrives, the stream
 * reaches EOF, an error happens, chttpsvr_config_t.stream_read_timeout_us
 * expires, chttpsvr_config_t.max_body_read_duration_us expires if you set
 * it, or the body's average rate falls below
 * chttpsvr_config_t.min_transfer_rate_bps.  So a slow streaming client can
 * occupy at most the streaming pool, never the threads of buffered routes.
 * A streaming request that finds no free streaming thread waits, with no
 * thread, in a bounded queue with a deadline.
 *
 * Both handler types, and every middleware, may block.  A handler of either
 * kind counts as a thread of its own server: the calls it must not make on
 * that server, such as chttpsvr_destroy() or a restart, are refused or
 * fatal on both pools alike.
 *
 * ### Request validation
 *
 * The server refuses a malformed or unsupported request before any route,
 * middleware or handler sees it, and closes the connection after
 * answering:
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
 * A body's chunk framing is judged as the body arrives: its chunk
 * extensions (which the server checks and then drops) and the rest of its
 * framing may exceed 16 bytes plus twice each chunk's data by 16 KiB in
 * all. A body past that bound is malformed: a buffered route answers 400,
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
 * The first chttpsvr_start() starts the shared ccol_event_loop reactor (the
 * "engine") on its own, and destroying the last running server stops it on
 * its own, so you don't need to call the engine lifecycle functions
 * yourself.
 *
 * Call chttpsvr_set_engine_logger() to install your own logger for
 * engine-level events (TLS handshake failures, listen-socket bind failures
 * and idle-timeout closures).  It takes effect at once and also serves
 * every later start of the engine, until a call with CLOG_INVALID releases
 * it.
 *
 * Call chttpsvr_engine_wait() to block the calling thread until the engine
 * exits, for example when an external shutdown signal such as SIGTERM makes
 * a signal handler call chttpsvr_engine_stop().
 *
 * A request handler, an admin or shutdown endpoint for example, can shut its
 * own server down from inside itself by calling chttpsvr_engine_stop(),
 * which is safe there by design.  Calling chttpsvr_destroy(), or
 * chttpsvr_stop() and then chttpsvr_start(), directly on the server that
 * runs that handler is refused or fatal instead (see each one's doc
 * comment), and so is chttpsvr_engine_wait(), for the same reason.  Such a
 * handler calls chttpsvr_engine_stop() and returns; it must not also wait
 * on the same thread for the drain it started.
 *
 * Several servers can run at once, each listening on its own port with its
 * own routes, middleware, worker pool and clog handle.
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
 * Every router, the root router (which holds the global middleware) and
 * each sub-router alike, caps its own middleware chain at 32 entries.  The
 * limit applies at registration time: chttpsvr_use and chttpsvr_router_use
 * return ccol_not_permitted, and add no entry, on the call that would take
 * that one router above 32 entries.
 *
 * At dispatch time a separate combined cap of 32 applies to one request's
 * effective chain, the global middleware plus the matched router's
 * middleware.  Each side of that sum can hold up to 32 entries on its own,
 * so the combined count can go above 32 while neither router reaches its
 * own cap at registration, and every request through the affected router
 * then gets a 500 response.
 *
 * ### Route precedence
 *
 * The server tries routers by how specific their mount prefix is, most
 * specific first: every sub-router first, in descending order of how many
 * path segments its prefix has, and the root router last, since the root
 * router's prefix matches every path.  The root router holds the routes
 * that chttpsvr_register_handler and chttpsvr_register_streaming_handler
 * register directly.  Two mounts with the same segment count are either
 * disjoint literals, at most one of which can match a given path, or the
 * same prefix registered twice, in which case the earlier registration is
 * tried first.  So precedence depends only on the prefixes and patterns
 * you register, never on registration order, and a root-level pattern of
 * any breadth can never hide a sub-router's routes or skip its
 * middleware.
 *
 * A mounted prefix owns every path under it.  A path is under a prefix when
 * its decoded form (the one chttpsvr_req_path() reports) equals the prefix
 * or continues it with a '/': "/admin", "/admin/x/y", "/%61dmin/x" and
 * "/admin%2Fx" are all under "/admin", but "/administrator" is not.  The
 * most specific mount that owns the path handles the request alone, so no
 * route of a less specific mount, and no root-level route, can answer it.
 * When none of the owner's routes matches, the owner answers 404 (or 405
 * when one of its routes matches the path but not the method), and the
 * owner's middleware runs for that rejection just as it does for its
 * routes.  So a guard in a mount's middleware covers every path under the
 * mount, whatever any other router registers.  A mount nested under
 * another, such as "/admin/deep" under "/admin", owns the paths under
 * itself, so the middleware of "/admin" doesn't run for them; give the
 * nested mount its own guard when it needs one.  Two sub-routers on the
 * same prefix share it: the routes of both can answer, the earlier
 * registration first, and the earlier one answers a 404 or a 405.  A path
 * under no mount goes to the root router.
 *
 * Within one router, routes are tried in registration order.  A 405
 * response has an Allow header listing each method that a route of the
 * answering router (or routers) accepts for the path, plus HEAD where a
 * CHTTP_GET route serves the path.  See chttpsvr_register_handler(3) and
 * doc/chttpserver.md.
 *
 * Route matching works on the path's raw segments, each decoded on its
 * own.  A decoded segment, and so a {param} value, can hold '/' (from %2F)
 * and can be "." or ".." (from %2E): the server neither merges nor removes
 * dot segments.  A path whose own prefix is spelled with %2F, such as
 * "/admin%2Fx", is under "/admin", but no route of that mount can match
 * it, so the mount answers it with a 404.  Never use a path or a parameter
 * value as a file name without checking it.
 *
 * A route registered for CHTTP_GET also serves HEAD requests for the same
 * path, as RFC 9110 SS9.3.2 requires.  The handler runs exactly as for GET,
 * and the server sends the response without its body but with the
 * content-length of the body the handler wrote.  A handler that writes no
 * body for HEAD can set that content-length itself with
 * chttpsvr_resp_set_header().  A route registered explicitly for CHTTP_HEAD
 * or CHTTP_ANY on that path wins over this fallback, whichever of the two
 * you register first.
 *
 * ### Route patterns
 *
 * Exact paths:   /health  /api/v1/status
 * Named params:  /users/{id}/posts/{postId}
 *
 * ### TLS
 *
 * Point chttpsvr_config_t.tls at a chttp_tls_config_t with cert_path and
 * key_path set (TLS needs -lssl -lcrypto at link time), or leave it NULL
 * for plaintext HTTP.
 *
 * A ca_bundle_path next to them turns on mutual TLS: the handshake fails
 * for a client that presents no certificate or one that doesn't verify
 * against that bundle. client_cert_optional relaxes the first rule, and
 * chttpsvr_req_peer_cert_verified() then tells a handler which kind of
 * client it is serving. chttpsvr_req_peer_cert_der(),
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
 * compare it through a pointer cast, and never treat it as an address;
 * compare it directly with CHTTPSVR_INVALID. A truth check works too, since
 * CHTTPSVR_INVALID is 0, so `if (!srv)` is fine. The library resolves every
 * use of a chttpsvr through a slot table it owns before touching the server
 * struct, so it always detects a handle whose slot was freed, or reused for
 * a different, later server, and never dereferences freed memory or the
 * wrong server's memory. See the doc comment of chttpsvr_destroy for what
 * happens when a stale handle reaches it.
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
 * It runs on a thread of a pool the server owns (the worker pool for a
 * buffered route, the streaming pool for a streaming route), and it may
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
 * Call it to move on to the next middleware in the chain, or to the final
 * handler when no middleware is left; a middleware that never calls it
 * stops the chain there. Don't call it more than once from one middleware
 * call: a second call skips the step the first call already ran and moves
 * the chain on once more, to the step after that, and when this middleware
 * is already the last in the chain the second call runs the final handler
 * again. That's never useful.
 */
typedef void (*chttpsvr_next_fn)(chttpsvr_req *req, chttpsvr_resp *resp);

/**
 * @brief Middleware function.
 *
 * It runs before the final handler, and a global middleware also runs
 * before every sub-router middleware. Call next(req, resp) to hand control
 * to the next handler.
 *
 * It runs on a ctpool worker thread and may block.
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
 * Use it with care: an unbounded queue can grow without limit under a long
 * overload, and most production deployments do better with a finite
 * capacity and a graceful 503 rejection.
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
 * @brief Pass this as max_partial_body_memory to put no limit on the memory
 *        that buffered request bodies hold.
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
 * Start from CHTTPSVR_CONFIG_DEFAULT, fill in the fields you want to
 * change, then pass a pointer to chttpsvr_start().
 *
 * Every timeout and duration field is a uint64_t count of microseconds with
 * a name ending in _us. The server keeps each one in whole milliseconds,
 * rounding up to the next millisecond, so a value above 0 never reads as 0.
 * A value longer than the server can keep saturates to the longest finite
 * limit it keeps for that field (never shorter than 24 days) rather than
 * turning into 0 or into "no limit". CHTTPSVR_NO_DEADLINE, in the two
 * fields that accept it, is the only value that means no limit.
 */
typedef struct chttpsvr_config {
  /** Bind address, with rules that follow the listener of Go's net
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
   *  A host name listens on its first IPv4 address if it has one, and on
   *  its first IPv6 address otherwise; with ipv6_only, on its first IPv6
   *  address if it has one, and on its first IPv4 address otherwise. So
   *  "localhost" listens on 127.0.0.1 whatever order the resolver returns
   *  its addresses in. The server tries that single address: if it can't
   *  bind it, for example because another socket holds that port on
   *  127.0.0.1, chttpsvr_start() fails, and the server never listens on
   *  another address of the name instead. A name that doesn't resolve
   *  fails the start too, and the engine logger reports the resolver's
   *  reason.
   *
   *  A value of the form "unix://path/to/socket" binds a Unix domain socket
   *  at that path instead of a TCP listener, and port is then ignored and
   *  may be 0. A socket file at the path that no process listens on is
   *  replaced. The start fails, leaving the file alone, when the path names
   *  anything other than a socket, or a socket a live server listens on.
   *  chttpsvr_stop() and the destroy remove the socket file only while the
   *  path names the socket this server created, and they reach it through
   *  the directory that held it at start time, so a later chdir() doesn't
   *  matter. One chttpsvr instance listens on TCP or on a Unix socket,
   *  never both; if you want both, create two chttpsvr instances, which
   *  share the process's one reactor at no extra cost. */
  const char *host;
  /** Port to listen on (default 8080), ignored when host is a "unix://"
   *  path. 0 asks the system for a free ephemeral port, as port 0 does for
   *  Go's net.Listen; chttpsvr_bound_port() reports the port the system
   *  picked. Every start, a restart included, binds a new ephemeral port,
   *  which may differ from the previous run's. enable_reuseport has no
   *  effect with port 0, since every such listener gets its own port. */
  uint16_t port;
  /** Maximum request body in bytes (default 4 MiB); 0 means unlimited. A
   *  buffered route whose body is above this is rejected with 413 before
   *  the handler runs. A streaming route's handler is always called, and
   *  there chttpsvr_req_read() returns -1 and chttpsvr_req_stream_error()
   *  gives ccol_msg_too_large once the body crosses the limit. Either way
   *  the server closes the connection after the response (Connection:
   *  close) instead of keeping it alive, since it doesn't read the body
   *  bytes above the limit. That close is a lingering close; see
   *  chttpsvr_start(). */
  size_t max_body_size;
  /** Read timeout for each connection, in microseconds (default 60000000,
   *  that is 60 s).
   *  0 turns the idle timeout off.
   *  The idle-timeout sweep uses this value as its threshold when
   *  idle_timeout_us is 0. It applies only while a connection sits idle
   *  between requests, i.e. while it waits for the headers of the next
   *  pipelined or keep-alive request, not while a worker thread handles a
   *  request (see max_body_read_duration_us for that).
   *
   *  A finite value bounds a connection that opens, sends part of a header
   *  block or nothing at all, and then goes quiet. With the timeout off,
   *  such a connection holds its slot until the peer closes it, so a
   *  handful of sockets can occupy the listener forever. Set this to 0 only
   *  when something else in the deployment, such as a reverse proxy's
   *  timeout, already bounds how long an idle connection lives.
   *
   *  This value measures the GAP between one bit of activity and the next,
   *  so it doesn't bound a peer that keeps sending, as slowly as it likes,
   *  and never finishes its header block; max_header_read_duration_us
   *  bounds that peer. */
  uint64_t read_timeout_us;
  /** Keep-alive idle timeout in microseconds (default 60000000, that is
   *  60 s).
   *  0 means the same value as read_timeout_us, and a value above 0
   *  overrides read_timeout_us as the threshold of the idle-timeout
   *  sweep. */
  uint64_t idle_timeout_us;
  /** Bounds the gap between one batch of body bytes and the *next* while
   *  the server reads a request body, in microseconds, for buffered and
   *  streaming bodies alike. For a buffered body no thread waits: the
   *  connection parks, and the periodic sweep, or the worker that resumes
   *  it, applies the limit.
   *  0 means the server waits forever, bounded only by read_timeout_us and
   *  idle_timeout_us, which reset on any connection activity and so don't
   *  protect you from a client that sends its bytes just slowly enough to
   *  always beat them.
   *  The default is 30000000 (30 s). Once it expires, chttpsvr_req_read()
   *  returns -1 and chttpsvr_req_stream_error() gives ccol_timed_out, and a
   *  buffered route answers 408 automatically.
   *
   *  A value of 0 here, or in max_body_read_duration_us, doesn't turn an
   *  unresponsive connection into a permanent problem: shutting the server
   *  down (chttpsvr_destroy() or chttpsvr_engine_stop()) finishes in bounded
   *  time even when a streaming handler really is blocked reading such a
   *  connection's body, while a restart with chttpsvr_stop() and
   *  chttpsvr_start() waits for nothing and cuts nothing off. The server
   *  gets that bound by force-closing the connection once its own bounded,
   *  graceful wait for in-flight requests is over. That only happens as
   *  part of the shutdown sequence, never just because a peer is slow, and
   *  never to a request that is making progress. */
  uint64_t stream_read_timeout_us;
  /** Bounds the *total* wall-clock time the server spends reading one
   *  request's body, in microseconds, for buffered and streaming bodies
   *  alike; 0 means no limit. stream_read_timeout_us only bounds each
   *  single gap between two batches, so it never fires on a client that
   *  sends a byte or two just before each gap runs out; this field closes
   *  that loophole by capping the sum of all such waits for one request.
   *  Once it expires, chttpsvr_req_read() returns -1 and
   *  chttpsvr_req_stream_error() gives ccol_timed_out, the same as when
   *  stream_read_timeout_us expires, and a buffered route answers 408
   *  automatically. The default is 300000000 (5 minutes), enough for an
   *  upload of max_body_size bytes (4 MiB by default) over a link so slow
   *  that the transfer itself is the bottleneck. Set it to 0 to turn the
   *  limit off, or lower it for a deployment with small bodies and nearby
   *  clients. */
  uint64_t max_body_read_duration_us;
  /** Bounds the gap between two writes that make progress while the server
   *  sends one response to a slow reader, in microseconds; 0 means the
   *  server uses the value of stream_read_timeout_us. No thread waits for
   *  such a client: the connection parks, and the periodic sweep, or the
   *  worker that resumes it, applies the limit. This field closes on the
   *  write side the same kind of gap stream_read_timeout_us closes on the
   *  read side. A value of 0 here keeps the bounded-shutdown property that
   *  the doc comment of stream_read_timeout_us describes, so it doesn't
   *  make a server shutdown wait forever on a stalled peer.
   *
   *  Progress means the client taking bytes of the response: a write the
   *  kernel accepts, and also the socket's send queue shrinking, as the
   *  sweep sees it about once a second. So a client that reads a large
   *  response steadily never hits this limit, however large a send buffer
   *  the kernel gives the socket. The sweep interval is the limit's
   *  precision: a client that stops reading is cut between one and two
   *  sweep intervals after the limit passes. */
  uint64_t response_write_timeout_us;
  /** Bounds the *total* wall-clock time the server spends sending one
   *  response, in microseconds; 0 means no limit.
   *  response_write_timeout_us only bounds each single write(2)-like call,
   *  so it never fires on a client that reads a byte or two just before
   *  each call's timeout runs out; this field caps the sum of all such
   *  waits for one response, closing on the write side the same loophole
   *  max_body_read_duration_us closes on the read side. Once it expires,
   *  sending the response fails and the server closes the connection, so
   *  the client sees a truncated response or a reset and keeps whatever it
   *  read before the deadline. The default is 300000000 (5 minutes), the
   *  write-side match of max_body_read_duration_us's default, for the same
   *  reasons. Set it to 0 to turn the limit off.
   *
   *  Whatever you set here, 0 included, a small internal ceiling of 2
   *  seconds always bounds two other writes: a courtesy rejection response
   *  (400, 404, 405, 408, 413, 500, 501, 503 or 505) that the server
   *  generates itself instead of a handler, and the interim "100 Continue"
   *  line for "Expect: 100-continue". Both have a fixed, small shape (no
   *  body, or one 25-byte status line) and no legitimate reason to need
   *  longer. Neither holds a thread while it waits for the peer: a write
   *  that meets a full socket parks the connection, as a handler's response
   *  does, and the ceiling bounds how long it may stay parked. The one
   *  exception is a streaming route's interim line, which
   *  chttpsvr_req_read() writes on the handler's thread, where it waits for
   *  the client just as it waits for the body; the ceiling bounds that
   *  wait. A value here below that internal ceiling applies in full: only a
   *  value of 0, or one above the ceiling, is narrowed, and only for these
   *  two internal writes. */
  uint64_t max_response_write_duration_us;
  /** Bounds the *total* wall-clock time one connection may spend in the
   *  reactor-owned phase of one request, in microseconds. That phase is the
   *  TLS handshake, if there is one, plus reading the request's whole
   *  header block. 0 means no limit; the default is 30000000 (30 s).
   *
   *  read_timeout_us and idle_timeout_us measure the gap between one bit of
   *  activity and the next, and every arriving byte resets them; this field
   *  caps the sum instead. It bounds a peer that opens a connection and then
   *  sends its request headers, or its handshake, one byte at a time,
   *  forever, never completing them. Such a peer never gets near
   *  max_header_bytes, since it never finishes the block, and it resets the
   *  gap timer on every byte, while each connection it holds costs a file
   *  descriptor, an epoll registration and a buffer. Once enough of them
   *  reach max_connections, the listener stops accepting anything. This
   *  field is the header-phase counterpart of max_body_read_duration_us,
   *  which closes the same loophole for the body phase.
   *
   *  The budget is per request, not per connection. It starts at the first
   *  TLS handshake step for a connection that has one, and at the first byte
   *  of the request's header block for one that doesn't. The next request
   *  on a keep-alive connection starts a fresh budget when its own first
   *  byte arrives, so time the connection spends idle between two requests
   *  is never charged to either. Once the budget expires the server closes
   *  the connection without a response, since the header block was never
   *  complete enough to answer. The expiry is noticed by the same periodic
   *  sweep that read_timeout_us and idle_timeout_us use, which runs once a
   *  second and closes every expired connection, however many there are,
   *  so a connection past its deadline lives up to about a second longer
   *  than the deadline.
   *
   *  The default is generous for any legitimate client: a browser or an API
   *  client sends its header block right after connecting, and even a full
   *  TLS handshake plus several kilobytes of headers over a lossy,
   *  high-latency link takes only seconds. Raise it for a deployment whose
   *  clients really are that slow, and set it to 0 to turn the limit off
   *  when something else, such as a reverse proxy in front of this server,
   *  already bounds the same phase. */
  uint64_t max_header_read_duration_us;
  /** Maximum combined size, in bytes, of one request's header block (the
   *  request line plus all header lines). 0 means the library's built-in
   *  default (64 KiB). A request whose headers are above this is rejected
   *  with 400 Bad Request and the connection is then closed, as for every
   *  other malformed request. The answer is best effort: a peer still
   *  streaming its oversized header block when the server closes may hit a
   *  reset before it reads anything.
   *
   *  Two fixed limits apply alongside this one, whatever its value, and a
   *  request past either gets the same 400: the request line and each
   *  header line must end within 8 KiB, CRLF included, and a request holds
   *  at most 100 header lines. A chunked body's chunk-size lines and trailer
   *  lines have the same 8 KiB bound, and its trailer lines count against
   *  the 100 and against this byte limit. */
  size_t max_header_bytes;
  /** Maximum number of connections open at once on this listener (default
   *  16384); 0 means unlimited. A finite value bounds how many file
   *  descriptors one listener can hold, so a peer opening connections
   *  faster than the idle sweep frees them can't use up the process's
   *  descriptor limit and take every other listener and every outbound
   *  socket of your application down with it. Raise it for a deployment
   *  that really serves more simultaneous connections than this, and raise
   *  RLIMIT_NOFILE to match.
   *  At capacity, new connections stay pending in the kernel's listen
   *  backlog instead of being accepted and then rejected: the server
   *  doesn't call accept(2) again for this listener until a connection
   *  closes and frees a slot. The listener doesn't resume at once, because
   *  a periodic sweep notices the free slot, so in the worst case about one
   *  second passes between a slot freeing and the listener resuming. */
  size_t max_connections;
  /** TLS configuration. NULL means plaintext. */
  const chttp_tls_config_t *tls;
  /** Number of worker threads in the server's own ctpool. The default is the
   *  CPU count, which passing 0 also gives you. */
  int worker_thread_count;
  /** Capacity of the worker task queue.
   *  0                       = the library default (1024 * worker_thread_
   *                            count).
   *  CHTTPSVR_QUEUE_UNBOUNDED = no limit. An overflow never gives a 503.
   *  Any other value         = the exact bounded capacity. The server gives
   *                            a 503 when the queue is full. */
  size_t worker_queue_capacity;
  /** Turn SO_KEEPALIVE on for every TCP connection the server accepts
   *  (default: off), so the OS, with its own keepalive probe interval,
   *  finds and closes a connection whose peer has gone silently
   *  unreachable, say after somebody pulls a network cable. It's separate
   *  from idle_timeout_us and adds to it, since idle_timeout_us only
   *  measures local inactivity, not whether the peer can be reached. No
   *  effect on a Unix domain socket listener. */
  bool enable_keepalive;
  /** Set SO_REUSEPORT on the listening socket (default: off), which lets
   *  several processes, or several chttpsvr instances in one process, bind
   *  the same host and port at once while the kernel load-balances accepted
   *  connections across them. The library doesn't coordinate several
   *  processes itself: this setting only controls whether the OS-level
   *  prerequisite is in place, so your application can do that
   *  coordination. No effect on a Unix domain socket listener. */
  bool enable_reuseport;
  /** Set IPV6_V6ONLY on the listening socket when it binds an IPv6 address
   *  (default: off). With host NULL or "", off means a dual-stack listener
   *  whatever the system default. With an IPv6 address or name, off keeps
   *  the OS default: on Linux a dual-stack socket that also accepts
   *  IPv4-mapped connections, unless something like
   *  /proc/sys/net/ipv6/bindv6only restricts it, and on FreeBSD an
   *  IPv6-only socket, unless the sysctl net.inet6.ip6.v6only is 0. Set it
   *  to true for an IPv6-only listener that must never accept IPv4 traffic
   *  as well; a host name then listens on its IPv6 address if it has one.
   *  No effect on an IPv4 listener or a Unix domain socket listener. */
  bool ipv6_only;
  /** The minimum average rate, in bytes per second, at which a client must
   *  send a request's body and read its response. 0 selects the default of
   *  240, and CHTTPSVR_NO_RATE_FLOOR turns the floor off.
   *
   *  The rate is measured over the whole phase, from when the server starts
   *  reading the body, or writing the response, up to now, and is judged
   *  only once min_transfer_rate_grace_us of that phase has passed. A body
   *  that falls below the floor gets 408 Request Timeout and the connection
   *  is then closed, while a response that falls below it is cut off and
   *  the connection closed. A streaming handler sees the floor as a failed
   *  chttpsvr_req_read(), with chttpsvr_req_stream_error() giving
   *  ccol_timed_out. Time the server itself holds a request back (waiting
   *  for body memory, or for a place in the streaming queue) never counts
   *  against the client. The floor adds to stream_read_timeout_us,
   *  max_body_read_duration_us, response_write_timeout_us and
   *  max_response_write_duration_us rather than replacing them. A
   *  connection waiting for its client holds no worker thread, so the floor
   *  bounds the memory and the descriptor such a connection keeps, not a
   *  thread. */
  unsigned min_transfer_rate_bps;
  /** How long a body read or a response write may run before
   *  min_transfer_rate_bps applies to it, in microseconds. 0 selects the
   *  default of 5000000 (5 s). */
  uint64_t min_transfer_rate_grace_us;
  /** The most memory, in bytes, that buffered request bodies may hold
   *  together on this server, from the first byte the server reads until
   *  the handler returns. 0 selects the default of 256 MiB, and
   *  CHTTPSVR_NO_MEMORY_CAP turns the limit off.
   *
   *  A reservation covers the memory a body's buffer really holds, and it
   *  lasts exactly as long as that buffer: while the body arrives, while the
   *  middleware and the handler run, and until the handler returns, when
   *  the server frees the body before the response goes out. A request
   *  that is refused, cut off or closed frees its body and its reservation
   *  at once, so a response waiting for a slow reader holds no body.
   *
   *  A body reserves its buffer as its bytes arrive: before the server reads
   *  a byte of it, a body framed by a Content-Length reserves a first buffer
   *  of at most 16 KiB (the bytes that came with the headers plus one read),
   *  and every body then reserves each growth of its buffer before the read
   *  that needs it. The buffer doubles as it grows and never grows past the
   *  declared length, so a client that declares a large body and sends
   *  little of it holds little memory. A growth holds the old and the new
   *  buffer together until the copy is done, so it reserves both for that
   *  time, and a body waiting for its client holds only its buffer. When the
   *  first buffer doesn't fit, the request waits, holding no thread, and the
   *  server reads nothing from it and sends no "100 Continue" until memory
   *  frees up; a later growth that doesn't fit waits the same way. Waiting
   *  requests are admitted in the order they started waiting. When every
   *  body that holds memory is itself waiting for more, the oldest waiter
   *  goes ahead past the limit, one request at a time, so the limit can be
   *  exceeded by less than twice max_body_size (that request's body and,
   *  while its buffer grows, the old buffer). A body whose handler is
   *  running isn't waiting, so a handler that doesn't return keeps its body
   *  counted, and a request that a handler waits for, and whose body doesn't
   *  fit next to the bodies running handlers hold, waits until
   *  body_memory_wait_timeout_us answers it with 503. A body that
   *  arrives complete together with its headers, and every streaming route,
   *  never reserves anything. */
  size_t max_partial_body_memory;
  /** How long a request may wait for body memory, in microseconds; 0
   *  selects the default of 30000000 (30 s). After that the server answers
   *  503 Service Unavailable with a Retry-After header and closes the
   *  connection. CHTTPSVR_NO_DEADLINE lets a request wait as long as it
   *  takes. */
  uint64_t body_memory_wait_timeout_us;
  /** Number of threads in the pool that runs streaming handlers (the ones
   *  chttpsvr_register_streaming_handler() and chttpsvr_router_on_stream()
   *  register). 0 selects the resolved worker_thread_count, and
   *  CHTTPSVR_STREAMING_POOL_OFF runs streaming handlers on the worker pool
   *  instead, next to buffered ones.
   *
   *  A streaming handler reads its body on its own thread, at the client's
   *  pace, so a pool of their own keeps slow streaming clients from taking
   *  the threads that serve buffered routes. The server creates the pool on
   *  the first streaming request it receives, so a server without streaming
   *  traffic starts no thread for it. */
  int streaming_thread_count;
  /** How many streaming requests may wait for a free streaming thread. 0
   *  selects four times the streaming thread count, and
   *  CHTTPSVR_QUEUE_UNBOUNDED means no limit. A streaming request that
   *  finds the queue full gets 503 Service Unavailable with a Retry-After
   *  header at once, and the connection is closed. A waiting request holds
   *  no thread. */
  size_t streaming_queue_capacity;
  /** How long a streaming request may wait in that queue, in microseconds;
   *  0 selects the default of 5000000 (5 s). A request that waits longer
   *  gets 503 Service Unavailable with a Retry-After header, and the
   *  connection is closed. CHTTPSVR_NO_DEADLINE lets a request wait as long
   *  as it takes. */
  uint64_t streaming_queue_timeout_us;
} chttpsvr_config_t;

/**
 * @brief Reasonable defaults for chttpsvr_config_t.
 *
 * Port 8080, every interface through one dual-stack listener (host NULL;
 * set host to "0.0.0.0" for IPv4 only), a 4 MiB body limit, no TLS, one
 * worker thread per CPU, and the library's default queue capacity.
 *
 * Every timeout and resource limit here is finite, so a server started from
 * these defaults is already bounded against two kinds of peer: one that
 * opens a connection and then stalls (a slowloris), and one that sends a
 * body, or reads a response, one byte at a time. Each field's doc comment
 * above describes its limit, and you can change each one on its own. For
 * the fields up to ipv6_only, 0 keeps its documented meaning of "off" or
 * "unlimited". The fields from min_transfer_rate_bps to
 * streaming_queue_timeout_us read 0 as "use the default", so a
 * configuration built with {0} or with designated initializers keeps these
 * defenses; each of them only turns off through its own named constant.
 *
 * For a buffered route a worker thread never waits on a slow client: when a
 * request's body stops arriving, or the client stops reading the response,
 * the server parks the connection with no thread and resumes it on any free
 * worker once the socket is ready again.
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
 * The engine logger captures the reactor's own diagnostics (TLS handshake
 * failures, listen-socket bind failures, and closures by the idle-timeout
 * sweep). It is separate from each server's logger, the one you pass to
 * chttpsvr_create().
 *
 * You can call this function at any time: before the first
 * chttpsvr_start(), while the engine runs, and between a full engine stop
 * and the next start. The logger serves the running engine, if there is
 * one, and every engine a later chttpsvr_start() brings up, until another
 * call of this function replaces it. An engine that starts with no such
 * logger in place installs its own fallback logger instead, which writes to
 * fd 2 at level CLOG_FATAL, like the internal stderr-only, FATAL-only
 * logger of chttpsvr_create. The engine logs all of its own diagnostics
 * below CLOG_FATAL, so the fallback's min_level filters every one of them
 * out and the fallback is silent in practice. Use this function to install
 * a logger that says more.
 *
 * This function derives a logger from cl with clog_derive() and adds the
 * field component=http-server-engine.  It closes the logger an earlier call
 * derived, and a running engine's fallback logger, if there is one.  The
 * library owns the derived logger and closes it when another call replaces
 * it, when a call with CLOG_INVALID releases it, or at process exit or
 * library unload; an engine still running then closes it as it stops. You
 * keep ownership of cl and may close cl whenever you like: the derived
 * logger is a handle of its own and keeps cl's destination open as long as
 * it lives.
 *
 * The derived logger allocates and frees through cl's allocator. An
 * application that destroys that allocator before the process exits must
 * first release the engine logger by calling chttpsvr_set_engine_logger()
 * with CLOG_INVALID, which closes the derived logger at once. A release is
 * refused with ccol_not_permitted while an engine runs, because the engine
 * logs through the logger: call it after chttpsvr_engine_wait() has
 * returned. The next engine to start then uses the fallback logger.
 *
 * When cl writes to a file descriptor the application opened, as a logger
 * from clog_create_from_fd() does, the descriptor stays the application's,
 * and the derived logger writes to it until it is closed. Keep the
 * descriptor open while the engine logger is installed, and release the
 * engine logger before you close the descriptor.
 *
 * A cl that isn't CLOG_INVALID but names no open logger, because it is
 * closed or stale, gives ccol_invalid_args and changes nothing; the engine
 * keeps the logger it had.
 *
 * @param cl  Parent logger to derive from, or CLOG_INVALID to release the
 *            engine logger that an earlier call installed.
 * @return ccol_success; ccol_not_permitted for CLOG_INVALID while an engine
 *         runs; ccol_invalid_args when cl is closed or stale; or
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
 * reactor (see the module notes on the shared engine). By default that
 * reactor allocates its own memory (the registration table, each
 * connection's dispatch state and other internal data) with the default
 * allocator. Calling this function with a non-NULL *mp* sends all of that to
 * the procs you give instead, just like the memory management procs every
 * other module in this library accepts, and calling it with NULL goes back
 * to the default.
 *
 * Each chttpsvr instance uses its own allocator for its own connections
 * and requests, set with chttpsvr_create_mp under the usual _mp convention.
 * This function is separate from that and only affects how the one shared
 * reactor is built.
 *
 * Unlike the logger of chttpsvr_set_engine_logger(), which you can swap at
 * any time, this function may only be called before the first
 * chttpsvr_start() in the process, because once the reactor has allocated
 * memory with one set of procs, swapping to a different malloc and free
 * pair corrupts the heap. You may call it again once the reactor has
 * stopped completely, i.e. after chttpsvr_engine_wait() returns, and before
 * the next chttpsvr_start().
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
 * The shared reactor uses exactly 1 thread by default, which is what you
 * get if you never call this function or call it with num_threads == 0.
 * That one dedicated thread both polls with epoll_wait and runs every
 * dispatch callback (parsing headers, stepping the TLS handshake) inline,
 * with no separate dispatch worker pool at all. Calling it with num_threads
 * above 1 starts that many OS threads instead, following the
 * num_reactor_threads rules of ccol_event_loop_create_mp (cthreadcomm.h):
 * one dedicated polling thread plus (num_threads - 1) separate dispatch
 * worker threads that run the callbacks.
 *
 * The default is 1 rather than a detected CPU count because 1 is faster for
 * the common case:
 *
 *   - Plain HTTP over reused connections, the typical traffic of a browser
 *     or an API client: one thread gives the best throughput. Parsing
 *     headers is fast, so plain HTTP's dispatch phase has almost no
 *     CPU-bound work in it, and more threads only add hand-off cost with
 *     nothing to run in parallel.
 *   - TLS over reused connections, the typical HTTPS traffic once you count
 *     a client's connection pool: a real trade, not a clean win either way.
 *     One thread gives lower throughput but a clearly better and steadier
 *     p99 latency than several dispatch threads. Most requests on a TLS
 *     connection take the same cheap steady-state path as plain HTTP; only
 *     the connection's handshake pays the expensive part, and that cost is
 *     spread over every request the connection then serves.
 *   - TLS with no connection reuse at all, where every request pays a new
 *     handshake: several dispatch threads give both higher throughput and
 *     lower p99 latency. This is a deliberately extreme case, not typical
 *     traffic. The asymmetric-crypto cost of a TLS handshake is the
 *     server's private-key operation, which is real CPU-bound work and
 *     gains from being spread across cores when there's enough of it.
 *
 * A larger num_threads pays for itself in one case: a steady *churn of
 * connections* with TLS, where many separate clients each open a
 * connection for just one request, or a few, before closing it, so a large
 * part of the total traffic pays a handshake's CPU cost instead of
 * spreading it. That's real for some deployments, such as a public API
 * with many one-off anonymous clients, an IoT gateway where unreliable
 * networks cause frequent reconnects, or a webhook receiver called by many
 * different external services. But it's the less common shape overall:
 * most HTTP client software (browsers, most serious HTTP client libraries,
 * and this library's chttpclient) pools and reuses connections precisely
 * to avoid paying the handshake again, and a server behind a load balancer
 * or reverse proxy often never sees raw handshake churn from the public
 * internet at all. A deployment that knows its traffic churns a lot should
 * raise num_threads, and this function is there for that override.
 *
 * This sets a value the reactor takes when it is built, like
 * chttpsvr_set_engine_mem_mgmt_procs, so you may only call it before the
 * first chttpsvr_start() in the process, or again once the engine has
 * stopped completely, i.e. after chttpsvr_engine_wait() returns, and before
 * the next chttpsvr_start().
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
 * Returns at once if the engine never started or has already stopped.
 *
 * Use it as an escape hatch when you want to keep the calling thread alive
 * until a shutdown signal makes the engine exit, without destroying the
 * server handle first.
 *
 * chttpsvr_destroy on the last running server quiesces that server
 * synchronously (it stops the listener, drains in-flight requests, closes
 * the connections and frees the server's engine reference), but it does NOT
 * block until the shared reactor itself has fully exited, because that
 * final teardown runs on a separate reaper thread. Call
 * chttpsvr_engine_wait() explicitly after chttpsvr_destroy() when your code
 * needs a firm guarantee that the engine has fully exited, for example just
 * before the process exits.
 *
 * Don't call this function from a request handler or middleware running on
 * the worker pool of ANY server: such a call is fatal at once rather than
 * hanging. That server's worker pool can never finish draining while this
 * in-flight request is itself waiting here for the engine to exit, which
 * deadlocks the shutdown of the whole shared engine, not just one server.
 * An admin or shutdown endpoint that wants the engine drained calls
 * chttpsvr_engine_stop() and returns normally; call chttpsvr_engine_wait()
 * from a separate thread.
 */
void chttpsvr_engine_wait(void);

/**
 * @brief Signal the shared ccol_event_loop engine to stop.
 *
 * It doesn't block and it is async-signal-safe, so you can call it from a
 * signal handler. The engine drains in-flight requests and exits; use
 * chttpsvr_engine_wait() to block until that drain is done.
 *
 * Every server registered at that moment stops taking new requests at
 * once, just like a server that chttpsvr_destroy() tears down (see the doc
 * comment of __chttpsvr_destroy): idle keep-alive connections are closed, a
 * request that arrives later gets 503 with Connection: close, and the
 * response of a request already running carries Connection: close, so the
 * drain ends when the running handlers return.
 *
 * You may call it more than once, also while an earlier call's drain is
 * still running, as happens with two SIGTERMs arriving moments apart or an
 * extra defensive call from an application's shutdown code. A repeated or
 * overlapping call does nothing and never causes a second teardown.
 *
 * A call only stops the engine running at the time of the call. A call
 * while no engine runs does nothing, and a call whose engine has already
 * stopped by the time the request is handled never stops an engine that a
 * later chttpsvr_start() brought up.
 *
 * A call that lands while a server's chttpsvr_destroy() is still draining
 * it waits for that drain: chttpsvr_engine_wait() returns only after that
 * server's handlers have returned.
 *
 * The library installs no signal handlers, so your application must hook
 * this function up to whatever signal or shutdown mechanism it uses.  It is
 * the only function in this module documented as async-signal-safe:
 * chttpsvr_stop() is not, and you must not call it directly from a signal
 * handler (see its doc comment).
 */
void chttpsvr_engine_stop(void);

/* ========================================================================== */
/*                         CONSTRUCTORS                                       */
/* ========================================================================== */

/**
 * @brief Create an HTTP server with a custom allocator.
 *
 * The server is idle until you call chttpsvr_start().  You can register
 * routes and middleware at any time, before or after the server starts
 * serving traffic.
 *
 * The server always manages its own logger, separate from any handle you
 * pass in:
 *   - cl == CLOG_INVALID: the server creates an internal logger that writes
 *                  only FATAL messages to stderr.
 *   - cl != CLOG_INVALID: the server derives a new logger from cl with
 *                  clog_derive() and sets the field component=http-server
 *                  on it.  Your cl doesn't change, and you keep ownership
 *                  of it.
 * Either way, chttpsvr_destroy() closes the server's logger automatically.
 *
 * @param mprocs   Custom allocator, or NULL for malloc and free.
 * @param cl       Parent logger to derive this server's logger from, or
 *                 CLOG_INVALID to use an internal logger that writes only FATAL
 *                 messages to stderr. A handle that is closed or was never
 *                 valid makes the call fail with an error string; it doesn't
 *                 end the process.
 * @param err_str  Optional. On failure it receives a static error string.
 * @return The new server handle, or CHTTPSVR_INVALID on failure. A call
 *         made after the library's exit-time destructor has released its
 *         handle table (from a later destructor or atexit handler of the
 *         application) fails this way too, with an error string; every
 *         other call that takes a handle then treats the handle as invalid.
 */
chttpsvr chttpsvr_create_mp(ccol_memmgmt_procs_t *mprocs, clog cl,
                            char **err_str);

/**
 * @brief Create an HTTP server with the default allocator.
 *
 * @return The new server handle, or CHTTPSVR_INVALID on a failure.
 */
static inline __attribute__((always_inline)) chttpsvr
chttpsvr_create(clog cl, char **err_str) {
  return chttpsvr_create_mp(NULL, cl, err_str);
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/**
 * @brief Internal destroy. Use the chttpsvr_destroy macro instead.
 *
 * Stops the server if it is running, drains its worker pool, closes every
 * remaining connection, and frees this server's reference to the shared
 * ccol_event_loop engine along with every resource the server owns,
 * including its sub-routers, its routes and its own ctpool.
 *
 * From the moment the teardown starts, srv takes no new request.
 * Connections waiting idle for their next request are closed. A request
 * that arrives later, on a keep-alive connection or pipelined behind
 * another request, runs no handler: it gets 503 Service Unavailable with
 * Connection: close. The response of a request whose handler is already
 * running carries Connection: close and ends its connection. So the wait
 * for in-flight requests ends when the handlers already running return,
 * however many requests the clients keep sending. If handlers are still
 * running after 30 s, the library closes the connections they read from or
 * write to, so a handler blocked on its client returns.
 *
 * Every guarantee of this function is about srv alone: once it returns,
 * srv's listener is closed, its connections are closed and its worker pool
 * is drained. When srv held the last engine reference, this function only
 * STARTS the shared engine's teardown, which destroys the reactor, stops
 * the idle sweep, tears down the process's server registry and closes the
 * fallback engine logger, and which runs to completion on a separate
 * reaper thread after this function returns. Call chttpsvr_engine_wait()
 * after this function whenever that final teardown must be done before
 * the next step:
 *   - before you return from main(), so the reaper isn't still running
 *     library code while the process tears itself down;
 *   - before you unload this library from an image that dlopen() loaded;
 *   - before you tear down, or otherwise invalidate, a custom allocator
 *     that chttpsvr_set_engine_mem_mgmt_procs() installed, because the
 *     reaper frees the memory of the reactor and of the fallback engine
 *     logger through that allocator.
 *
 * The reaper thread itself needs none of that: an application that destroys
 * its last server and then calls neither chttpsvr_engine_wait() nor a later
 * chttpsvr_start() leaves no unjoined thread and no thread stack mapping
 * behind.
 *
 * The destroy frees everything the server allocated, except that each
 * sub-router from chttpsvr_subrouter() keeps about 100 bytes until the
 * process exits, so a pointer to it stays safe to pass. A server destroyed
 * by an application destructor or atexit handler after the library's
 * exit-time cleanup has run frees that memory as well, together with the
 * library's bookkeeping, and its engine closes the logger of
 * chttpsvr_set_engine_logger() when it stops.
 *
 * srv must be a live handle, from chttpsvr_create or chttpsvr_create_mp,
 * that nothing has destroyed yet. A stale handle is a fatal error, and so
 * are a forged value and garbage. A handle is stale when an earlier,
 * completed call to this function destroyed it, and also when another
 * thread is destroying it right now, racing this call. This function then
 * calls _ccol_fatal_err(), i.e. abort() and SIGABRT, rather than causing a
 * use-after-free or a double free; that covers both a purely sequential
 * double destroy and a concurrent one that overlaps in time.
 * CHTTPSVR_INVALID (0) is the one exception and stays a silent no-op,
 * matching the chttpsvr_destroy idiom, where the destroy sets the handle to
 * the invalid value.
 *
 * Don't call this function from a request handler or middleware running on
 * srv's worker pool: that destroys the very server the handler runs for,
 * from inside that same handler, and it is the same fatal misuse. The
 * thread's in-flight request can never finish while the thread itself
 * blocks here destroying the server it belongs to, so this function's
 * worker pool teardown could never make progress anyway. The library
 * detects this at once and reports it like a stale handle instead of
 * hanging. To shut a server down from inside one of its own handlers,
 * destroy it from a different thread, or call chttpsvr_engine_stop() if
 * you want a full engine shutdown, which is safe from inside a handler
 * (see its doc comment).
 *
 * You may call this function at the same time as a chttpsvr_engine_stop()
 * that is still force-stopping this same server on its own background
 * reaper thread, for example when a signal handler calls
 * chttpsvr_engine_stop() while an application thread calls this function
 * on the same handle. One of the two finishes srv's teardown first, and the
 * other then waits for that teardown to finish completely instead of
 * racing it.
 */
void __chttpsvr_destroy(chttpsvr srv);

/**
 * @brief RAII cleanup helper. Use it with _ccol_destructor.
 *
 * Safe on a *pp that is already CHTTPSVR_INVALID, where it does nothing.
 * Calling it on a stale handle that isn't CHTTPSVR_INVALID and that
 * something else has already destroyed is the same fatal misuse that
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
 * Stops the server if it is running, drains every in-flight request, and
 * frees this server's reference to the shared engine, which starts the
 * engine's asynchronous teardown if this was the last reference. Call
 * chttpsvr_engine_wait() afterwards when that teardown must be done
 * before the next step; see the doc comment of __chttpsvr_destroy. A second
 * call on a separate copy of the same handle value is a fatal error,
 * whether it runs concurrently or later, after the first call completes,
 * and so is a call from inside a request handler or middleware of this
 * server. See the doc comment of __chttpsvr_destroy.
 *
 * @note The macro evaluates name exactly once, and name must be a
 * modifiable lvalue, such as a variable or an element of an array
 */
#define chttpsvr_destroy(name) \
  _ccol_chttpsvr_destroy_impl( \
      name, _ccol_uniq(__ccol_chttpsvr_destroy_slot, __COUNTER__))

/* Internal: the body of chttpsvr_destroy. slot is a name from _ccol_uniq(),
 * so the macro can nest inside another destroy macro's argument and stay
 * -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_chttpsvr_destroy_impl(name, slot) \
  do {                                          \
    __typeof__(name) *slot = &(name);           \
    __chttpsvr_destroy(*slot);                  \
    *slot = CHTTPSVR_INVALID;                   \
  } while (0)

/* ========================================================================== */
/*                         LIFECYCLE MACROS                                   */
/* ========================================================================== */

/** @brief Declare an uninitialized server variable. */
#define chttpsvr_declare(name) chttpsvr name

/** @brief Declare a server variable that is destroyed automatically at the
 *         end of its scope.
 */
#define chttpsvr_declare_scoped(name) \
  chttpsvr name _ccol_destructor(___chttpsvr_destroy) = CHTTPSVR_INVALID

/**
 * @brief Declare and initialize a server, calling _ccol_fatal_err on a
 *        failure.
 *
 * @param name  The variable name for the server handle.
 * @param cl    Parent logger to derive the logger of this server from, or
 *              CLOG_INVALID to use an internal logger that writes only FATAL
 *              messages to stderr.  See chttpsvr_create_mp() for the
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
#define chttpsvr_construct(name, cl)                          \
  chttpsvr name = CHTTPSVR_INVALID;                           \
  do {                                                        \
    char *_chs_err = NULL;                                    \
    (name) = chttpsvr_create((cl), &_chs_err);                \
    if (!(name)) {                                            \
      _ccol_fatal_err("chttpsvr_construct('%s'): %s", #name,  \
                      _chs_err ? _chs_err : "unknown error"); \
    }                                                         \
  } while (0)

/**
 * @brief Declare a server, initialize it, and destroy it automatically at
 *        the end of the scope, calling _ccol_fatal_err on a failure.
 *
 * @param name  The variable name for the server handle.
 * @param cl    Parent logger to derive the logger of this server from, or
 *              CLOG_INVALID to use an internal logger that writes only FATAL
 *              messages to stderr.  See chttpsvr_create_mp() for the
 *              details.
 */
#define chttpsvr_construct_scoped(name, cl)                               \
  chttpsvr name _ccol_destructor(___chttpsvr_destroy) = CHTTPSVR_INVALID; \
  do {                                                                    \
    char *_chs_err = NULL;                                                \
    (name) = chttpsvr_create((cl), &_chs_err);                            \
    if (!(name)) {                                                        \
      _ccol_fatal_err("chttpsvr_construct_scoped('%s'): %s", #name,       \
                      _chs_err ? _chs_err : "unknown error");             \
    }                                                                     \
  } while (0)

/* ========================================================================== */
/*                         SERVER LIFECYCLE                                   */
/* ========================================================================== */

/**
 * @brief Register this server's listener on the shared engine.
 *
 * Binds the listening socket and starts accepting connections. A
 * cfg->port of 0 binds an ephemeral port the system picks, and
 * chttpsvr_bound_port() reports it once this call has succeeded; the
 * system picks again on every start, so a restart may move the server to
 * another port.  If
 * this is the first chttpsvr_start() in the process, the library starts the
 * shared ccol_event_loop reactor automatically on background threads, and
 * a later call for another server reuses the engine that is already
 * running.
 *
 * The server creates its own ctpool here, with worker_thread_count threads
 * and a queue depth of worker_queue_capacity. If the server ran before, the
 * new pools replace the old ones at once while the old ones drain in the
 * background: a request of the previous run that is still going finishes
 * normally with its own response, and this call doesn't wait for it,
 * whereas chttpsvr_destroy() does.
 *
 * Every TCP connection the server accepts gets TCP_NODELAY and keeps the
 * kernel's socket buffer sizes, so Linux buffer autotuning (tcp(7)) stays
 * on for it.  A connection on a Unix domain socket listener gets both
 * socket buffers raised to at least 128 KiB, since such a socket has no
 * autotuning.
 *
 * When the server closes a connection after a complete response while the
 * client may still be sending (the unread body of a request the server
 * refused, a body a handler left unread, or pipelined bytes), it uses a
 * lingering close. A plain close would make the kernel answer those bytes
 * with a reset, which can wipe out the response before the client reads
 * it, so instead the server sends a TLS close_notify on a TLS connection,
 * shuts down its writing side, and reads and discards whatever the client
 * still sends, for at most 2 seconds, before closing. So a client that
 * sends the whole body of a refused upload before reading the response
 * still gets the response, as long as it finishes sending in that time: it
 * reads the whole response and then an orderly end of stream. A lingering
 * connection holds no thread, and it counts against max_connections until
 * it closes.
 *
 * The library leaves SIGPIPE's disposition to the application. No server
 * write raises that signal, since every write to a connection (TLS records
 * and the close_notify of a TLS teardown included) is a send(2) or
 * sendmsg(2) with MSG_NOSIGNAL. So a client that closes or resets its
 * connection while the server writes a response ends that one connection,
 * never the process, whatever SIGPIPE disposition the application has
 * chosen.
 *
 * Restarting srv means calling chttpsvr_stop() and then this function,
 * which applies cfg at once: the new listener is bound before this call
 * returns, while a request the previous run is still serving runs to its
 * end in the background, and a keep-alive connection that stayed open
 * across the stop is served under the new configuration from its next
 * request on. A restart is safe even while a chttpsvr_stop() on the same
 * handle from another thread is still in flight: this function waits for
 * that call's teardown of the old listener to finish completely and then
 * binds a new listener on the same host and port, instead of racing a
 * bind() against a socket the old call hasn't closed yet.
 *
 * This function is also safe against a concurrent engine-wide teardown,
 * whether it comes from chttpsvr_engine_stop() or from the shared reactor
 * shutting down gracefully after the last other server referencing it was
 * destroyed. It retries transparently until that teardown is done rather
 * than racing it.
 *
 * Returns at once; use chttpsvr_engine_wait() to block until the engine
 * exits.
 *
 * Don't call this function from a request handler or middleware of srv to
 * restart the very server the handler runs on (that handler usually calls
 * chttpsvr_stop() first). Such a call is refused with ccol_not_permitted
 * without being tried, and the refusal leaves srv in a safe, recoverable
 * state: the configuration srv had before, or a stopped server if
 * chttpsvr_stop() already ran. A later, legitimate call to this function
 * from another thread restarts it normally.
 *
 * @param srv  Server handle.
 * @param cfg  Startup configuration.  The server uses
 *             CHTTPSVR_CONFIG_DEFAULT when this is NULL.
 * @return ccol_success            The listener is bound and registered.
 *         ccol_invalid_args       srv is CHTTPSVR_INVALID, or a stale handle
 *                                 that something already destroyed; or
 *                                 cfg->tls is set but
 *                                 cert_path and key_path are not both set.
 *                                 A server certificate and its private key
 *                                 are a pair, and neither one alone is ever
 *                                 a valid server-side TLS configuration;
 *                                 nor is ca_bundle_path alone, nor an
 *                                 otherwise empty chttp_tls_config_t such as
 *                                 CHTTP_TLS_DEFAULT; or
 *                                 cfg->streaming_thread_count is negative
 *                                 and not CHTTPSVR_STREAMING_POOL_OFF.
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
 *                                 thread or the thread that serves
 *                                 chttpsvr_engine_stop(), which the server
 *                                 logger reports; or the process already
 *                                 holds PTHREAD_KEYS_MAX thread-specific
 *                                 keys, so the library could not create
 *                                 the one that marks the worker threads of
 *                                 a server (a later call tries again).
 *         ccol_http_tls_cert_load_failed  cfg->tls was set, and the
 *                                 library could not load its certificate
 *                                 and key pair or its ca_bundle_path. An
 *                                 allocation that fails while it loads
 *                                 them gives ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_start(chttpsvr srv, const chttpsvr_config_t *cfg);

/**
 * @brief Close this server's listener socket.
 *
 * Stops accepting new connections on this server's port, but doesn't close
 * connections the server has already accepted: a request already running
 * continues until it completes, and a keep-alive connection stays open and
 * keeps being served, request after request, until its client or the idle
 * timeout closes it, as on a running server.  The engine and every other
 * registered server keep running.
 *
 * Call chttpsvr_destroy(), which waits internally, to close every
 * connection and wait for every in-flight request on this server to drain
 * before the resources are freed.
 *
 * Does nothing, silently, when srv is CHTTPSVR_INVALID or a stale handle
 * that something has already destroyed.
 *
 * You may call it at the same time as a chttpsvr_start() restarting the
 * same handle on another thread: the restart waits for this call's
 * teardown of the old listener to finish completely and then binds a new
 * one, instead of racing it.
 *
 * It is synchronous and NOT async-signal-safe, so don't call it directly
 * from a signal handler; use chttpsvr_engine_stop() there instead. This
 * function takes internal locks that the interrupted thread may already
 * hold, inside chttpsvr_start() or chttpsvr_destroy() for example, so the
 * calling thread can deadlock itself. Use chttpsvr_engine_stop() for a
 * signal-driven shutdown hook: it is documented as async-signal-safe,
 * because it moves all of this work off the interrupted thread.
 */
void chttpsvr_stop(chttpsvr srv);

/** The size, NUL byte included, of a buffer that holds every address that
 *  chttpsvr_bound_address() can write. */
#define CHTTPSVR_BOUND_ADDRESS_MAX 128

/**
 * @brief Report whether the server listens.
 *
 * The server listens from the return of a successful chttpsvr_start() until
 * its stop begins, whether that stop comes from chttpsvr_stop(),
 * chttpsvr_engine_stop() or the destroy. A server that was never started,
 * one whose start failed, and one in the middle of a start or a stop is
 * not listening. The answer is a snapshot: another thread may start or stop
 * the server right after the call.
 *
 * @param srv          Server handle.
 * @param running_out  Receives true when the server listens and false
 *                     otherwise. It receives false on every failure.
 * @return ccol_success       *running_out holds the answer.
 *         ccol_invalid_args  srv is CHTTPSVR_INVALID or a stale handle, or
 *                            running_out is NULL.
 */
ccol_retval_t chttpsvr_is_running(chttpsvr srv, bool *running_out);

/**
 * @brief Report the TCP port that the listener of the server is bound to.
 *
 * This is the port the system actually bound: the one cfg->port named, or,
 * for a cfg->port of 0, the ephemeral port the system picked. The library
 * reads it from the socket (getsockname(2)) when the start binds it, the
 * same way a Go net.Listener's Addr() method and libmicrohttpd's
 * MHD_DAEMON_INFO_BIND_PORT report it.
 *
 * @param srv       Server handle.
 * @param port_out  Receives the port in host byte order. It receives 0 on
 *                  every failure.
 * @return ccol_success        *port_out holds the port.
 *         ccol_invalid_args   srv is CHTTPSVR_INVALID or a stale handle, or
 *                             port_out is NULL.
 *         ccol_not_permitted  The server does not listen (see
 *                             chttpsvr_is_running()), or it listens on a
 *                             "unix://" path, which has no port.
 */
ccol_retval_t chttpsvr_bound_port(chttpsvr srv, uint16_t *port_out);

/**
 * @brief Write the address that the listener of the server is bound to.
 *
 * For a TCP listener it writes the numeric form of the local address the
 * system bound, without brackets or port: "127.0.0.1" for a listener on the
 * IPv4 loopback, "0.0.0.0" or "::" for a listener on every interface, and
 * the name's first address for a host name. For a Unix domain socket it
 * writes the "unix://" form of the path the configuration named, so the
 * text can be used as the host of another configuration. The text ends
 * with a NUL byte.
 *
 * @param srv     Server handle.
 * @param buf     The buffer that receives the text. A buffer of
 *                CHTTPSVR_BOUND_ADDRESS_MAX bytes holds every address.
 * @param buflen  The size of buf in bytes. On every failure where buflen is
 *                not 0, buf receives the empty string.
 * @return ccol_success        buf holds the address.
 *         ccol_invalid_args   srv is CHTTPSVR_INVALID or a stale handle, or
 *                             buf is NULL, or buflen is 0.
 *         ccol_not_permitted  The server does not listen (see
 *                             chttpsvr_is_running()).
 *         ccol_msg_too_large  The address and its NUL byte do not fit in
 *                             buflen bytes.
 */
ccol_retval_t chttpsvr_bound_address(chttpsvr srv, char *buf, size_t buflen);

/* ========================================================================== */
/*                         ROUTE REGISTRATION (SERVER)                        */
/* ========================================================================== */

/**
 * @brief Register a buffered-body route on the server.
 *
 * The server buffers the complete request body, if there is one, before it
 * calls the handler, which runs on a ctpool worker thread and may block.
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
 *         method is not one of the seven methods or CHTTP_ANY, when
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
 * soon as it has parsed the headers, before reading any body byte, and
 * dispatches the handler at once, whatever the body size, to the
 * server's streaming pool.  That pool is separate from the buffered routes'
 * worker pool, so a slow streaming client never holds a thread a buffered
 * route needs.  Use chttpsvr_req_read() inside the handler to pull each
 * body batch as it arrives on the socket: the streaming thread reads it
 * live instead of the server buffering it first.
 *
 * A streaming request that finds every streaming thread busy waits, with no
 * thread, in a queue of chttpsvr_config_t.streaming_queue_capacity entries.
 * The server answers 503 Service Unavailable, with Retry-After, at once
 * when that queue is full, and when a request waits in it longer than
 * chttpsvr_config_t.streaming_queue_timeout_us; the handler never runs for
 * such a request.  With chttpsvr_config_t.streaming_thread_count set to
 * CHTTPSVR_STREAMING_POOL_OFF, the handler runs on the worker pool instead,
 * and the full-worker-queue 503 applies.
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
 *         method is not one of the seven methods or CHTTP_ANY, when
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
 * Middleware runs in registration order.  Every global middleware runs
 * before every sub-router middleware, and before the final handler.
 *
 * Global middleware also runs for a request this server rejects before any
 * handler could run, so a rate limiter, a per-peer ban list or an access
 * log built as middleware sees that traffic instead of the server quietly
 * leaving it out.  Three rejections run the chain before their response
 * goes out: a 404 when no route matched, a 405 when no route has that
 * method, and a 413 when the declared Content-Length is above
 * max_body_size.  For a 404 or 405 of a path under a mounted prefix, the
 * chain is the global one plus that of the sub-router owning the path (see
 * "Route precedence" at the top of this file); for a 404 or 405 of any
 * other path it is the global chain alone.  The server decides a 413 after
 * a route has matched, so a 413 runs that request's full effective chain,
 * sub-router middleware included.  The final handler is never reached in
 * any of these cases, so a chain that runs to its end sends the rejection
 * response as it stands, while a middleware that doesn't call next answers
 * the request itself, just as for a matched route.  Such a request has a
 * method, a path, headers and a query string to read, but no path
 * parameters, since no pattern captured any, and its body is never read,
 * so chttpsvr_req_param() returns NULL and chttpsvr_req_read() returns -1.
 *
 * A response a middleware makes for such a request is sent as an
 * internally generated rejection, so the small unconditional write-duration
 * ceiling described under max_response_write_duration_us applies to it.
 * Keep such a response small, as a rejection response is.
 *
 * Three rejections deliberately run no middleware: a 500, an allocation
 * failure of this server, where application code allocating again is the
 * wrong answer to memory pressure; a 503, where the worker pool is already
 * full, so more application work is exactly what must not start; and a
 * 501 or 505, for a method token the server doesn't recognize, a transfer
 * coding it doesn't implement, or an HTTP version whose major number isn't
 * 1, which the server decides from the request line or the framing headers
 * before routing, so there is no routed request to observe. A rejection the
 * server sends inline, which happens when the server's rejection thread
 * pool is saturated or shutting down, runs no middleware either.
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
 * A route you register on the sub-router only matches when the request path
 * is under prefix.  The sub-router owns every path under prefix unless a
 * more specific mount owns it: a request for such a path reaches this
 * sub-router's routes or gets a 404 or 405 from it, and never reaches a
 * root-level route.  The sub-router's middleware runs after the global
 * middleware and before the final route handler, or before that 404 or
 * 405. See "Route precedence" at the top of this file.
 *
 * A sub-router lives as long as its server, so don't free the pointer this
 * function gives you.  Every chttpsvr_router_on, chttpsvr_router_on_stream
 * and chttpsvr_router_use call on a sub-router first checks that the
 * owning server hasn't been destroyed, a check that also covers a
 * concurrent destroy from another thread, like every other state-changing
 * call in this API that takes a chttpsvr handle directly.  So it is safe to
 * keep a sub-router pointer across a restart with chttpsvr_stop() and
 * chttpsvr_start(), and a registration call racing a concurrent
 * chttpsvr_destroy() of the owning server returns ccol_invalid_args and
 * touches no freed memory. To keep that promise for a pointer that outlives
 * its server, each sub-router keeps a small part of its memory, about 100
 * bytes, until the process exits; destroying the server frees its routes,
 * its middleware and its prefix.
 *
 * IMPORTANT: the special case of the "/" prefix:
 *   No trailing slash is removed from a prefix of "/", because the strip
 *   loop only runs for a prefix longer than one character, so the prefix is
 *   stored as it is, with prefix_len = 1.  A "/" sub-router owns only the
 *   exact request path "/": no other path is under it, and every other
 *   path, one starting with a second slash such as "//foo" included, is
 *   routed as if the "/" sub-router didn't exist.  So every route on a "/"
 *   sub-router can only be reached through the exact path "/".
 *
 *   That makes this prefix a trap for a sub-router with its own middleware:
 *   an authenticator or rate limiter on a "/" sub-router doesn't refuse
 *   everything outside "/", because a root-level route, or another
 *   sub-router, serves any other path normally and the "/" sub-router's
 *   middleware doesn't run for it.
 *
 *   Register middleware globally with chttpsvr_use() when you want it to
 *   run for every request whatever the path, and register routes directly
 *   on the server with chttpsvr_register_handler or
 *   chttpsvr_register_streaming_handler when you want a catch-all that gets
 *   every request. Don't use a sub-router with the prefix "/" for either.
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
 *         NULL, when method is not one of the seven methods or CHTTP_ANY,
 *         when pattern does not start with '/', when pattern has two
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
 *         NULL, when method is not one of the seven methods or CHTTP_ANY,
 *         when pattern does not start with '/', when pattern has two
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
 * It runs after the global middleware and before the sub-router's
 * handlers.
 *
 * It also runs for a request the server routes to this sub-router and then
 * rejects with 413 before its handler could run, and for the 404 or 405 of
 * a path this sub-router owns that none of its routes matches.  See
 * chttpsvr_use for the full rule and for the rejections that run no
 * middleware at all.
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
 * Returns CHTTP_GET when req is NULL: the other request accessors return
 * NULL there, but this function can't report an error through its return
 * type.
 * The result is always one of the seven real chttp_method_t constants,
 * never CHTTP_ANY or any other value, because the server rejects a request
 * whose method it doesn't recognize with 501 before any handler runs, so
 * req always belongs to a request whose method is one of the seven.
 */
chttp_method_t chttpsvr_req_method(const chttpsvr_req *req);

/**
 * @brief Return the URL-decoded request path, which ends with a NUL byte.
 *
 * Percent-encoded sequences are decoded under the path rules of RFC 3986,
 * so the string you get back is the path that was matched against a route
 * pattern.
 *
 * A middleware running for a rejected request (see chttpsvr_use()) may see
 * a path whose percent-encoding is malformed, which is itself one of the
 * reasons no route could match it. Such a path has no decoded form, so this
 * function returns it exactly as it arrived, still encoded, rather than
 * nothing at all: an access log that leaves out the one request most worth
 * recording is worse than one that records exactly what the peer sent.
 *
 * The path always begins with "/". A request whose target is neither an
 * origin-form path nor an absolute-form http or https URL is answered with
 * 400 Bad Request before any middleware runs. The one exception is
 * "OPTIONS *", which the server answers itself with 200 and a
 * Content-Length of 0, just as Go's net/http server does, with no
 * middleware and no handler; up to 4 KiB of its body are read and
 * discarded, and a larger body closes the connection after the 200. Any
 * other method with the target "*" gets 400 and a close. For an
 * absolute-form target the path is that URL's path, and an empty one is
 * "/".
 *
 * The pointer stays valid for the lifetime of the request.
 */
const char *chttpsvr_req_path(const chttpsvr_req *req);

/**
 * @brief Report whether the client of this request presented a TLS
 *        certificate that verified against the ca_bundle_path of the server.
 *
 * True only when the connection is TLS, the client presented a certificate
 * during the handshake, and that certificate verified; false for a
 * plaintext connection, for a server with no ca_bundle_path, for a client
 * that presented no certificate, and when req is NULL.
 *
 * With the default configuration, a server with a ca_bundle_path accepts no
 * client without such a certificate, so every request there reports true.
 * With client_cert_optional set (chttp_tls_config_t) a client that presents
 * nothing is accepted too, and this function is how a handler or middleware
 * tells it apart from a verified one. A client whose certificate doesn't
 * verify never reaches a handler in either mode.
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
 * You get the certificate only when chttpsvr_req_peer_cert_verified()
 * reports true for req: the leaf certificate the client presented, which
 * verified against the server's ca_bundle_path. It returns NULL, and 0 in
 * *len_out, in every case where that function reports false, and also when
 * an allocation fails; if you need to tell the two apart, ask
 * chttpsvr_req_peer_cert_verified().
 *
 * Use it to read any field of the certificate with the library of your
 * choice, such as OpenSSL's d2i_X509(), or to compare the whole
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
 * chttpsvr_req_peer_cert_der() returns, the same value
 * `openssl x509 -noout -fingerprint -sha256` prints. It identifies exactly
 * one certificate, so a handler can compare it with a list of allowed
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
 * The name lists its attributes from most to least specific, for example
 * "CN=alice,OU=staff,O=Example". Every byte above 0x7F, every control
 * character and every character RFC 2253 reserves is escaped with a
 * backslash, so the string is printable ASCII and a name a client crafted
 * can't pass itself off as another. Compare it as a whole string: a
 * substring of a subject may come from any attribute.
 *
 * You get the subject only when chttpsvr_req_peer_cert_verified() reports
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
 * Lifetime: the pointer is valid only for the duration of the handler call,
 * so do NOT keep it after the handler returns. It points into the copied
 * header array the request context owns, which the server frees when it
 * tears the request down.
 *
 * The trailer fields of a chunked request body (RFC 7230 SS4.1.2) are NOT
 * request headers, and this function never returns one: it only answers
 * from the request's header block, so a trailer field of any name reads as
 * absent. Trailers arrive after the server has routed the request and the
 * middleware has run, and on a buffered route also after the header block
 * was read. Indexing trailers next to headers would let a client add a
 * trailer that displaces a header an upstream proxy set (such as
 * X-Forwarded-For, or an authentication gateway's identity header) on a
 * request the server has already committed to. This server has no trailer
 * accessor at all; a deployment that needs trailer semantics should
 * terminate the trailers in front of this server.
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
 * Reads up to buflen bytes of the request body into buf, batch by batch, as
 * they arrive on the connection.  This call reads the socket itself, on the
 * streaming thread running the handler, never on the reactor thread, and
 * it blocks that thread until one of these
 * happens: at least one byte is available, the body ends, an error happens,
 * stream_read_timeout_us (chttpsvr_config_t) expires with no new data, the
 * request's total body read time goes above max_body_read_duration_us
 * (chttpsvr_config_t) if you set that field, or the body's average rate
 * since the first call falls below min_transfer_rate_bps
 * (chttpsvr_config_t) once min_transfer_rate_grace_us has passed. Each of
 * the last three makes chttpsvr_req_stream_error() give ccol_timed_out.
 *
 * Returns 0 at once when buflen is 0, whatever buf holds, and the call
 * then does nothing, matching the rules of POSIX read(2).
 *
 * It returns -1 for two groups of causes. The first is caller misuse: req
 * is NULL, buf is NULL while buflen is above 0, or the handler was
 * registered with chttpsvr_register_handler instead of
 * chttpsvr_register_streaming_handler. The second is a real, data-dependent
 * transfer error: a broken connection, an expired stream_read_timeout_us or
 * max_body_read_duration_us, or a body that goes above max_body_size
 * mid-stream. Call chttpsvr_req_stream_error() right after this function to
 * tell the second group apart; it also reports a NULL or invalid req. It
 * gives ccol_success, not a separate code, for the NULL-buf misuse and for
 * a non-streaming route, because both are static programming errors: you
 * can only make them by breaking this function's documented preconditions,
 * and they happen on every single call of the faulty code, not sometimes,
 * so they need no diagnosis at run time.
 *
 * When the request carried "Expect: 100-continue" and really has a body to
 * receive (a Content-Length or a chunked Transfer-Encoding), the first call
 * to this function for that request writes the interim "100 Continue"
 * response before trying to read anything, telling the client to go ahead
 * and upload the body. A streaming handler can instead reject a request
 * outright, for bad authentication or an unacceptable Content-Type for
 * example, by writing a final response and never calling this function.
 * The server then skips the interim response entirely, so the client sees
 * the real rejection directly, exactly as when it never sends "Expect:
 * 100-continue", and isn't asked to upload a body the server was never
 * going to read. A request that carries "Expect: 100-continue" but has no
 * body at all gets no interim response either, for the same reason: there
 * is nothing to invite the client to upload.
 *
 * @param req     Request handle, which must come from a route that
 *                chttpsvr_register_streaming_handler registered.
 * @param buf     The destination buffer, which may be NULL only when buflen
 *                is 0.
 * @param buflen  The buffer capacity in bytes.  A value of 0 is valid, and
 *                the call then does nothing.
 * A call returns the body bytes the connection has on hand, up to buflen,
 * and only waits when it has none. It reads the socket at most once for
 * that, and one read brings at most 8 KiB of body, so a call may return
 * far fewer bytes than buflen while more of the body is still on its way.
 * As with read(2), only a return of 0 means the body has ended.
 *
 * @return The number of bytes that it read, which is above 0; 0 at EOF and
 *         for a buflen of 0; or -1 on an error.
 */
ssize_t chttpsvr_req_read(chttpsvr_req *req, void *buf, size_t buflen);

/**
 * @brief Report why the last chttpsvr_req_read() call returned -1, covering
 *        only the causes that need a diagnosis at run time.
 *
 * Meaningful only right after chttpsvr_req_read() returns -1; at any other
 * time the result is unspecified, since there may be no error to report.
 *
 * It only distinguishes chttpsvr_req_read()'s real, data-dependent transfer
 * errors (see that function's doc comment). When caller misuse caused the
 * -1 instead, it returns ccol_success, which you can't tell apart from "no
 * error at all". Two kinds of misuse do that: buf was NULL while buflen was
 * above 0, or the request's route wasn't registered with
 * chttpsvr_register_streaming_handler. Both are static programming errors
 * that happen on every call of the faulty code, not just sometimes, so a
 * caller that hits one is expected to find and fix it in ordinary testing
 * rather than diagnose it at run time.
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
 *         of the misuse cases above caused the -1. ccol_invalid_args means
 *         that req is NULL, or that it names no request.
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
 * Every key=value pair whose key or value has a malformed percent-escape,
 * such as a=%zz, or an escape that decodes to a NUL byte, such as a=%00,
 * is left out as if the query string didn't hold it. Read
 * chttpsvr_req_raw_query() to see such a pair.
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
 * NOTE: this function can't tell an out-of-memory failure, which may
 * happen while the query string is parsed or the result array grows, apart
 * from a real "key is absent" result: both give NULL and a count of 0.
 * Call chttpsvr_req_query_oom() after a NULL return to tell the two apart,
 * or use chttpsvr_req_query_one for a key that has one value.
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
 * @return ccol_success              The function found exactly one value
 *                                   and set *val_out.
 *         ccol_key_not_found        The key is absent.
 *         ccol_key_already_present  The key has more than one value.
 *         ccol_not_enough_memory    The library could not allocate the
 *                                   parse buffer for the query string.
 *         ccol_invalid_args         req or key is NULL.
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
 * Two allocations can fail this way: growing the result array inside
 * chttpsvr_req_query, and the one-time, shared parse of this request's
 * query string, which chttpsvr_req_query and chttpsvr_req_query_one each
 * start lazily on first use. chttpsvr_req_raw_query never starts that
 * parse, since it returns the raw string taken from the request line, so
 * it has no effect on this flag.
 *
 * chttpsvr_req_query gives NULL and a count of 0 in two cases: the key is
 * absent, or one of the two allocations above ran out of memory. Call this
 * function after any NULL return from chttpsvr_req_query to tell the two
 * apart.
 *
 * The flag latches: once set, it stays true for the lifetime of the
 * request, whatever query calls follow.
 *
 * Returns false when req is NULL.
 */
bool chttpsvr_req_query_oom(const chttpsvr_req *req);

/**
 * @brief Attach a value to the request under a key, for a later middleware
 *        or the handler to read with chttpsvr_req_context().
 *
 * A middleware that authenticates a request, or gives it a trace
 * identifier, stores what it found here, and every later step of the chain
 * reads it back. Each request has its own set of values, starting empty,
 * so nothing one request stores reaches another request, even on the same
 * connection.
 *
 * Keys are compared by address only, just as Go's context.WithValue
 * compares keys by identity. The usual key is the address of a static
 * object in the module that stores the value, such as
 * "static const char auth_key;" and "&auth_key", so two independent
 * middleware never share a key by accident. A second call with a key the
 * request already holds replaces its value; a NULL value is stored like
 * any other.
 *
 * The values stay attached until the request's chain returns: for a
 * matched route that is when the handler and every middleware around it
 * return, and for a rejected request that runs its middleware (see
 * chttpsvr_use()) it is when the outermost middleware returns. The library
 * then forgets them. It never frees, dereferences or copies a value, so a
 * value pointing to memory a middleware allocated must be freed by that
 * middleware, for example after its call to next() returns.
 *
 * One thread owns a request at a time: the one running the chain. Call
 * this function and chttpsvr_req_context() only on that thread while the
 * chain runs; neither function takes a lock.
 *
 * The first value costs no allocation. Further values use the server's
 * allocator, and their storage is released when the chain returns.
 *
 * @param req    The request.
 * @param key    The key, compared by address. It must not be NULL.
 * @param value  The value to attach, which may be NULL.
 * @return ccol_success            The value is attached.
 *         ccol_invalid_args       req or key is NULL.
 *         ccol_not_enough_memory  The storage for the value could not be
 *                                 allocated; the request keeps the values
 *                                 that it had.
 */
ccol_retval_t chttpsvr_req_set_context(chttpsvr_req *req, const void *key,
                                       void *value);

/**
 * @brief Return the value that chttpsvr_req_set_context() attached to the
 *        request under key.
 *
 * @param req  The request.
 * @param key  The key, compared by address.
 * @return The value attached under key, or NULL when req or key is NULL or
 *         no value is attached under key. A stored NULL value also gives
 *         NULL.
 */
void *chttpsvr_req_context(const chttpsvr_req *req, const void *key);

/* ========================================================================== */
/*                         RESPONSE API                                       */
/* ========================================================================== */

/**
 * @brief Set the status code of the HTTP response (the default is 200).
 *
 * status_code is stored exactly as you give it and nothing is checked here,
 * but when the response is sent, a value outside the range of final status
 * codes, 200 to 999, silently goes on the wire as 500 instead. A 1xx falls
 * in that case: it is an interim status (RFC 9110 SS15.2), so a client
 * that got it as the response would keep waiting for a final one. The
 * server writes the interim "100 Continue" itself.
 *
 * A status_code of 204, 205 or 304 drops whatever body
 * chttpsvr_resp_write, chttpsvr_resp_write_str, chttpsvr_resp_printf or
 * chttpsvr_resp_write_json has already written or writes later, in any
 * order of calls. See the doc comment of chttpsvr_resp_write.
 *
 * The status line carries the reason phrase the IANA HTTP Status Code
 * Registry gives the code, such as "Reset Content" for 205, or "Unknown"
 * for a code the registry doesn't list. Clients ignore the phrase.
 */
void chttpsvr_resp_set_status(chttpsvr_resp *resp, int status_code);

/**
 * @brief Set a response header, replacing every field of the same name.
 *
 * If fields with the same name already exist, the first one takes the new
 * value in place and keeps its position and the others are dropped, so the
 * response carries exactly one field of that name. The name comparison
 * ignores case, as RFC 7230 requires.  The stored header name keeps the
 * letter case of the FIRST call, and the letter case a later call gives
 * for the same name is ignored; HTTP/1.1 header names are case-insensitive,
 * so this doesn't change what goes on the wire. Use
 * chttpsvr_resp_add_header() instead to send one more field of a name,
 * such as a second Set-Cookie.
 *
 * Fields go on the wire in the order of the calls that first set each of
 * them.
 *
 * The server adds no Content-Type of its own: a response only carries one
 * when the handler sets it, with this function or with
 * chttpsvr_resp_write_json(). The server doesn't guess one from the body as
 * Go's net/http server does, and has no default type like nginx's
 * default_type.
 *
 * None of the three arguments may be NULL: a NULL in any of them gives
 * ccol_invalid_args and changes nothing in the response. name must also be
 * a non-empty token of tchar bytes only, as RFC 7230 SS3.2.6 defines them,
 * so every byte must be an ASCII letter, an ASCII digit, or one of
 * "!#$%&'*+-.^_`|~". A zero-length name has no valid wire form. A name with
 * any other byte in it, a space or a literal ':' for example, can't inject
 * a CRLF, but it makes a structurally malformed wire line that a
 * strict parser downstream may misread: the name "X Foo: bar", for
 * example, puts "X Foo:bar:baz\r\n" on the wire, which is not a real split
 * into two fields.
 *
 * value must hold no control byte: no byte below 0x20 other than the
 * horizontal tab, and no 0x7F (DEL). RFC 9110 SS5.5 allows only visible
 * characters, the space, the tab and bytes 0x80 to 0xFF in a field value,
 * and a client that follows it, chttpclient included, refuses a response
 * carrying any other byte. Name and value go on the wire exactly as they
 * are, with no further escaping, so a CR or LF inside value would also let
 * whoever controls data the caller reflects (a query parameter, a path
 * parameter or an echoed request header put into a response header)
 * inject extra header lines or split the response in two. Such a byte is
 * refused with ccol_invalid_args rather than silently stripped or the
 * value truncated.
 *
 * "Connection" is accepted, and its name and value are checked and stored
 * like any other header, but the server never sends it: it always decides
 * and emits its own Connection header, which shows whether the server
 * really keeps the connection open afterwards.
 *
 * "Content-Length" must be one or more ASCII digits whose number fits a
 * signed 64-bit integer; any other value gives ccol_invalid_args. Whether
 * the server sends it depends on the response:
 *
 * - A response that sends a body carries the length of the body that
 *   chttpsvr_resp_write, chttpsvr_resp_write_str, chttpsvr_resp_printf or
 *   chttpsvr_resp_write_json really wrote, never the value you set, because
 *   that length is the response's framing on the wire, and a value that
 *   disagrees with it would desynchronize a kept-alive connection for
 *   whichever request comes next.
 * - A 304, and a HEAD response for which the handler wrote no body, send
 *   no body whatever the field says, and carry the Content-Length you set,
 *   which is the length a 200 to a GET would carry (RFC 9110 SS8.6), or
 *   none at all if you set none.
 * - A 205 always carries "content-length:0", whatever you set or wrote,
 *   since it has no content (RFC 9110 SS15.3.6).
 * - A 204 never carries one.
 *
 * This is how Go's net/http server behaves.
 *
 * A Date the caller sets is sent as given and replaces the Date the server
 * would otherwise add, so the response carries exactly one. Without one
 * from the caller, every final response carries the current UTC time in
 * the IMF-fixdate form of RFC 9110 SS5.6.7, such as "Sun, 06 Nov 1994
 * 08:49:37 GMT", while the interim "100 Continue" carries none. That's Go's
 * net/http server's rule. A repeated call with the same name in any letter
 * case, through this function or chttpsvr_resp_add_header(), replaces the
 * value, so no call sequence can produce two Date lines.
 *
 * "Transfer-Encoding" is refused outright with ccol_invalid_args. This
 * server never transfer-codes a response body, so it can't obey a
 * Transfer-Encoding header a caller sets, and such a header on the wire
 * would also sit next to the computed Content-Length header, over a body
 * nothing transfer-coded. That framing is ambiguous: an intermediary that
 * prefers Transfer-Encoding over Content-Length (RFC 7230 SS3.3.3) may
 * parse it wrongly, the same kind of response-splitting hazard the
 * handling of Connection and Content-Length above prevents.
 * chttp_request_set_header refuses Transfer-Encoding the same way on the
 * client side (chttpclient.h).
 *
 * @param resp   Response handle. It must not be NULL.
 * @param name   The header name, which must not be NULL or empty, must hold
 *               only RFC 7230 tchar bytes, and must not be
 *               "Transfer-Encoding".
 * @param value  The header value, which must not be NULL and must hold no
 *               control byte other than the tab.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory. The
 *         library gives ccol_invalid_args when resp, name or value is NULL,
 *         when name is empty, when name holds a byte outside the RFC 7230
 *         tchar set, when value holds a byte below 0x20 other than the tab,
 *         or the byte 0x7F, when name is
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
 * Unlike chttpsvr_resp_set_header(), this keeps existing fields of the same
 * name, so a response can carry, say, two Set-Cookie lines; each call puts
 * one "name:value" line on the wire, in call order.
 *
 * It checks name and value exactly as chttpsvr_resp_set_header() does,
 * with the same results. Three names carry at most one field in any
 * response ("Connection", "Content-Length" and "Date"), and for those three
 * this function does what chttpsvr_resp_set_header() does and replaces the
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
 * Several calls add to the same body, and the server flushes the complete
 * buffer after the handler returns.
 *
 * However much you add here, the collected body is silently never written
 * to the wire when the response's final status code (the one the last
 * chttpsvr_resp_set_status call set by the time the handler returns) is a
 * 204, 205 or 304, because RFC 9110 SS6.4.1, SS15.3.6 and SS15.4.5 forbid
 * these codes from ever carrying a body. This function itself returns
 * ccol_success for an append that really worked, and nothing in this API
 * reports an error for a body dropped this way. For example, if a handler
 * writes diagnostic or informational body content and a cache-validation
 * middleware then lowers its status to one of these codes, that content
 * never reaches the client and no error signal anywhere catches it. A 205
 * carries "content-length:0", a 204 carries no Content-Length, and a 304
 * carries the Content-Length the handler set with
 * chttpsvr_resp_set_header(), and none otherwise; see that function.
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
 * The body is dropped for a 204, a 205 and a 304 here exactly the same
 * way; see the doc comment of chttpsvr_resp_write.
 */
ccol_retval_t chttpsvr_resp_write_str(chttpsvr_resp *resp, const char *str);

/**
 * @brief Format and append a printf-style string to the response body.
 *
 * The same as formatting `fmt` and `...` under the printf rules and then
 * calling chttpsvr_resp_write_str() with the result, with no allocation in
 * between that the caller can see. The body is dropped for a 204, a 205
 * and a 304 here exactly the same way; see the doc comment of
 * chttpsvr_resp_write.
 *
 * @param resp  Response handle, which must not be NULL.
 * @param fmt   The format string, in the printf style, which must not be
 *              NULL.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory. The
 *         library gives ccol_invalid_args when resp or fmt is NULL, and
 *         when the vsnprintf encoding under it fails.
 */
ccol_retval_t chttpsvr_resp_printf(chttpsvr_resp *resp, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * @brief Append a JSON body and set Content-Type to application/json.
 *
 * Appends len bytes of json to the response body, then sets the
 * content-type response header to "application/json".  The body is written
 * first, so the response doesn't change at all if the body allocation runs
 * out of memory: no header is set and no bytes are buffered.  If the body
 * write works and the header set after it then runs out of memory, the
 * body bytes are already in the buffer, so you get ccol_not_enough_memory
 * with a partly written response body. That case is unlikely and is only
 * mentioned here for completeness.
 *
 * Gives ccol_invalid_args when resp or json is NULL, and when len is 0: a
 * zero-length JSON body with the Content-Type header set means nothing
 * valid, so it is refused rather than silently setting the header with no
 * body bytes.
 *
 * The body is dropped for a 204, a 205 and a 304 here exactly the same
 * way; see the doc comment of chttpsvr_resp_write.
 *
 * @param resp  Response handle.
 * @param json  The JSON data, which must not be NULL.
 * @param len   The length of json in bytes, which must be above 0.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory.
 */
ccol_retval_t chttpsvr_resp_write_json(chttpsvr_resp *resp, const char *json,
                                       size_t len);

#pragma GCC visibility pop
