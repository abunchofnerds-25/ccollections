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

#include "chttp.h"
#include "citerators.h"
#include "clogger.h"
#include "common.h"
#include "cthreadpool.h"

/* Everything that this header declares from here to the end is part of the
 * public Application Binary Interface (ABI) of libccollections, and the
 * shared library exports all of it. Because the library build uses
 * -fvisibility=hidden, a function or an object that is not inside one of
 * these blocks stays internal to the library: its name is not in the dynamic
 * symbol table of the library, the application that links against the
 * library cannot interpose it, and a symbol with the same name in that
 * application cannot collide with it. */
#pragma GCC visibility push(default)

/**
 * @file chttpclient.h
 * @brief A hand-written HTTP/1.1 client. An internal chttp1_parser frames
 *        the request and the response over raw sockets, ctls (the same
 *        reactor-agnostic OpenSSL wrapper that chttpserver uses for its own
 *        TLS) gives the TLS support, and ccol_event_loop gives the reactor
 *        for Tier 2 and Tier 3.
 *
 * ### Connection pool
 *
 * Each chttpcli handle has two independent layers. The first is a limiter
 * for concurrency, which bounds the number of requests that are in flight at
 * the same time: chttpclient_do blocks once the client reaches that limit,
 * which defaults to the CPU count and which chttpclient_set_pool_size
 * overrides. The second is a keep-alive cache of idle connections, keyed by
 * the origin (the scheme, the host without regard to case, and the port). It
 * lets a request reuse a connection that an earlier request opened (and, for
 * HTTPS, whose handshake already finished), so the request does not pay for
 * DNS resolution, the TCP handshake or the TLS handshake a second time. A
 * cheap liveness probe runs before each reuse, and the client drops a
 * connection that the peer closed and replaces it without any work from the
 * caller. The number of idle connections has a bound for each origin and a
 * bound in total, and an idle connection also expires after a short time.
 * Once the client reaches a cap, it closes a finished connection instead of
 * putting it into the cache; this loses an optimisation, and it is never a
 * problem for correctness.
 *
 * ### Default client
 *
 * chttp_default_client() gives a default client for the whole process,
 * which the library initializes on the first call. You can configure it with
 * the same chttpclient_set_* functions as any other client, and the short
 * wrappers (chttp_get, chttp_post and the others) use it for you.
 *
 * ### Thread safety
 *
 * Every function that takes a chttpcli handle is thread-safe, with one
 * exception that the next paragraph names. More than one thread can call
 * chttpclient_do, chttpclient_do_async or chttpclient_do_pooled against the
 * same client at the same time, and the same is true for the
 * chttpclient_set_* configuration functions. A chttpclient_destroy of that
 * client can also run at the same time as those calls; it waits for the
 * requests that are already in flight.
 *
 * The exception is a destroy against another destroy: two
 * chttpclient_destroy calls that overlap on one handle stop the process with
 * ccol_fatal_err, because the second call finds a handle that the first call
 * already retired and cannot tell it from a stale handle of some other
 * client. A destroy of a client is therefore the one operation that a caller
 * must serialize, and it is the last thing that any thread of the caller
 * does with that handle.
 *
 * This thread safety does NOT cover a change to one caller-owned
 * chttp_request_t or chttpcli_response object from more than one thread at
 * the same time, because neither object has synchronization of its own:
 * chttp_request_set_header, chttp_request_get_header, chttpclient_resp_header
 * and direct access to a field all need exclusive access to that one object.
 * A caller that shares one request or one response between threads must
 * therefore add its own external synchronization.
 *
 * ### Example
 *
 * @code
 * // A simple GET with the default client
 * chttpcli_response *resp;
 * if (chttp_get("https://api.example.com/users", &resp) == ccol_success) {
 *     // resp->body is NULL for a body that is truly empty, for example a
 *     // 204 or a HEAD. See the doc comment of chttpcli_response below.
 *     printf("status=%d body=%s\n", resp->status_code,
 *            resp->body ? resp->body : "");
 *     chttpclient_resp_free(resp);
 * }
 *
 * // Custom client
 * chttpcli_construct(cli);
 * chttpclient_set_pool_size(cli, 8);
 * chttpclient_set_request_timeout(cli, 5000000); // 5 s
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
 * @brief The opaque handle of an HTTP client.
 *
 * chttpcli is an opaque VALUE handle, not a pointer: it packs a slot index
 * and a generation into one value. Never cast it to void* or from void*,
 * never compare it through a pointer cast, and never treat it as an address.
 * Compare it directly against CHTTPCLI_INVALID, or test it for truth,
 * because CHTTPCLI_INVALID is 0, so `if (!cli)` asks the same question: is
 * there a client here? Inside the library, each use of a chttpcli goes
 * through a slot table that the library owns, and this happens before
 * anything touches the client object behind the handle, so the library
 * always detects a handle whose slot is free again, or whose slot belongs to
 * a different, later client, and it never dereferences freed memory or the
 * memory of the wrong object. See the doc comment of chttpclient_destroy for
 * what a stale handle does in that one function.
 */
typedef uint64_t chttpcli;

/** @brief The sentinel value for "no client": the chttpcli form of
 *  NULL. */
#define CHTTPCLI_INVALID ((chttpcli)0)

/* ========================================================================== */
/*                         STREAMING CALLBACK                                 */
/* ========================================================================== */

/**
 * @brief The streaming callback for a response body, for
 *        chttpclient_do_streaming.
 *
 * The client calls it zero or more times while the bytes of the response
 * body arrive. Return the number of bytes that you consume; a value less
 * than len aborts the transfer.
 *
 * @param data  A pointer to the chunk that arrived, which does not end with
 *              a NUL.
 * @param len   The number of bytes in this chunk.
 * @param ctx   The context pointer that the caller sets at the call site.
 * @return      The number of bytes that you handled, which must be equal to
 *              len to continue.
 */
typedef size_t (*chttpcli_write_fn)(const void *data, size_t len, void *ctx);

/* ========================================================================== */
/*                         REQUEST OBJECT                                     */
/* ========================================================================== */

/**
 * @brief An HTTP request, which is partially transparent.
 *
 * The fields method, url, body and expect_continue are public, so you can
 * read and write them directly. The headers are internal and have no
 * iterator API of their own: use chttp_request_set_header to set one header
 * by name, and chttp_request_get_header to read one header back by name.
 * The request owns copies of url and body.data, and also of
 * body.content_type if you set that field; chttp_request_new_mp allocates
 * these copies, and chttp_request_free frees them.
 */
typedef struct chttp_request {
  chttp_method_t method;
  char *url;                 /* owned copy */
  chttp_request_body_t body; /* body.data is an owned copy */
  chmap_declare(headers, char *, char *);
  ccol_memmgmt_procs_t *_m_procs;
  /** Set this field to true after construction, for example with
   *  req->expect_continue = true. The client then sends
   *  "Expect: 100-continue" and waits up to one second for the first byte
   *  of a response before it sends the body; a response that starts inside
   *  that second is read to its end, bounded only by the request timeout.
   *  "100 Continue" makes the client send the body, but a response that
   *  arrives in the same read as the "100 Continue" is read first, and a
   *  final one of 300 or above, or a complete one, ends the request with no
   *  byte of the body sent. A final status in place of the "100 Continue"
   *  is the answer: the client never sends the body and never reuses the
   *  connection. Another interim status, such as "103 Early Hints", is
   *  dropped and the wait goes on, and a second with no byte of a response
   *  makes the client send the body anyway. See the chttpclient_do(3) man
   *  page for the full protocol. The field has no effect if the request has
   *  no body, or if the caller already set an explicit "Expect" header. The
   *  default is false, because chttp_request_new_mp fills this field with
   *  zero. Every tier honors this field in the same way: chttpclient_do and
   *  chttpclient_do_streaming, chttpclient_do_async and
   *  chttpclient_do_async_streaming, and chttpclient_do_pooled and
   *  chttpclient_do_pooled_streaming, the last two of which sit on the
   *  async engine.
   *
   *  This field is the ONLY thing that turns on the real wait for the
   *  100-Continue protocol; the client does not derive it from the content
   *  on the wire. A direct call to
   *  chttp_request_set_header(req, "Expect", "100-continue"), without
   *  setting this field, puts that literal header on the wire but does NOT
   *  make the client wait for an interim response before the body. The
   *  header and the behaviour are fully independent by design, so a caller
   *  keeps full manual control of the content of the header on the wire,
   *  whatever this field does. A caller that wants the real protocol must
   *  set this field to true and must not set the header; the client then
   *  adds the header for the caller. */
  bool expect_continue;
  /** Set this field to true after construction, for example with
   *  req->prevent_tls_downgrade_on_redirect = true. The client then refuses
   *  to follow a redirect that would take this request from https to http,
   *  and reports
   *  ccol_http_invalid_url without opening the plaintext connection.
   *
   *  The default is false, because chttp_request_new_mp fills this field with
   *  zero, so a request follows such a redirect by default, which is what
   *  curl does (its CURLOPT_REDIR_PROTOCOLS permits both schemes) and what
   *  the Go net/http client does. This library matches them rather than
   *  surprising a caller who is porting from either.
   *
   *  Understand what the default costs before you leave it: the peer that
   *  sent the Location chooses the downgrade, not you, and once a chain
   *  leaves TLS, that hop and every later hop travel in clear, so whoever can
   *  see the traffic can read the request and the response, and can rewrite
   *  both. Set this field wherever a downgrade is not an acceptable outcome,
   *  which in most deployments is anywhere an https URL appears.
   *
   *  What the client protects whatever this field says: an Authorization
   *  value that the library injected from the userinfo of a URL is dropped as
   *  soon as the origin changes. The headers that you set yourself follow
   *  the rule of chttpclient_do for a redirect: a hop whose scheme, host or
   *  port differs from the original request carries none of your
   *  Authorization, Cookie, Cookie2 and WWW-Authenticate headers, and a hop
   *  whose host differs carries none of your Host header either. Every
   *  other header of yours follows the redirect.
   *
   *  This field has no effect on the AF_UNIX rule, which is not optional: a
   *  redirect can never add the http+unix transport and can never re-point it
   *  at another socket, whatever this field holds.
   *
   *  An UPGRADE from http to https is never affected; it can only add
   *  protection, so the client always follows it.
   *
   *  Every tier honours this field in the same way: chttpclient_do and
   *  chttpclient_do_streaming, chttpclient_do_async and
   *  chttpclient_do_async_streaming, and chttpclient_do_pooled and
   *  chttpclient_do_pooled_streaming. */
  bool prevent_tls_downgrade_on_redirect;
} chttp_request_t;

