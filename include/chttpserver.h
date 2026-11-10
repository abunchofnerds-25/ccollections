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

// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal.
#pragma GCC visibility push(default)

// An HTTP/1.1 server on a shared ceventloop reactor, with routes, path
// parameters, middleware, sub-routers, streaming bodies and optional TLS. The
// first chttpsvr_start() brings up the shared reactor (the engine) and
// destroying the last running server takes it down, so you rarely touch the
// engine yourself. Several servers can run at once, each with its own port,
// routes, pools and logger. The usual shape is chttpsvr_construct(),
// chttpsvr_register_handler(), chttpsvr_start(srv, &cfg),
// chttpsvr_engine_wait() and chttpsvr_destroy().
//
// The server routes a request as soon as its headers are parsed, before it
// reads a body byte, so the reactor thread never runs your code and never waits
// for a slow body. A buffered route runs on the server's worker pool once its
// whole body is in memory; a body or a response that stalls parks its
// connection with no thread, and any free worker picks it up again later. A
// streaming route runs on a separate streaming pool and pulls its body with
// chttpsvr_req_read(). Handlers and middleware may block. A handler counts as a
// thread of its own server: chttpsvr_destroy(), a restart and
// chttpsvr_engine_wait() are refused or fatal there, while
// chttpsvr_engine_stop() is safe.
//
// The server refuses a malformed request before any route sees it and closes
// the connection after the answer: 505 for a version other than HTTP/1.x, 400
// for a bad Host, a line above 8 KiB, more than 100 header lines or a header
// block above max_header_bytes, and 501 for a transfer coding other than
// chunked.
//
// Routers are tried from the most specific mount prefix to the least, and the
// root router last. A mount owns every path under its prefix (judged on the
// decoded path): its routes answer, or it answers 404 or 405 itself, and its
// middleware runs either way, so a guard on a mount covers all of its paths
// (a nested mount owns its own paths and needs its own guard).
// Inside one router, routes are tried in registration order. A CHTTP_GET route
// also serves HEAD. Paths and {param} values are decoded and can hold '/' (from
// %2F) or be "..", so never use them as file names unchecked. Each router holds
// at most 32 middleware, and a request whose global plus sub-router chain is
// longer than 32 gets 500.
//
// Point chttpsvr_config.tls at a chttp_tls_config to serve HTTPS; a
// ca_bundle_path there turns on mutual TLS. The man pages (chttpserver(7) and
// one page per function) hold the full contract.

// The opaque server struct. You only ever hold a chttpsvr handle to it.
typedef struct chttpserver chttpserver;

// A server handle. It's a value (a slot index and a generation packed into 64
// bits), not a pointer, so the library detects a stale handle instead of
// touching freed memory. Compare it with CHTTPSVR_NULL and never cast it to a
// pointer.
typedef uint64_t chttpsvr;

// The "no server" value, which a failed create returns and a destroy leaves
// behind. It's 0, so if (!srv) works.
#define CHTTPSVR_NULL ((chttpsvr)0)

// A sub-router that groups routes and middleware under a path prefix. The
// server owns it.
typedef struct chttpsvr_router chttpsvr_router;

// The request that a handler or middleware reads through the chttpsvr_req_
// functions. It's valid only while the chain for that request runs.
typedef struct chttpsvr_req chttpsvr_req;

// The response that a handler fills through the chttpsvr_resp_ functions; the
// server sends it after the handler returns.
typedef struct chttpsvr_resp chttpsvr_resp;

// A request handler, which is how your code answers a matched route. It runs on
// a thread of the server's worker pool (buffered route) or streaming pool
// (streaming route) and may block. req and resp are valid only during the call,
// and ctx is the pointer you gave at registration.
typedef void (*chttpsvr_handler_fn)(chttpsvr_req *req, chttpsvr_resp *resp,
                                    void *ctx);

// Hands the request to the next middleware, or to the final handler when none
// is left. Each middleware gets one, and a middleware that doesn't call it
// stops the chain there. Call it at most once per middleware call, because a
// second call moves the chain on once more.
typedef void (*chttpsvr_next_fn)(chttpsvr_req *req, chttpsvr_resp *resp);

// A middleware. It runs before the handler on the same pool thread and may
// block, and global middleware runs before sub-router middleware. ctx is the
// pointer you passed to chttpsvr_use() or chttpsvr_router_use(), and next hands
// the request on.
typedef void (*chttpsvr_middleware_fn)(chttpsvr_req *req, chttpsvr_resp *resp,
                                       void *ctx, chttpsvr_next_fn next);

// A worker_queue_capacity or streaming_queue_capacity for a queue with no
// limit, where an overflow never gives 503. Such a queue grows without bound
// under a long overload, so a finite one is usually the better choice.
#define CHTTPSVR_QUEUE_UNBOUNDED ((size_t)-1)

