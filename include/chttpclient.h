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
#include <citerators.h>
#include <clogger.h>
#include <common.h>
#include <cthreadpool.h>

/**
 * @file chttpclient.h
 * @brief Hand-rolled HTTP/1.1 client: an internal chttp1_parser drives
 *        request/response framing over raw sockets, with TLS provided by
 *        ctls (the same reactor-agnostic OpenSSL wrapper chttpserver uses
 *        for its own TLS) and Tier 2/3's reactor provided by event_loop.
 *
 * ### Connection pool
 *
 * Each chttpcli handle has two independent layers. A concurrency limiter
 * bounds the number of simultaneous in-flight requests (chttpclient_do
 * blocks once the limit is reached); it defaults to the CPU count and is
 * overridden via chttpclient_set_pool_size. Separately, a keep-alive idle
 * connection cache (keyed by origin (scheme + host + port)) lets a
 * request reuse an already-open (and, for HTTPS, already-handshaked)
 * connection from a prior request instead of paying for DNS resolution and
 * the TCP/TLS handshakes again. A cheap liveness probe runs before reuse;
 * a connection the peer has since closed is discarded and replaced
 * transparently. Idle connections are bounded per origin and in total and
 * expire after a short idle period; once a cap is hit, a completed
 * connection is simply closed instead of cached (a lost optimisation, never
 * a correctness issue).
 *
 * ### Default client
 *
 * chttp_default_client() returns a process-level default client that is
 * lazily initialised on first call. It can be configured via the same
 * chttpclient_set_* functions as any other client. Convenience wrappers
 * (chttp_get, chttp_post, etc.) use it transparently.
 *
 * ### Thread safety
 *
 * All public functions in this module are thread-safe.
 *
 * ### Example
 *
 * @code
 * // Simple GET with the default client
 * chttpcli_response *resp;
 * if (chttp_get("https://api.example.com/users", &resp) == ccol_success) {
 *     printf("status=%d body=%s\n", resp->status_code, resp->body);
 *     chttpclient_resp_free(resp);
 * }
 *
 * // Custom client
 * chttpcli_construct(cli);
 * chttpclient_set_pool_size(cli, 8);
 * chttpclient_set_request_timeout(cli, 5000);
 *
 * chttp_request_t *req = chttp_request_new(CHTTP_POST,
 *     "https://api.example.com/items",
 *     &CHTTP_JSON_BODY(json_str, json_len), NULL);
 * chttp_request_set_header(req, "Authorization", "Bearer token");
 *
 * chttpcli_response *r;
 * chttpclient_do(cli, req, &r);
 * chttp_request_free(req);
 * chttpclient_resp_free(r);
 * chttpclient_destroy(cli);
 * @endcode
 */

/* ========================================================================== */
/*                         OPAQUE HANDLE                                      */
/* ========================================================================== */

/**
 * @brief Opaque HTTP client handle.
 *
 * chttpcli is an opaque VALUE handle (a packed {slot index, generation}
 * pair), not a pointer; it must never be cast to/from void*, compared via
 * a pointer cast, or otherwise treated as an address. Compare it directly
 * against CHTTPCLI_INVALID (or use it in a truthiness check; CHTTPCLI_INVALID
 * is 0, so `if (!cli)` still works exactly as it did when this was a raw
 * pointer). Internally, every use of a chttpcli is resolved through a
 * library-owned slot table before the underlying client object is touched:
 * a handle whose slot has since been freed (or reused for an unrelated,
 * later client) is always detected, rather than silently dereferencing
 * freed or wrong-object memory. See chttpclient_destroy's own doc comment
 * for what happens when a stale handle reaches it specifically.
 */
typedef uint64_t chttpcli;

/** @brief Sentinel value for "no client"; the chttpcli analogue of NULL. */
#define CHTTPCLI_INVALID ((chttpcli)0)

/* ========================================================================== */
/*                         STREAMING CALLBACK                                 */
/* ========================================================================== */

/**
 * @brief Streaming response body callback for chttpclient_do_streaming.
 *
 * Called zero or more times as response body bytes arrive. Return the number
 * of bytes consumed; returning a value less than len aborts the transfer.
 *
 * @param data  Pointer to the received chunk (not NUL-terminated).
 * @param len   Number of bytes in this chunk.
 * @param ctx   User-supplied context pointer set at call site.
 * @return      Number of bytes handled; must equal len to continue.
 */
typedef size_t (*chttpcli_write_fn)(const void *data, size_t len, void *ctx);

/* ========================================================================== */
/*                         REQUEST OBJECT                                     */
/* ========================================================================== */

/**
 * @brief HTTP request (partially transparent).
 *
 * The fields method, url, body, and expect_continue are public and may be
 * read/written directly. Headers are internal; use chttp_request_set_header
 * / get_header / headers_begin. url and body.data (and body.content_type,
 * if set) are owned copies allocated by chttp_request_new_mp and freed by
 * chttp_request_free.
 */
typedef struct chttp_request {
  chttp_method_t method;
  char *url;                 /* owned copy */
  chttp_request_body_t body; /* body.data is an owned copy */
  chmap_declare(headers, char *, char *);
  ccol_memmgmt_procs_t *_m_procs;
  /** Set to true (after construction, e.g. req->expect_continue = true) to
   *  send "Expect: 100-continue" and wait for the server's interim response
   *  before sending the body; see chttpclient_do's own doc comment for the
   *  full protocol. Has no effect if the request has no body, or the
   *  caller already set an explicit "Expect" header. Default false
   *  (chttp_request_new_mp zero-initializes this field). Tier 1
   *  (chttpclient_do/chttpclient_do_streaming) only; Tier 2/3
   *  (chttpclient_do_async and everything built on it) silently ignore
   *  this field for now and send the body immediately, exactly as if it
   *  were false. */
  bool expect_continue;
} chttp_request_t;