/* ========================================================================== */
/*                         RESPONSE OBJECT                                    */
/* ========================================================================== */

/**
 * @brief An HTTP response, which is partially transparent.
 *
 * The fields status_code, body, body_len and headers are public.
 *
 * body is a buffer on the heap that ends with a NUL, and it is not NULL
 * whenever it holds one byte or more; body_len is the number of bytes
 * before the sentinel NUL. body is NULL in two cases. The first is a call to
 * chttpclient_do_streaming, or to its async or pooled equivalent, where the
 * write callback gets the body instead. The second is a response with a
 * body of zero length on the non-streaming path, such as a 204, a response
 * to HEAD, or an explicit "Content-Length: 0"; there body_len is 0, and
 * since there is nothing to hold, the library does not allocate an empty
 * string for it. A caller must therefore check body != NULL, or
 * body_len > 0, before it dereferences body, even after a request that
 * succeeds and that does not stream.
 *
 * headers maps each field name of the response header block, in lower
 * case, to one value. A name that occurs once maps to its value, and a name
 * that occurs more than once maps to the combined value that RFC 9110
 * section 5.3 defines: every occurrence in the order of the wire, joined
 * with ", ". Set-Cookie is the one exception, because RFC 9110 section 5.3
 * and RFC 6265 section 3 forbid that combination for it: "set-cookie" maps
 * to its FIRST occurrence. chttpclient_resp_header reads the same value,
 * while chttpclient_resp_header_count and chttpclient_resp_header_at give
 * each occurrence on its own, which is how a caller reads every Set-Cookie.
 *
 * Call chttpclient_resp_free to free all the memory that the response owns.
 * Because the response does not depend on the client that produced it, you
 * can free it before or after chttpclient_destroy; see chttpclient_resp_free
 * for more.
 */
typedef struct chttpcli_response {
  int status_code;
  char *body; /* on the heap, and it ends with a NUL. It is NULL for the
               * streaming path, and for a body of zero length on the
               * non-streaming path. */
  size_t body_len;
  chmap_declare(headers, char *, char *); /* chmap(char* -> char*) */
  ccol_memmgmt_procs_t *_m_procs;
  void *_field_lines; /* internal: every occurrence of each repeated name */
} chttpcli_response;

/* ========================================================================== */
/*                    REQUEST LIFECYCLE */
/* ========================================================================== */

/**
 * @brief Allocate and initialize an HTTP request (custom allocator).
 *
 * The function copies url into a buffer that the request owns, and copies
 * body->data and body->content_type in the same way if they are not NULL,
 * so the caller can free its own originals at once.
 *
 * @param method   The HTTP method.
 * @param url      The target URL, which the function copies. It must not
 *                 be NULL.
 * @param body     The request body; give NULL or &CHTTP_NO_BODY for a
 *                 method with no body. body->data can be NULL only when
 *                 body->len == 0: the function rejects a NULL body->data
 *                 with a body->len that is not zero.
 * @param mprocs   A custom allocator, or NULL for malloc/free.
 * @param err_str  Optional: it gets a static error string if the call
 *                 fails.
 * @return A new request, or NULL if the call fails, which happens for a
 *         NULL url, for a NULL body->data with a body->len that is not
 *         zero, and for a failure of an allocation.
 */
chttp_request_t *chttp_request_new_mp(chttp_method_t method, const char *url,
                                      const chttp_request_body_t *body,
                                      ccol_memmgmt_procs_t *mprocs,
                                      char **err_str);

/**
 * @brief Allocate and initialize an HTTP request (default allocator).
 */
static inline __attribute__((always_inline)) chttp_request_t *chttp_request_new(
    chttp_method_t method, const char *url, const chttp_request_body_t *body,
    char **err_str) {
  return chttp_request_new_mp(method, url, body, NULL, err_str);
}

/**
 * @brief Set a request header, or replace one.
 *
 * The request stores each header name in lowercase, so a lookup with
 * chttp_request_get_header ignores the case of the name. If a header with
 * the same name already exists, this function replaces its value.
 *
 * The function always rejects a "Transfer-Encoding" header. This client
 * never applies a transfer coding to a request body: it always sends a
 * request with a body whole, framed with Content-Length, so it cannot obey
 * a Transfer-Encoding header from the caller, and it must not accept such a
 * header silently either. Otherwise the automatic Content-Length header of
 * chttpclient_do would sit beside it on the wire, over a body with no
 * transfer coding; that framing is ambiguous, and the chttp1_parser of this
 * library rejects such a framing in a message that it parses.
 *
 * The function DOES accept a "Content-Length" header, because it has no body
 * length to check that header against, and a check here would also need a
 * copy of the method and of the redirect context of the request, which come
 * later. The client validates the header later, when the request reaches
 * the wire: chttpclient_do, chttpclient_do_streaming, chttpclient_do_async,
 * chttpclient_do_async_streaming, chttpclient_do_pooled,
 * chttpclient_do_pooled_streaming and chttp_run_query all reject a bad
 * Content-Length from the caller. A good Content-Length is a plain unsigned
 * string of decimal digits, with no leading '+' or '-' and no whitespace
 * inside it, and it must match the body that the client sends exactly, for
 * the same reason that the client rejects Transfer-Encoding above: the
 * declared framing and the wire must not disagree. See the doc comment of
 * chttpclient_do.
 *
 * @param req    The request to change.
 * @param name   The header name, for example "Content-Type".
 * @param value  The header value.
 * @return ccol_success, ccol_invalid_args or ccol_not_enough_memory. The
 *         function gives ccol_invalid_args when req, name or value is
 *         NULL, and in four more cases: an empty name, a name that holds a
 *         byte outside the RFC 7230 tchar set, a name or a value that holds
 *         a CR byte or an LF byte, and a name of "Transfer-Encoding".
 */
ccol_retval_t chttp_request_set_header(chttp_request_t *req, const char *name,
                                       const char *value);

/**
 * @brief Look up a request header by name, ignoring the case of the name.
 *
 * @param req   The request to query.
 * @param name  The header name.
 * @return A pointer to the stored value string, or NULL if the header is
 *         not there. The pointer stays valid until the next call to
 *         chttp_request_set_header on this request.
 */
const char *chttp_request_get_header(const chttp_request_t *req,
                                     const char *name);

/**
 * @brief Free a request and every resource that it owns.
 *
 * A call with NULL is safe.
 */
void chttp_request_free(chttp_request_t *req);

/* ========================================================================== */
/*                    CLIENT CONSTRUCTORS */
/* ========================================================================== */

