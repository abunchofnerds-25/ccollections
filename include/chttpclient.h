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

/* Everything this header declares from here to the end belongs to the public
 * Application Binary Interface (ABI) of libccollections, and the shared
 * library exports all of it. The library is built with -fvisibility=hidden,
 * so a function or an object outside one of these blocks stays internal: its
 * name isn't in the library's dynamic symbol table, an application linking
 * against the library can't interpose it, and a symbol of the same name in
 * that application can't collide with it. */
#pragma GCC visibility push(default)

/**
 * @file chttpclient.h
 * @brief A hand-written HTTP/1.1 client. An internal chttp1_parser frames
 *        the request and the response over raw sockets, ctls (the same
 *        reactor-agnostic OpenSSL wrapper that chttpserver uses for its own
 *        TLS) handles TLS, and ccol_event_loop is the reactor of Tier 2 and
 *        Tier 3.
 *
 * ### Connection pool
 *
 * Each chttpcli handle has two independent layers. The first one limits
 * concurrency, so only so many requests are in flight at the same time:
 * chttpclient_do blocks once the client reaches that limit, which is the CPU
 * count unless chttpclient_set_pool_size says otherwise. The second one is a
 * keep-alive cache of idle connections, keyed by the origin (the scheme, the
 * host compared without regard to case, and the port). A request can reuse a
 * connection that an earlier request opened (and for HTTPS, one whose
 * handshake has already finished), so it doesn't pay for DNS resolution, the
 * TCP handshake or the TLS handshake a second time. Before each reuse the
 * client runs a cheap liveness probe, and a connection that the peer closed
 * is dropped and replaced without the caller doing anything. Idle
 * connections are bounded per origin and in total, and an idle connection
 * also expires after a short time. Once a cap is reached, the client closes a
 * finished connection instead of caching it; you lose the optimisation but
 * correctness is never affected.
 *
 * ### Default client
 *
 * chttp_default_client() gives you one default client for the whole process,
 * which the library initializes on the first call. You configure it with the
 * same chttpclient_set_* functions as any other client, and the short
 * wrappers (chttp_get, chttp_post and the others) use it for you.
 *
 * ### Thread safety
 *
 * Every function that takes a chttpcli handle is thread-safe, with the one
 * exception described in the next paragraph. Several threads can call
 * chttpclient_do, chttpclient_do_async or chttpclient_do_pooled on the same
 * client at the same time, and the same goes for the chttpclient_set_*
 * configuration functions. A chttpclient_destroy of that client may also run
 * concurrently with those calls; it waits for the requests that are already
 * in flight.
 *
 * The exception is one destroy against another: two chttpclient_destroy
 * calls that overlap on one handle stop the process with _ccol_fatal_err,
 * because the second call finds a handle that the first one already retired
 * and can't tell it apart from a stale handle of some other client. So the
 * destroy is the one operation that a caller must serialize, and it has to be
 * the last thing any thread of the caller does with that handle.
 *
 * This thread safety does NOT extend to changing one caller-owned
 * chttp_request_t or chttpcli_response object from more than one thread at
 * the same time, since neither object has any synchronization of its own:
 * chttp_request_set_header, chttp_request_get_header, chttpclient_resp_header
 * and direct field access all need exclusive access to that object. If you
 * share one request or one response between threads, you must add your own
 * external synchronization.
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
 * chttp_request_t *req = chttp_request_create(CHTTP_POST,
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
 * chttpcli is an opaque VALUE handle that packs a slot index and a
 * generation into one value. Never cast it to or from void*, never compare it
 * through a pointer cast, and never treat it as an address. Compare it
 * directly against CHTTPCLI_INVALID, or test it for truth: CHTTPCLI_INVALID
 * is 0, so `if (!cli)` asks the same question (is there a client here?).
 * Inside the library every use of a chttpcli goes through a slot table that
 * the library owns before anything touches the client object behind the
 * handle. That way the library always detects a handle whose slot is free
 * again, or whose slot belongs to a different, later client, and it never
 * dereferences freed memory or the memory of the wrong object. The doc
 * comment of chttpclient_destroy says what a stale handle does in that one
 * function.
 */
typedef uint64_t chttpcli;

/** @brief The sentinel value for "no client", which plays the role of NULL
 *  for a chttpcli. */
#define CHTTPCLI_INVALID ((chttpcli)0)

/* ========================================================================== */
/*                         STREAMING CALLBACK                                 */
/* ========================================================================== */

/**
 * @brief The streaming callback for a response body, for
 *        chttpclient_do_streaming.
 *
 * The client calls it zero or more times as the bytes of the response body
 * arrive. Return the number of bytes you consumed; anything less than len
 * aborts the transfer. For a Tier 2 or Tier 3 request no call starts after
 * the request's future is fulfilled, so the caller may free ctx once it has
 * the result. A callback must not destroy the client that runs its request;
 * see __chttpclient_destroy.
 *
 * @param data  The chunk that arrived. It is not NUL-terminated.
 * @param len   The number of bytes in this chunk.
 * @param ctx   The context pointer that the caller gave at the call site.
 * @return      The number of bytes you handled; it must equal len for the
 *              transfer to continue.
 */
typedef size_t (*chttpcli_write_fn)(const void *data, size_t len, void *ctx);

/* ========================================================================== */
/*                         REQUEST OBJECT                                     */
/* ========================================================================== */

/**
 * @brief An HTTP request, partially transparent.
 *
 * You can read and write the fields method, expect_continue and
 * prevent_tls_downgrade_on_redirect directly. The fields url and body are
 * public for reading only: the request owns copies of url and body.data (and
 * of body.content_type when the body names one), which
 * chttp_request_create_mp allocates with the request's allocator and
 * chttp_request_free frees with that same allocator. Don't assign to url or
 * to any member of body after construction; if you want to send another URL
 * or another body, build another request. The headers are internal and have
 * no iterator API of their own, so you set one header by name with
 * chttp_request_set_header and read one back by name with
 * chttp_request_get_header.
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
   *  of a response before it sends the body; a response that starts within
   *  that second is read to its end, bounded only by the request timeout.
   *  A "100 Continue" makes the client send the body, but a response that
   *  comes in the same read as the "100 Continue" is read first, and if it
   *  is a final one of 300 or above, or a complete one, the request ends
   *  without any byte of the body being sent. A final status that comes in
   *  place of the "100 Continue" is the answer: the client never sends the
   *  body and never reuses the connection. Any other interim status, such
   *  as "103 Early Hints", is dropped and the wait goes on, and once a
   *  second passes with no byte of a response the client sends the body
   *  anyway. The chttpclient_do(3) man page has the full protocol. The
   *  field has no effect when the request has no body, or when the caller
   *  already set an explicit "Expect" header. It defaults to false because
   *  chttp_request_create_mp fills it with zero. Every tier honors it the
   *  same way: chttpclient_do and chttpclient_do_streaming,
   *  chttpclient_do_async and chttpclient_do_async_streaming, and
   *  chttpclient_do_pooled and chttpclient_do_pooled_streaming (the last
   *  two sit on the async engine).
   *
   *  Only this field turns on the real 100-Continue wait; the client does
   *  not derive it from what goes on the wire. If you call
   *  chttp_request_set_header(req, "Expect", "100-continue") without
   *  setting this field, that literal header goes on the wire but the
   *  client does NOT wait for an interim response before the body. The
   *  header and the behaviour are fully independent by design, so you keep
   *  full manual control of the header's content on the wire whatever this
   *  field does. If you want the real protocol, you must set this field to
   *  true and must not set the header yourself; the client adds the header
   *  for you. */
  bool expect_continue;
  /** Set this field to true after construction, for example with
   *  req->prevent_tls_downgrade_on_redirect = true. The client then refuses
   *  to follow a redirect that would take this request from https to http,
   *  and reports ccol_http_invalid_url without opening the plaintext
   *  connection.
   *
   *  The default is false, since chttp_request_create_mp fills this field
   *  with zero, so by default a request follows such a redirect. That is
   *  what curl does (its CURLOPT_REDIR_PROTOCOLS permits both schemes) and
   *  what the Go net/http client does, and we match them so that a caller
   *  porting from either one isn't surprised.
   *
   *  Know what the default costs before you keep it. The peer that sent the
   *  Location picks the downgrade, not you, and once a chain leaves TLS that
   *  hop and every later hop travel in clear, so anyone who can see the
   *  traffic can read the request and the response and rewrite both. Set
   *  this field wherever a downgrade is not an acceptable outcome, which in
   *  most deployments means anywhere an https URL appears.
   *
   *  Some protection holds whatever this field says. An Authorization value
   *  that the library injected from a URL's userinfo is dropped as soon as
   *  the origin changes. The headers you set yourself follow the redirect
   *  rule of chttpclient_do: a hop whose scheme, host or port differs from
   *  the original request carries none of your Authorization, Cookie,
   *  Cookie2 and WWW-Authenticate headers, and a hop whose host differs
   *  carries none of your Host header either. All your other headers follow
   *  the redirect.
   *
   *  The AF_UNIX rule is not optional and this field doesn't touch it: a
   *  redirect can never add the http+unix transport and can never re-point
   *  it at another socket, whatever this field holds.
   *
   *  An UPGRADE from http to https is never affected; since it can only add
   *  protection, the client always follows it.
   *
   *  Every tier honours this field the same way: chttpclient_do and
   *  chttpclient_do_streaming, chttpclient_do_async and
   *  chttpclient_do_async_streaming, and chttpclient_do_pooled and
   *  chttpclient_do_pooled_streaming. */
  bool prevent_tls_downgrade_on_redirect;
} chttp_request_t;

