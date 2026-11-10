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

// Everything from here to the matching pop is the library's public ABI and is
// exported; the library builds with -fvisibility=hidden, so anything outside
// these blocks stays internal.
//
// chttpclient is an HTTP/1.1 client over raw sockets, with ctls for TLS. It has
// three tiers: chttpcli_do blocks the calling thread (Tier 1),
// chttpcli_do_async gives a future and runs on one shared engine (Tier 2), and
// chttpcli_do_pooled blocks on that engine (Tier 3).
//
// Each client limits how many Tier 1 requests run at once (the CPU count by
// default) and keeps idle keep-alive connections per origin, so a request can
// skip DNS, the TCP handshake and the TLS handshake. A pooled connection is
// probed before reuse, and the pool is capped per origin and in total.
//
// Every function that takes a chttpcli is thread-safe, and a destroy may run
// while other threads make requests; it waits for them. Two destroys of the
// same handle are a fatal error. A chttp_request or chttpcli_response has no
// locking of its own, so share one between threads only with your own lock.
// chttp_default_client() gives a process-wide client that chttp_get and the
// other short calls use.
//
//    chttpcli_response *resp;
//    if (chttp_get("https://api.example.com/users", &resp) == ccol_success) {
//      printf("status=%d body=%s\n", resp->status_code,
//             resp->body ? resp->body : "");
//      chttpcli_resp_free(resp);
//    }
#pragma GCC visibility push(default)

// The handle of an HTTP client. It's a value that packs a slot index and a
// generation, not a pointer, so never cast it to or from void *. Compare it
// with CHTTPCLI_NULL (which is 0). Every call checks the handle against the
// library's slot table first, so a stale handle is caught instead of touching
// freed memory.
typedef uint64_t chttpcli;

// The "no client" value of a chttpcli; a failed create gives it.
#define CHTTPCLI_NULL ((chttpcli)0)

// The connect and inactivity timeouts a new client starts with (30 s and 60 s,
// in microseconds), so no request waits for ever on a silent peer.
#define CHTTPCLI_DEFAULT_CONNECT_TIMEOUT_US ((uint64_t)30000000)

#define CHTTPCLI_DEFAULT_INACTIVITY_TIMEOUT_US ((uint64_t)60000000)

// The callback of a streaming request, which gets the response body in chunks
// as they arrive (data is not NUL-terminated) instead of a buffer. ctx is what
// you passed with it. Return len to go on; anything less aborts the transfer
// with ccol_http_transfer_aborted. For Tier 2 and 3 no call starts after the
// future is fulfilled, so ctx may be freed once you have the result. Don't
// destroy the client that runs the request from inside it.
typedef size_t (*chttpcli_write_fn)(const void *data, size_t len, void *ctx);

// An HTTP request. method, expect_continue and
// prevent_tls_downgrade_on_redirect may be set directly. url and body are
// copies the request owns; read them, but build another request rather than
// change them. Headers are set and read through chttp_request_set_header and
// chttp_request_get_header. Create it with chttp_request_create_mp and free it
// with chttp_request_free.
typedef struct chttp_request {
  chttp_method method;
  char *url;
  chttp_request_body body;
  chmap_declare(headers, char *, char *);
  ccol_memmgmt_procs *ccol__m_procs;
  // Sends "Expect: 100-continue" and holds the body back for up to 1 s, until
  // the server answers. A final response that comes first means the body is
  // never sent. It applies only to a hop that carries a body.
  bool expect_continue;
  // Refuses a redirect from https to http with ccol_http_invalid_url. It's off
  // by default, as in curl and Go, which means the peer can move the rest of
  // the chain to clear text; set it wherever that's not acceptable. Credentials
  // you set are dropped on a cross-origin hop either way, and an AF_UNIX target
  // is never reachable through a redirect.
  bool prevent_tls_downgrade_on_redirect;
} chttp_request;

// An HTTP response. status_code, body, body_len and headers are public. body is
// a NUL-terminated buffer, or NULL on the streaming path and for an empty body
// (204, HEAD, Content-Length: 0), so check it before you use it. Read headers
// through chttpcli_resp_header and its siblings, and free the response with
// chttpcli_resp_free; it carries its own allocator copy, so that works after
// the client is destroyed.
typedef struct chttpcli_response {
  int status_code;
  char *body;
  size_t body_len;
  chmap_declare(headers, char *, char *);
  ccol_memmgmt_procs *ccol__m_procs;
  void *ccol__field_lines;
} chttpcli_response;