/**
 * @brief Create an HTTP client with a custom allocator.
 *
 * The new client has the default settings; call a chttpclient_set_*
 * function before the first request to change its behaviour.
 *
 * The default configuration, before any chttpclient_set_* call, is:
 *   pool_size              = the CPU count (resolved on the first request)
 *   connect_timeout_us     = 0 (no timeout)
 *   request_timeout_us     = 0 (no timeout)
 *   max_response_body_size = 0 (no limit)
 *   TLS                    = peer and host verification on, system CA bundle
 *
 * @param mprocs   A custom allocator, or NULL for malloc/free.
 * @param err_str  Optional: it gets a static error string if the call
 *                 fails.
 * @return A new client handle, or CHTTPCLI_INVALID if the call fails.
 */
chttpcli ccol_create_chttpclient_mp(ccol_memmgmt_procs_t *mprocs,
                                    char **err_str);

/**
 * @brief Create an HTTP client with the default allocator.
 *
 * @return A new client handle, or CHTTPCLI_INVALID if the call fails.
 */
static inline __attribute__((always_inline)) chttpcli
ccol_create_chttpclient(char **err_str) {
  return ccol_create_chttpclient_mp(NULL, err_str);
}

/* ========================================================================== */
/*                    CLIENT CONFIGURATION */
/* ========================================================================== */

/**
 * @brief Set the maximum number of requests that can be in flight at the
 *        same time.
 *
 * This cap applies ONLY to Tier 1 (chttpclient_do and
 * chttpclient_do_streaming): a caller of one of those two functions blocks
 * once the client reaches the cap and waits for a free slot, in the manner
 * of a counting semaphore. The cap does NOT apply to Tier 2
 * (chttpclient_do_async and chttpclient_do_async_streaming), and it does NOT
 * apply to Tier 3 (chttpclient_do_pooled and
 * chttpclient_do_pooled_streaming). A caller can submit any number of Tier 2
 * and Tier 3 requests at the same time: the reactor of the shared async
 * engine processes all of them, and a submit never blocks. That engine has
 * its own thread-count setting for the whole process (see
 * chttpcli_set_engine_num_reactor_threads), which this function does not
 * affect, and which is not a cap on concurrency either: it only controls how
 * many OS threads dispatch the work that is already in the queue. You can
 * call this function before the first request or after it; it takes effect
 * at once in both cases, and for Tier 1 traffic only. An n of 0 selects the
 * CPU count.
 *
 * This is only a cap on concurrency, independent of the keep-alive cache of
 * idle connections of the client (see the doc comment at the top of this
 * file for that cache). The cache has its own fixed internal caps for each
 * origin and in total, and those caps DO apply to all three tiers.
 *
 * @param cli  The client handle.
 * @param n    The pool size. 0 means the CPU count.
 * @return ccol_success or ccol_invalid_args. The function gives
 *         ccol_invalid_args when cli is CHTTPCLI_INVALID, and when cli is
 *         a stale handle of a client that is already destroyed.
 */
ccol_retval_t chttpclient_set_pool_size(chttpcli cli, size_t n);

/**
 * @brief Set the TCP connect timeout in microseconds. 0 means no timeout.
 *
 * For an HTTPS request, this timeout also bounds the TLS handshake, which
 * shares one budget with the TCP connect phase.
 *
 * Note: DNS resolution is one blocking getaddrinfo() call, and the system
 * offers no way to cancel it. If the resolution alone goes past this
 * timeout, the client skips the connect that follows and gives
 * ccol_timed_out back at once, without waiting any more, but nothing can
 * interrupt the resolution call itself while it runs.
 *
 * Every value other than 0 is honoured as a limit, however far away it
 * lies. A value too large for the clock to reach, such as UINT64_MAX,
 * saturates to the latest deadline that the clock can hold, and never wraps
 * into the past.
 *
 * @param cli  The client handle.
 * @param us   The timeout in microseconds. 0 means no timeout.
 * @return ccol_success or ccol_invalid_args. The function gives
 *         ccol_invalid_args when cli is CHTTPCLI_INVALID, and when cli is a
 *         stale handle of a client that is already destroyed.
 */
ccol_retval_t chttpclient_set_connect_timeout(chttpcli cli, uint64_t us);

/**
 * @brief Set the total request timeout in microseconds. 0 means no timeout.
 *
 * This is the maximum time from the call to chttpclient_do to the arrival
 * of the last byte of the response body.
 *
 * Every tier checks this deadline before each read of the response. Once
 * it has passed, the request ends with ccol_timed_out, even when the rest
 * of the response has already arrived and one more read would complete it,
 * while a response that the client finished reading before the deadline
 * passed is a success. chttpclient_do, chttpclient_do_async and
 * chttpclient_do_pooled, and their streaming forms, give the same result
 * for the same timeline.
 *
 * Every value other than 0 is honoured as a limit, however far away it
 * lies. A value too large for the clock to reach, such as UINT64_MAX,
 * saturates to the latest deadline that the clock can hold, and never wraps
 * into the past.
 *
 * @param cli  The client handle.
 * @param us   The timeout in microseconds. 0 means no timeout.
 * @return ccol_success or ccol_invalid_args. The function gives
 *         ccol_invalid_args when cli is CHTTPCLI_INVALID, and when cli is a
 *         stale handle of a client that is already destroyed.
 */
ccol_retval_t chttpclient_set_request_timeout(chttpcli cli, uint64_t us);

/**
 * @brief Cap the size of a response body in the buffer. 0 means no limit,
 *        and 0 is the default.
 *
 * This cap applies to every request path that buffers the body instead of
 * streaming it: chttpclient_do, chttpclient_do_async, the pooled-sync
 * wrappers on top of chttpclient_do_async, chttp_get, chttp_post,
 * chttp_put, chttp_delete, chttp_patch and chttp_run_query. The cap has no
 * effect on chttpclient_do_streaming, chttpclient_do_async_streaming or
 * chttpclient_do_pooled_streaming, because a caller that streams already
 * controls its own memory with the value that chttpcli_write_fn gives back
 * (a value less than len aborts the transfer), so this cap has nothing to
 * bound on those paths.
 *
 * The client applies the cap in two ways. A response whose Content-Length
 * header alone declares more than max_bytes fails at once, before the
 * client reads one byte of the body off the wire. A chunked body, or a body
 * that ends with the close of the connection, declares no length up front,
 * so the client checks it as it arrives and fails the request at the moment
 * the total body so far goes past max_bytes. Both cases give
 * ccol_msg_too_large: chttpclient_do gives it, the result of
 * chttpclient_do_async holds it, and chttpclient_do_pooled gives it. The
 * client does not reuse that connection afterwards; it drops it instead of
 * putting it into the pool, as it does for every other malformed response.
 *
 * This cap does not apply to a redirect hop, whatever the declared size and
 * the real size of the body of such a hop, because the client always drops
 * the body of an intermediate hop and never buffers it (see the
 * documentation of chttpclient_do about the way it follows a redirect).
 * Only the body of the final response that the caller gets counts against
 * max_bytes.
 *
 * You can call this function before the first request or after it. It takes
 * effect at once for every request that follows, and does not change a
 * request that is already in flight.
 *
 * @param cli        The client handle.
 * @param max_bytes  The maximum size in bytes of a response body in the
 *                   buffer. 0 means no limit.
 * @return ccol_success or ccol_invalid_args. The function gives
 *         ccol_invalid_args when cli is CHTTPCLI_INVALID, and when cli is
 *         a stale handle of a client that is already destroyed.
 */
ccol_retval_t chttpclient_set_max_response_body_size(chttpcli cli,
                                                     size_t max_bytes);

/**
 * @brief Set the TLS configuration of this client.
 *
 * A tls of NULL puts the defaults back: verify the chain and the hostname of
 * the server against the system CA bundle, and present no client
 * certificate. These are the same defaults that a zero-initialised
 * chttp_tls_config_t carries; see the field comment of
 * insecure_skip_verify in chttp.h.
 *
 * The function returns ccol_invalid_args for a configuration that cannot
 * mean one thing, such as exactly one of cert_path and key_path, or
 * insecure_skip_verify together with a ca_bundle_path.
 *
 * This function does not check that it can read cert_path, key_path and
 * ca_bundle_path; the client checks each path later, at the moment an HTTPS
 * request needs it. So this function accepts a path that does not exist and
 * reports no error for it, and that path then gives
 * ccol_http_tls_cert_load_failed from chttpclient_do or from
 * chttpclient_do_streaming once a request needs it.
 *
 * cert_path and key_path are a pair: if you set one of the two and leave
 * the other NULL, the function gives ccol_invalid_args instead of treating
 * that state as "no client certificate", because such silence leaves an
 * mTLS deployment in the belief that it presents a client certificate while
 * it never does.
 *
 * @param cli  The client handle.
 * @param tls  The TLS configuration to copy, or NULL to put the defaults
 *             back.
 * @return ccol_success, ccol_invalid_args or ccol_not_enough_memory. The
 *         function gives ccol_invalid_args in four cases: a cli of
 *         CHTTPCLI_INVALID, a stale cli of a client that is already
 *         destroyed, exactly one of cert_path and key_path set, and
 *         insecure_skip_verify set together with a ca_bundle_path.
 */