/* ========================================================================== */
/*                         RESPONSE OBJECT                                    */
/* ========================================================================== */

/**
 * @brief An HTTP response, partially transparent.
 *
 * The fields status_code, body, body_len and headers are public.
 *
 * body is a NUL-terminated heap buffer, and it is never NULL when it holds
 * one byte or more; body_len counts the bytes before that terminating NUL.
 * body is NULL in two cases. One is a call to chttpclient_do_streaming, or
 * to its async or pooled equivalent, where the write callback gets the body
 * instead. The other is a zero-length body on the non-streaming path, such
 * as a 204, a response to HEAD or an explicit "Content-Length: 0": body_len
 * is 0 there, and since there's nothing to hold, the library doesn't
 * allocate an empty string. So you must check body != NULL, or
 * body_len > 0, before you dereference body, even after a request that
 * succeeded and did not stream.
 *
 * headers maps each field name of the response header block, lowercased,
 * to one value. A name that appears once maps to its value, and a name that
 * appears more than once maps to the combined value of RFC 9110 section
 * 5.3, which is every occurrence in wire order joined with ", ". Set-Cookie
 * is the one exception, because RFC 9110 section 5.3 and RFC 6265 section 3
 * forbid combining it: "set-cookie" maps to its FIRST occurrence.
 * chttpclient_resp_header reads that same value, while
 * chttpclient_resp_header_count and chttpclient_resp_header_at give you
 * each occurrence separately, and that's how you read every Set-Cookie.
 *
 * chttpclient_resp_free frees all the memory that the response owns. The
 * response doesn't depend on the client that produced it, so you can free
 * it before or after chttpclient_destroy; chttpclient_resp_free has the
 * details.
 */