/* ========================================================================== */
/*                         RESPONSE OBJECT                                    */
/* ========================================================================== */

/**
 * @brief HTTP response (partially transparent).
 *
 * status_code, body, body_len, and headers are public.
 *
 * body is a heap-allocated, NUL-terminated buffer. body_len is the number of
 * bytes before the sentinel NUL. body is NULL when chttpclient_do_streaming
 * was used (the body was delivered via the write callback instead).
 *
 * Call chttpclient_resp_free to release all owned memory.  When a custom
 * allocator was supplied to create_chttpclient_mp, free the response BEFORE
 * destroying the client; see chttpclient_resp_free for details.
 */
typedef struct chttpcli_response {
  int status_code;
  char *body; /* heap-allocated, NUL-terminated; NULL for streaming path */
  size_t body_len;
  chmap_declare(headers, char *, char *); /* internal: chmap(char* -> char*) */
  ccol_memmgmt_procs_t *_m_procs;
} chttpcli_response;

/* ========================================================================== */
/*                    REQUEST LIFECYCLE */
/* ========================================================================== */

/**
 * @brief Allocate and initialise an HTTP request (custom allocator).
 *
 * Copies url and (if non-NULL) body->data and body->content_type into
 * internally owned buffers so the caller may free its originals immediately.
 *
 * @param method   HTTP method.
 * @param url      Target URL (copied; must not be NULL).
 * @param body     Request body, or NULL / &CHTTP_NO_BODY for bodyless methods.
 *                 body->data may be NULL only when body->len == 0; a NULL
 *                 body->data paired with a nonzero body->len is rejected.
 * @param mprocs   Custom allocator, or NULL for malloc/free.
 * @param err_str  Optional: receives a static error string on failure.
 * @return Newly allocated request, or NULL on failure (including a NULL url,
 *         a NULL body->data with a nonzero body->len, or allocation failure).
 */
chttp_request_t *chttp_request_new_mp(chttp_method_t method, const char *url,
                                      const chttp_request_body_t *body,
                                      ccol_memmgmt_procs_t *mprocs,
                                      char **err_str);

/**
 * @brief Allocate and initialise an HTTP request (default allocator).
 */
static inline __attribute__((always_inline)) chttp_request_t *chttp_request_new(
    chttp_method_t method, const char *url, const chttp_request_body_t *body,
    char **err_str) {
  return chttp_request_new_mp(method, url, body, NULL, err_str);
}

/**
 * @brief Set or replace a request header.
 *
 * Header names are normalised to lowercase on storage; lookup via
 * chttp_request_get_header is therefore case-insensitive. If a header with
 * the same name already exists its value is replaced.
 *
 * A "Transfer-Encoding" header is always rejected: this client never
 * transfer-codes a request body (a body-carrying request is always sent
 * whole, Content-Length-framed), so honouring a caller-set Transfer-Encoding
 * header is impossible, and silently accepting it would let
 * chttpclient_do's own automatic Content-Length header sit alongside it on
 * the wire over a body that was never actually transfer-coded; an
 * ambiguous framing this library's own chttp1_parser rejects outright when
 * it appears on a message being parsed.
 *
 * A "Content-Length" header IS accepted here (this function has no body
 * length to check it against without also duplicating the request's
 * eventual method/redirect context), but is validated later, when the
 * request actually reaches the wire: chttpclient_do/_do_streaming/
 * _do_async/_do_async_streaming/_do_pooled/_do_pooled_streaming/
 * chttp_run_query all reject a request whose caller-set Content-Length does
 * not exactly match the body actually being sent, for the identical
 * "declared framing disagrees with the wire" reason Transfer-Encoding is
 * rejected outright above; see chttpclient_do's own doc comment.
 *
 * @param req    Request to modify.
 * @param name   Header name (e.g. "Content-Type").
 * @param value  Header value.
 * @return ccol_success, ccol_invalid_args (req/name/value is NULL, name or
 *         value contains a CR or LF byte, or name is "Transfer-Encoding"),
 *         or ccol_not_enough_memory.
 */
ccol_retval_t chttp_request_set_header(chttp_request_t *req, const char *name,
                                       const char *value);

/**
 * @brief Look up a request header by name (case-insensitive).
 *
 * @param req   Request to query.
 * @param name  Header name.
 * @return Pointer to the stored value string, or NULL if not present.
 *         Valid until the next chttp_request_set_header call on this request.
 */
const char *chttp_request_get_header(const chttp_request_t *req,
                                     const char *name);

/**
 * @brief Free a request and all its owned resources.
 *
 * Safe to call with NULL.
 */
void chttp_request_free(chttp_request_t *req);

/* ========================================================================== */
/*                    CLIENT CONSTRUCTORS */
/* ========================================================================== */

/**
 * @brief Create an HTTP client with a custom allocator.
 *
 * The client is created with default settings. Call chttpclient_set_* before
 * the first request to customise behaviour.
 *
 * Default configuration (before any chttpclient_set_* calls):
 *   pool_size              = CPU count (resolved on first request)
 *   connect_timeout_ms     = 0 (no timeout)
 *   request_timeout_ms     = 0 (no timeout)
 *   max_response_body_size = 0 (no limit)
 *   TLS                    = peer + host verification on, system CA bundle
 *
 * @param mprocs   Custom allocator, or NULL for malloc/free.
 * @param err_str  Optional: receives a static error string on failure.
 * @return New client handle, or CHTTPCLI_INVALID on failure.
 */
chttpcli create_chttpclient_mp(ccol_memmgmt_procs_t *mprocs, char **err_str);

/**
 * @brief Create an HTTP client with the default allocator.
 *
 * @return New client handle, or CHTTPCLI_INVALID on failure.
 */