ccol_retval_t chttpclient_set_tls(chttpcli cli, const chttp_tls_config_t *tls);

/* ========================================================================== */
/*                         ENGINE LOGGER                                      */
/* ========================================================================== */

/**
 * @brief Install a custom logger for the events at the reactor level of the
 *        async engine of chttpclient (Tier 2 and Tier 3).
 *
 * chttpclient_do_async, chttpclient_do_async_streaming and the pooled-sync
 * wrappers on top of them (chttpclient_do_pooled and
 * chttpclient_do_pooled_streaming) share one ccol_event_loop reactor, which
 * covers the whole process and every chttpcli instance in it, the default
 * client included, and which the library starts only when it first needs
 * it. This engine logger captures the diagnostics of that reactor, such as
 * a failure of a TLS handshake or an error on a connect. It is separate
 * from the logger of each client, and also from the reactor of
 * chttpsvr_set_engine_logger: chttpserver and chttpclient each own a fully
 * independent static reactor, and one process can run both at the same
 * time.
 *
 * You can call this function at any time. It takes effect at once and
 * stays in force across every stop and restart of the engine until another
 * call replaces it. While no logger from this function is installed, the
 * library opens a fallback logger each time that the engine starts, which
 * writes to fd 2 at the level CLOG_FATAL. Because this engine never logs
 * its own diagnostics above CLOG_INFO, the fallback logger is silent until
 * you use this function to install a logger that says more.
 *
 * Inside, this function derives a logger from cl with clog_derive(), adds
 * the field component=http-client-engine, installs the derived logger, and
 * closes the engine logger that was there before, if there was one. The
 * caller keeps the ownership of cl and may close it after this call
 * returns: the derived logger shares the destination of cl, which stays
 * open while any logger that shares it is open. The engine closes the
 * derived logger when another call replaces it, and at the exit of the
 * process or the unload of the library when no engine runs then.
 *
 * For a logger from clog_open_fd or clog_open_fd_mp, "the destination" is
 * the logger state, not the file descriptor: such a logger never owns its
 * fd, and no clog_close closes it (see clog_open_fd_mp). Because the engine
 * keeps writing to that fd after you close cl, the fd itself must stay open
 * until another call to this function replaces the engine logger, or until
 * the process exits.
 *
 * @param cl  The parent logger to derive from. It must not be
 *            CLOG_INVALID.
 * @return ccol_success, ccol_invalid_args when cl is CLOG_INVALID, or
 *         ccol_not_enough_memory when the derived logger cannot be
 *         allocated.
 */
ccol_retval_t chttpcli_set_engine_logger(clog cl);

/* ========================================================================== */
/*                    ENGINE MEMORY MANAGEMENT                                */
/* ========================================================================== */

/**
 * @brief Install custom memory management procs for the allocations at the
 *        reactor level of the async engine of chttpclient (Tier 2 and
 *        Tier 3).
 *
 * Every chttpcli instance in this process shares one ccol_event_loop
 * reactor for its Tier 2 and Tier 3 work. By default that reactor allocates
 * its own memory (the registration table, the dispatch state of each
 * connection, and more) with the default allocator; a call to this function
 * with an mp that is not NULL sends all of those allocations to your procs
 * instead, in the same way as the memory management procs of every other
 * module in this library. An mp of NULL puts the default behaviour back.
 *
 * This function configures only the construction of the shared reactor and
 * has no effect on the allocator of one chttpcli instance, which you
 * configure on its own with ccol_create_chttpclient_mp.
 *
 * You can call this function only at two times: before the reactor ever
 * starts in this process (that is, before the first call to
 * chttpclient_do_async or to chttpclient_do_async_streaming anywhere), and
 * after the engine stops completely, which happens once every async user of
 * a chttpcli drops its reference and the automatic teardown finishes. There
 * is no chttpcli_engine_wait() function: the engine starts and stops on its
 * own with the Tier 2 and Tier 3 traffic, unlike the reactor of chttpserver,
 * which usually lives for the whole process.
 *
 * @param mp  The custom memory management procs, or NULL to put the default
 *            back. If mp is not NULL, all four function pointers in it must
 *            be set.
 * @return ccol_success, ccol_invalid_args or ccol_not_permitted. The
 *         function gives ccol_invalid_args when mp is not NULL but holds a
 *         function pointer that is NULL, and ccol_not_permitted while the
 *         engine runs; wait for the engine to stop completely first.
 */
ccol_retval_t chttpcli_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp);

/* ========================================================================== */
/*                    ENGINE REACTOR THREAD COUNT                             */
/* ========================================================================== */

/**
 * @brief Configure how many OS threads the reactor of the async engine of
 *        chttpclient (Tier 2 and Tier 3) gives to its poll and its
 *        dispatch.
 *
 * By default the shared reactor sizes itself to
 * sysconf(_SC_NPROCESSORS_ONLN), falling back to 1 if that query fails. You
 * get that default if you never call this function, and also if you call it
 * with num_threads == 0, while a call with a positive num_threads overrides
 * that automatic size and pins the reactor to exactly that number of OS
 * threads. The rules for num_reactor_threads in
 * ccol_event_loop_create_with_mprocs apply here too (see cthreadcomm.h): a
 * value of 1 means one thread that both polls and dispatches inline, and a
 * larger value means one dedicated poller thread and (num_threads - 1)
 * dispatch worker threads.
 *
 * This function configures only the construction of the shared reactor and
 * is independent of the settings of one chttpcli instance.
 *
 * You can call this function only at two times: before the reactor ever
 * starts in this process (that is, before the first call to
 * chttpclient_do_async or to chttpclient_do_async_streaming anywhere), and
 * after the engine stops completely, which happens once every async user of
 * a chttpcli drops its reference and the automatic teardown finishes. There
 * is no chttpcli_engine_wait() function: the engine starts and stops on its
 * own with the Tier 2 and Tier 3 traffic, unlike the reactor of chttpserver,
 * which usually lives for the whole process.
 *
 * @param num_threads  The OS thread count that you want for the reactor, or
 *                     0 to put the automatic size back.
 * @return ccol_success, or ccol_not_permitted while the engine runs; wait
 *         for the engine to stop completely first.
 */
ccol_retval_t chttpcli_set_engine_num_reactor_threads(size_t num_threads);

/* ========================================================================== */
/*                    CLIENT DESTRUCTION */
/* ========================================================================== */

/**
 * @brief The internal destroy. Use the chttpclient_destroy macro instead.
 *
 * It waits for every request that is in flight to finish, and then frees
 * the resources.
 *
 * cli must be a handle that is live now: one that ccol_create_chttpclient,
 * ccol_create_chttpclient_mp or chttp_default_client gave, and that nothing
 * has destroyed yet. Three other kinds of value are a fatal error: a stale
 * handle that another, finished call to this same function already
 * destroyed; a stale handle that another thread destroys at this same
 * moment; and a forged value or garbage. For all of them this function
 * calls ccol_fatal_err(), which calls abort() and raises SIGABRT, because
 * that is safer than the risk of a use-after-free or of a double free; this
 * holds both for two destroy calls one after the other and for two destroy
 * calls that overlap in time. CHTTPCLI_INVALID (0) is the one exception and
 * stays a silent no-op, which matches the idiom of chttpclient_destroy,
 * where a destroy sets the handle to NULL.
 */
void __chttpclient_destroy(chttpcli cli);

/**
 * @brief The RAII cleanup helper. You use it with _ccol_destructor.
 *
 * A call on a *pp that is already CHTTPCLI_INVALID is a safe no-op. A call
 * on a stale handle that is not CHTTPCLI_INVALID, which belongs to a client
 * that something else already destroyed, is the same fatal misuse that
 * __chttpclient_destroy documents.
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
 * The macro blocks until every request that is in flight finishes: another
 * thread can run a request at the same time as this call, and the macro
 * waits for that request instead of cutting it off. Do not run this macro
 * at the same time as ANOTHER DESTROY of the same handle; see the doc
 * comment of __chttpclient_destroy for the result of that misuse, which is
 * a fatal error, not a silent race.
 *
 * @note The macro evaluates cli exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define chttpclient_destroy(cli)  \
  _ccol_chttpclient_destroy_impl( \
      cli, _ccol_uniq(__ccol_chttpcli_destroy_slot, __COUNTER__))

/* Internal. The body of chttpclient_destroy. slot is a name from
 * _ccol_uniq(), so the macro nests inside the argument of another destroy
 * macro and stays -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_chttpclient_destroy_impl(cli, slot) \
  do {                                            \
    __typeof__(cli) *slot = &(cli);               \
    __chttpclient_destroy(*slot);                 \
    *slot = CHTTPCLI_INVALID;                     \
  } while (0)

/* ========================================================================== */
/*                    LIFECYCLE MACROS */
/* ========================================================================== */