typedef struct chttpcli_response {
  int status_code;
  char *body; /* heap buffer ending with a NUL. NULL on the streaming path,
               * and for a zero-length body on the non-streaming path. */
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
 * url is copied into a buffer that the request owns, and so are body->data
 * and body->content_type when they are not NULL, so the caller can free its
 * own originals immediately.
 *
 * @param method   The HTTP method, one of the seven concrete constants
 *                 CHTTP_GET to CHTTP_OPTIONS. CHTTP_ANY (a server route
 *                 wildcard) is refused, and so is every value outside the
 *                 enum.
 * @param url      The target URL, which gets copied. It must not be NULL.
 * @param body     The request body; pass NULL or &CHTTP_NO_BODY for a
 *                 method with no body. body->data may be NULL only when
 *                 body->len == 0, so a NULL body->data with a non-zero
 *                 body->len is rejected.
 * @param mprocs   A custom allocator, or NULL for malloc/free.
 * @param err_str  Optional; receives a static error string when the call
 *                 fails.
 * @return A new request, or NULL on failure: a NULL url, a method that
 *         names no HTTP method, a NULL body->data with a non-zero
 *         body->len, or a failed allocation.
 */
chttp_request_t *chttp_request_create_mp(chttp_method_t method, const char *url,
                                         const chttp_request_body_t *body,
                                         ccol_memmgmt_procs_t *mprocs,
                                         char **err_str);

/**
 * @brief Allocate and initialize an HTTP request (default allocator).
 */
static inline __attribute__((always_inline)) chttp_request_t *
chttp_request_create(chttp_method_t method, const char *url,
                     const chttp_request_body_t *body, char **err_str) {
  return chttp_request_create_mp(method, url, body, NULL, err_str);
}

/**
 * @brief Set a request header, or replace one.
 *
 * Header names are stored in lowercase, so a lookup with
 * chttp_request_get_header ignores the case of the name. When a header of
 * the same name already exists, its value gets replaced.
 *
 * A "Transfer-Encoding" header is always rejected. This client never
 * applies a transfer coding to a request body (a request with a body always
 * goes out whole, framed with Content-Length), so it can't obey a
 * Transfer-Encoding header from the caller, and it must not take one
 * silently either. If it did, the automatic Content-Length header of
 * chttpclient_do would sit beside it on the wire over a body with no
 * transfer coding; that framing is ambiguous, and this library's own
 * chttp1_parser rejects it in any message it parses.
 *
 * A "Content-Length" header IS accepted here, since there is no body length
 * to check it against yet, and checking it here would also need a copy of
 * the request's method and redirect context, which only come later. The
 * client validates it when the request reaches the wire: chttpclient_do,
 * chttpclient_do_streaming, chttpclient_do_async,
 * chttpclient_do_async_streaming, chttpclient_do_pooled,
 * chttpclient_do_pooled_streaming and chttp_run_query all reject a bad
 * Content-Length from the caller. A good Content-Length is a plain unsigned
 * string of decimal digits with no leading '+' or '-' and no whitespace in
 * it, and it must match exactly the body that the client sends, for the
 * same reason Transfer-Encoding is rejected above: the declared framing
 * must not disagree with the wire. The doc comment of chttpclient_do has
 * more.
 *
 * @param req    The request to change.
 * @param name   The header name, for example "Content-Type".
 * @param value  The header value.
 * @return ccol_success, ccol_invalid_args or ccol_not_enough_memory.
 *         ccol_invalid_args comes back when req, name or value is NULL,
 *         and also for an empty name, a name with a byte outside the
 *         RFC 7230 tchar set, a value with any control byte other than a
 *         horizontal tab (every byte below 0x20 except HTAB, so CR and LF
 *         too, and 0x7F), and the name "Transfer-Encoding".
 */
ccol_retval_t chttp_request_set_header(chttp_request_t *req, const char *name,
                                       const char *value);

/**
 * @brief Look up a request header by name, ignoring the case of the name.
 *
 * @param req   The request to query.
 * @param name  The header name.
 * @return The stored value string, or NULL when there is no such header.
 *         The pointer is valid until the next chttp_request_set_header
 *         call on this request.
 */
const char *chttp_request_get_header(const chttp_request_t *req,
                                     const char *name);

/**
 * @brief Free a request and everything it owns.
 *
 * Passing NULL is safe.
 */
void chttp_request_free(chttp_request_t *req);

/* ========================================================================== */
/*                    CLIENT CONSTRUCTORS */
/* ========================================================================== */

/**
 * @brief Create an HTTP client with a custom allocator.
 *
 * The new client starts with the default settings, and you change its
 * behaviour with the chttpclient_set_* functions before the first request.
 *
 * Until any chttpclient_set_* call, the configuration is:
 *   pool_size              = the CPU count (resolved on the first request)
 *   connect_timeout_us     = 0 (no timeout)
 *   request_timeout_us     = 0 (no timeout)
 *   max_response_body_size = 0 (no limit)
 *   TLS                    = peer and host verification on, system CA bundle
 *
 * @param mprocs   A custom allocator, or NULL for malloc/free.
 * @param err_str  Optional; receives a static error string when the call
 *                 fails.
 * @return A new client handle, or CHTTPCLI_INVALID on failure.
 */
chttpcli chttpclient_create_mp(ccol_memmgmt_procs_t *mprocs, char **err_str);

/**
 * @brief Create an HTTP client with the default allocator.
 *
 * @return A new client handle, or CHTTPCLI_INVALID on failure.
 */
static inline __attribute__((always_inline)) chttpcli
chttpclient_create(char **err_str) {
  return chttpclient_create_mp(NULL, err_str);
}

/* ========================================================================== */
/*                    CLIENT CONFIGURATION */
/* ========================================================================== */

/**
 * @brief Set the maximum number of requests that can be in flight at the
 *        same time.
 *
 * The cap applies ONLY to Tier 1 (chttpclient_do and
 * chttpclient_do_streaming): a caller of either function blocks once the
 * client reaches it and waits for a free slot, much like a counting
 * semaphore. It does NOT apply to Tier 2 (chttpclient_do_async and
 * chttpclient_do_async_streaming) and it does NOT apply to Tier 3
 * (chttpclient_do_pooled and chttpclient_do_pooled_streaming). You can
 * submit any number of Tier 2 and Tier 3 requests at the same time; the
 * shared async engine's reactor processes all of them and a submit never
 * blocks. That engine has its own process-wide thread-count setting (see
 * chttpcli_set_engine_num_reactor_threads), which this function doesn't
 * affect and which isn't a concurrency cap either, since it only decides how
 * many OS threads dispatch the work already in the queue. You may call this
 * function before or after the first request; either way it takes effect at
 * once, and for Tier 1 traffic only. An n of 0 selects the CPU count.
 *
 * It caps concurrency and nothing else, so it is independent of the
 * client's keep-alive cache of idle connections (described in the doc
 * comment at the top of this file). That cache has its own fixed internal
 * caps per origin and in total, and those DO apply to all three tiers.
 *
 * @param cli  The client handle.
 * @param n    The pool size. 0 means the CPU count.
 * @return ccol_success, or ccol_invalid_args when cli is CHTTPCLI_INVALID
 *         or a stale handle of a client that has been destroyed.
 */
ccol_retval_t chttpclient_set_pool_size(chttpcli cli, size_t n);

/**
 * @brief Set the TCP connect timeout in microseconds. 0 means no timeout.
 *
 * For an HTTPS request the TLS handshake shares one budget with the TCP
 * connect phase, so this timeout bounds the handshake as well.
 *
 * A name lookup is a single blocking getaddrinfo() call, and the system
 * gives no way to cancel it. chttpclient_do makes that call on the calling
 * thread, and if the lookup alone runs past this timeout, the connect that
 * would follow is skipped and the call gives ccol_timed_out once the lookup
 * returns. chttpclient_do_async and chttpclient_do_pooled give
 * ccol_timed_out at the deadline itself, while the lookup goes on in the
 * background and its result is thrown away. A request to an address literal
 * or to a unix socket makes no lookup at all.
 *
 * For chttpclient_do_async and chttpclient_do_pooled, an engine thread
 * watches the deadline and wakes at the earliest deadline among the
 * requests in flight, so a request ends within a few milliseconds of its
 * deadline, however short the timeout is.
 *
 * That deadline holds even while the engine can't run the request's work,
 * for example while its allocator fails. The request then ends with
 * ccol_timed_out at most about 100 ms after the deadline, and its connection
 * is closed in the background once the engine can run it again; until that
 * happens, chttpclient_destroy of that client waits for it.
 *
 * Any value other than 0 is honoured as a limit, however far away it is. A
 * value too large for the clock to reach, such as UINT64_MAX, saturates to
 * the latest deadline the clock can hold and never wraps into the past.
 *
 * @param cli  The client handle.
 * @param us   The timeout in microseconds. 0 means no timeout.
 * @return ccol_success, or ccol_invalid_args when cli is CHTTPCLI_INVALID
 *         or a stale handle of a client that has been destroyed.
 */
ccol_retval_t chttpclient_set_connect_timeout(chttpcli cli, uint64_t us);

/**
 * @brief Set the total request timeout in microseconds. 0 means no timeout.
 *
 * It is the longest time allowed from the call to chttpclient_do until the
 * last byte of the response body arrives.
 *
 * Every tier checks this deadline before each read of the response. Once
 * the deadline has passed the request ends with ccol_timed_out, even if the
 * rest of the response is already there and one more read would complete
 * it, but a response that the client finished reading before the deadline
 * is a success. chttpclient_do, chttpclient_do_async and
 * chttpclient_do_pooled, and their streaming forms, all give the same
 * result for the same timeline.
 *
 * A name lookup is one blocking getaddrinfo() call that the system has no
 * way to cancel. chttpclient_do runs it on the calling thread, and when the
 * lookup alone goes past this timeout, the call skips the connect that
 * follows and gives ccol_timed_out once the lookup returns.
 * chttpclient_do_async and chttpclient_do_pooled give ccol_timed_out right
 * at the deadline, and the lookup keeps running in the background with its
 * result unused. Requests to an address literal or a unix socket make no
 * lookup.
 *
 * chttpclient_do_async and chttpclient_do_pooled have an engine thread
 * watch the deadline, waking at the earliest deadline of the requests in
 * flight, so a request ends within a few milliseconds of its deadline
 * however short the timeout is.
 *
 * For those two the deadline holds even while the engine can't run the
 * request's work (say, because its allocator fails): the request ends with
 * ccol_timed_out at most about 100 ms after the deadline, and its
 * connection gets closed in the background once the engine can run it
 * again. chttpclient_destroy of that client waits for it until then.
 *
 * Any value other than 0 is honoured as a limit, however far off it is. A
 * value the clock can't reach, such as UINT64_MAX, saturates to the latest
 * deadline the clock can hold, and it never wraps into the past.
 *
 * @param cli  The client handle.
 * @param us   The timeout in microseconds. 0 means no timeout.
 * @return ccol_success, or ccol_invalid_args when cli is CHTTPCLI_INVALID
 *         or a stale handle of a client that has been destroyed.
 */
ccol_retval_t chttpclient_set_request_timeout(chttpcli cli, uint64_t us);

/**
 * @brief Cap the size of a buffered response body. 0 means no limit, and 0
 *        is the default.
 *
 * The cap covers every request path that buffers the body instead of
 * streaming it: chttpclient_do, chttpclient_do_async, the pooled-sync
 * wrappers on top of chttpclient_do_async, chttp_get, chttp_post,
 * chttp_put, chttp_delete, chttp_patch and chttp_run_query. It has no
 * effect on chttpclient_do_streaming, chttpclient_do_async_streaming or
 * chttpclient_do_pooled_streaming: a streaming caller controls its own
 * memory through the value its chttpcli_write_fn returns (anything less
 * than len aborts the transfer), so there is nothing for this cap to bound
 * on those paths.
 *
 * The cap is applied in two ways. When the Content-Length header alone
 * declares more than max_bytes, the response fails at once, before a single
 * byte of the body is read off the wire. A chunked body, or one that ends
 * when the connection closes, declares no length up front, so the client
 * counts it as it arrives and fails the request as soon as the body so far
 * goes past max_bytes. Either way the error is ccol_msg_too_large, which
 * chttpclient_do returns, the result of chttpclient_do_async holds, and
 * chttpclient_do_pooled returns. That connection is never reused: it is
 * dropped instead of going back into the pool, like after any other
 * malformed response.
 *
 * A redirect hop isn't subject to the cap, whatever the declared and the
 * real size of its body, because the client always drops the body of an
 * intermediate hop without buffering it (the chttpclient_do documentation
 * explains how redirects are followed). Only the body of the final response
 * that reaches the caller counts against max_bytes.
 *
 * You may call this function before or after the first request. It takes
 * effect at once for every request that follows and doesn't change a
 * request that is already in flight.
 *
 * @param cli        The client handle.
 * @param max_bytes  The largest buffered response body in bytes. 0 means no
 *                   limit.
 * @return ccol_success, or ccol_invalid_args when cli is CHTTPCLI_INVALID
 *         or a stale handle of a client that has been destroyed.
 */
ccol_retval_t chttpclient_set_max_response_body_size(chttpcli cli,
                                                     size_t max_bytes);

/**
 * @brief Set the TLS configuration of this client.
 *
 * Passing NULL for tls puts the defaults back: the server's chain and
 * hostname are verified against the system CA bundle, and no client
 * certificate is presented. A zero-initialised chttp_tls_config_t carries
 * the same defaults (see the field comment of insecure_skip_verify in
 * chttp.h).
 *
 * The system CA bundle is OpenSSL's default CA file, which is the file
 * named by the SSL_CERT_FILE environment variable or else the one OpenSSL
 * was built with (the variable is ignored in a set-user-ID or set-group-ID
 * process). The library reads it through a close-on-exec descriptor and
 * parses it once per process, the first time a client needs it, and every
 * later client shares those parsed certificates, so changing the file or
 * SSL_CERT_FILE after that point has no effect. The client falls back to
 * OpenSSL's hashed directory (SSL_CERT_DIR, or the directory OpenSSL was
 * built with) only when that file is missing or holds no certificate, and
 * in that case OpenSSL opens the directory's files itself during the
 * handshakes that need them, with descriptors that are not close-on-exec.
 *
 * A configuration that can't mean one single thing gets ccol_invalid_args,
 * for example only one of cert_path and key_path, or insecure_skip_verify
 * together with a ca_bundle_path.
 *
 * Whether cert_path, key_path and ca_bundle_path can be read is not checked
 * here; the client checks each path later, when an HTTPS request needs it.
 * So a path that doesn't exist is accepted here without an error, and it
 * gives ccol_http_tls_cert_load_failed from chttpclient_do or
 * chttpclient_do_streaming once a request needs it.
 *
 * cert_path and key_path go together. If you set one and leave the other
 * NULL you get ccol_invalid_args rather than "no client certificate",
 * because silently accepting it would let an mTLS deployment believe it
 * presents a client certificate when it never does.
 *
 * @param cli  The client handle.
 * @param tls  The TLS configuration to copy, or NULL to put the defaults
 *             back.
 * @return ccol_success, ccol_invalid_args or ccol_not_enough_memory.
 *         ccol_invalid_args is returned for a cli of CHTTPCLI_INVALID, a
 *         stale cli of a client that has been destroyed, exactly one of
 *         cert_path and key_path set, and insecure_skip_verify set together
 *         with a ca_bundle_path.
 */
ccol_retval_t chttpclient_set_tls(chttpcli cli, const chttp_tls_config_t *tls);

/* ========================================================================== */
/*                         ENGINE LOGGER                                      */
/* ========================================================================== */

/**
 * @brief Install a custom logger for the reactor-level events of the
 *        chttpclient async engine (Tier 2 and Tier 3).
 *
 * chttpclient_do_async, chttpclient_do_async_streaming and the pooled-sync
 * wrappers on top of them (chttpclient_do_pooled and
 * chttpclient_do_pooled_streaming) share one ccol_event_loop reactor for the
 * whole process and for every chttpcli instance in it, the default client
 * included, and the library starts it only when it is first needed. The
 * engine logger gets that reactor's diagnostics, such as a failed TLS
 * handshake or a connect error. It is separate from each client's logger,
 * and separate from the reactor of chttpsvr_set_engine_logger too, since
 * chttpserver and chttpclient each own a fully independent static reactor
 * and one process can run both at the same time.
 *
 * You can call this function at any time. It takes effect at once and
 * stays in force across every stop and restart of the engine until another
 * call replaces it. While no logger from this function is installed, the
 * library opens a fallback logger every time the engine starts, writing to
 * fd 2 at level CLOG_FATAL. The engine never logs its own diagnostics above
 * CLOG_INFO, so that fallback stays silent until you install a more
 * talkative logger here.
 *
 * Internally the function derives a logger from cl with clog_derive(), adds
 * the field component=http-client-engine, installs the derived logger and
 * closes the previous engine logger if there was one. The caller keeps
 * ownership of cl and may close it once this call returns, because the
 * derived logger shares cl's destination, which stays open as long as any
 * logger sharing it is open. The engine closes the derived logger when
 * another call replaces it, and at process exit or library unload if no
 * engine runs at that point.
 *
 * For a logger made by clog_create_from_fd or clog_create_from_fd_mp, "the
 * destination" means the logger state and not the file descriptor: such a
 * logger never owns its fd and no clog_close closes it (see
 * clog_create_from_fd_mp). The engine goes on writing to that fd after you
 * close cl, so the fd itself must stay open until another call to this
 * function replaces or releases the engine logger, or until the process
 * exits.
 *
 * Calling it with cl == CLOG_INVALID releases the installed engine logger:
 * the engine closes its derived logger and every later engine start uses
 * the fallback logger again. While the engine runs the release is refused
 * with ccol_not_permitted, since the engine logs through that logger, so
 * destroy every client and call chttpcli_engine_wait() first. Once a
 * release has returned ccol_success, the engine makes no further call
 * through that logger, and its destination, its file descriptor and cl's
 * allocator may go away.
 *
 * A cl other than CLOG_INVALID that names no open logger (it's closed or
 * stale) gives ccol_invalid_args and changes nothing, and the engine keeps
 * the logger it had.
 *
 * @param cl  The parent logger to derive from, or CLOG_INVALID to release
 *            the installed engine logger.
 * @return ccol_success, ccol_invalid_args when cl is closed or stale,
 *         ccol_not_enough_memory when the derived logger can't be
 *         allocated, or ccol_not_permitted for a release while the engine
 *         runs.
 */
ccol_retval_t chttpcli_set_engine_logger(clog cl);

/* ========================================================================== */
/*                    ENGINE MEMORY MANAGEMENT                                */
/* ========================================================================== */

/**
 * @brief Install custom memory management procs for the reactor-level
 *        allocations of the chttpclient async engine (Tier 2 and Tier 3).
 *
 * All chttpcli instances of the process share one ccol_event_loop reactor
 * for their Tier 2 and Tier 3 work. That reactor allocates its own memory
 * (the registration table, each connection's dispatch state, the worker
 * pool for name lookups and connects, and more) with the default allocator
 * unless you call this function with a non-NULL mp, which sends all of
 * those allocations to your procs instead, the same way the memory
 * management procs of every other module in this library work. An mp of
 * NULL puts the default behaviour back.
 *
 * Only the construction of the shared reactor is affected; the allocator of
 * a single chttpcli instance is configured separately with
 * chttpclient_create_mp.
 *
 * There are only two moments when you may call this function: before the
 * reactor has ever started in this process (so before the first call to
 * chttpclient_do_async or chttpclient_do_async_streaming anywhere), and
 * after the engine has stopped completely. The engine stops once every
 * async user of a chttpcli drops its reference and the automatic teardown
 * finishes, and chttpcli_engine_wait() blocks until that teardown is over.
 *
 * The engine allocates and frees through mp until its teardown ends, and
 * that teardown runs on its own thread after the last client lets go of the
 * engine. So mp, and the memory behind its function pointers, must stay
 * valid until chttpcli_engine_wait() returns after the last client is
 * destroyed. The default client from chttp_default_client() can hold the
 * engine through its Tier 2 and Tier 3 idle connections until the library's
 * exit handler destroys it, which means a process that uses the default
 * client for those tiers must keep mp valid until it exits.
 *
 * @param mp  The custom memory management procs, or NULL to put the default
 *            back. When mp is not NULL, all four function pointers in it
 *            must be set.
 * @return ccol_success, ccol_invalid_args when mp is not NULL but holds a
 *         NULL function pointer, or ccol_not_permitted while the engine
 *         runs (wait for it to stop completely first).
 */
ccol_retval_t chttpcli_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp);