// Request lifecycle
//
// Creates a request; it's the one constructor behind chttp_request_create, for
// callers with their own allocator. method is one of CHTTP_GET to CHTTP_OPTIONS
// (CHTTP_ANY and any other value are refused). url, body->data and
// body->content_type are copied, so you may free yours at once; body may be
// NULL or &CHTTP_NO_BODY, and body->data may be NULL only when body->len is 0.
// mprocs is your allocator or NULL. Gives the request, or NULL with *err_str
// set (err_str may be NULL).
chttp_request *chttp_request_create_mp(chttp_method method, const char *url,
                                       const chttp_request_body *body,
                                       ccol_memmgmt_procs *mprocs,
                                       char **err_str);

// Creates a request with the default allocator; see chttp_request_create_mp.
static inline __attribute__((always_inline)) chttp_request *
chttp_request_create(chttp_method method, const char *url,
                     const chttp_request_body *body, char **err_str) {
  return chttp_request_create_mp(method, url, body, NULL, err_str);
}

// Sets a request header, replacing an earlier value of the same name, so you
// can add authentication, content negotiation and the like. Names are stored in
// lower case. The name must be a token, and the value may hold no control byte
// but a tab (CR and LF would split the request). Transfer-Encoding is refused,
// since a request body always goes out whole with Content-Length. Gives
// ccol_success, ccol_invalid_args or ccol_not_enough_memory.
ccol_retval chttp_request_set_header(chttp_request *req, const char *name,
                                     const char *value);

// Looks up a request header by name, case ignored, so you can check what a
// request will send. Gives the value, valid until the next
// chttp_request_set_header on req, or NULL.
const char *chttp_request_get_header(const chttp_request *req,
                                     const char *name);

// Frees a request and everything it owns. NULL is safe.
void chttp_request_free(chttp_request *req);

// Client constructors
//
// Creates an HTTP client that allocates through mprocs (NULL for malloc and
// free). It starts with the pool size of the CPU count, a 30 s connect timeout,
// a 60 s inactivity timeout, no request timeout, no body cap and full TLS
// verification against the system CA bundle; change them with the
// chttpcli_set_* calls before the first request. Gives the handle, or
// CHTTPCLI_NULL with a static string in *err_str (err_str may be NULL).
chttpcli chttpcli_create_mp(ccol_memmgmt_procs *mprocs, char **err_str);

// Creates an HTTP client with the default allocator; see chttpcli_create_mp.
static inline __attribute__((always_inline)) chttpcli
chttpcli_create(char **err_str) {
  return chttpcli_create_mp(NULL, err_str);
}

// Client configuration
//
// Sets how many Tier 1 requests of cli may run at once (0 gives the CPU count),
// so a client can bound its own load. A Tier 1 call beyond it blocks for a free
// slot, within its request timeout. Tier 2 and Tier 3 aren't limited by it.
// Gives ccol_success, or ccol_invalid_args for a bad handle.
ccol_retval chttpcli_set_pool_size(chttpcli cli, size_t n);

// Sets the connect timeout of cli in microseconds (0 for none). It covers the
// name lookup, the TCP connect and, for https, the TLS handshake, as one
// budget. A lookup can't be cancelled: Tier 1 gives ccol_timed_out once it
// returns, Tiers 2 and 3 at the deadline itself. Applies to requests that start
// later.
ccol_retval chttpcli_set_connect_timeout(chttpcli cli, uint64_t us);

// Sets the total timeout of cli's requests in microseconds (0 for none), from
// the call until the last body byte. Every tier checks it before each read, so
// a response not fully read by the deadline ends in ccol_timed_out even when
// the rest is buffered. Time waiting for a pool slot counts.
ccol_retval chttpcli_set_request_timeout(chttpcli cli, uint64_t us);

// Sets the inactivity timeout of cli in microseconds (0 for none): a request
// ends with ccol_timed_out once it has waited this long on its connection with
// no byte moving either way. Every byte restarts it, and time spent in a
// streaming write_fn doesn't count. A server that takes longer to start its
// response (a long poll) needs a larger value or 0.
ccol_retval chttpcli_set_inactivity_timeout(chttpcli cli, uint64_t us);