/** @brief Declare a client variable that has no value yet. */
#define chttpcli_declare(name) chttpcli name

/** @brief Declare a client variable that the library destroys automatically
 *  at the exit of the scope.
 */
#define chttpcli_declare_scoped(name) \
  chttpcli name _ccol_destructor(___chttpclient_destroy) = CHTTPCLI_INVALID;

/**
 * @brief Declare and initialize an HTTP client, calling ccol_fatal_err if
 *        the creation fails.
 *
 * Example:
 * @code
 * chttpcli_construct(cli);
 * chttpclient_set_pool_size(cli, 4);
 * chttpclient_set_request_timeout(cli, 10000000); // 10 s
 * chttpcli_response *resp;
 * if (chttpclient_do(cli, req, &resp) == ccol_success) {
 *   // ... use resp ...
 *   chttpclient_resp_free(resp);
 * }
 * chttpclient_destroy(cli);
 * @endcode
 */
#define chttpcli_construct(name)                               \
  chttpcli name = CHTTPCLI_INVALID;                            \
  do {                                                         \
    char *_clic_err = NULL;                                    \
    (name) = ccol_create_chttpclient(&_clic_err);              \
    if (!(name)) {                                             \
      ccol_fatal_err("chttpcli_construct('%s'): %s", #name,    \
                     _clic_err ? _clic_err : "unknown error"); \
    }                                                          \
  } while (0)

/**
 * @brief Declare and initialize an HTTP client that the library destroys
 *        automatically at the exit of the scope, calling ccol_fatal_err if
 *        the creation fails.
 */
#define chttpcli_construct_scoped(name)                                      \
  chttpcli name _ccol_destructor(___chttpclient_destroy) = CHTTPCLI_INVALID; \
  do {                                                                       \
    char *_clic_err = NULL;                                                  \
    (name) = ccol_create_chttpclient(&_clic_err);                            \
    if (!(name)) {                                                           \
      ccol_fatal_err("chttpcli_construct_scoped('%s'): %s", #name,           \
                     _clic_err ? _clic_err : "unknown error");               \
    }                                                                        \
  } while (0)

/* ========================================================================== */
/*                    REQUEST EXECUTION */
/* ========================================================================== */