/* ========================================================================== */
/*                    ENGINE REACTOR THREAD COUNT                             */
/* ========================================================================== */

/**
 * @brief Configure how many OS threads the chttpclient async engine's
 *        reactor (Tier 2 and Tier 3) uses for polling and dispatch.
 *
 * The shared reactor sizes itself to sysconf(_SC_NPROCESSORS_ONLN) by
 * default, or to 1 if that query fails. You get that default when you never
 * call this function and also when you call it with num_threads == 0, and a
 * positive num_threads overrides the automatic size and pins the reactor to
 * exactly that many OS threads. The num_reactor_threads rules of
 * ccol_event_loop_create_mp apply here as well (see cthreadcomm.h): 1 means
 * a single thread that polls and dispatches inline, and a larger value
 * means one dedicated poller thread plus (num_threads - 1) dispatch worker
 * threads.
 *
 * Only the construction of the shared reactor is affected, independently of
 * the settings of any single chttpcli instance.
 *
 * You may call this function at two moments only: before the reactor has
 * ever started in this process (before the first call to
 * chttpclient_do_async or chttpclient_do_async_streaming anywhere), or once
 * the engine has stopped completely. It stops when every async user of a
 * chttpcli has dropped its reference and the automatic teardown has
 * finished; chttpcli_engine_wait() blocks until that teardown is over.
 *
 * @param num_threads  The OS thread count you want for the reactor, or 0 to
 *                     go back to the automatic size.
 * @return ccol_success, or ccol_not_permitted while the engine runs (wait
 *         for it to stop completely first).
 */
ccol_retval_t chttpcli_set_engine_num_reactor_threads(size_t num_threads);

/* ========================================================================== */
/*                    ENGINE WAIT                                             */
/* ========================================================================== */

/**
 * @brief Block until the chttpclient async engine (Tier 2 and Tier 3) has
 *        stopped completely.
 *
 * The engine starts with the first Tier 2 or Tier 3 request and stops on
 * its own once no client holds it: the last request has ended and the last
 * idle connection of those tiers is closed, which chttpclient_destroy does
 * for its own client. The teardown then runs on an engine thread, after
 * chttpclient_destroy has returned. This function waits for that teardown
 * and joins the thread that ran it, so by the time it returns no engine
 * thread is running and the engine makes no further call to the allocator
 * of chttpcli_set_engine_mem_mgmt_procs or to the logger of
 * chttpcli_set_engine_logger. When no engine runs it returns at once.
 *
 * The wait lasts as long as any client holds the engine, whether that is a
 * client that hasn't been destroyed and has Tier 2 or Tier 3 idle
 * connections, a request of those tiers in flight, or the default client
 * from chttp_default_client() once it has used those tiers (the library's
 * exit handler destroys the default client). Destroy every such client
 * first.
 *
 * Called from an engine thread, meaning from the write_fn of
 * chttpclient_do_async_streaming or chttpclient_do_pooled_streaming, it
 * would wait for its own thread, so in that case it calls _ccol_fatal_err()
 * instead.
 */
void chttpcli_engine_wait(void);

/* ========================================================================== */
/*                    CLIENT DESTRUCTION */
/* ========================================================================== */