// Caps a buffered response body of cli at max_bytes (0 for none), so a bad
// server can't make you hold an unbounded body. A larger declared
// Content-Length fails before the body is read, and any other body fails as it
// crosses the cap, with ccol_msg_too_large. Streaming requests aren't affected;
// their write_fn bounds their memory.
ccol_retval chttpcli_set_max_response_body_size(chttpcli cli, size_t max_bytes);

// Sets cli's TLS configuration (see chttp_tls_config in chttp.h); NULL, or a
// zeroed struct, means full verification against the system CA bundle and no
// client certificate. The system bundle is read once per process. A certificate
// needs its key, and insecure_skip_verify beside a CA bundle is refused, as two
// conflicting policies. A file that doesn't exist yet isn't an error here:
// requests that need TLS then fail with ccol_http_tls_cert_load_failed. Running
// out of memory puts the defaults back and gives ccol_not_enough_memory. Pooled
// connections made under the old settings aren't reused.
ccol_retval chttpcli_set_tls(chttpcli cli, const chttp_tls_config *tls);

// Engine logger
//
// Installs a logger for the diagnostics of the shared Tier 2 and Tier 3 engine
// (failed handshakes, connect errors), so you choose where they go. The engine
// derives its own handle from cl, and the setting holds across engine restarts.
// CLOG_NULL releases it, which is ccol_not_permitted while the engine runs or
// stops (wait with chttpcli_engine_wait first). A closed or stale cl is
// ccol_invalid_args.
ccol_retval chttpcli_set_engine_logger(clog cl);

// Engine memory management
//
// Sends the allocations of the shared Tier 2 and Tier 3 engine (its reactor,
// dispatch state and lookup workers) to mp, or back to the default for NULL. It
// applies to the next engine start and is ccol_not_permitted while one runs. mp
// must stay valid until chttpcli_engine_wait returns after your last client.
ccol_retval chttpcli_set_engine_mem_mgmt_procs(ccol_memmgmt_procs *mp);

// Engine reactor thread count
//
// Sets how many threads the shared engine's reactor uses (0 for the CPU count;
// 1 polls and dispatches on one thread), so you can size it. It applies to the
// next engine start and is ccol_not_permitted while one runs.
ccol_retval chttpcli_set_engine_num_reactor_threads(size_t num_threads);

// Engine wait
//
// Blocks until the shared engine has stopped completely and its threads are
// joined, so the engine allocator and logger can go away safely. The engine
// stops on its own once no client holds it. It returns at once when no engine
// runs, and calling it from an engine thread (a write_fn) is a fatal error.
void chttpcli_engine_wait(void);

// Client destruction
//
// Destroys cli; use the chttpcli_destroy macro, which also clears the handle.
// It waits for every request in flight on cli and closes its idle connections.
// A handle that isn't live (stale, being destroyed elsewhere, garbage) is a
// fatal error, and so is a destroy from inside one of cli's own requests or
// from any engine thread, since it would wait for itself. Don't destroy the
// default client.
void chttpcli__destroy(chttpcli cli);

// The cleanup function of the scoped macros, for ccol__destructor. A NULL
// handle does nothing.
static inline __attribute__((always_inline)) void chttpcli__destroy_cleanup(
    chttpcli *pp) {
  if (pp && *pp) {
    chttpcli__destroy(*pp);
    *pp = CHTTPCLI_NULL;
  }
}

// Destroys a client and sets the handle to CHTTPCLI_NULL. It waits for every
// request in flight. cli is evaluated once and must be a modifiable lvalue.
#define chttpcli_destroy(cli)  \
  ccol__chttpcli_destroy_impl( \
      cli, ccol__uniq(ccol__chttpcli_destroy_slot, __COUNTER__))

// The body of chttpcli_destroy. slot is a ccol__uniq() name, so the macro nests
// inside another destroy macro's argument without -Wshadow warnings.
#define ccol__chttpcli_destroy_impl(cli, slot) \
  do {                                         \
    __typeof__(cli) *slot = &(cli);            \
    chttpcli__destroy(*slot);                  \
    *slot = CHTTPCLI_NULL;                     \
  } while (0)