/**
 * @brief Do an HTTP request and buffer the whole response body.
 *
 * The function blocks until a pool slot is free, then runs the request
 * synchronously and gives back a response on the heap, which the caller
 * owns and must free with chttpclient_resp_free at the end. The function
 * sets *resp_out to NULL at once, before it starts any other work, and
 * *resp_out stays NULL on every return that is not a success, so you can
 * always call chttpclient_resp_free(resp) after this call, whatever
 * ccol_retval_t it gives back, without setting your own local pointer
 * before the call.
 *
 * The client follows a redirect automatically: the redirect codes are 301,
 * 302, 303, 307 and 308, and the client follows up to 50 hops. For 301, 302
 * and 303 it rewrites the method to a GET with no body, but leaves HEAD as
 * HEAD, because the RFC asks for that. For 307 and 308 it keeps the
 * original method and sends the original body again without a change. The
 * Location header can hold an absolute URL, a protocol-relative reference
 * ("//host/path"), an absolute-path reference ("/foo"), or a general
 * relative reference such as "foo", "../foo", "./foo" or "?query", and the
 * client resolves all of them as RFC 3986 asks. A Location value with a
 * scheme of its own (for example "mailto:x@y", "ftp://host/path", or any
 * scheme other than http, https and http+unix) is always absolute, because
 * RFC 3986 SS5.2.2 says that a reference with a scheme is never relative,
 * even if this client cannot fetch that scheme. Such a value resolves to
 * itself without a change, and the next hop then reports
 * ccol_http_invalid_url, the same code that an unsupported scheme in the
 * original request URL gets; the client does not merge the reference onto
 * the path of the current origin as if it were relative. A response with a
 * redirect code and no Location header, or an empty one, is not a redirect:
 * the client delivers it as the final response, as the Go net/http client
 * does. A response with more than one Location line redirects to the first
 * one, as curl and the Go net/http client do.
 *
 * An empty port in a URL, as in "http://host:/", names the default port of
 * the scheme (RFC 3986 section 3.2.3). A hostname with one trailing dot, as
 * in "https://www.example.com./", is resolved and sent in the Host header
 * as written, and TLS uses the name without that dot for SNI and for the
 * check of the certificate, as curl and Go do.
 *
 * A redirect from https to http is followed, which matches curl and the Go
 * net/http client. Set the prevent_tls_downgrade_on_redirect field of
 * chttp_request_t to refuse it instead: the client then reports
 * ccol_http_invalid_url and opens no plaintext connection. A Location that
 * merely inherits the scheme, such as "//host/path" or "/path", resolves
 * back to https and is followed either way, as is an upgrade from http to
 * https. See the field comment of prevent_tls_downgrade_on_redirect for
 * what the default costs.
 *
 * The AF_UNIX rule below is NOT optional and no field relaxes it.
 *
 * A hop can name a Unix domain socket ("http+unix://...") only when the hop
 * that carried the Location was bound to that same socket path; the client
 * refuses every other resolved http+unix:// target with
 * ccol_http_invalid_url and tries no connection. The rule holds for every
 * hop, against the hop that the Location came from, so a chain that leaves
 * a socket cannot come back to it, and a chain that never used a socket
 * cannot reach one, whatever the length of the chain. The match of the
 * scheme ignores case, and the client compares the socket path after it
 * decodes the percent escapes, so a different spelling or a second encoding
 * cannot get past the rule. A relative Location on a Unix hop resolves back
 * onto the socket of that hop, and the client follows it in the normal way.
 * Without this rule, any http or https server could redirect a caller onto
 * any local socket, while also controlling the target of the request.
 *
 * When the client reaches the cap of 50 hops and the response of the last
 * hop is itself a redirect, the client neither follows nor delivers it, and
 * gives ccol_http_too_many_redirects instead.
 *
 * The request URL accepts http://, https:// and http+unix://. Use
 * http+unix:// to connect to a server that listens on a Unix domain socket,
 * for example "http+unix://%2Fvar%2Frun%2Fapp.sock/api/users"; see the
 * section "Unix domain sockets" of chttpclient(7) for the full scheme. The
 * client does not support "https+unix://".
 *
 * The two network forms accept three kinds of host: a plain hostname, an
 * IPv4 literal, and an IPv6 literal in brackets, as in
 * "https://[::1]:8443/path". The host must match the uri-host rule of
 * RFC 3986 section 3.2.2, which allows these items:
 * - letters, digits, '-', '.', '_' and '~'
 * - the sub-delims !$&'()*+,;=
 * - "%XX" escapes
 * - an IPv6 address or an IPvFuture literal in brackets.
 *
 * Any other character, such as a space, a control byte, a byte of 0x80 or
 * more, or a backslash, makes the URL invalid (ccol_http_invalid_url). This
 * rule applies to the URL of the caller and to each Location. Write a host
 * name that is not ASCII in its punycode (IDNA) form; the client does not
 * support an IPv6 zone identifier.
 *
 * A URL can also contain credentials ("http://user:pass@host/path"), which
 * can use the characters of the RFC 3986 userinfo rule, "%XX" escapes, and
 * an unescaped '@' in the password; percent-encode every other byte. The
 * client changes the credentials into an "Authorization: Basic ..." header,
 * unless the request sets its own Authorization header, or the URL is
 * "http+unix://", which has no accepted convention for userinfo. The client
 * sends that automatic header again on each redirect hop that stays on the
 * same origin, where an origin is a scheme, a host and a port (the
 * comparison of the host ignores case, while the comparison of the scheme
 * and the port is exact). When a hop first changes the origin, the client
 * removes the header for the remainder of the chain.
 *
 * The headers that the caller sets with chttp_request_set_header follow a
 * redirect with two exceptions, which are the rules of curl, and the client
 * judges every hop against the ORIGINAL request, never against the hop
 * before it. A hop whose scheme, host or port differs from the original
 * request carries none of the Authorization, Cookie, Cookie2 and
 * WWW-Authenticate headers of the caller, and a hop whose host differs also
 * carries none of the Host header of the caller; the client writes its own
 * Host line for the new target instead. Hosts and header names match
 * without regard to case. So a hop that comes back to the original origin
 * carries those headers again, while a second hop on the same foreign
 * origin carries none of them. Every other header of the caller follows
 * every hop.
 *
 * The client builds the request-target of the request line from the path
 * and the query of the URL, and from those of every Location that it
 * follows, percent-encoding as "%XX" every byte that RFC 3986 does not
 * allow there literally: the space, the control bytes, DEL, every byte of
 * 0x80 and above, and the characters '"', '<', '>', '\', '^', '`', '{',
 * '|', '}', '[' and ']'. An existing "%XX" escape is kept as it is and is
 * never encoded a second time, while a '%' that two hex digits do not
 * follow makes the URL invalid (ccol_http_invalid_url), in the URL of the
 * caller and in a Location alike, as Go's net/url does for the same input.
 * The client also finds a "#fragment" at the end of a URL and drops it, so
 * it never sends a fragment to a server.
 *
 * When a host name resolves to several addresses, the client connects with
 * Happy Eyeballs (RFC 8305), as the default dialer of Go does. It takes the
 * addresses in the order that the resolver gives (the order of RFC 6724)
 * and alternates the address families, starting with the family of the
 * first address. It starts the first address at once, and the next one when
 * the attempts in flight have given no result for 250 ms, or at once when
 * an attempt fails. The first attempt that connects wins, the client closes
 * every other one, and it reports ccol_http_connection_failed only after
 * every address failed. The connect timeout is one budget for the whole
 * race; when it runs out, the connect ends with ccol_timed_out. So an
 * address that drops its packets costs about 250 ms instead of the connect
 * timeout, and the name "localhost", which a standard /etc/hosts lists as
 * ::1 before 127.0.0.1, reaches a server that listens on 127.0.0.1 alone.
 * chttpclient_do starts the next address exactly at 250 ms, while
 * chttpclient_do_async and chttpclient_do_pooled start it between 250 ms
 * and 350 ms after the one before it.
 *
 * A server can answer before it has read the whole request (as a 401 from
 * a check of credentials or a 413 from a size limit does) and then close
 * the connection or stop reading, so every tier watches for such an answer
 * while it sends the request, and returns it. An interim (1xx) response
 * other than a 101 is dropped and the send goes on; a final response of 300
 * or above stops the send once its header block has arrived; a final
 * response of 200 to 299 lets the send go on while the response arrives,
 * and a complete response stops it. A write that fails because the peer
 * went away stops the send, and the client returns the answer that the peer
 * sent before it went, if there is one. A connection whose response started
 * before its request was sent in full is never reused. Because a reset
 * discards what the client has not read yet, an answer is returned only
 * when the client reads it before a reset arrives: this function waits for
 * readability during every wait of the send, while chttpclient_do_async and
 * chttpclient_do_pooled look for an answer whenever a write would block,
 * and after a write once 32 KiB or more went out since the last look.
 *
 * Every socket of the client is close-on-exec, so a process that the
 * application spawns inherits no connection of it.
 *
 * @param cli       The client handle.
 * @param req       The request to run.
 * @param resp_out  The function sets it to NULL at once; if the call
 *                  succeeds, it then gets a pointer to the response.
 * @return ccol_success
 *             The request finished, and *resp_out is valid.
 *         ccol_invalid_args
 *             An argument is NULL; or cli is CHTTPCLI_INVALID, or a stale
 *             handle of a client that is already destroyed; or req sets a
 *             "Transfer-Encoding" header (see the doc comment of
 *             chttp_request_set_header for the reason that the client
 *             always rejects that header); or req sets a bad
 *             "Content-Length" header on a request that carries a body
 *             (POST, PUT or PATCH). A good value is a plain unsigned string
 *             of decimal digits, with no leading '+' or '-' and no
 *             whitespace inside it, and it must match the real length of
 *             the body that the client sends. A declared length that is
 *             malformed, or that does not match, is the same hazard that
 *             the rejection of Transfer-Encoding prevents: the framing
 *             disagrees with the wire, only through a wrong length instead
 *             of a wrong transfer coding. The client does not make this
 *             check for a request that carries no body (GET, DELETE, HEAD
 *             or OPTIONS), because it removes every Content-Length header
 *             from the wire for such a request; no framing is left there,
 *             so a value that does not match can desync nothing.
 *         ccol_not_enough_memory
 *             An allocation failed.
 *         ccol_timed_out
 *             The request timeout or the connect timeout fired.
 *         ccol_not_permitted
 *             The client is in its destroy.
 *         ccol_http_invalid_url
 *             The URL is malformed; or it uses an unsupported scheme (only
 *             http://, https:// and http+unix:// are supported); or its
 *             host, port or userinfo part is missing or invalid, including
 *             a host outside the uri-host rule (see above), any byte
 *             outside the userinfo rule in the credentials, a CR byte or
 *             an LF byte in the path or in the query, and a '%' in the
 *             path or the query that two hex digits do not follow. Without
 *             this check the client would copy such a byte onto the wire,
 *             and the URL could then inject more header lines, or smuggle
 *             a second request. Or, for http+unix://, the URL names a
 *             socket path that is too long for sockaddr_un.sun_path. The
 *             client also gives this code for a redirect that resolves to
 *             a http+unix:// target when the hop that carried the
 *             Location was not bound to that target; see above.
 *         ccol_http_host_resolution_failed
 *             The DNS resolution of the target host failed. The client
 *             never gives this code for http+unix://, which has no DNS
 *             step.
 *         ccol_http_connection_failed
 *             The client could not make the connection: every address of
 *             the host refused the connection or failed, or a http+unix://
 *             socket path does not exist. A socket error on a connection
 *             that the client already made is ccol_http_transfer_aborted
 *             instead.
 *         ccol_http_too_many_redirects
 *             The chain of redirects went past 50 hops.
 *         ccol_http_tls_handshake_failed
 *             The TLS handshake failed for a reason other than the
 *             verification of a certificate.
 *         ccol_http_tls_cert_verification_failed
 *             The client could not verify the certificate of the peer or
 *             its hostname.
 *         ccol_http_tls_cert_load_failed
 *             The client could not read the client certificate, the key or
 *             the CA bundle path of the configuration, or ctls could not
 *             load or parse one of them.
 *         ccol_http_transfer_aborted
 *             The connection failed in the middle of the transfer, or the
 *             server sent a malformed HTTP/1.1 response. Over https://,
 *             this includes a response whose body ends at the close of the
 *             connection (no Content-Length and no chunked framing) when
 *             the server closes without a TLS close_notify alert: without
 *             that alert the client cannot tell the end of the body from a
 *             connection that an attacker or a failure cut short, so it
 *             reports the response as truncated. A body with a length or
 *             with chunked framing does not depend on the alert. A
 *             connection that fails while the request is sent gives this
 *             code only when the client found no answer of the server
 *             (see above). A "101 Switching Protocols" response gives this
 *             code too, because this client never asks for an upgrade, and
 *             the bytes after a 101 belong to another protocol.
 *         ccol_msg_too_large
 *             The response body went past the cap of
 *             chttpclient_set_max_response_body_size (see the doc comment
 *             of that function). The client gives this code only after you
 *             set that cap.
 *         ccol_unexpected_failure
 *             Any other internal failure that the codes above do not
 *             cover.
 */
ccol_retval_t chttpclient_do(chttpcli cli, const chttp_request_t *req,
                             chttpcli_response **resp_out);

/**
 * @brief Do an HTTP request and stream the response body.
 *
 * The client calls write_fn one or more times with chunks of the response
 * body, while those chunks arrive. This path gives you no access to the
 * response headers; if status_code_out is not NULL, it gets the HTTP status
 * code after a call that succeeds.
 *
 * @param cli             The client handle.
 * @param req             The request to run.
 * @param write_fn        The callback that gets each chunk. It must not be
 *                        NULL.
 * @param write_ctx       The client gives this value to write_fn without a
 *                        change.
 * @param status_code_out It gets the HTTP status code after a call that
 *                        succeeds. It can be NULL.
 * @return The same codes as chttpclient_do, with two differences. The
 *         client never gives ccol_msg_too_large here, because
 *         chttpclient_set_max_response_body_size has no effect on this
 *         streaming path (see the doc comment of that function). The client
 *         also gives ccol_http_transfer_aborted when write_fn gives back a
 *         value other than len, which aborts the transfer; this matches the
 *         contract of chttpcli_write_fn, which covers a value above len as
 *         well as a value below len.
 */