/**
 * @brief The internal destroy. Use the chttpclient_destroy macro instead.
 *
 * It waits until every request in flight has finished and then frees the
 * resources.
 *
 * cli must be a live handle: one that chttpclient_create,
 * chttpclient_create_mp or chttp_default_client gave you and that nothing
 * has destroyed yet. Anything else is a fatal error, and that covers a
 * stale handle that an earlier, finished call to this function destroyed,
 * a stale handle that another thread is destroying at the same moment, and
 * a forged or garbage value. For all of them this function calls
 * _ccol_fatal_err(), which calls abort() and raises SIGABRT, since that is
 * safer than risking a use-after-free or a double free, and it does so
 * whether the two destroy calls come one after the other or overlap in
 * time. The one exception is CHTTPCLI_INVALID (0), which is a silent
 * no-op, so it fits the idiom of chttpclient_destroy where a destroy sets
 * the handle to NULL.
 *
 * Destroying a client from inside one of its own requests is fatal too. A
 * call from the write_fn of chttpclient_do_streaming,
 * chttpclient_do_async_streaming or chttpclient_do_pooled_streaming on that
 * client would wait for ever for the very request that runs it, so the
 * function calls _ccol_fatal_err() instead, the same way a self-destroy of
 * a thread pool or an event loop does.
 *
 * Destroying ANY client on a thread of the async engine (from the write_fn
 * of chttpclient_do_async_streaming or chttpclient_do_pooled_streaming) is
 * fatal as well. The destroy waits for that client's Tier 2 and Tier 3
 * requests and for the teardown of its idle connections, and it's the
 * engine threads that run that work, so with one or two reactor threads
 * the thread that would do it is the one waiting and the destroy would
 * never return. Destroying another client from the write_fn of
 * chttpclient_do_streaming, which runs on the caller's thread, is an
 * ordinary destroy.
 */
void __chttpclient_destroy(chttpcli cli);

/**
 * @brief The RAII cleanup helper, used with _ccol_destructor.
 *
 * Calling it on a *pp that is already CHTTPCLI_INVALID is a safe no-op.
 * Calling it on a stale handle that isn't CHTTPCLI_INVALID, one whose
 * client something else has already destroyed, is the same fatal misuse
 * that __chttpclient_destroy describes.
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
 * The macro blocks until every request in flight has finished. Another
 * thread may be running a request at the same time as this call, and the
 * macro waits for it instead of cutting it off. Do not run this macro at
 * the same time as ANOTHER DESTROY of the same handle; that misuse is a
 * fatal error and not a silent race, as the doc comment of
 * __chttpclient_destroy explains.
 *
 * @note The macro evaluates cli exactly once, and it must be a modifiable
 * lvalue, such as a variable or an element of an array.
 */
#define chttpclient_destroy(cli)  \
  _ccol_chttpclient_destroy_impl( \
      cli, _ccol_uniq(__ccol_chttpcli_destroy_slot, __COUNTER__))

/* Internal. The body of chttpclient_destroy. slot is a name from
 * _ccol_uniq(), which lets the macro nest inside the argument of another
 * destroy macro without a -Wshadow warning. The argument is evaluated
 * exactly once. */
#define _ccol_chttpclient_destroy_impl(cli, slot) \
  do {                                            \
    __typeof__(cli) *slot = &(cli);               \
    __chttpclient_destroy(*slot);                 \
    *slot = CHTTPCLI_INVALID;                     \
  } while (0)

/* ========================================================================== */
/*                    LIFECYCLE MACROS */
/* ========================================================================== */

/** @brief Declare a client variable without giving it a value. */
#define chttpcli_declare(name) chttpcli name

/** @brief Declare a client variable that the library destroys on its own
 *  when the scope ends.
 */
#define chttpcli_declare_scoped(name) \
  chttpcli name _ccol_destructor(___chttpclient_destroy) = CHTTPCLI_INVALID;

/**
 * @brief Declare and initialize an HTTP client, and call _ccol_fatal_err
 *        if the creation fails.
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
#define chttpcli_construct(name)                                \
  chttpcli name = CHTTPCLI_INVALID;                             \
  do {                                                          \
    char *_clic_err = NULL;                                     \
    (name) = chttpclient_create(&_clic_err);                    \
    if (!(name)) {                                              \
      _ccol_fatal_err("chttpcli_construct('%s'): %s", #name,    \
                      _clic_err ? _clic_err : "unknown error"); \
    }                                                           \
  } while (0)

/**
 * @brief Declare and initialize an HTTP client that the library destroys
 *        on its own when the scope ends, and call _ccol_fatal_err if the
 *        creation fails.
 */
#define chttpcli_construct_scoped(name)                                      \
  chttpcli name _ccol_destructor(___chttpclient_destroy) = CHTTPCLI_INVALID; \
  do {                                                                       \
    char *_clic_err = NULL;                                                  \
    (name) = chttpclient_create(&_clic_err);                                 \
    if (!(name)) {                                                           \
      _ccol_fatal_err("chttpcli_construct_scoped('%s'): %s", #name,          \
                      _clic_err ? _clic_err : "unknown error");              \
    }                                                                        \
  } while (0)

/* ========================================================================== */
/*                    REQUEST EXECUTION */
/* ========================================================================== */