static inline __attribute__((always_inline)) chttpcli
create_chttpclient(char **err_str) {
  return create_chttpclient_mp(NULL, err_str);
}

/* ========================================================================== */
/*                    CLIENT CONFIGURATION */
/* ========================================================================== */

/**
 * @brief Set the maximum number of concurrent in-flight requests.
 *
 * Excess callers of chttpclient_do block until a slot is free. May be called
 * before or after the first request, and takes effect immediately in either
 * case. Passing 0 selects the CPU count.
 *
 * This is purely a concurrency cap; it is independent of the client's
 * keep-alive idle-connection cache (see the file-level doc comment), which
 * has its own fixed internal per-origin/total caps.
 *
 * @param cli  Client handle.
 * @param n    Pool size; 0 = CPU count.
 * @return ccol_success or ccol_invalid_args (cli is CHTTPCLI_INVALID, or a
 *         stale/already-destroyed handle).
 */
ccol_retval_t chttpclient_set_pool_size(chttpcli cli, size_t n);

/**
 * @brief Set the TCP connect timeout in milliseconds (0 = no timeout).
 *
 * For HTTPS requests, this also bounds the TLS handshake (folded into the
 * same budget as the TCP connect phase).
 *
 * Note: DNS resolution itself is a single blocking getaddrinfo() call with
 * no native cancellation; if resolution alone exceeds this timeout, the
 * connect attempt that follows is skipped and ccol_timed_out is returned
 * without waiting further, but the resolution call itself cannot be
 * interrupted mid-flight.
 *
 * @param cli  Client handle.
 * @param ms   Timeout in milliseconds.
 * @return ccol_success or ccol_invalid_args (cli is CHTTPCLI_INVALID, or a
 *         stale/already-destroyed handle).
 */
ccol_retval_t chttpclient_set_connect_timeout(chttpcli cli, long ms);

/**
 * @brief Set the total request timeout in milliseconds (0 = no timeout).
 *
 * This is the maximum time from when chttpclient_do is called to when the
 * last byte of the response body is received.
 *
 * @param cli  Client handle.
 * @param ms   Timeout in milliseconds.
 * @return ccol_success or ccol_invalid_args (cli is CHTTPCLI_INVALID, or a
 *         stale/already-destroyed handle).
 */
ccol_retval_t chttpclient_set_request_timeout(chttpcli cli, long ms);

/**
 * @brief Cap the buffered response body size (0 = unlimited, the default).
 *
 * Applies to every buffered (non-streaming) request path: chttpclient_do,
 * chttpclient_do_async (and the pooled-sync wrappers built on it),
 * chttp_get/post/put/delete/patch, and chttp_run_query. Has no effect on
 * chttpclient_do_streaming / chttpclient_do_async_streaming /
 * chttpclient_do_pooled_streaming: a streaming caller already controls its
 * own memory via chttpcli_write_fn's return value (returning fewer bytes
 * than len aborts the transfer), so there is nothing for this cap to bound
 * there.
 *
 * Enforced two ways: a response whose Content-Length header alone already
 * declares more than max_bytes is rejected immediately, before any body byte
 * is read off the wire; a chunked or connection-close-delimited body (which
 * has no declared length to check up front) is instead rejected reactively,
 * the moment the cumulative body received so far would exceed max_bytes.
 * Either case reports ccol_msg_too_large from chttpclient_do /
 * chttpclient_do_async's result / chttpclient_do_pooled, and the connection
 * is not reused afterward (mirroring how any other malformed-response
 * failure discards rather than pools its connection).
 *
 * Redirect hops are unaffected by this cap regardless of their own declared
 * or actual body size: an intermediate hop's body is always discarded
 * without ever being buffered (see chttpclient_do's own redirect-following
 * documentation), so only the final, delivered response's body counts
 * against max_bytes.
 *
 * May be called before or after the first request, and takes effect
 * immediately for every subsequent request; an already in-flight request is
 * unaffected.
 *
 * @param cli        Client handle.
 * @param max_bytes  Maximum buffered response body size in bytes; 0 = no
 *                    limit.
 * @return ccol_success or ccol_invalid_args (cli is CHTTPCLI_INVALID, or a
 *         stale/already-destroyed handle).
 */
ccol_retval_t chttpclient_set_max_response_body_size(chttpcli cli,
                                                     size_t max_bytes);

/**
 * @brief Set TLS configuration for this client.
 *
 * Passing NULL restores the default (verify_peer=true, verify_host=true,
 * system CA bundle, no client certificate).
 *
 * cert_path/key_path/ca_bundle_path are validated for readability lazily, at
 * the time an HTTPS request actually needs them, rather than here; a path
 * that does not currently exist is accepted here without error and only
 * surfaces as ccol_http_tls_cert_load_failed from chttpclient_do /
 * chttpclient_do_streaming once a request needs it.
 *
 * cert_path and key_path are a pair: exactly one of the two set (the other
 * NULL) is rejected as ccol_invalid_args rather than silently treated as "no
 * client certificate configured", since the latter would leave an mTLS
 * deployment believing it presents a client certificate when it never does.
 *
 * @param cli  Client handle.
 * @param tls  TLS configuration to copy, or NULL to restore defaults.
 * @return ccol_success, ccol_invalid_args (cli is CHTTPCLI_INVALID, a
 *         stale/already-destroyed handle, or exactly one of cert_path/
 *         key_path is set), or ccol_not_enough_memory.
 */
ccol_retval_t chttpclient_set_tls(chttpcli cli, const chttp_tls_config_t *tls);

/* ========================================================================== */
/*                         ENGINE LOGGER                                      */
/* ========================================================================== */