ccol_retval_t chttpclient_do_streaming(chttpcli cli, const chttp_request_t *req,
                                       chttpcli_write_fn write_fn,
                                       void *write_ctx, int *status_code_out);

/* ========================================================================== */
/*                         ASYNC API (TIER 2)                                 */
/* ========================================================================== */

/**
 * @brief The result of a request that you submit with chttpclient_do_async
 *        or with chttpclient_do_async_streaming.
 *
 * rv holds the same result codes that chttpclient_do gives (see the
 * documentation of that function), including ccol_msg_too_large when the
 * response goes past the cap of chttpclient_set_max_response_body_size,
 * with one exception: a request that you submit with
 * chttpclient_do_async_streaming is never under that cap, exactly like
 * chttpclient_do_streaming, so it never gives ccol_msg_too_large. resp is
 * not NULL only when rv == ccol_success. Free resp with
 * chttpclient_resp_free before you free this result, in the same order as
 * for the resp_out of chttpclient_do. For a request from
 * chttpclient_do_async_streaming the client fills resp after a success
 * too, so you can read status_code in the same way from a streaming result
 * and from a non-streaming one; but resp->headers is always NULL there, and
 * resp->body_len is always 0, because the client drops the response headers
 * instead of keeping them, and the write callback already got the body
 * while it arrived, with nothing buffering it. This matches the convention
 * of chttpclient_do_streaming, where chttpcli_response.body is NULL on the
 * streaming path.
 *
 * You get this result with chttpclient_async_result_get, a thin typed
 * wrapper over ctpool_future_get, and free it with
 * chttpclient_async_result_free before you call ctpool_future_free on the
 * future itself.
 */
typedef struct chttpcli_async_result {
  ccol_retval_t rv;
  chttpcli_response *resp;
  ccol_memmgmt_procs_t *_m_procs;
} chttpcli_async_result_t;

/**
 * @brief Submit an HTTP request for asynchronous execution.
 *
 * This function does not block: it puts the request into the queue of the
 * shared reactor engine of chttpclient and returns at once. The library
 * starts that engine only when it first needs it, and the engine is
 * independent of the synchronous connection work of chttpclient_do and of
 * the engine of chttpserver (see "Async engine" below). The whole cycle of
 * the request and the response, every redirect hop included, runs on the
 * threads of the engine.
 *
 * @param cli The client handle.
 * @param req The request to run. Unlike with chttpclient_do, req does not
 *            need to stay valid after this call returns, because the
 *            library copies or serializes everything that it needs before
 *            this call returns.
 * @return A future, or NULL if the library cannot even queue the request:
 *         for a req that is NULL, for a cli of CHTTPCLI_INVALID or a stale
 *         cli of a client that is already destroyed, for a malformed URL,
 *         for TLS that it cannot use, when it is out of memory, and when
 *         the engine fails to start. After a success, the caller owns the
 *         future and must call chttpclient_async_result_get, then
 *         chttpclient_async_result_free, and then exactly one
 *         ctpool_future_free.
 *
 * ### Async engine
 *
 * The first call to chttpclient_do_async or to
 * chttpclient_do_async_streaming anywhere in the process starts a small
 * shared pool of reactor threads, which the library sizes to the CPU count,
 * and a companion worker pool whose only job is to take the DNS resolution
 * and the connect() off the reactor threads. The engine has a refcount: it
 * stops on its own when no request is in flight and no connection is left
 * in the async idle pool of any chttpcli, and starts again on the next
 * call, with no work from the caller. This engine owns its own static
 * reactor, fully separate from the independent reactor of chttpserver and
 * from the synchronous connection work of chttpclient_do. So one process
 * can run chttpserver together with chttpclient_do_async and
 * chttpclient_do_async_streaming, and can also use chttpclient_do together
 * with those two functions, with no limits, because the two engines share
 * no state at all.
 *
 * The engine stops its threads before the process exits. A handler that
 * the library registers with atexit(3), after the one of OpenSSL so that it
 * runs first, waits for a stop in progress, destroys the default client
 * when no call runs on it, and stops an engine that nothing holds any more,
 * so no thread of the engine is left to race the cleanup of OpenSSL. Two
 * cases keep the engine running at exit, and the handler then does
 * nothing: a request in flight, which includes an exit() from inside a
 * write_fn, and a client that the application never destroyed, whose
 * pooled connections hold the engine. Destroy every client before the
 * process returns from main.
 *
 * Neither this engine nor any other part of the client changes the
 * disposition of SIGPIPE, and no write of the client raises it: every write
 * to a socket, a TLS record and the close_notify of a TLS teardown
 * included, is a send(2) with MSG_NOSIGNAL.
 */
ctpool_future *chttpclient_do_async(chttpcli cli, const chttp_request_t *req);

/**
 * @brief Submit an HTTP request for asynchronous execution, and stream the
 *        response body.
 *
 * The submit works in the same way as in chttpclient_do_async and does not
 * block, but the client calls write_fn one or more times with chunks of the
 * response body while those chunks arrive over the wire, as in
 * chttpclient_do_streaming.
 *
 * write_fn runs on one of the reactor threads of the engine, NOT on the
 * thread of the caller and NOT on a thread of its own for this request.
 * Because the callback of chttpclient_do_streaming runs on the thread of
 * the caller, this path adds two hard rules. First, write_fn must not
 * block: it must do no blocking I/O, hold no lock for a long time, and wait
 * on the future of no other request, because a block there stalls every
 * other connection that the engine multiplexes on that reactor thread.
 * Second, write_fn must not call chttpclient_do_async or
 * chttpclient_do_async_streaming again, and must call nothing that waits on
 * the future of this same request, directly or indirectly, whether for the
 * same chttpcli or for a different chttpcli that shares the engine; such a
 * call can deadlock against the reactor thread that runs write_fn.
 *
 * @param cli       The client handle.
 * @param req       The request to run; see chttpclient_do_async.
 * @param write_fn  The callback that gets each chunk. It must not be NULL.
 *                  A value other than len from write_fn aborts the
 *                  transfer, which matches the contract of
 *                  chttpcli_write_fn (it covers a value above len as well
 *                  as a value below len), and the result of the future then
 *                  holds ccol_http_transfer_aborted.
 * @param write_ctx The client gives this value to write_fn without a
 *                  change.
 * @return A future, or NULL in the same conditions as in
 *         chttpclient_do_async, plus a write_fn of NULL.
 */
ctpool_future *chttpclient_do_async_streaming(chttpcli cli,
                                              const chttp_request_t *req,
                                              chttpcli_write_fn write_fn,
                                              void *write_ctx);

/**
 * @brief Block until the future of an async request is fulfilled, then give
 *        its typed result.
 *
 * This is a thin wrapper over ctpool_future_get that casts the void* result
 * of that function to chttpcli_async_result_t*. As in the contract of
 * ctpool_future_get, more than one call on the same future is safe: every
 * call after the first gives the same result pointer, which the future owns
 * until somebody frees it.
 *
 * @param f The future from chttpclient_do_async or from
 *          chttpclient_do_async_streaming.
 * @return The result, or NULL if f is NULL or if something cancelled the
 *         future before it was fulfilled.
 */
chttpcli_async_result_t *chttpclient_async_result_get(ctpool_future *f);

/**
 * @brief Free a chttpcli_async_result_t that chttpclient_async_result_get
 *        gave you.
 *
 * This function does NOT free result->resp; free that on its own first,
 * with chttpclient_resp_free, if rv == ccol_success. It does NOT free the
 * future either, so pair it with exactly one ctpool_future_free, called on
 * its own before or after this call, in either order. The result carries
 * its own copy of the allocator of the client, so this call is valid before
 * or after chttpclient_destroy of that client.
 *
 * @param result The result to free. NULL is a safe no-op.
 */
void chttpclient_async_result_free(chttpcli_async_result_t *result);

/* ========================================================================== */
/*                    POOLED-SYNC API (TIER 3)                                */
/* ========================================================================== */