/**
 * @brief Do an HTTP request and buffer the whole response body.
 *
 * The function blocks until a pool slot is free, runs the request
 * synchronously and gives back a response on the heap, which the caller
 * owns and must free with chttpclient_resp_free when done. *resp_out is set
 * to NULL right away, before any other work, and it stays NULL on every
 * return that isn't a success. So you can always call
 * chttpclient_resp_free(resp) after this call, whatever ccol_retval_t it
 * returned, without initializing your own local pointer first.
 *
 * Redirects are followed automatically. The redirect codes are 301, 302,
 * 303, 307 and 308, and the client follows up to 50 hops. For 301, 302 and
 * 303 the method becomes a GET with no body, except that HEAD stays HEAD
 * as the RFC asks; for 307 and 308 the original method is kept and the
 * original body is sent again unchanged. The Location header may hold an
 * absolute URL, a protocol-relative reference ("//host/path"), an
 * absolute-path reference ("/foo") or a general relative reference such as
 * "foo", "../foo", "./foo" or "?query", and all of them are resolved the
 * way RFC 3986 says. A Location value with its own scheme (for example
 * "mailto:x@y", "ftp://host/path", or any scheme other than http, https
 * and http+unix) is always absolute, because RFC 3986 SS5.2.2 says a
 * reference with a scheme is never relative, even if this client can't
 * fetch that scheme. Such a value resolves to itself unchanged and the next
 * hop reports ccol_http_invalid_url, which is the code an unsupported
 * scheme in the original request URL gets; the client doesn't merge it
 * onto the path of the current origin as if it were relative. A response
 * with a redirect code but no Location header, or an empty one, isn't a
 * redirect and is delivered as the final response, as the Go net/http
 * client does. When a response has more than one Location line, the client
 * goes to the first one, like curl and the Go net/http client.
 *
 * An empty port in a URL, as in "http://host:/", means the default port of
 * the scheme (RFC 3986 section 3.2.3). A hostname with one trailing dot,
 * such as "https://www.example.com./", is resolved and sent in the Host
 * header as written, while TLS uses the name without the dot for SNI and
 * for the certificate check, as curl and Go do.
 *
 * A redirect from https to http is followed, which is what curl and the Go
 * net/http client do. To refuse it, set the
 * prevent_tls_downgrade_on_redirect field of chttp_request_t, and the
 * client then reports ccol_http_invalid_url without opening any plaintext
 * connection. A Location that only inherits the scheme, like "//host/path"
 * or "/path", resolves back to https and is followed either way, and so is
 * an upgrade from http to https. The field comment of
 * prevent_tls_downgrade_on_redirect explains what the default costs.
 *
 * The AF_UNIX rule below is NOT optional and no field relaxes it.
 *
 * A hop may name a Unix domain socket ("http+unix://...") only if the hop
 * that carried the Location was bound to that same socket path. Every other
 * resolved http+unix:// target is refused with ccol_http_invalid_url and no
 * connection is tried. The rule is checked on every hop against the hop
 * the Location came from, so a chain that leaves a socket can't come back
 * to it and a chain that never used a socket can't reach one, however long
 * the chain is. The scheme is matched without regard to case and the
 * socket path is compared after its percent escapes are decoded, so a
 * different spelling or a double encoding doesn't get past the rule. A
 * relative Location on a Unix hop resolves back onto that hop's socket and
 * is followed normally. Without this rule any http or https server could
 * send a caller onto any local socket while also controlling the target of
 * the request.
 *
 * If the client hits the cap of 50 hops and the last hop's response is
 * itself a redirect, that response is neither followed nor delivered, and
 * the call gives ccol_http_too_many_redirects.
 *
 * The request URL can be http://, https:// or http+unix://. http+unix://
 * connects to a server listening on a Unix domain socket, for example
 * "http+unix://%2Fvar%2Frun%2Fapp.sock/api/users"; the section "Unix
 * domain sockets" of chttpclient(7) describes the full scheme.
 * "https+unix://" is not supported.
 *
 * The two network forms take three kinds of host: a plain hostname, an
 * IPv4 literal, and an IPv6 literal in brackets such as
 * "https://[::1]:8443/path". The host must match the uri-host rule of
 * RFC 3986 section 3.2.2, which allows:
 * - letters, digits, '-', '.', '_' and '~'
 * - the sub-delims !$&'()*+,;=
 * - "%XX" escapes
 * - an IPv6 address or an IPvFuture literal in brackets.
 *
 * Any other character (a space, a control byte, a byte of 0x80 or more, a
 * backslash and so on) makes the URL invalid with ccol_http_invalid_url.
 * The rule covers the caller's URL and every Location. A host name that
 * isn't ASCII has to be written in its punycode (IDNA) form, and an IPv6
 * zone identifier is not supported.
 *
 * A URL can carry credentials ("http://user:pass@host/path"). They may use
 * the characters of the RFC 3986 userinfo rule, "%XX" escapes and an
 * unescaped '@' in the password, and every other byte must be
 * percent-encoded. The client turns them into an "Authorization: Basic
 * ..." header, unless the request sets its own Authorization header or the
 * URL is "http+unix://", which has no accepted convention for userinfo.
 * That automatic header is sent again on each redirect hop that stays on
 * the same origin, an origin being a scheme, a host and a port (the host
 * is compared without regard to case, the scheme and the port exactly).
 * Once a hop changes the origin, the header is dropped for the rest of the
 * chain.
 *
 * Headers that the caller sets with chttp_request_set_header follow a
 * redirect with two exceptions, which are curl's rules, and every hop is
 * judged against the ORIGINAL request, never against the hop before it. A
 * hop whose scheme, host or port differs from the original request carries
 * none of the caller's Authorization, Cookie, Cookie2 and WWW-Authenticate
 * headers, and a hop whose host differs doesn't carry the caller's Host
 * header either; the client writes its own Host line for the new target
 * instead. Hosts and header names are matched without regard to case. So a
 * hop that returns to the original origin carries those headers again,
 * while a second hop on the same foreign origin carries none of them. All
 * other headers of the caller follow every hop.
 *
 * The request-target of the request line is built from the path and query
 * of the URL, and of every Location that is followed. Every byte that
 * RFC 3986 doesn't allow there literally is percent-encoded as "%XX": the
 * space, the control bytes, DEL, every byte of 0x80 and above, and the
 * characters '"', '<', '>', '\', '^', '`', '{', '|', '}', '[' and ']'. An
 * existing "%XX" escape is kept as it is and never encoded twice, but a
 * '%' that isn't followed by two hex digits makes the URL invalid
 * (ccol_http_invalid_url), both in the caller's URL and in a Location, as
 * Go's net/url does for the same input. A "#fragment" at the end of a URL
 * is found and dropped, so no fragment ever goes to a server.
 *
 * When a host name resolves to several addresses, the client connects with
 * Happy Eyeballs (RFC 8305) like Go's default dialer. The addresses are
 * taken in the order the resolver gives (the order of RFC 6724) with the
 * address families alternating, starting with the family of the first
 * address. The first address starts at once, and the next one starts when
 * the attempts in flight have given no result for 250 ms, or at once when
 * an attempt fails. The first attempt that connects wins and every other
 * one is closed, and ccol_http_connection_failed is reported only after
 * every address has failed. The connect timeout is one budget for the
 * whole race, and when it runs out the connect ends with ccol_timed_out.
 * So an address that drops its packets costs about 250 ms instead of the
 * whole connect timeout, and the name "localhost", which a standard
 * /etc/hosts lists as ::1 before 127.0.0.1, reaches a server that listens
 * on 127.0.0.1 only. Every tier starts the next address 250 ms after the
 * one before it.
 *
 * A server may answer before it has read the whole request (a 401 from a
 * credentials check or a 413 from a size limit, for example) and then
 * close the connection or stop reading, so every tier watches for such an
 * answer while sending the request and returns it. An interim (1xx)
 * response other than a 101 is dropped and the send goes on. A final
 * response of 300 or above stops the send once its header block has
 * arrived, while a final response of 200 to 299 lets the send go on as the
 * response arrives and a complete response stops it. If a write fails
 * because the peer went away, the send stops and the client returns
 * whatever answer the peer sent before going, if there is one. A
 * connection whose response started before its request was fully sent is
 * never reused. Because a reset throws away what the client hasn't read
 * yet, an answer is returned only if the client reads it before a reset
 * arrives. This function waits for
 * readability during every wait of the send, and chttpclient_do_async and
 * chttpclient_do_pooled look for an answer whenever a write would block,
 * and after a write once 32 KiB or more have gone out since the last look.
 *
 * Every socket of the client is close-on-exec, so a process the
 * application spawns inherits none of its connections.
 *
 * @param cli       The client handle.
 * @param req       The request to run.
 * @param resp_out  Set to NULL first; on success it then gets a
 *                  pointer to the response.
 * @return ccol_success
 *             The request finished and *resp_out is valid.
 *         ccol_invalid_args
 *             An argument is NULL; or cli is CHTTPCLI_INVALID, or a stale
 *             handle of a client that has been destroyed; or req sets a
 *             "Transfer-Encoding" header (the doc comment of
 *             chttp_request_set_header explains why the client always
 *             rejects that header); or req sets a bad "Content-Length"
 *             header on a request that carries a body (POST, PUT or
 *             PATCH). A good value is a plain unsigned string of decimal
 *             digits, with no leading '+' or '-' and no whitespace in it,
 *             and it must match the real length of the body the client
 *             sends. A declared length that is malformed or doesn't match
 *             is the same hazard that rejecting Transfer-Encoding
 *             prevents: the framing disagrees with the wire, only through
 *             a wrong length instead of a wrong transfer coding. There is
 *             no such check for a request without a body (GET, DELETE,
 *             HEAD or OPTIONS), because the client removes every
 *             Content-Length header from the wire for it; with no framing
 *             left there, a mismatched value can't desync anything.
 *         ccol_not_enough_memory
 *             An allocation failed.
 *         ccol_timed_out
 *             The request timeout or the connect timeout fired.
 *         ccol_not_permitted
 *             The client is being destroyed.
 *         ccol_http_invalid_url
 *             The URL is malformed; or its scheme is unsupported (only
 *             http://, https:// and http+unix:// are supported); or its
 *             host, port or userinfo part is missing or invalid. That
 *             includes a host outside the uri-host rule (see above), any
 *             byte outside the userinfo rule in the credentials, a CR or
 *             LF byte in the path or the query, and a '%' in the path or
 *             the query that isn't followed by two hex digits. Without
 *             this check such a byte would be copied onto the wire, and
 *             the URL could inject extra header lines or smuggle a second
 *             request. Or, for http+unix://, the socket path in the URL is
 *             too long for sockaddr_un.sun_path. The client also gives
 *             this code for a redirect that resolves to a http+unix://
 *             target when the hop that carried the Location wasn't bound
 *             to that target; see above.
 *         ccol_http_host_resolution_failed
 *             The DNS resolution of the target host failed. http+unix://
 *             has no DNS step, so it never gives this code.
 *         ccol_http_connection_failed
 *             The connection couldn't be made: every address of the host
 *             refused it or failed, or a http+unix:// socket path doesn't
 *             exist. A socket error on a connection that is already made
 *             is ccol_http_transfer_aborted instead.
 *         ccol_http_too_many_redirects
 *             The redirect chain went past 50 hops.
 *         ccol_http_tls_handshake_failed
 *             The TLS handshake failed for a reason other than certificate
 *             verification.
 *         ccol_http_tls_cert_verification_failed
 *             The peer's certificate or its hostname couldn't be verified.
 *         ccol_http_tls_cert_load_failed
 *             The client certificate, the key or the CA bundle path of the
 *             configuration couldn't be read, or ctls couldn't load or
 *             parse one of them.
 *         ccol_http_transfer_aborted
 *             The connection failed in the middle of the transfer, or the
 *             server sent a malformed HTTP/1.1 response. A response whose
 *             body ends at the close of the connection (no Content-Length
 *             and no chunked framing) ends there over https:// too when
 *             the server closes without a TLS close_notify alert, which
 *             curl and the Go net/http client accept; only a close in the
 *             middle of a TLS record gives this code. Such a body can't be
 *             told apart from one that a failure cut at a record boundary,
 *             so a server whose responses must arrive whole sends a
 *             Content-Length or chunked framing. A connection that fails
 *             while the request is being sent gives this code only when no
 *             answer of the server was found (see above). A "101
 *             Switching Protocols" response gives it too, because this
 *             client never asks for an upgrade and the bytes after a 101
 *             belong to another protocol.
 *         ccol_msg_too_large
 *             The response body went past the cap of
 *             chttpclient_set_max_response_body_size (see the doc comment
 *             of that function). You only get this code once you have set
 *             that cap.
 *         ccol_unexpected_failure
 *             Any other internal failure that the codes above don't
 *             cover.
 */
ccol_retval_t chttpclient_do(chttpcli cli, const chttp_request_t *req,
                             chttpcli_response **resp_out);