/**
 * @brief Install a custom logger for chttpclient's own async-engine (Tier
 *        2/3) reactor-level events.
 *
 * chttpclient_do_async/_streaming and the pooled-sync wrappers built on top
 * of them (chttpclient_do_pooled/_streaming) share one lazily-started,
 * process-wide event_loop reactor across every chttpcli instance (including
 * the default client). This engine logger captures that reactor's own
 * diagnostics (TLS handshake failures, connect errors); it is separate from
 * the per-client logger, and separate from chttpsvr_set_engine_logger's own
 * reactor (chttpserver and chttpclient each own a fully independent static
 * reactor; a process may freely run both at once).
 *
 * Call this before the first chttpclient_do_async/_streaming call anywhere
 * in the process if you want a custom engine logger. If no logger has been
 * installed when the engine first starts, a fallback logger (fd 2, level
 * CLOG_FATAL) is installed automatically; since this engine's own
 * diagnostics are never logged above CLOG_INFO, that fallback logger is
 * silent in practice unless this function is used to install a more
 * verbose one.
 *
 * Internally this function derives a logger from cl via clog_derive(), adds
 * the field component=http-client-engine, and installs the derived logger.
 * The previously installed engine logger (if any) is closed. The caller
 * retains ownership of cl and must keep it alive for as long as any
 * chttpcli's async engine may be running.
 *
 * @param cl  Parent logger to derive from; must not be NULL.
 * @return ccol_success or ccol_invalid_args (cl is NULL).
 */
ccol_retval_t chttpcli_set_engine_logger(clog cl);

/* ========================================================================== */
/*                    ENGINE MEMORY MANAGEMENT                                */
/* ========================================================================== */

/**
 * @brief Install custom memory management procs for chttpclient's own
 *        async-engine (Tier 2/3) reactor-level allocations.
 *
 * By default, the event_loop reactor shared by every chttpcli instance's
 * Tier 2/3 work in this process allocates its own memory (the registration
 * table, per-connection dispatch state, and so on) using the default
 * allocator. Calling this function with a non-NULL mp redirects all of that
 * to the supplied procs instead, exactly like the memory management procs
 * accepted by every other module in this library. Passing NULL reverts to
 * the default behavior.
 *
 * This configures only the shared reactor's own construction; it has no
 * effect on any individual chttpcli instance's own allocator, which is
 * configured independently via create_chttpclient_mp, exactly as before.
 *
 * May only be called before the reactor has ever started in this process
 * (i.e. before the first chttpclient_do_async/_streaming call anywhere), or
 * after the engine has fully stopped (every chttpcli async user has
 * released its reference and the automatic teardown has completed; there is
 * no explicit chttpcli_engine_wait(); the engine starts and stops on its own
 * as Tier 2/3 usage comes and goes, unlike chttpserver's typically
 * process-lifetime-long reactor).
 *
 * @param mp  Custom memory management procs, or NULL to revert to the
 *            default. If non-NULL, all four function pointers must be set.
 * @return ccol_success, ccol_invalid_args (mp is non-NULL but has a NULL
 *         function pointer), or ccol_not_permitted (the engine is currently
 *         running; wait for it to fully stop first).
 */
ccol_retval_t chttpcli_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp);

/* ========================================================================== */
/*                    ENGINE REACTOR THREAD COUNT                             */
/* ========================================================================== */

/**
 * @brief Configure how many OS threads chttpclient's own async-engine (Tier
 *        2/3) reactor devotes to its own polling and dispatch.
 *
 * By default (never having called this function, or having called it with
 * num_threads == 0), the shared reactor sizes itself to
 * sysconf(_SC_NPROCESSORS_ONLN) (falling back to 1 if that query fails),
 * matching this library's long-standing default behavior. Calling this
 * function with a positive num_threads overrides that auto-detection and
 * pins the reactor to exactly that many OS threads instead, following
 * event_loop_create_with_mprocs's own num_reactor_threads semantics
 * (cthreadcomm.h): 1 means a single thread both polls and dispatches
 * inline; any larger value means one dedicated polling thread plus
 * (num_threads - 1) dispatch worker threads.
 *
 * This configures only the shared reactor's own construction, independent
 * of any individual chttpcli instance's own settings.
 *
 * May only be called before the reactor has ever started in this process
 * (i.e. before the first chttpclient_do_async/_streaming call anywhere), or
 * after the engine has fully stopped (every chttpcli async user has
 * released its reference and the automatic teardown has completed; there is
 * no explicit chttpcli_engine_wait(); the engine starts and stops on its own
 * as Tier 2/3 usage comes and goes, unlike chttpserver's typically
 * process-lifetime-long reactor).
 *
 * @param num_threads  Desired reactor OS thread count, or 0 to restore the
 *                      default auto-detected sizing.
 * @return ccol_success, or ccol_not_permitted (the engine is currently
 *         running; wait for it to fully stop first).
 */
ccol_retval_t chttpcli_set_engine_num_reactor_threads(size_t num_threads);

/* ========================================================================== */
/*                    CLIENT DESTRUCTION */
/* ========================================================================== */

/**
 * @brief Internal destroy; use chttpclient_destroy macro instead.
 *
 * Waits for all in-flight requests to complete before freeing resources.
 *
 * cli must be a currently-live handle (one returned by create_chttpclient/
 * _mp or chttp_default_client and not yet destroyed). A stale handle
 * (one that has already been destroyed, whether by an earlier, completed
 * call to this same function, or concurrently, by another thread racing
 * this one right now), a forged value, or garbage is a fatal error:
 * this function calls fatal_err() (abort()/SIGABRT), rather than risking a
 * use-after-free or double-free, for both a purely sequential double-destroy
 * and a temporally-overlapping concurrent one. CHTTPCLI_INVALID (0) is the
 * one exception and remains a silent no-op, matching chttpclient_destroy's
 * own "destroy NULLs the handle" idiom.
 */