/**
 * @brief Do an HTTP request on the shared Tier 2 engine, and block until it
 *        finishes.
 *
 * This is a thin wrapper over chttpclient_do_async that submits the request
 * to the shared engine and blocks until the request finishes. It gives back
 * the same ccol_retval_t and uses the same resp_out call shape as
 * chttpclient_do, but the connect, the write and the read run on the
 * reactor threads of the engine instead of on the thread of the caller, so
 * many callers across many chttpcli handles share one small reactor thread
 * pool of a fixed size, whereas with chttpclient_do each caller blocks its
 * own OS thread for the whole request.
 *
 * @param cli      The client handle.
 * @param req      The request to run.
 * @param resp_out It must not be NULL. The function sets it to NULL at
 *                 once, and if the call succeeds, it then gets the
 *                 response; this is the same *resp_out contract as in
 *                 chttpclient_do.
 * @return The same result codes as chttpclient_do. An allocation that
 *         fails, before the request reaches the engine or while the engine
 *         runs it, gives ccol_not_enough_memory, and an engine that fails to
 *         start gives the code of that failure (ccol_not_enough_memory or
 *         ccol_unexpected_failure). A bad URL, a TLS failure, a connection
 *         failure, a transfer error, a timeout and too many redirects each
 *         give the same specific code that chttpclient_do uses.
 */
ccol_retval_t chttpclient_do_pooled(chttpcli cli, const chttp_request_t *req,
                                    chttpcli_response **resp_out);

/**
 * @brief Do an HTTP request on the shared Tier 2 engine, stream the response
 *        body, and block until the request finishes.
 *
 * This is a thin wrapper over chttpclient_do_async_streaming, with the same
 * call shape as chttpclient_do_streaming. write_fn runs on one of the
 * reactor threads of the engine instead of on the thread of the caller, so
 * the two rules in the documentation of chttpclient_do_async_streaming
 * apply: write_fn must not block, and it must not call back into the
 * engine. The thread that runs write_fn is the only difference between this
 * function and chttpclient_do_streaming, whose callback runs on the thread
 * of the caller.
 *
 * @param cli             The client handle.
 * @param req             The request to run.
 * @param write_fn        The callback that gets each chunk. It must not be
 *                        NULL.
 * @param write_ctx       The client gives this value to write_fn without a
 *                        change.
 * @param status_code_out It gets the HTTP status code after a call that
 *                        succeeds. It can be NULL.
 * @return The same codes as chttpclient_do_pooled, except that the function
 *         never gives ccol_msg_too_large, because
 *         chttpclient_set_max_response_body_size has no effect on this
 *         streaming path (see the doc comment of that function).
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
 * @brief Give the default client of the process, which the library
 *        initializes only when it first needs it.
 *
 * This function is thread-safe. The default client uses the default
 * settings (pool_size is the CPU count, there are no timeouts, and TLS
 * verification is on), and you can configure it by giving the handle from
 * this function to a chttpclient_set_* function.
 *
 * Do NOT give this handle to chttpclient_destroy. This module owns the
 * default client and destroys it automatically at the exit of the process,
 * from a handler that it registers with atexit(3) the first time that it
 * builds the default client; a call of this function from an exit handler
 * that runs after that one gives CHTTPCLI_INVALID. A destroy by you does
 * not crash this function, because the module knows the handle and clears
 * its own reference to it, but every later call to chttp_default_client,
 * chttp_do, chttp_get and the other wrappers then has no default client and
 * fails for the rest of the life of the process, since there is no way to
 * build the default client again after such a destroy. If you need a client
 * whose life the caller bounds and controls, make your own client with
 * ccol_create_chttpclient or with ccol_create_chttpclient_mp.
 *
 * @return The handle of the default client, or CHTTPCLI_INVALID if the
 *         initialization fails.
 */
chttpcli chttp_default_client(void);

/**
 * @brief Do a request with the default client.
 *
 * It is the same as chttpclient_do(chttp_default_client(), req, resp_out).
 */
ccol_retval_t chttp_do(const chttp_request_t *req,
                       chttpcli_response **resp_out);

/**
 * @brief Run one request with the default client.
 *
 * This is a short wrapper that builds a chttp_request_t inside, attaches
 * @p headers without a transfer of ownership, calls chttp_do, and then
 * frees the request object that it built. The caller keeps the full
 * ownership of @p headers and must destroy that map when it is done with
 * it.
 *
 * @param method    The HTTP method.
 * @param url       The target URL, which must not be NULL.
 * @param body      The request body, or NULL for a request with no body.
 * @param headers   A chmap(char* -> char*) of the request headers, or NULL,
 *                  which the function borrows for the time of the call
 *                  instead of consuming it.
 * @param resp_out  If the call succeeds, it gets the pointer to the
 *                  response.
 * @return The same codes as chttpclient_do; the function gives
 *         ccol_not_enough_memory if the allocation of the internal request
 *         fails.
 */
ccol_retval_t chttp_run_query(chttp_method_t method, const char *url,
                              const chttp_request_body_t *body, chmap headers,
                              chttpcli_response **resp_out);

/**
 * @brief A GET request with the default client.
 *
 * @param url      The target URL.
 * @param resp_out It gets the pointer to the response after a call that
 *                 succeeds.
 * @return The same codes as chttpclient_do: ccol_success,
 *         ccol_invalid_args, ccol_not_enough_memory, ccol_timed_out and
 *         ccol_unexpected_failure, plus ccol_msg_too_large and
 *         ccol_not_permitted, and the codes of the ccol_http_ family that
 *         the work on the URL, on a redirect and on TLS can produce. See
 *         chttpclient_do for the full list that decides.
 */
ccol_retval_t chttp_get(const char *url, chttpcli_response **resp_out);

/**
 * @brief A POST request with the default client.
 *
 * @param url      The target URL.
 * @param body     The request body, or NULL for a POST with no body.
 * @param resp_out It gets the pointer to the response after a call that
 *                 succeeds.
 */
ccol_retval_t chttp_post(const char *url, const chttp_request_body_t *body,
                         chttpcli_response **resp_out);

/**
 * @brief A PUT request with the default client.
 */
ccol_retval_t chttp_put(const char *url, const chttp_request_body_t *body,
                        chttpcli_response **resp_out);

/**
 * @brief A DELETE request with the default client.
 */
ccol_retval_t chttp_delete(const char *url, chttpcli_response **resp_out);

/**
 * @brief A PATCH request with the default client.
 */
ccol_retval_t chttp_patch(const char *url, const chttp_request_body_t *body,
                          chttpcli_response **resp_out);

/* ========================================================================== */
/*                    RESPONSE API */
/* ========================================================================== */

/**
 * @brief Look up a response header by name.
 *
 * The lookup ignores the case of the name. A name that the server sent more
 * than once gives the combined value of RFC 9110 section 5.3: every
 * occurrence in the order of the wire, joined with ", ". Set-Cookie is not
 * combined, because RFC 9110 section 5.3 and RFC 6265 section 3 forbid it
 * (a cookie value can itself hold a comma), so for "set-cookie" this
 * function gives the FIRST occurrence; use chttpclient_resp_header_count
 * and chttpclient_resp_header_at to read every occurrence.
 *
 * @param resp  The response to query.
 * @param name  The header name, for example "Content-Type".
 * @return A pointer to the value string, or NULL if the header is not
 *         there. The pointer stays valid until a call to
 *         chttpclient_resp_free.
 */
const char *chttpclient_resp_header(const chttpcli_response *resp,
                                    const char *name);

/**
 * @brief Count the occurrences of a response header.
 *
 * The lookup ignores the case of the name. Every field line of the response
 * header block with that name counts once, whatever its value.
 *
 * @param resp  The response to query.
 * @param name  The header name, for example "Set-Cookie".
 * @return The number of field lines with that name, which is 0 when the
 *         header is not there, when resp or name is NULL, and for the
 *         response of a streaming request, which keeps no headers.
 */
size_t chttpclient_resp_header_count(const chttpcli_response *resp,
                                     const char *name);

/**
 * @brief Get one occurrence of a response header.
 *
 * The lookup ignores the case of the name. index counts the field lines with
 * that name in the order of the wire, from 0. Each value is the value of one
 * field line exactly as the server sent it, and never a combined value.
 *
 * @param resp   The response to query.
 * @param name   The header name, for example "Set-Cookie".
 * @param index  Which occurrence, from 0 to
 *               chttpclient_resp_header_count(resp, name) - 1.
 * @return A pointer to the value string, or NULL when index is not below
 *         that count, when resp or name is NULL, and for the response of a
 *         streaming request. The pointer stays valid until a call to
 *         chttpclient_resp_free.
 */
const char *chttpclient_resp_header_at(const chttpcli_response *resp,
                                       const char *name, size_t index);

/**
 * @brief Free a response and every resource that it owns.
 *
 * A call with NULL is safe.
 *
 * The response carries its own copy of the allocator procs that you gave
 * at the creation of the client, with ccol_create_chttpclient_mp, and does
 * not refer to the client or to the copy that the client owns, so this call
 * is valid before or after chttpclient_destroy of that client. The
 * functions that the procs name must remain callable, as for any
 * allocation that they made.
 */
void chttpclient_resp_free(chttpcli_response *resp);

#pragma GCC visibility pop