/**
 * @brief Do an HTTP request and stream the response body.
 *
 * write_fn is called one or more times with chunks of the response body as
 * they arrive. This path doesn't give you the response headers; if
 * status_code_out isn't NULL, it gets the HTTP status code after a
 * successful call.
 *
 * @param cli             The client handle.
 * @param req             The request to run.
 * @param write_fn        The callback that gets each chunk. It must not be
 *                        NULL.
 * @param write_ctx       Passed to write_fn unchanged.
 * @param status_code_out Gets the HTTP status code after a successful
 *                        call. It can be NULL.
 * @return The same codes as chttpclient_do, with two differences. You
 *         never get ccol_msg_too_large here, since
 *         chttpclient_set_max_response_body_size has no effect on this
 *         streaming path (see the doc comment of that function). And you
 *         get ccol_http_transfer_aborted when write_fn returns a value
 *         other than len, which aborts the transfer; that is the contract
 *         of chttpcli_write_fn, which covers a value above len as well as
 *         one below it.
 */
ccol_retval_t chttpclient_do_streaming(chttpcli cli, const chttp_request_t *req,
                                       chttpcli_write_fn write_fn,
                                       void *write_ctx, int *status_code_out);

/* ========================================================================== */
/*                         ASYNC API (TIER 2)                                 */
/* ========================================================================== */

/**
 * @brief The result of a request submitted with chttpclient_do_async or
 *        chttpclient_do_async_streaming.
 *
 * rv holds the same result codes as chttpclient_do (see that function's
 * documentation), ccol_msg_too_large included when the response goes past
 * the cap of chttpclient_set_max_response_body_size. The one exception is
 * a request submitted with chttpclient_do_async_streaming, which is never
 * under that cap, exactly like chttpclient_do_streaming, so it never gives
 * ccol_msg_too_large. resp is not NULL only when rv == ccol_success. Free
 * resp with chttpclient_resp_free before you free this result, the same
 * order as for the resp_out of chttpclient_do. A request from
 * chttpclient_do_async_streaming also gets resp filled after a success, so
 * status_code reads the same way from a streaming and a non-streaming
 * result. But there resp->headers is always NULL and resp->body_len is
 * always 0, because the response headers are dropped instead of kept and
 * the write callback has already received the body as it arrived, with
 * nothing buffering it. That follows the convention of
 * chttpclient_do_streaming, where chttpcli_response.body is NULL on the
 * streaming path.
 *
 * You get this result with chttpclient_async_result_get, a thin typed
 * wrapper over ctpool_future_get, and you free it with
 * chttpclient_async_result_free before calling ctpool_future_free on the
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
 * This function doesn't block. It puts the request in the queue of the
 * shared reactor engine of chttpclient and returns at once. The library
 * starts that engine the first time it's needed, and the engine is
 * independent of the synchronous connection work of chttpclient_do and of
 * chttpserver's engine (see "Async engine" below). The whole request and
 * response cycle, every redirect hop included, runs on the engine's
 * threads.
 *
 * @param cli The client handle.
 * @param req The request to run. Unlike with chttpclient_do, req doesn't
 *            have to stay valid after this call returns, because the
 *            library copies or serializes everything it needs before
 *            returning.
 * @return A future, or NULL if the request can't even be queued: when req
 *         is NULL, when cli is CHTTPCLI_INVALID or a stale handle of a
 *         client that has been destroyed, for a malformed URL, for TLS
 *         that can't be used, when memory runs out, and when the engine
 *         fails to start. On success the caller owns the future and must
 *         call chttpclient_async_result_get, then
 *         chttpclient_async_result_free, and then exactly one
 *         ctpool_future_free.
 *
 * ### Async engine
 *
 * The first call to chttpclient_do_async or
 * chttpclient_do_async_streaming anywhere in the process starts a small
 * shared pool of reactor threads sized to the CPU count, plus a companion
 * worker pool of the same size for name lookups, which block. A request to
 * an address literal or to a unix socket makes no lookup and never waits
 * for that pool, so lookups that hang don't delay such requests. If a
 * request's lookup outlives its connect or request timeout, the request
 * ends with ccol_timed_out at its deadline and the lookup goes on in the
 * background; until it returns, chttpclient_destroy of that client waits
 * for it and the exit handler below counts it as a request in flight. The
 * same goes for a request whose deadline passes while the engine can't run
 * its work (see chttpclient_set_request_timeout). The engine is
 * refcounted: it stops on its own once no request is in flight and no
 * connection is left in the async idle pool of any chttpcli, and it starts
 * again on the next call without the caller doing anything. This engine
 * owns its own static reactor, fully separate from chttpserver's
 * independent reactor and from the synchronous connection work of
 * chttpclient_do. So one process can run chttpserver together with
 * chttpclient_do_async and chttpclient_do_async_streaming, and can mix
 * chttpclient_do with those two functions, without any limits, because the
 * two engines share no state at all.
 *
 * The engine's threads are stopped before the process exits. The library
 * registers a handler with atexit(3) after OpenSSL's, so that ours runs
 * first. It waits for a stop that is in progress, destroys the default
 * client when no call is running on it, and stops an engine that nothing
 * holds, so no engine thread is left to race OpenSSL's cleanup. Two cases
 * keep the engine running at exit, and then the handler does nothing: a
 * request in flight (an exit() from inside a write_fn, or a request whose
 * lookup hasn't returned, counts as one), and a client that the
 * application never destroyed, whose pooled connections hold the engine.
 * Destroy every client before the process returns from main.
 *
 * Neither this engine nor any other part of the client touches the
 * disposition of SIGPIPE, and no write of the client raises it: every
 * write to a socket, TLS records and the close_notify of a TLS teardown
 * included, is a send(2) with MSG_NOSIGNAL.
 */
ctpool_future *chttpclient_do_async(chttpcli cli, const chttp_request_t *req);

/**
 * @brief Submit an HTTP request for asynchronous execution, and stream the
 *        response body.
 *
 * The submit works as in chttpclient_do_async and doesn't block, but
 * write_fn is called one or more times with chunks of the response body as
 * they come off the wire, as in chttpclient_do_streaming.
 *
 * write_fn runs on one of the engine's reactor threads, NOT on the
 * caller's thread and NOT on a thread of its own for this request. Since
 * the callback of chttpclient_do_streaming runs on the caller's thread,
 * this path comes with two hard rules. First, write_fn must not block: no
 * blocking I/O, no lock held for a long time, and no wait on the future of
 * another request, because blocking there stalls every other connection
 * that the engine multiplexes on that reactor thread. Second, write_fn
 * must not call chttpclient_do_async or chttpclient_do_async_streaming
 * again, and must not call anything that waits on the future of this same
 * request, directly or indirectly, whether for the same chttpcli or for
 * another chttpcli sharing the engine, because such a call can deadlock
 * against the reactor thread that runs write_fn.
 *
 * @param cli       The client handle.
 * @param req       The request to run; see chttpclient_do_async.
 * @param write_fn  The callback that gets each chunk. It must not be NULL.
 *                  If it returns a value other than len the transfer is
 *                  aborted, following the contract of chttpcli_write_fn
 *                  (which covers a value above len as well as one below
 *                  it), and the future's result then holds
 *                  ccol_http_transfer_aborted.
 * @param write_ctx Passed to write_fn unchanged.
 * @return A future, or NULL in the same cases as chttpclient_do_async,
 *         plus a NULL write_fn.
 */
ctpool_future *chttpclient_do_async_streaming(chttpcli cli,
                                              const chttp_request_t *req,
                                              chttpcli_write_fn write_fn,
                                              void *write_ctx);

/**
 * @brief Block until the future of an async request is fulfilled, then
 *        give its typed result.
 *
 * A thin wrapper over ctpool_future_get that casts its void* result to
 * chttpcli_async_result_t*. As in the contract of ctpool_future_get, it's
 * safe to call it more than once on the same future: every call after the
 * first gives the same result pointer, which the future owns until it is
 * freed.
 *
 * @param f The future from chttpclient_do_async or
 *          chttpclient_do_async_streaming.
 * @return The result, or NULL if f is NULL or if the future was cancelled
 *         before it was fulfilled.
 */
chttpcli_async_result_t *chttpclient_async_result_get(ctpool_future *f);

/**
 * @brief Free a chttpcli_async_result_t that chttpclient_async_result_get
 *        gave you.
 *
 * This function does NOT free result->resp, so if rv == ccol_success free
 * that first, on its own, with chttpclient_resp_free. It does NOT free the
 * future either; pair it with exactly one ctpool_future_free, called
 * separately before or after this call, in either order. The result keeps
 * its own copy of the client's allocator, which makes this call valid
 * before or after chttpclient_destroy of that client.
 *
 * @param result The result to free. NULL is a safe no-op.
 */
void chttpclient_async_result_free(chttpcli_async_result_t *result);

/* ========================================================================== */
/*                    POOLED-SYNC API (TIER 3)                                */
/* ========================================================================== */