void __chttpclient_destroy(chttpcli cli);

/**
 * @brief RAII cleanup helper (used with _ccol_destructor).
 *
 * Safe to call on an already-CHTTPCLI_INVALID *pp (a no-op); calling it on a
 * stale, non-CHTTPCLI_INVALID handle that was already destroyed some other
 * way is the same fatal misuse __chttpclient_destroy itself documents.
 */
static inline __attribute__((always_inline)) void ___chttpclient_destroy(
    chttpcli *pp) {
  if (pp && *pp) {
    __chttpclient_destroy(*pp);
    *pp = CHTTPCLI_INVALID;
  }
}

/**
 * @brief Destroy an HTTP client and set the handle to CHTTPCLI_INVALID.
 *
 * Blocks until all in-flight requests complete. Must not be called
 * concurrently with other calls on the same handle; see
 * __chttpclient_destroy's own doc comment for what happens if it is (a
 * fatal error, not a silent race).
 */
#define chttpclient_destroy(cli)  \
  do {                            \
    __chttpclient_destroy((cli)); \
    (cli) = CHTTPCLI_INVALID;     \
  } while (0)

/* ========================================================================== */
/*                    LIFECYCLE MACROS */
/* ========================================================================== */

/** @brief Declare an uninitialised client variable. */
#define chttpcli_declare(name) chttpcli name

/** @brief Declare a client variable with automatic destruction on scope exit.
 */
#define chttpcli_declare_scoped(name) \
  chttpcli name _ccol_destructor(___chttpclient_destroy) = CHTTPCLI_INVALID;

/**
 * @brief Declare and initialise an HTTP client; fatal_err on failure.
 *
 * Example:
 * @code
 * chttpcli_construct(cli);
 * chttpclient_set_pool_size(cli, 4);
 * chttpclient_set_request_timeout(cli, 10000);
 * chttpcli_response *resp;
 * chttpclient_do(cli, req, &resp);
 * chttpclient_destroy(cli);
 * @endcode
 */
#define chttpcli_construct(name)                          \
  chttpcli name = CHTTPCLI_INVALID;                       \
  do {                                                    \
    char *_clic_err = NULL;                               \
    (name) = create_chttpclient(&_clic_err);              \
    if (!(name)) {                                        \
      fatal_err("chttpcli_construct('%s'): %s", #name,    \
                _clic_err ? _clic_err : "unknown error"); \
    }                                                     \
  } while (0)

/**
 * @brief Declare, initialise, and auto-destroy on scope exit; fatal_err on
 *        failure.
 */
#define chttpcli_construct_scoped(name)                                      \
  chttpcli name _ccol_destructor(___chttpclient_destroy) = CHTTPCLI_INVALID; \
  do {                                                                       \
    char *_clic_err = NULL;                                                  \
    (name) = create_chttpclient(&_clic_err);                                 \
    if (!(name)) {                                                           \
      fatal_err("chttpcli_construct_scoped('%s'): %s", #name,                \
                _clic_err ? _clic_err : "unknown error");                    \
    }                                                                        \
  } while (0)

/* ========================================================================== */
/*                    REQUEST EXECUTION */
/* ========================================================================== */