// A min_transfer_rate_bps that turns the minimum transfer rate off, so only the
// gap and total limits bound a slow client.
#define CHTTPSVR_NO_RATE_FLOOR ((unsigned)-1)

// A max_partial_body_memory that puts no limit on the memory buffered bodies
// hold.
#define CHTTPSVR_NO_MEMORY_CAP ((size_t)-1)

// A body_memory_wait_timeout_us or streaming_queue_timeout_us that lets a
// request wait with no deadline.
#define CHTTPSVR_NO_DEADLINE UINT64_MAX

// A streaming_thread_count that runs streaming handlers on the worker pool,
// with no pool of their own.
#define CHTTPSVR_STREAMING_POOL_OFF (-1)

// The configuration that chttpsvr_start() takes. Start from
// CHTTPSVR_CONFIG_DEFAULT and change the fields you need. Every _us field
// counts microseconds. The server rounds it up to whole milliseconds and
// saturates a value too large to keep at its longest finite limit (at least 24
// days), so only CHTTPSVR_NO_DEADLINE means no limit.
typedef struct chttpsvr_config {
  // Bind address, with the rules of Go's listener. NULL or "" listens on every
  // interface through one dual-stack [::] socket (0.0.0.0 on a host without
  // IPv6). A literal address listens on that address alone, and an address in
  // brackets means the address inside. A host name listens on its first IPv4
  // address, else its first IPv6 address (the other way round with ipv6_only),
  // and on that one only: a failed lookup or bind fails the start.
  // "unix://path" binds a Unix domain socket instead and ignores port. A stale
  // socket file there is replaced, but any other file, or a socket that a live
  // server listens on, fails the start, and stop and destroy remove only the
  // socket file this server created. One server listens on TCP or on a Unix
  // socket, never both.
  const char *host;
  // Port to listen on (default 8080). 0 asks the system for an ephemeral port,
  // picked again at every start; chttpsvr_bound_port() reports it.
  uint16_t port;
  // Largest request body in bytes (default 4 MiB); 0 means no limit. A buffered
  // route above it gets 413 before the handler runs, and a streaming read fails
  // with ccol_msg_too_large. Either way the connection closes after the
  // response.
  size_t max_body_size;
  // Idle limit for a connection waiting for the headers of its next request
  // (default 60 s); 0 turns it off. It bounds the gap between bytes, not a peer
  // that sends its headers slowly for ever (max_header_read_duration_us does
  // that). Turn it off only when something in front of the server already
  // bounds idle connections.
  uint64_t read_timeout_us;
  // Keep-alive idle limit (default 60 s). 0 uses read_timeout_us, and any other
  // value replaces it as the idle threshold.
  uint64_t idle_timeout_us;
  // Longest gap between two batches of body bytes, buffered or streaming
  // (default 30 s); 0 means no limit. When it passes, a buffered route answers
  // 408 and chttpsvr_req_read() fails with ccol_timed_out. A 0 here or in
  // max_body_read_duration_us doesn't make chttpsvr_destroy() or
  // chttpsvr_engine_stop() wait for ever: the shutdown force-closes such a
  // connection once its graceful wait is over.
  uint64_t stream_read_timeout_us;
  // Total time one request body may take to arrive (default 5 minutes); 0 means
  // no limit. It catches a client that sends a byte just before each
  // stream_read_timeout_us gap runs out, and fails the same way.
  uint64_t max_body_read_duration_us;
  // Longest gap without progress while a response goes to a slow reader; 0 (the
  // default) uses stream_read_timeout_us. Progress is a write the kernel
  // accepts or a shrinking send queue, which the sweep checks about once a
  // second, so a steady reader never hits it and the limit is only that
  // precise.
  uint64_t response_write_timeout_us;
  // Total time one response may take to send (default 5 minutes); 0 means no
  // limit. On expiry the connection closes and the client keeps what it has
  // read. Rejections that the server writes itself and the interim "100
  // Continue" always have a 2-second ceiling of their own, whatever this says.
  uint64_t max_response_write_duration_us;
  // Total time for the TLS handshake plus the header block of one request
  // (default 30 s); 0 means no limit. It bounds a peer that sends its headers
  // one byte at a time and never finishes. The budget is per request and starts
  // at its first byte (or handshake step), so idle time between keep-alive
  // requests isn't charged. An expired connection closes with no response, up
  // to a second late because the sweep checks it.
  uint64_t max_header_read_duration_us;
  // Largest header block in bytes, request line included; 0 means the built-in
  // 64 KiB. A larger block gets 400 and a close. Each line is also limited to 8
  // KiB and a request to 100 header lines, chunked trailers included.
  size_t max_header_bytes;
  // Most connections open at once on this listener (default 16384); 0 means no
  // limit. At the limit the server stops calling accept(2), new connections
  // wait in the kernel backlog, and accepting resumes within about a second of
  // a slot freeing.
  size_t max_connections;
  // TLS configuration, or NULL for plain HTTP.
  const chttp_tls_config *tls;
  // Threads in the server's worker pool; 0 (the default) means one per CPU.
  int worker_thread_count;
  // Capacity of the worker task queue: 0 means 1024 per worker thread,
  // CHTTPSVR_QUEUE_UNBOUNDED means no limit, and any other value is the exact
  // limit. A request that finds the queue full gets 503.
  size_t worker_queue_capacity;
  // Turns SO_KEEPALIVE on for accepted TCP connections (default off), so the OS
  // finds a peer that became unreachable. No effect on a Unix socket.
  bool enable_keepalive;
  // Sets SO_REUSEPORT on the listener (default off), so several processes or
  // servers can share the host and port while the kernel spreads connections
  // between them. No effect on a Unix socket or with port 0.
  bool enable_reuseport;
  // Sets IPV6_V6ONLY on an IPv6 listener (default off). Off gives a dual-stack
  // listener for a NULL or empty host and the OS default for an explicit IPv6
  // address; on refuses IPv4 traffic.
  bool ipv6_only;
  // Lowest average rate, in bytes per second, at which a client must send its
  // body and read its response; 0 means the default of 240 and
  // CHTTPSVR_NO_RATE_FLOOR turns it off. It's judged over the whole phase once
  // min_transfer_rate_grace_us has passed. A slow body gets 408, a slow
  // response is cut off, and a streaming read fails with ccol_timed_out. Time
  // the server itself holds a request back is never charged to the client.
  unsigned min_transfer_rate_bps;
  // How long a body read or a response write runs before min_transfer_rate_bps
  // applies; 0 means the default of 5 s.
  uint64_t min_transfer_rate_grace_us;
  // Most memory, in bytes, that the buffered request bodies of this server hold
  // together, from the first byte read until the handler returns; 0 means 256
  // MiB and CHTTPSVR_NO_MEMORY_CAP turns it off. A body reserves its buffer as
  // its bytes arrive (a first buffer of at most 16 KiB, then each doubling, up
  // to the declared length), so a client that declares much and sends little
  // holds little. A request whose next buffer doesn't fit waits with no thread,
  // in arrival order, and gets no "100 Continue" meanwhile. When every holder
  // is waiting, the oldest goes ahead alone, so the limit can be passed by less
  // than twice max_body_size. A body that arrives with its headers, and every
  // streaming route, reserves nothing.
  size_t max_partial_body_memory;
  // How long a request may wait for body memory before it gets 503 with
  // Retry-After and a close; 0 means 30 s and CHTTPSVR_NO_DEADLINE means no
  // limit.
  uint64_t body_memory_wait_timeout_us;
  // Threads in the streaming pool, which the server creates on the first
  // streaming request; 0 means the resolved worker_thread_count and
  // CHTTPSVR_STREAMING_POOL_OFF runs streaming handlers on the worker pool. A
  // pool of their own keeps slow streaming clients off the threads of buffered
  // routes.
  int streaming_thread_count;
  // How many streaming requests may wait, holding no thread, for a free
  // streaming thread; 0 means four times the streaming thread count and
  // CHTTPSVR_QUEUE_UNBOUNDED means no limit. A request that finds the queue
  // full gets 503 and a close.
  size_t streaming_queue_capacity;
  // How long a streaming request may wait in that queue before it gets 503 and
  // a close; 0 means 5 s and CHTTPSVR_NO_DEADLINE means no limit.
  uint64_t streaming_queue_timeout_us;
} chttpsvr_config;