// Lifecycle macros
//
// Declares a client variable without a value.
#define chttpcli_declare(name) chttpcli name

// Declares a client variable that is destroyed when the scope ends.
#define chttpcli_declare_scoped(name) \
  chttpcli name ccol__destructor(chttpcli__destroy_cleanup) = CHTTPCLI_NULL;

// Declares a client named name and creates it, calling ccol__fatal_err when the
// create fails, for code that has no use for a failed create.
#define chttpcli_construct(name)                                        \
  chttpcli name = CHTTPCLI_NULL;                                        \
  do {                                                                  \
    char *chttpcli__err = NULL;                                         \
    (name) = chttpcli_create(&chttpcli__err);                           \
    if (!(name)) {                                                      \
      ccol__fatal_err("chttpcli_construct('%s'): %s", #name,            \
                      chttpcli__err ? chttpcli__err : "unknown error"); \
    }                                                                   \
  } while (0)

// Like chttpcli_construct, but the client is destroyed when the scope ends.
#define chttpcli_construct_scoped(name)                                      \
  chttpcli name ccol__destructor(chttpcli__destroy_cleanup) = CHTTPCLI_NULL; \
  do {                                                                       \
    char *chttpcli__err = NULL;                                              \
    (name) = chttpcli_create(&chttpcli__err);                                \
    if (!(name)) {                                                           \
      ccol__fatal_err("chttpcli_construct_scoped('%s'): %s", #name,          \
                      chttpcli__err ? chttpcli__err : "unknown error");      \
    }                                                                        \
  } while (0)

// Request execution
//
// Runs req on cli synchronously and buffers the whole response, for the plain
// blocking call. It waits for a pool slot, then follows up to 50 redirects
// (301, 302 and 303 turn into a bodiless GET unless HEAD; 307 and 308 keep the
// method and body), resolving every Location by RFC 3986. A redirect can't
// reach an AF_UNIX socket other than the one the chain started on, and drops
// your credential headers on a hop to another origin. The URL may be http://,
// https:// or http+unix://, with optional userinfo that becomes a Basic
// Authorization header. A reused connection that died before answering is
// retried once when the request is safe to resend. *resp_out is NULL until a
// success, and the response is yours to free with chttpcli_resp_free. Gives
// ccol_success or a failure code (see chttpcli_do(3)).
ccol_retval chttpcli_do(chttpcli cli, const chttp_request *req,
                        chttpcli_response **resp_out);

// Runs req on cli synchronously and passes the body to write_fn in chunks, so a
// large body needs no buffer. write_ctx goes to write_fn. *status_code_out (may
// be NULL) gets the status after a success. Gives the codes of chttpcli_do,
// except ccol_msg_too_large, plus ccol_http_transfer_aborted when write_fn
// stops the transfer.
ccol_retval chttpcli_do_streaming(chttpcli cli, const chttp_request *req,
                                  chttpcli_write_fn write_fn, void *write_ctx,
                                  int *status_code_out);

// Async API (Tier 2)
//
// What an async request's future gives: rv as chttpcli_do would give it, and
// resp when rv is ccol_success. Free resp with chttpcli_resp_free, then this
// with chttpcli_async_result_free.
typedef struct chttpcli_async_result {
  ccol_retval rv;
  chttpcli_response *resp;
  ccol_memmgmt_procs *ccol__m_procs;
} chttpcli_async_result;

// Submits req to the shared async engine and returns at once, so a thread can
// run many requests without blocking. The engine starts on first use, and the
// whole request, redirects included, runs on its threads. req may be freed
// right after the call. Gives a future (get the result with
// chttpcli_async_result_get, free it once with cthrpool_future_free), or NULL
// when the request can't be queued.
cthrpool_future *chttpcli_do_async(chttpcli cli, const chttp_request *req);

// Submits req like chttpcli_do_async, with the body passed to write_fn in
// chunks. write_fn runs on an engine thread, so it must not block and must not
// wait on another request of the engine. Gives a future, or NULL.
cthrpool_future *chttpcli_do_async_streaming(chttpcli cli,
                                             const chttp_request *req,
                                             chttpcli_write_fn write_fn,
                                             void *write_ctx);