/**
 * @brief Perform an HTTP request and buffer the entire response body.
 *
 * Blocks until a pool slot is free, executes the request synchronously, and
 * returns a heap-allocated response. The caller owns *resp_out and must call
 * chttpclient_resp_free when done. *resp_out is set to NULL immediately
 * (before any other work begins) and stays NULL on every non-success return;
 * it is safe to unconditionally call chttpclient_resp_free(resp) after this
 * call regardless of the returned ccol_retval_t, without the caller having
 * to separately pre-initialise its own local pointer.
 *
 * Redirects (301, 302, 303, 307, 308) are followed automatically, up to 50
 * hops. 301/302/303 rewrite the method to a bodyless GET (HEAD is left as
 * HEAD, per RFC semantics); 307/308 preserve the original method and resend
 * the original body unchanged. The Location header may be an absolute URL,
 * a protocol-relative reference ("//host/path"), an absolute-path reference
 * ("/foo"), or a general relative reference ("foo", "../foo", "./foo",
 * "?query"); all are resolved per RFC 3986. A Location value that carries its
 * own scheme (e.g. "mailto:x@y", "ftp://host/path", or any scheme other than
 * http/https/http+unix) is always treated as absolute (RFC 3986 SS5.2.2: a
 * reference with a scheme is never relative, regardless of whether that
 * scheme is one this client can actually fetch) and resolves to itself
 * unchanged; the next hop then reports ccol_http_invalid_url, the same code
 * an unsupported scheme in the original request URL already gets, rather
 * than the reference being silently merged onto the current origin's path as
 * though it were relative. If the 50-hop cap is reached and
 * the last hop's response is itself a would-be redirect, it is not followed
 * or delivered; ccol_http_too_many_redirects is returned instead.
 *
 * The request URL accepts http:// and https:// only. Both a plain
 * hostname/IPv4 literal and a bracketed IPv6 literal
 * ("https://[::1]:8443/path") are accepted. A URL may embed credentials
 * ("http://user:pass@host/path"); they are turned into an
 * "Authorization: Basic ..." header automatically unless the request
 * already sets its own Authorization header. That auto-injected header is
 * resent on every redirect hop that stays on the same origin (scheme,
 * host, and port) and is dropped permanently the first time a hop changes
 * origin. A trailing "#fragment" is recognized and discarded (fragments
 * are never sent to a server).
 *
 * @param cli       Client handle.
 * @param req       Request to execute.
 * @param resp_out  Set to NULL immediately, then, on success, receives a
 *                   pointer to the response.
 * @return ccol_success
 *             Request completed; *resp_out is valid.
 *         ccol_invalid_args
 *             Any argument is NULL, cli is CHTTPCLI_INVALID or a stale/
 *             already-destroyed handle, req sets a "Transfer-Encoding" header
 *             (see chttp_request_set_header's own doc comment for why this
 *             is always rejected), or req sets a "Content-Length" header
 *             whose value does not exactly match the actual body length
 *             being sent on a body-carrying request (POST/PUT/PATCH); a
 *             mismatched declared length is the identical "framing
 *             disagrees with what's actually on the wire" hazard the
 *             Transfer-Encoding rejection exists to prevent, just reached
 *             through a wrong length instead of a wrong transfer-coding.
 *             Not checked for a non-body-carrying request (GET, DELETE,
 *             HEAD, OPTIONS): any Content-Length header is stripped from
 *             the wire entirely there, so there is no framing left for a
 *             mismatched value to desync from.
 *         ccol_not_enough_memory
 *             Allocation failed.
 *         ccol_timed_out
 *             Request or connect timeout triggered.
 *         ccol_not_permitted
 *             Client is being destroyed.
 *         ccol_http_invalid_url
 *             URL is malformed, uses an unsupported scheme (only http:// and
 *             https:// are supported), or has a missing/invalid host,
 *             port, or userinfo component (including an embedded CR or LF
 *             byte anywhere in the host or the path/query, which would
 *             otherwise be carried verbatim onto the wire and let it inject
 *             extra header lines or a smuggled second request).
 *         ccol_http_host_resolution_failed
 *             DNS resolution failed for the target host.
 *         ccol_http_connection_failed
 *             The TCP connection could not be established.
 *         ccol_http_too_many_redirects
 *             The redirect chain exceeded 50 hops.
 *         ccol_http_tls_handshake_failed
 *             The TLS handshake failed for a reason other than certificate
 *             verification.
 *         ccol_http_tls_cert_verification_failed
 *             The peer certificate or hostname could not be verified.
 *         ccol_http_tls_cert_load_failed
 *             The configured client certificate, key, or CA bundle path was
 *             not readable, or ctls failed to load/parse it.
 *         ccol_http_transfer_aborted
 *             The connection failed mid-transfer, or the server sent a
 *             malformed HTTP/1.1 response.
 *         ccol_msg_too_large
 *             The response body exceeded chttpclient_set_max_response_
 *             body_size's configured cap; see that function's own doc
 *             comment. Never returned unless that cap has been set.
 *         ccol_unexpected_failure
 *             Any other internal failure not covered above.
 */
ccol_retval_t chttpclient_do(chttpcli cli, const chttp_request_t *req,
                             chttpcli_response **resp_out);

/**
 * @brief Perform an HTTP request with a streaming response body.
 *
 * write_fn is called one or more times with chunks of the response body as
 * they arrive. Response headers are not accessible via this path. If
 * status_code_out is non-NULL it receives the HTTP status code on success.
 *
 * @param cli             Client handle.
 * @param req             Request to execute.
 * @param write_fn        Chunk delivery callback (must not be NULL).
 * @param write_ctx       Passed verbatim to write_fn.
 * @param status_code_out Receives HTTP status code on success, or NULL.
 * @return Same codes as chttpclient_do, except ccol_msg_too_large is never
 *         returned (chttpclient_set_max_response_body_size has no effect on
 *         this streaming path; see that function's own doc comment).
 *         Additionally, ccol_http_transfer_aborted is returned when write_fn
 *         returns fewer bytes than len, aborting the transfer.
 */
ccol_retval_t chttpclient_do_streaming(chttpcli cli, const chttp_request_t *req,
                                       chttpcli_write_fn write_fn,
                                       void *write_ctx, int *status_code_out);

/* ========================================================================== */
/*                         ASYNC API (TIER 2)                                 */
/* ========================================================================== */

/**
 * @brief Result of a request submitted via chttpclient_do_async or
 *        chttpclient_do_async_streaming.
 *
 * rv carries the same result codes chttpclient_do returns (see its own
 * documentation), including ccol_msg_too_large when chttpclient_set_max_
 * response_body_size's configured cap is exceeded; except for a request
 * submitted via chttpclient_do_async_streaming, which (like chttpclient_
 * do_streaming) is never subject to that cap and so never returns
 * ccol_msg_too_large. resp is non-NULL only when rv == ccol_success; free it
 * with chttpclient_resp_free before freeing this result, exactly as with
 * chttpclient_do's resp_out. For a request submitted via
 * chttpclient_do_async_streaming, resp is still populated on success (so
 * status_code and headers presence can be checked uniformly), but its body
 * is NULL; the body was already delivered via the write callback as it
 * arrived, exactly mirroring chttpclient_do_streaming's own
 * chttpcli_response.body == NULL convention for the streaming path.
 *
 * Obtained via chttpclient_async_result_get (a thin, typed wrapper over
 * ctpool_future_get) and released via chttpclient_async_result_free; do
 * this before calling ctpool_future_free on the future itself.
 */
typedef struct chttpcli_async_result {
  ccol_retval_t rv;
  chttpcli_response *resp;
  ccol_memmgmt_procs_t *_m_procs;
} chttpcli_async_result_t;