// The default configuration as a compound literal: port 8080 on every
// interface, a 4 MiB body limit, no TLS, one worker per CPU and a finite value
// for every timeout and resource limit, so a server started from it is already
// bounded against peers that stall or trickle. The fields from
// min_transfer_rate_bps on read 0 as their default, so a configuration built
// with {0} keeps those limits too.
#define CHTTPSVR_CONFIG_DEFAULT                          \
  ((chttpsvr_config){                                    \
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

// Installs the logger for the engine's own diagnostics (TLS handshake failures,
// bind failures, idle-timeout closes), which is separate from the logger of
// each server. Without one, an engine logs through a FATAL-only stderr logger
// that is silent in practice. It derives a logger from cl with
// component=http-server-engine and closes the one an earlier call derived; you
// keep cl. The derived logger serves the running engine and every later one
// until another call replaces it. CLOG_NULL releases it, which is refused while
// an engine runs: release it after chttpsvr_engine_wait() and before you
// destroy the allocator of cl or close its file descriptor. Gives ccol_success,
// ccol_not_permitted for that refused release, ccol_invalid_args for a closed
// or stale cl (the installed logger stays), or ccol_not_enough_memory.
ccol_retval chttpsvr_set_engine_logger(clog cl);

// Sets the allocator for the shared reactor's own memory, so the engine can use
// the same allocator as the rest of your program; NULL goes back to the
// default. Each server's own memory follows chttpsvr_create_mp() instead. Call
// it only while no engine exists (before the first chttpsvr_start(), or after
// chttpsvr_engine_wait() returns), because a switch of allocator under live
// memory corrupts the heap. Gives ccol_success, ccol_invalid_args when mp has a
// NULL function pointer, or ccol_not_permitted while an engine runs.
ccol_retval chttpsvr_set_engine_mem_mgmt_procs(ccol_memmgmt_procs *mp);

// Sets how many threads the shared reactor uses; 0 means the default of 1. One
// thread polls and runs every dispatch itself; with more, the reactor follows
// the num_reactor_threads rule of ceventloop_create_mp(), one poller plus
// num_threads - 1 dispatch workers. One thread is faster for reused
// connections, plain or TLS. Raise it for TLS traffic where many clients
// connect for one request each, so their handshakes spread over the cores. You
// may call it when chttpsvr_set_engine_mem_mgmt_procs() may be called. Gives
// ccol_success, or ccol_not_permitted while an engine runs.
ccol_retval chttpsvr_set_engine_num_reactor_threads(size_t num_threads);

// Blocks until the threads of the shared engine have exited, and returns at
// once when no engine runs. Use it to keep main() alive until a signal stops
// the engine, and after the last chttpsvr_destroy(), which only starts the
// engine teardown on a reaper thread: wait before you return from main(),
// unload the library or free the engine allocator. A call from a handler or
// middleware of any server is fatal, because that request would wait for a
// drain that waits for it.
void chttpsvr_engine_wait(void);

// Asks the shared engine to stop and returns at once. It's async-signal-safe,
// so a SIGTERM handler can call it, and a handler may call it to shut its own
// server down. Every running server stops taking requests: idle connections
// close, later requests get 503, and responses already running carry
// Connection: close. chttpsvr_engine_wait() waits for the drain, a destroy that
// is draining included. A repeated or overlapping call does nothing more, a
// call while no engine runs does nothing, and a call never reaches an engine
// started after it. The library installs no signal handler of its own.
void chttpsvr_engine_stop(void);

// Creates a server that allocates through mprocs (NULL for malloc and free). It
// stays idle until chttpsvr_start(), and routes and middleware can be added
// before or after that. The server logs through a logger derived from cl with
// component=http-server, or a FATAL-only stderr logger for CLOG_NULL, and
// closes it at destroy; you keep cl. Gives the handle, or CHTTPSVR_NULL with a
// static string in *err_str (when err_str isn't NULL). A closed or stale cl,
// and a call after the library's exit-time cleanup has run, fail this way.
chttpsvr chttpsvr_create_mp(ccol_memmgmt_procs *mprocs, clog cl,
                            char **err_str);

// chttpsvr_create_mp() with the default allocator, for the common case.
static inline __attribute__((always_inline)) chttpsvr
chttpsvr_create(clog cl, char **err_str) {
  return chttpsvr_create_mp(NULL, cl, err_str);
}

// Internal: destroys the server; it's the body of chttpsvr_destroy(), which
// also clears your handle. Use the macro instead.
void chttpsvr__destroy(chttpsvr srv);

// Internal: destroys *pp and sets it to CHTTPSVR_NULL. The _scoped macros
// attach it with ccol__destructor, so the server goes away at the end of its
// scope.
static inline __attribute__((always_inline)) void chttpsvr__destroy_cleanup(
    chttpsvr *pp) {
  if (pp && *pp) {
    chttpsvr__destroy(*pp);
    *pp = CHTTPSVR_NULL;
  }
}

// Destroys the server and sets name to CHTTPSVR_NULL. It stops the listener,
// closes idle connections, answers later requests with 503, waits for the
// running handlers (cutting their connections after 30 s), and frees the
// server's pools, routes and engine reference. For the last server it only
// starts the engine teardown on a reaper thread; call chttpsvr_engine_wait()
// when that must be finished. name must be a modifiable lvalue and is evaluated
// once. CHTTPSVR_NULL does nothing. A stale or forged handle, a second destroy
// (sequential or concurrent) and a call from a handler or middleware of this
// server are fatal errors. Each sub-router keeps about 100 bytes until the
// process exits, so a pointer to it stays safe to pass.
#define chttpsvr_destroy(name) \
  ccol__chttpsvr_destroy_impl( \
      name, ccol__uniq(ccol__chttpsvr_destroy_slot, __COUNTER__))

// Internal: the body of chttpsvr_destroy(). It takes the address of name once
// into slot, which comes from ccol__uniq() so destroy macros can nest without
// -Wshadow warnings.
#define ccol__chttpsvr_destroy_impl(name, slot) \
  do {                                          \
    __typeof__(name) *slot = &(name);           \
    chttpsvr__destroy(*slot);                   \
    *slot = CHTTPSVR_NULL;                      \
  } while (0)

// Declares an uninitialized server handle; create the server with
// chttpsvr_create() later.
#define chttpsvr_declare(name) chttpsvr name

// Declares a server handle set to CHTTPSVR_NULL that is destroyed at the end of
// its scope.
#define chttpsvr_declare_scoped(name) \
  chttpsvr name ccol__destructor(chttpsvr__destroy_cleanup) = CHTTPSVR_NULL

// These declare name and create a server whose logger derives from cl (or is
// the FATAL-only stderr logger for CLOG_NULL; see chttpsvr_create_mp()), so a
// program that can't run without its server needs no error handling. They stop
// the program with ccol__fatal_err() when the create fails. The _scoped form
// also destroys the server at the end of the scope.
#define chttpsvr_construct(name, cl)                                    \
  chttpsvr name = CHTTPSVR_NULL;                                        \
  do {                                                                  \
    char *chttpsvr__err = NULL;                                         \
    (name) = chttpsvr_create((cl), &chttpsvr__err);                     \
    if (!(name)) {                                                      \
      ccol__fatal_err("chttpsvr_construct('%s'): %s", #name,            \
                      chttpsvr__err ? chttpsvr__err : "unknown error"); \
    }                                                                   \
  } while (0)

#define chttpsvr_construct_scoped(name, cl)                                  \
  chttpsvr name ccol__destructor(chttpsvr__destroy_cleanup) = CHTTPSVR_NULL; \
  do {                                                                       \
    char *chttpsvr__err = NULL;                                              \
    (name) = chttpsvr_create((cl), &chttpsvr__err);                          \
    if (!(name)) {                                                           \
      ccol__fatal_err("chttpsvr_construct_scoped('%s'): %s", #name,          \
                      chttpsvr__err ? chttpsvr__err : "unknown error");      \
    }                                                                        \
  } while (0)

// Binds the listener and starts serving, bringing up the shared engine when no
// engine runs. A NULL cfg means CHTTPSVR_CONFIG_DEFAULT. The server creates its
// worker pool here; on a restart (chttpsvr_stop(), then this) the new pools
// replace the old ones at once and requests of the earlier run finish in the
// background. A restart waits for a concurrent stop of the same server, and a
// start retries through a concurrent engine teardown. A connection closed while
// its client may be sending gets a lingering close (up to 2 s) so the client
// reads the response, and no write raises SIGPIPE. Returns at once. Gives
// ccol_success; ccol_invalid_args for a bad handle, a tls without both
// cert_path and key_path, or a negative streaming_thread_count other than
// CHTTPSVR_STREAMING_POOL_OFF; ccol_not_permitted when the server runs or the
// call comes from one of its own handlers or middleware;
// ccol_not_enough_memory; ccol_http_tls_cert_load_failed when the certificate,
// key or CA bundle won't load; or ccol_unexpected_failure when the bind, the
// registration with the reactor, or a thread or thread key creation fails (the
// logs say why).
ccol_retval chttpsvr_start(chttpsvr srv, const chttpsvr_config *cfg);

// Closes the listener, so the server takes no new connections while the ones it
// accepted keep being served; chttpsvr_destroy() is what closes those and
// drains them. The engine and other servers keep running. A NULL or stale
// handle does nothing. It takes internal locks, so it isn't async-signal-safe:
// use chttpsvr_engine_stop() from a signal handler.
void chttpsvr_stop(chttpsvr srv);

// Size of a buffer, NUL byte included, that holds every address
// chttpsvr_bound_address() can write.
#define CHTTPSVR_BOUND_ADDRESS_SIZE 128

// Tells whether the server listens, which it does from a successful
// chttpsvr_start() until a stop begins; a server in the middle of a start or a
// stop doesn't. The answer is a snapshot. Gives ccol_success with the answer in
// *running_out, or ccol_invalid_args (and false) for a bad handle or a NULL
// running_out.
ccol_retval chttpsvr_is_running(chttpsvr srv, bool *running_out);

// Gives the TCP port the listener really bound, which you need when cfg->port
// was 0 and the system picked it. Gives ccol_success with the port in *port_out
// (host byte order), ccol_invalid_args for a bad handle or a NULL port_out, or
// ccol_not_permitted when the server doesn't listen or listens on a Unix
// socket; *port_out is 0 on failure.
ccol_retval chttpsvr_bound_port(chttpsvr srv, uint16_t *port_out);

// Writes the address the listener bound into buf: the numeric address without
// port or brackets for TCP, or "unix://path" for a Unix socket, so the text can
// be the host of another configuration. Gives ccol_success, ccol_invalid_args
// for a bad handle, a NULL buf or a buflen of 0, ccol_not_permitted when the
// server doesn't listen, or ccol_msg_too_large when the text doesn't fit; buf
// holds "" on failure.
ccol_retval chttpsvr_bound_address(chttpsvr srv, char *buf, size_t buflen);

// Registers a buffered route on the root router: the server reads the whole
// body, then runs fn(req, resp, ctx) on a worker thread. method is one of the
// seven methods, or CHTTP_ANY for all of them; a route registered earlier for
// one method wins over a later CHTTP_ANY on the same pattern. pattern starts
// with '/' and may hold {name} parameters of [A-Za-z0-9_], each name once, with
// no double or trailing slash. Gives ccol_success, ccol_not_enough_memory, or
// ccol_invalid_args for a bad handle, method or pattern, or a NULL fn.
ccol_retval chttpsvr_register_handler(chttpsvr srv, chttp_method method,
                                      const char *pattern,
                                      chttpsvr_handler_fn fn, void *ctx);

// Registers a streaming route on the root router: fn runs on the streaming pool
// right after the headers and reads the body with chttpsvr_req_read() as it
// arrives. A request that finds no free streaming thread waits in the streaming
// queue, and gets 503 when the queue is full or its wait runs out. The
// arguments and results are those of chttpsvr_register_handler().
ccol_retval chttpsvr_register_streaming_handler(chttpsvr srv,
                                                chttp_method method,
                                                const char *pattern,
                                                chttpsvr_handler_fn fn,
                                                void *ctx);

// Adds global middleware to the root router. It runs, in registration order,
// before every sub-router middleware and every handler. It also runs for the
// 404, 405 and 413 rejections of the server, so an access log or a rate limiter
// sees them; such a request has no path parameters and no body, and a
// middleware that answers it itself is held to the 2-second write ceiling of
// rejections. A 500, 501, 503 or 505, and a rejection sent inline, run no
// middleware. Gives ccol_success, ccol_not_enough_memory, ccol_invalid_args for
// a bad handle or a NULL fn, or ccol_not_permitted when the chain holds 32
// entries.
ccol_retval chttpsvr_use(chttpsvr srv, chttpsvr_middleware_fn fn, void *ctx);

// Creates a sub-router for prefix, so a group of routes can share a prefix and
// middleware. It owns every path equal to prefix or continuing it with '/'
// (unless a more specific mount does): such a request reaches its routes or
// gets its 404 or 405, never a root-level route, and its middleware runs after
// the global middleware. prefix starts with '/', has no double slash, and loses
// a trailing slash. The prefix "/" owns the exact path "/" only, so it's the
// wrong place for a guard meant for every path; use chttpsvr_use() for that.
// The server owns the sub-router, so don't free it. Its pointer stays safe to
// pass after a restart and after the server is destroyed (calls then give
// ccol_invalid_args), because about 100 bytes of it live until the process
// exits. Gives NULL for a bad handle or prefix, or when memory runs out.
chttpsvr_router *chttpsvr_subrouter(chttpsvr srv, const char *prefix);

// chttpsvr_register_handler() for a sub-router: the route matches prefix plus
// pattern, so "/api/v1" plus "/users/{id}" matches "/api/v1/users/42". It also
// gives ccol_invalid_args when router is NULL or its server is destroyed.
ccol_retval chttpsvr_router_on(chttpsvr_router *router, chttp_method method,
                               const char *pattern, chttpsvr_handler_fn fn,
                               void *ctx);

// chttpsvr_register_streaming_handler() for a sub-router, with the rules of
// chttpsvr_router_on().
ccol_retval chttpsvr_router_on_stream(chttpsvr_router *router,
                                      chttp_method method, const char *pattern,
                                      chttpsvr_handler_fn fn, void *ctx);

// Adds middleware to a sub-router. It runs after the global middleware and
// before the sub-router's handlers, and for the 404, 405 and 413 rejections of
// its paths (see chttpsvr_use()). Gives ccol_success, ccol_not_enough_memory,
// ccol_invalid_args for a NULL router or fn or a destroyed server, or
// ccol_not_permitted when the chain holds 32 entries.
ccol_retval chttpsvr_router_use(chttpsvr_router *router,
                                chttpsvr_middleware_fn fn, void *ctx);

// Gives the request method, always one of the seven real methods, since the
// server answers an unknown one with 501. It gives CHTTP_GET for a NULL req,
// because the return type has no error value.
chttp_method chttpsvr_req_method(const chttpsvr_req *req);

// Gives the decoded request path that the routes matched. It always begins with
// '/', and an absolute-form target gives the path of its URL. A rejected
// request whose percent-encoding is malformed has no decoded form, so a
// middleware gets that path as it arrived. "OPTIONS *" never reaches you: the
// server answers it with 200 itself. The string lives as long as the request.
const char *chttpsvr_req_path(const chttpsvr_req *req);

// Tells whether the client presented a TLS certificate that verified against
// the server's ca_bundle_path. With client_cert_optional a client may present
// none, and this is how a handler tells the two kinds apart. It's false for
// plain HTTP, a server without a bundle, a client without a certificate and a
// NULL req. The answer belongs to the connection, so every request on it gets
// the same one.
bool chttpsvr_req_peer_cert_verified(const chttpsvr_req *req);

// Length in bytes of the fingerprint chttpsvr_req_peer_cert_sha256() writes.
#define CHTTPSVR_PEER_CERT_SHA256_LEN 32

// Gives the DER bytes of the verified client certificate, so a handler can read
// any field of it or compare it with a pinned one; *len_out (when len_out isn't
// NULL) gets the length. Gives NULL and 0 whenever
// chttpsvr_req_peer_cert_verified() is false, and when an allocation fails. The
// bytes live as long as the request; don't free them.
const void *chttpsvr_req_peer_cert_der(const chttpsvr_req *req,
                                       size_t *len_out);

// Writes the SHA-256 fingerprint of the verified client certificate (of its DER
// form, as openssl x509 -fingerprint -sha256 prints it) into out, so a handler
// can check the client against a list. Gives ccol_success, ccol_key_not_found
// when no certificate verified, ccol_not_enough_memory, or ccol_invalid_args
// for a NULL req or out; out is all zeroes on failure.
ccol_retval chttpsvr_req_peer_cert_sha256(
    const chttpsvr_req *req, unsigned char out[CHTTPSVR_PEER_CERT_SHA256_LEN]);

// Gives the subject of the verified client certificate as an RFC 2253 name,
// such as "CN=alice,OU=staff,O=Example", with every non-printable or reserved
// byte escaped; compare it as a whole string. Gives NULL when no certificate
// verified or an allocation fails. The string lives as long as the request.
const char *chttpsvr_req_peer_cert_subject(const chttpsvr_req *req);

// Gives the value of the request header name, compared without case, or NULL
// when it's absent; for a repeated header it gives the last value. Trailers of
// a chunked body are never returned, so a trailer can't displace a header a
// proxy set. The pointer is valid only during the handler call.
const char *chttpsvr_req_header(const chttpsvr_req *req, const char *name);

// Gives the body of a buffered route, with its length in *len_out (when len_out
// isn't NULL). The bytes don't end with a NUL. Gives NULL for an empty body,
// and NULL with a length of 0 on a streaming route, which reads with
// chttpsvr_req_read(). The pointer lives as long as the request.
const void *chttpsvr_req_body(const chttpsvr_req *req, size_t *len_out);

// Reads up to buflen bytes of the body on a streaming route, so a handler can
// process an upload as it arrives. It gives what the connection has on hand (at
// most one 8 KiB socket read) and blocks the calling streaming thread only when
// there's nothing, until data comes, the body ends, an error happens or a limit
// passes (stream_read_timeout_us, max_body_read_duration_us,
// min_transfer_rate_bps). The first call for a request with "Expect:
// 100-continue" and a body sends the interim "100 Continue", so a handler that
// answers without reading never invites the upload. Gives the byte count, 0 at
// the end of the body or for a buflen of 0, or -1. A -1 comes from misuse (a
// NULL req, a NULL buf with buflen above 0, a buffered route) or from a
// transfer error; chttpsvr_req_stream_error() tells which.
ssize_t chttpsvr_req_read(chttpsvr_req *req, void *buf, size_t buflen);

// Tells why the last chttpsvr_req_read() returned -1; it means something only
// right after that call. Gives ccol_timed_out for an expired limit or a rate
// below the floor, ccol_msg_too_large past max_body_size,
// ccol_not_enough_memory when the body buffer couldn't grow,
// ccol_http_transfer_aborted for a closed connection or bad framing,
// ccol_invalid_args for a NULL or unknown req, and ccol_success when nothing
// was recorded. ccol_success is also the answer for a NULL buf and for a
// buffered route, because both are programming errors that every call of the
// faulty code hits.
ccol_retval chttpsvr_req_stream_error(const chttpsvr_req *req);

// Gives the decoded value of the path parameter name (written without braces)
// of the matched route, so /users/{id} on /users/42 gives "42", or NULL when
// the route has no such parameter. The string lives as long as the request.
const char *chttpsvr_req_param(const chttpsvr_req *req, const char *name);

// Gives every value of the query key, so ?foo=1&foo=2 gives {"1", "2"}, as a
// NULL-terminated array with the count in *count_out (when count_out isn't
// NULL), or NULL when the key is absent. key is compared with each decoded key,
// as Go's url.Values does, and a pair with a malformed escape or an escaped NUL
// is skipped. The strings live as long as the request, but the next call on the
// same request reuses the array. NULL also comes from a failed allocation;
// chttpsvr_req_query_oom() tells the two apart.
const char **chttpsvr_req_query(chttpsvr_req *req, const char *key,
                                size_t *count_out);

// Gives the single value of the query key in *val_out, for a key that must
// appear once. Gives ccol_success, ccol_key_not_found when the key is absent,
// ccol_key_already_present when it repeats, ccol_not_enough_memory, or
// ccol_invalid_args for a NULL req or key; *val_out is NULL on failure.
ccol_retval chttpsvr_req_query_one(chttpsvr_req *req, const char *key,
                                   const char **val_out);

// Gives the query string as it came, still encoded (for /search?q=a%20b&page=2
// that's "q=a%20b&page=2"), or NULL when there's none.
const char *chttpsvr_req_raw_query(const chttpsvr_req *req);

// Tells whether parsing this request's query string, or growing a
// chttpsvr_req_query() result, ran out of memory, since chttpsvr_req_query()
// gives NULL both for that and for an absent key. Once set, the flag stays set
// for the rest of the request. Gives false for a NULL req.
bool chttpsvr_req_query_oom(const chttpsvr_req *req);

// Attaches value to the request under key, so a middleware can hand what it
// found (an identity, a trace id) to later middleware and the handler. Keys
// compare by address, as with Go's context.WithValue, so use the address of a
// static object of your module. A second call with the same key replaces the
// value. The values belong to one request and are forgotten when its chain
// returns; the library never frees or reads them. Call it only on the thread
// that runs the chain, because it takes no lock. The first value needs no
// allocation. Gives ccol_success, ccol_invalid_args for a NULL req or key, or
// ccol_not_enough_memory (the earlier values stay).
ccol_retval chttpsvr_req_set_context(chttpsvr_req *req, const void *key,
                                     void *value);

// Gives the value that chttpsvr_req_set_context() attached under key, or NULL
// when there's none or req or key is NULL. A stored NULL also gives NULL.
void *chttpsvr_req_context(const chttpsvr_req *req, const void *key);

// Sets the response status (200 by default). A value outside 200 to 999, a 1xx
// included, goes out as 500, because a client takes a 1xx as interim and keeps
// waiting. A 204, 205 or 304 drops any body written before or after. The reason
// phrase comes from the IANA registry.
void chttpsvr_resp_set_status(chttpsvr_resp *resp, int status_code);

// Sets a response header, replacing every field of the same name (compared
// without case), so the response carries exactly one; the spelling and position
// of the first call stay. Fields go out in the order they were first set, and
// the server adds no Content-Type of its own. name must be a non-empty RFC 7230
// token and value must hold no control byte other than the tab, so reflected
// data can't inject header lines. Connection is stored but never sent, because
// the server writes its own. Content-Length must be digits: a response with a
// body carries the real body length instead, a 304 or a HEAD without a body
// carries yours, a 205 carries 0 and a 204 none. A Date you set replaces the
// server's. Transfer-Encoding is refused. Gives ccol_success,
// ccol_not_enough_memory, or ccol_invalid_args for a NULL argument or a refused
// name or value; a failed call changes nothing.
ccol_retval chttpsvr_resp_set_header(chttpsvr_resp *resp, const char *name,
                                     const char *value);

// Adds one more header field after the existing ones and keeps fields of the
// same name, so a response can carry two Set-Cookie lines. name and value are
// checked as by chttpsvr_resp_set_header(), with the same results; Connection,
// Content-Length and Date stay single and get replaced instead.
ccol_retval chttpsvr_resp_add_header(chttpsvr_resp *resp, const char *name,
                                     const char *value);

// Appends len bytes of data to the response body, which the server sends after
// the handler returns. A final status of 204, 205 or 304 drops the body
// silently, with no error anywhere. Gives ccol_success, ccol_invalid_args or
// ccol_not_enough_memory.
ccol_retval chttpsvr_resp_write(chttpsvr_resp *resp, const void *data,
                                size_t len);

// Appends a NUL-terminated string to the body, as chttpsvr_resp_write() does.
ccol_retval chttpsvr_resp_write_str(chttpsvr_resp *resp, const char *str);

// Formats fmt and its arguments as printf() does and appends the result to the
// body, as chttpsvr_resp_write_str() does. Gives ccol_invalid_args for a NULL
// resp or fmt, or when the formatting fails, and otherwise what
// chttpsvr_resp_write() gives.
ccol_retval chttpsvr_resp_printf(chttpsvr_resp *resp, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

// Appends len bytes of json to the body and sets Content-Type to
// application/json. The body goes first, so a failed body allocation changes
// nothing, while a header set that fails after it leaves the body written and
// gives ccol_not_enough_memory. A NULL resp or json, or a len of 0, gives
// ccol_invalid_args.
ccol_retval chttpsvr_resp_write_json(chttpsvr_resp *resp, const char *json,
                                     size_t len);

#pragma GCC visibility pop