// Blocks until the future f of an async request is fulfilled and gives its
// result, which the future owns. Calling it again gives the same pointer. Gives
// NULL for a NULL or cancelled future.
chttpcli_async_result *chttpcli_async_result_get(cthrpool_future *f);

// Frees a result from chttpcli_async_result_get. It frees neither result->resp
// nor the future, which you free separately. It works after the client is
// destroyed. NULL is safe.
void chttpcli_async_result_free(chttpcli_async_result *result);

// Pooled sync API (Tier 3)
//
// Runs req on the shared async engine and blocks until it ends, with the call
// shape of chttpcli_do, so many callers share a few engine threads instead of
// one blocked thread each. *resp_out is NULL until a success. Gives the codes
// of chttpcli_do.
ccol_retval chttpcli_do_pooled(chttpcli cli, const chttp_request *req,
                               chttpcli_response **resp_out);

// Like chttpcli_do_pooled, with the body passed to write_fn in chunks on an
// engine thread (it must not block). *status_code_out (may be NULL) gets the
// status.
ccol_retval chttpcli_do_pooled_streaming(chttpcli cli, const chttp_request *req,
                                         chttpcli_write_fn write_fn,
                                         void *write_ctx, int *status_code_out);

// Default client and convenience API
//
// Gives the process's default client, created on first use and thread-safe, for
// callers that need no client of their own. Configure it with the
// chttpcli_set_* calls, but never destroy it: the library does at exit, and
// afterwards this gives CHTTPCLI_NULL.
chttpcli chttp_default_client(void);

// Runs req with the default client, like chttpcli_do(chttp_default_client(),
// req, resp_out).
ccol_retval chttp_do(const chttp_request *req, chttpcli_response **resp_out);

// Builds a request from method, url, body and headers, runs it with the default
// client and frees it, for a one-call request. headers (may be NULL) is
// borrowed, never consumed. Gives the codes of chttpcli_do; a bad method or a
// body with NULL data and a non-zero len is ccol_invalid_args.
ccol_retval chttp_run_query(chttp_method method, const char *url,
                            const chttp_request_body *body, chmap headers,
                            chttpcli_response **resp_out);

// Runs a GET of url with the default client, the shortest way to fetch a URL.
// *resp_out gets the response; gives the codes of chttpcli_do.
ccol_retval chttp_get(const char *url, chttpcli_response **resp_out);

// Runs a POST of body (may be NULL) to url with the default client. Gives the
// codes of chttp_run_query.
ccol_retval chttp_post(const char *url, const chttp_request_body *body,
                       chttpcli_response **resp_out);

// Runs a PUT of body to url with the default client. Gives the codes of
// chttp_run_query.
ccol_retval chttp_put(const char *url, const chttp_request_body *body,
                      chttpcli_response **resp_out);

// Runs a DELETE of url with the default client. Gives the codes of
// chttp_run_query.
ccol_retval chttp_delete(const char *url, chttpcli_response **resp_out);

// Runs a PATCH of body to url with the default client. Gives the codes of
// chttp_run_query.
ccol_retval chttp_patch(const char *url, const chttp_request_body *body,
                        chttpcli_response **resp_out);

// Response API
//
// Looks up a response header by name, case ignored, for the common case of one
// value per name. A repeated name gives every occurrence joined by ", ", except
// Set-Cookie, which gives its first. Gives the value, valid until
// chttpcli_resp_free, or NULL.
const char *chttpcli_resp_header(const chttpcli_response *resp,
                                 const char *name);

// Counts the field lines named name (case ignored) in resp, so you can read a
// repeated header one by one. It's 0 for a missing name, a NULL argument or a
// streaming response.
size_t chttpcli_resp_header_count(const chttpcli_response *resp,
                                  const char *name);

// Gives occurrence index (from 0, in wire order) of the header name in resp,
// exactly as the server sent it, or NULL when index is out of range, an
// argument is NULL or the response came from a streaming request.
const char *chttpcli_resp_header_at(const chttpcli_response *resp,
                                    const char *name, size_t index);

// Frees a response and everything it owns. NULL is safe. It uses the allocator
// copy the response carries, so it works after the client is destroyed.
void chttpcli_resp_free(chttpcli_response *resp);

#pragma GCC visibility pop