/**
 * @brief Submit an HTTP request for asynchronous execution.
 *
 * Non-blocking: queues the request onto chttpclient's shared, lazily-started
 * reactor engine (independent of chttpclient_do's synchronous connection
 * handling, and independent of chttpserver's own engine; see "Async engine"
 * below) and returns immediately. The whole request/response cycle,
 * including any redirect hops, runs on the engine's own threads.
 *
 * @param cli Client handle.
 * @param req Request to execute. Unlike chttpclient_do, req need not remain
 *            valid after this call returns; everything needed is copied
 *            or serialised internally before the call returns.
 * @return A future, or NULL if the request could not even be queued (NULL
 *         req, cli is CHTTPCLI_INVALID or a stale/already-destroyed handle,
 *         malformed URL, TLS unusable, OOM, or the engine failing to
 *         start). On success, the caller owns the future and must
 *         eventually call chttpclient_async_result_free (after
 *         chttpclient_async_result_get) followed by exactly one
 *         ctpool_future_free.
 *
 * ### Async engine
 *
 * The first call to chttpclient_do_async or chttpclient_do_async_streaming
 * anywhere in the process lazily starts a small, shared pool of reactor
 * threads (sized to the CPU count) plus a companion worker pool used solely
 * to offload DNS resolution and connect() off of reactor threads. The engine
 * is reference-counted and stops automatically once no request is in flight
 * and no connection remains in any chttpcli's async idle pool; it restarts
 * transparently on the next call. This engine owns its own static reactor,
 * entirely separate from chttpserver's own (independent) reactor and from
 * chttpclient_do's synchronous connection handling; a process may freely
 * run chttpserver and chttpclient_do_async/_streaming together, or use
 * chttpclient_do and chttpclient_do_async/_streaming together, with no
 * restrictions (the two engines share no state at all).
 */
ctpool_future *chttpclient_do_async(chttpcli cli, const chttp_request_t *req);

/**
 * @brief Submit an HTTP request for asynchronous execution with a streaming
 *        response body.
 *
 * Same non-blocking submission semantics as chttpclient_do_async, but
 * write_fn is invoked one or more times with chunks of the response body as
 * they arrive over the wire, exactly like chttpclient_do_streaming.
 *
 * write_fn runs on one of the engine's own reactor threads, NOT on the
 * calling thread and NOT on a dedicated thread for this request. This has
 * two hard requirements, unlike chttpclient_do_streaming's caller-thread
 * callback: write_fn must not block (no blocking I/O, no long-held locks,
 * no waiting on another request's future) (doing so stalls every other
 * connection the engine is currently multiplexing on that reactor thread)
 * and write_fn must not call back into chttpclient_do_async/_streaming (or
 * anything that transitively waits on this same request's future) for the
 * same or a different chttpcli sharing the engine, or it may deadlock
 * against the very reactor thread it is running on.
 *
 * @param cli       Client handle.
 * @param req       Request to execute (see chttpclient_do_async).
 * @param write_fn  Chunk delivery callback (must not be NULL). Returning
 *                  fewer bytes than len aborts the transfer; the future's
 *                  result then carries ccol_http_transfer_aborted.
 * @param write_ctx Passed verbatim to write_fn.
 * @return A future, or NULL under the same conditions as
 *         chttpclient_do_async (including a NULL write_fn).
 */
ctpool_future *chttpclient_do_async_streaming(chttpcli cli,
                                              const chttp_request_t *req,
                                              chttpcli_write_fn write_fn,
                                              void *write_ctx);

/**
 * @brief Block until an async request's future is fulfilled and return its
 *        typed result.
 *
 * A thin wrapper over ctpool_future_get that casts its void* result to
 * chttpcli_async_result_t*. Safe to call more than once on the same future
 * (matching ctpool_future_get's own contract); every call after the first
 * returns the same result pointer, still owned by the future until freed.
 *
 * @param f Future returned by chttpclient_do_async or
 *          chttpclient_do_async_streaming.
 * @return The result, or NULL if f is NULL or the future was cancelled
 *         before being fulfilled.
 */
chttpcli_async_result_t *chttpclient_async_result_get(ctpool_future *f);

/**
 * @brief Release a chttpcli_async_result_t obtained via
 *        chttpclient_async_result_get.
 *
 * Does NOT free result->resp; free that separately with
 * chttpclient_resp_free first if rv == ccol_success. Does NOT free the
 * future itself; pair with exactly one ctpool_future_free, called
 * separately (before or after this call, order does not matter).
 *
 * @param result Result to free; NULL is a safe no-op.
 */
void chttpclient_async_result_free(chttpcli_async_result_t *result);

/* ========================================================================== */
/*                    POOLED-SYNC API (TIER 3)                                */
/* ========================================================================== */

/**
 * @brief Perform an HTTP request using the shared Tier 2 engine, blocking
 *        until it completes.
 *
 * A thin wrapper over chttpclient_do_async: submits the request to the
 * shared engine, blocks until it completes, and returns the exact same
 * ccol_retval_t / resp_out call shape chttpclient_do uses; but the
 * connect/write/read work happens on the engine's own reactor threads
 * rather than the calling thread, and concurrent callers across many
 * chttpcli handles share one small, fixed-size reactor thread pool instead
 * of each blocking its own OS thread for the duration of its request.
 *
 * @param cli      Client handle.
 * @param req      Request to execute.
 * @param resp_out Must not be NULL. Set to NULL immediately, then, on
 *                  success, receives the response (matching
 *                  chttpclient_do's identical *resp_out contract).
 * @return Same result codes as chttpclient_do, with one difference: a
 *         failure to even submit the request to the engine (OOM, or the
 *         engine failing to start) is reported as ccol_unexpected_failure
 *         rather than a more specific code. Everything detected once the
 *         request is actually in flight (bad URL, TLS failure, connection
 *         failure, transfer errors, timeouts, too many redirects) is
 *         reported with the exact same specific codes chttpclient_do uses.
 */
ccol_retval_t chttpclient_do_pooled(chttpcli cli, const chttp_request_t *req,
                                    chttpcli_response **resp_out);