/**
 * @brief Do an HTTP request on the shared Tier 2 engine and block until it
 *        finishes.
 *
 * A thin wrapper over chttpclient_do_async: it submits the request to the
 * shared engine and blocks until the request is done. It gives the same
 * ccol_retval_t and the same resp_out call shape as chttpclient_do, but the
 * connect, the write and the read run on the engine's reactor threads and
 * not on the caller's thread. So many callers across many chttpcli handles
 * share one small, fixed-size pool of reactor threads, while with
 * chttpclient_do every caller blocks its own OS thread for the whole
 * request.
 *
 * @param cli      The client handle.
 * @param req      The request to run.
 * @param resp_out It must not be NULL. It is set to NULL at once and
 *                 gets the response on success, the same *resp_out
 *                 contract as in chttpclient_do.
 * @return The same result codes as chttpclient_do. A failed allocation,
 *         whether before the request reaches the engine or while the
 *         engine runs it, gives ccol_not_enough_memory, and an engine that
 *         fails to start gives the code of that failure
 *         (ccol_not_enough_memory or ccol_unexpected_failure). A bad URL,
 *         a TLS failure, a connection failure, a transfer error, a timeout
 *         and too many redirects each give the same specific code as in
 *         chttpclient_do.
 */
ccol_retval_t chttpclient_do_pooled(chttpcli cli, const chttp_request_t *req,
                                    chttpcli_response **resp_out);

/**
 * @brief Do an HTTP request on the shared Tier 2 engine, stream the
 *        response body, and block until the request finishes.
 *
 * A thin wrapper over chttpclient_do_async_streaming, with the call shape
 * of chttpclient_do_streaming. write_fn runs on one of the engine's
 * reactor threads instead of the caller's thread, so the two rules in the
 * documentation of chttpclient_do_async_streaming apply: write_fn must not
 * block, and it must not call back into the engine. Which thread runs
 * write_fn is the only difference from chttpclient_do_streaming, whose
 * callback runs on the caller's thread.
 *
 * @param cli             The client handle.
 * @param req             The request to run.
 * @param write_fn        The callback that gets each chunk. It must not be
 *                        NULL.
 * @param write_ctx       Passed to write_fn unchanged.
 * @param status_code_out Gets the HTTP status code after a successful
 *                        call. It can be NULL.
 * @return The same codes as chttpclient_do_pooled, except that you never
 *         get ccol_msg_too_large, because
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
 * @brief Give the process's default client, which the library initializes
 *        the first time it's needed.
 *
 * This function is thread-safe. The default client has the default
 * settings (pool_size is the CPU count, no timeouts, TLS verification on),
 * and you can configure it by passing the handle from this function to a
 * chttpclient_set_* function.
 *
 * Do NOT pass this handle to chttpclient_destroy. The module owns the
 * default client and destroys it on its own when the process exits, from a
 * handler that it registers with atexit(3) the first time it builds the
 * default client; calling this function from an exit handler that runs
 * after that one gives CHTTPCLI_INVALID. If you destroy it anyway this
 * function doesn't crash, since the module knows the handle and clears its
 * own reference to it. But every later call to chttp_default_client,
 * chttp_do, chttp_get and the other wrappers then finds no default client
 * and fails for the rest of the process's life, because there is no way
 * to build the default client again after such a destroy. If you need a
 * client whose lifetime you bound and control yourself, make your own with
 * chttpclient_create or chttpclient_create_mp.
 *
 * @return The handle of the default client, or CHTTPCLI_INVALID if its
 *         initialization fails.
 */
chttpcli chttp_default_client(void);

/**
 * @brief Do a request with the default client.
 *
 * Same as chttpclient_do(chttp_default_client(), req, resp_out).
 */
ccol_retval_t chttp_do(const chttp_request_t *req,
                       chttpcli_response **resp_out);

/**
 * @brief Run one request with the default client.
 *
 * A short wrapper that builds a chttp_request_t internally, attaches
 * @p headers without taking ownership of it, calls chttp_do and then frees
 * the request object it built. The caller keeps full ownership of
 * @p headers and has to destroy that map when it's done with it.
 *
 * @param method    The HTTP method.
 * @param url       The target URL. It must not be NULL.
 * @param body      The request body, or NULL for a request without a body.
 * @param headers   A chmap(char* -> char*) of request headers, or NULL. The
 *                  function borrows it for the duration of the call and
 *                  doesn't consume it.
 * @param resp_out  Gets the pointer to the response on success.
 * @return The same codes as chttpclient_do. The function gives
 *         ccol_invalid_args for a method that names no HTTP method
 *         (CHTTP_ANY included) and for a body with NULL data and a
 *         non-zero len, since chttp_request_create refuses both, and
 *         ccol_not_enough_memory if allocating the internal request
 *         fails.
 */
ccol_retval_t chttp_run_query(chttp_method_t method, const char *url,
                              const chttp_request_body_t *body, chmap headers,
                              chttpcli_response **resp_out);

/**
 * @brief A GET request with the default client.
 *
 * @param url      The target URL.
 * @param resp_out Gets the pointer to the response after a successful
 *                 call.
 * @return The same codes as chttpclient_do: ccol_success,
 *         ccol_invalid_args, ccol_not_enough_memory, ccol_timed_out and
 *         ccol_unexpected_failure, plus ccol_msg_too_large and
 *         ccol_not_permitted, and the ccol_http_ codes that handling the
 *         URL, a redirect or TLS can produce. The full list in
 *         chttpclient_do is the one that decides.
 */
ccol_retval_t chttp_get(const char *url, chttpcli_response **resp_out);

/**
 * @brief A POST request with the default client.
 *
 * @param url      The target URL.
 * @param body     The request body, or NULL for a POST without a body.
 * @param resp_out Gets the pointer to the response after a successful
 *                 call.
 * @return The same codes as chttp_run_query with CHTTP_POST.
 */
ccol_retval_t chttp_post(const char *url, const chttp_request_body_t *body,
                         chttpcli_response **resp_out);

/**
 * @brief A PUT request with the default client.
 *
 * @return The same codes as chttp_run_query with CHTTP_PUT.
 */
ccol_retval_t chttp_put(const char *url, const chttp_request_body_t *body,
                        chttpcli_response **resp_out);

/**
 * @brief A DELETE request with the default client.
 *
 * @return The same codes as chttp_run_query with CHTTP_DELETE.
 */
ccol_retval_t chttp_delete(const char *url, chttpcli_response **resp_out);

/**
 * @brief A PATCH request with the default client.
 *
 * @return The same codes as chttp_run_query with CHTTP_PATCH.
 */
ccol_retval_t chttp_patch(const char *url, const chttp_request_body_t *body,
                          chttpcli_response **resp_out);

/* ========================================================================== */
/*                    RESPONSE API */
/* ========================================================================== */

/**
 * @brief Look up a response header by name.
 *
 * The name is matched without regard to case. A name the server sent more
 * than once gives the combined value of RFC 9110 section 5.3, which is
 * every occurrence in wire order joined with ", ". Set-Cookie isn't
 * combined, because RFC 9110 section 5.3 and RFC 6265 section 3 forbid it
 * (a cookie value can hold a comma), so for "set-cookie" you get the FIRST
 * occurrence; chttpclient_resp_header_count and chttpclient_resp_header_at
 * read every occurrence.
 *
 * @param resp  The response to query.
 * @param name  The header name, for example "Content-Type".
 * @return A pointer to the value string, or NULL if the header isn't
 *         there. The pointer stays valid until chttpclient_resp_free is
 *         called.
 */
const char *chttpclient_resp_header(const chttpcli_response *resp,
                                    const char *name);

/**
 * @brief Count the occurrences of a response header.
 *
 * The name is matched without regard to case. Each field line of the
 * response header block with that name counts once, whatever its value.
 *
 * @param resp  The response to query.
 * @param name  The header name, for example "Set-Cookie".
 * @return The number of field lines with that name. It is 0 when the
 *         header isn't there, when resp or name is NULL, and for the
 *         response of a streaming request, which keeps no headers.
 */
size_t chttpclient_resp_header_count(const chttpcli_response *resp,
                                     const char *name);

/**
 * @brief Get one occurrence of a response header.
 *
 * The name is matched without regard to case. index counts the field lines
 * with that name in wire order, starting at 0. Each value is the value of
 * one field line exactly as the server sent it, never a combined value.
 *
 * @param resp   The response to query.
 * @param name   The header name, for example "Set-Cookie".
 * @param index  Which occurrence, from 0 to
 *               chttpclient_resp_header_count(resp, name) - 1.
 * @return A pointer to the value string, or NULL when index isn't below
 *         that count, when resp or name is NULL, and for the response of a
 *         streaming request. The pointer stays valid until
 *         chttpclient_resp_free is called.
 */
const char *chttpclient_resp_header_at(const chttpcli_response *resp,
                                       const char *name, size_t index);

/**
 * @brief Free a response and everything it owns.
 *
 * Calling it with NULL is safe.
 *
 * The response keeps its own copy of the allocator procs that you passed to
 * chttpclient_create_mp when you created the client, and it refers neither
 * to the client nor to the client's copy, so this call is valid before or
 * after chttpclient_destroy of that client. The functions that the procs
 * name must stay callable, as for any allocation they made.
 */
void chttpclient_resp_free(chttpcli_response *resp);

#pragma GCC visibility pop