/**
 * @brief Perform an HTTP request with a streaming response body using the
 *        shared Tier 2 engine, blocking until it completes.
 *
 * A thin wrapper over chttpclient_do_async_streaming with the exact same
 * call shape as chttpclient_do_streaming. write_fn runs on one of the
 * engine's own reactor threads (see chttpclient_do_async_streaming's
 * documentation for the resulting must-not-block, must-not-call-back-into-
 * the-engine contract) rather than the calling thread; that is the only
 * respect in which this function behaves differently from
 * chttpclient_do_streaming's caller-thread callback.
 *
 * @param cli             Client handle.
 * @param req             Request to execute.
 * @param write_fn        Chunk delivery callback (must not be NULL).
 * @param write_ctx       Passed verbatim to write_fn.
 * @param status_code_out Receives HTTP status code on success, or NULL.
 * @return Same codes as chttpclient_do_pooled, except ccol_msg_too_large is
 *         never returned (chttpclient_set_max_response_body_size has no
 *         effect on this streaming path; see that function's own doc
 *         comment).
 */
ccol_retval_t chttpclient_do_pooled_streaming(chttpcli cli,
                                              const chttp_request_t *req,
                                              chttpcli_write_fn write_fn,
                                              void *write_ctx,
                                              int *status_code_out);

/* ========================================================================== */
/*                    DEFAULT CLIENT AND CONVENIENCE API */
/* ========================================================================== */

/**
 * @brief Return the process-level default client (lazily initialised).
 *
 * Thread-safe. The default client uses default settings (pool_size = CPU
 * count, no timeouts, TLS verification on). It may be configured by passing
 * the returned handle to chttpclient_set_*.
 *
 * Do NOT pass the returned handle to chttpclient_destroy: it is owned by
 * this module, which destroys it automatically at process exit. Destroying
 * it yourself is safe against crashing this specific call (the handle is
 * recognised and the module's own reference to it is cleared), but every
 * chttp_default_client/chttp_do/chttp_get/... call made afterward, by this
 * process, for the rest of its lifetime, then has no default client to use
 * and fails accordingly; there is no way to rebuild it once destroyed
 * this way. If you need a client with a bounded, caller-controlled
 * lifetime, create your own via create_chttpclient/_mp instead.
 *
 * @return Default client handle, or CHTTPCLI_INVALID if initialisation
 *         failed.
 */
chttpcli chttp_default_client(void);

/**
 * @brief Perform a request using the default client.
 *
 * Equivalent to chttpclient_do(chttp_default_client(), req, resp_out).
 */
ccol_retval_t chttp_do(const chttp_request_t *req,
                       chttpcli_response **resp_out);

/**
 * @brief Execute a one-shot request via the default client.
 *
 * Convenience wrapper: constructs a chttp_request_t internally, attaches
 * @p headers without taking ownership, calls chttp_do, then frees the
 * internal request object.  The caller retains full ownership of @p headers
 * and must destroy it when no longer needed.
 *
 * @param method    HTTP method.
 * @param url       Target URL; must not be NULL.
 * @param body      Request body, or NULL for bodyless requests.
 * @param headers   chmap(char* -> char*) of request headers, or NULL.
 *                  Borrowed for the duration of the call; not consumed.
 * @param resp_out  On success, receives the response pointer.
 * @return Same codes as chttpclient_do, or ccol_not_enough_memory if the
 *         internal request allocation fails.
 */
ccol_retval_t chttp_run_query(chttp_method_t method, const char *url,
                              const chttp_request_body_t *body, chmap headers,
                              chttpcli_response **resp_out);

/**
 * @brief GET request via the default client.
 *
 * @param url      Target URL.
 * @param resp_out Receives the response pointer on success.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory,
 *         ccol_timed_out, or ccol_unexpected_failure.
 */
ccol_retval_t chttp_get(const char *url, chttpcli_response **resp_out);

/**
 * @brief POST request via the default client.
 *
 * @param url      Target URL.
 * @param body     Request body, or NULL for an empty POST.
 * @param resp_out Receives the response pointer on success.
 */
ccol_retval_t chttp_post(const char *url, const chttp_request_body_t *body,
                         chttpcli_response **resp_out);

/**
 * @brief PUT request via the default client.
 */
ccol_retval_t chttp_put(const char *url, const chttp_request_body_t *body,
                        chttpcli_response **resp_out);

/**
 * @brief DELETE request via the default client.
 */
ccol_retval_t chttp_delete(const char *url, chttpcli_response **resp_out);

/**
 * @brief PATCH request via the default client.
 */
ccol_retval_t chttp_patch(const char *url, const chttp_request_body_t *body,
                          chttpcli_response **resp_out);

/* ========================================================================== */
/*                    RESPONSE API */
/* ========================================================================== */

/**
 * @brief Look up a response header by name.
 *
 * Lookup is case-insensitive. If the server sent duplicate headers with the
 * same name, the last occurrence wins (the map stores one value per name and
 * updates it in-place on duplicate insert).
 *
 * @param resp  Response to query.
 * @param name  Header name (e.g. "Content-Type").
 * @return Pointer to the value string, or NULL if not found.
 *         Valid until chttpclient_resp_free is called.
 */
const char *chttpclient_resp_header(const chttpcli_response *resp,
                                    const char *name);

/**
 * @brief Free a response and all its owned resources.
 *
 * Safe to call with NULL.
 *
 * Lifetime constraint with custom allocators: the response holds a
 * non-owning reference to the allocator supplied when the client was created
 * (via create_chttpclient_mp).  When a custom allocator is in use,
 * chttpclient_resp_free MUST be called before chttpclient_destroy; calling
 * it after the client has been destroyed is undefined behaviour.  With the
 * default allocator (NULL mprocs / malloc) the order does not matter.
 */
void chttpclient_resp_free(chttpcli_response *resp);
