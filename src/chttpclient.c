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

#include <chashmap.h>
#include <chttp1_parser.h>
#include <chttpclient.h>
#include <cthreadcomm.h>
#include <cthreadpool.h>
#include <ctls.h>
#include <ctype.h>
#include <cvector.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ========================================================================== */
/*                         CONSTANTS                                          */
/* ========================================================================== */

#define CHTTP_MAX_REDIRECTS 50
#define CHTTP_MAX_IDLE_PER_ORIGIN 4
#define CHTTP_MAX_IDLE_TOTAL 32
#define CHTTP_IDLE_MAX_AGE_MS 60000L
/* How long chttp_do_internal waits for a "100 Continue" interim response
 * (or the server's real final response, if it answers directly without
 * waiting) before giving up and sending the body anyway; matches curl's
 * own CURLOPT_EXPECT_100_TIMEOUT_MS default. See chttp_request_t.
 * expect_continue's own doc comment. */
#define CHTTP_100_CONTINUE_WAIT_MS 1000L

/* ========================================================================== */
/*                         INTERNAL TYPES                                     */
/* ========================================================================== */

/* Parsed request-target. All fields are owned copies (mp-allocated).
 *
 * A "http+unix://" URL (see _parse_chttp_url) sets is_unix and
 * unix_socket_path instead of host/port/is_ipv6: host is left NULL, port is
 * left 0, and is_ipv6 is left false for a unix target. origin_key is always
 * populated, using a distinct "unix://<path>" literal prefix (never
 * colliding with a "http://"/"https://" TCP origin_key) so the idle-pool
 * chmaps can key a unix-socket target the same way a TCP one is keyed. */
typedef struct {
  bool is_https;
  bool is_ipv6; /* host is a raw (unbracketed) IPv6 literal; connect()/TLS
                 * need it unbracketed, but the Host header, origin_key, and
                 * redirect-URL reconstruction all need it re-bracketed. */
  bool is_unix;
  char *unix_socket_path; /* owned, percent-decoded; NULL unless is_unix */
  char *host;
  uint16_t port;
  char *path_and_query;
  char *origin_key; /* "scheme://host:port" or "unix://<path>"; used for SNI
                     * (TCP only) and idle-pool keying (both). */
  char *userinfo_authorization; /* owned "Basic <b64>" derived from THIS
                                 * URL's own "user:pass@" component, or NULL
                                 * if the URL had none. */
} chttp_url_t;

/* Absolute-time deadline helper; `active == false` means "no limit". */
typedef struct {
  bool active;
  struct timespec deadline;
} chttp_deadline_t;

/* A single (possibly pooled) connection. Always stack/value-resident (never
 * individually heap-allocated) so it can be stored by value in the idle
 * pool's cvector without an extra allocation layer. */
typedef struct {
  int fd;
  ctls_conn_t *tls; /* NULL for plain HTTP */
  char *origin_key; /* owned copy, matches chttp_url_t.origin_key */
  struct timespec last_used;
} chttp_conn_t;

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  ccol_memmgmt_procs_t *mp;
  bool oom;
} chttp_bodybuf_t;

/* Drives one HTTP/1.1 response parse (one hop). A fresh instance is used for
 * every hop of a redirect chain, which is what makes "intermediate redirect
 * headers never leak into the final response" fall out naturally; there is
 * no shared, reset-in-place header map to leak from. */
typedef struct {
  ccol_memmgmt_procs_t *mp;
  chmap headers; /* chmap(char* -> char*); owned until transferred/destroyed */

  bool is_head_request;
  bool redirects_still_allowed;
  bool will_redirect;
  char *location; /* owned; set only when will_redirect */

  bool message_complete;
  bool trailing_garbage;
  bool error;   /* allocation failure inside a callback */
  bool aborted; /* sink_fn returned short (streaming caller aborted) */
  int status_code;

  chttpcli_write_fn requested_sink_fn;
  void *requested_sink_ctx;
  chttpcli_write_fn sink_fn; /* resolved once headers are complete */
  void *sink_ctx;
} chttp_parse_ctx_t;

struct chttpclient {
  mutex_t lock;
  cond_var_t available;

  /* Concurrency limiter: bounds simultaneous in-flight requests. */
  size_t pool_cap;
  size_t configured_pool_size;
  size_t in_flight_count;
  bool pool_initialized;
  bool destroying;

  /* Keep-alive idle pool: chmap(char *origin_key -> cvec of chttp_conn_t),
   * independent of the concurrency limiter above. */
  chmap idle_pools;
  size_t idle_total_count;

  /* Tier 2's own keep-alive idle pool: chmap(char *origin_key -> cvec of
   * chttp_async_ctx_t*). Separate from idle_pools above since the stored
   * value type differs (a plain fd this thread owns vs. a connection
   * attached to the shared async reactor); see the "ASYNC IDLE POOL"
   * section further down for the full design. idle_async_drained is
   * broadcast whenever idle_total_count_async reaches 0, so
   * __chttpclient_destroy can wait for any in-flight idle-connection
   * teardowns (triggered by its own drain, or already in progress from a
   * natural death) to actually finish before freeing this struct out from
   * under them. */
  chmap idle_pools_async;
  size_t idle_total_count_async;
  cond_var_t idle_async_drained;

  /* Tracks active (not-yet-idle-pooled) Tier 2/3 async chains created for
   * THIS client: incremented once per chain in _async_chain_create,
   * decremented in _async_chain_release once a chain's refcount reaches
   * zero (its last hop is torn down or joins the idle pool above).
   * Independent of both in_flight_count above (Tier 1 only) and
   * idle_total_count_async (idle-pooled connections are the OPPOSITE of
   * in-flight). Guarded by a DEDICATED leaf lock/condvar, deliberately
   * never cli->lock/available: _async_chain_release (the only place this is
   * decremented) runs as often from inside an event_loop dispatch callback,
   * already holding that registration's own dispatch_lock, as it does from
   * a safe synchronous context; reusing cli->lock here would introduce a
   * brand new dispatch_lock -> cli->lock ordering this module has otherwise
   * never needed to reason about, for no benefit over a lock that is never
   * held across any other call. __chttpclient_destroy waits on
   * async_count_drained until this reaches zero before freeing cli, closing
   * a real use-after-free: chain->cli/ctx->cli are dereferenced throughout
   * an active hop's lifecycle (cli->lock, cli->idle_pools_async,
   * cli->m_procs), well after chttpclient_do_async/_streaming has already
   * returned to the caller. */
  mutex_t async_count_lock;
  cond_var_t async_count_drained;
  size_t async_in_flight_count;

  long connect_timeout_ms;
  long request_timeout_ms;
  chttp_tls_config_t tls;
  char *owned_cert_path;
  char *owned_key_path;
  char *owned_ca_bundle_path;
  ctls_ctx_t *tls_ctx; /* rebuilt whenever chttpclient_set_tls is called */
  bool tls_ctx_usable; /* false if the configured cert/key/ca paths are not
                          readable; see _rebuild_tls_ctx_locked */

  ccol_memmgmt_procs_t *m_procs;
};

/* ========================================================================== */
/*                         DEFAULT CLIENT                                     */
/* ========================================================================== */

/* client is _Atomic so __chttpclient_destroy can safely clear it via a
 * compare-and-swap if the handle it's given happens to be this singleton
 * (see __chttpclient_destroy's own comment on this); chttp_default_client
 * reads it with a plain atomic_load, matching this file's existing
 * _Atomic(event_reg *) precedent rather than adding a new lock for a single
 * pointer. once never resets: this module never re-initialises the default
 * client, deliberately (see chttp_default_client's own doc comment). */
static struct {
  _Atomic(chttpcli) client;
  once_flag_t once;
} default_client_bundler = {0};

/* ========================================================================== */
/*                         URL PARSING                                        */
/* ========================================================================== */

static void _url_free(ccol_memmgmt_procs_t *mp, chttp_url_t *u) {
  if (!u) return;
  _mem_free(mp, u->unix_socket_path);
  _mem_free(mp, u->host);
  _mem_free(mp, u->path_and_query);
  _mem_free(mp, u->origin_key);
  _mem_free(mp, u->userinfo_authorization);
  memset(u, 0, sizeof(*u));
}

/* Percent-encodes a unix socket path into a newly allocated NUL-terminated
 * string, for reconstructing a "http+unix://<path>" URL string (origin_key,
 * a redirect target resolved against a unix-socket base). Mirrors the
 * percent-encoding a real client library would apply to any URL authority
 * component: every byte outside an unreserved/sub-delim set is escaped,
 * '/' included (the whole point of this scheme is packing a path that
 * itself contains '/' into a single authority component). */
static char *_percent_encode_unix_path(ccol_memmgmt_procs_t *mp,
                                       const char *path) {
  static const char *unreserved =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  size_t len = strlen(path);
  char *out = (char *)_mem_alloc(mp, len * 3 + 1);
  if (!out) return NULL;
  size_t w = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)path[i];
    if (strchr(unreserved, (char)c)) {
      out[w++] = (char)c;
    } else {
      static const char *hex = "0123456789ABCDEF";
      out[w++] = '%';
      out[w++] = hex[(c >> 4) & 0xF];
      out[w++] = hex[c & 0xF];
    }
  }
  out[w] = '\0';
  return out;
}

/* Percent-decodes [start, start+len) into a newly allocated NUL-terminated
 * string. Used only for userinfo components; path/query bytes are never
 * decoded, since they must be forwarded to the server exactly as received.
 * Rejects a malformed escape ('%' not followed by 2 hex digits) and a
 * decoded embedded NUL byte: silently truncating a password at a NUL would
 * produce a subtly wrong Authorization header instead of a clear error. */
static ccol_retval_t _percent_decode_component(ccol_memmgmt_procs_t *mp,
                                               const char *start, size_t len,
                                               char **out) {
  char *buf = (char *)_mem_alloc(mp, len + 1);
  if (!buf) return ccol_not_enough_memory;
  size_t w = 0;
  for (size_t i = 0; i < len; i++) {
    char c = start[i];
    if (c == '%') {
      if (i + 2 >= len || !isxdigit((unsigned char)start[i + 1]) ||
          !isxdigit((unsigned char)start[i + 2])) {
        _mem_free(mp, buf);
        return ccol_http_invalid_url;
      }
      char hex[3] = {start[i + 1], start[i + 2], '\0'};
      int v = (int)strtol(hex, NULL, 16);
      if (v == 0) {
        _mem_free(mp, buf);
        return ccol_http_invalid_url;
      }
      buf[w++] = (char)v;
      i += 2;
    } else {
      buf[w++] = c;
    }
  }
  buf[w] = '\0';
  *out = buf;
  return ccol_success;
}

/*
 * Parses the authority component of a "http+unix://" URL: p points just past
 * the "http+unix://" prefix, at a percent-encoded filesystem path (e.g.
 * "%2Fvar%2Frun%2Fapp.sock"), matching Python's requests-unixsocket
 * convention. Only "http+unix://" is recognised; "https+unix://" is not (TLS
 * over a local socket has no real use case) and simply falls through to the
 * caller's own ccol_http_invalid_url return, since it matches no recognised
 * prefix at all. No userinfo ("user:pass@") support for this scheme: there is
 * no established convention combining the two, and the percent-encoded path
 * itself may legitimately contain '@' bytes once decoded.
 */
static ccol_retval_t _parse_chttp_unix_url(ccol_memmgmt_procs_t *mp,
                                           const char *p, chttp_url_t *out) {
  const char *authority_end = p;
  while (*authority_end && *authority_end != '/' && *authority_end != '?' &&
         *authority_end != '#')
    authority_end++;
  size_t enc_len = (size_t)(authority_end - p);
  if (enc_len == 0) return ccol_http_invalid_url;

  char *unix_path = NULL;
  ccol_retval_t drv = _percent_decode_component(mp, p, enc_len, &unix_path);
  if (drv != ccol_success) return drv;
  if (unix_path[0] == '\0') {
    _mem_free(mp, unix_path);
    return ccol_http_invalid_url;
  }

  const char *q = authority_end;
  const char *pq = (*q && *q != '#') ? q : "/";
  size_t pq_raw_len = strlen(pq);
  const char *frag = memchr(pq, '#', pq_raw_len);
  size_t pq_len = frag ? (size_t)(frag - pq) : pq_raw_len;

  char *path_and_query;
  if (pq_len > 0 && pq[0] == '/') {
    path_and_query = (char *)_mem_alloc(mp, pq_len + 1);
    if (!path_and_query) {
      _mem_free(mp, unix_path);
      return ccol_not_enough_memory;
    }
    memcpy(path_and_query, pq, pq_len);
    path_and_query[pq_len] = '\0';
  } else {
    path_and_query = (char *)_mem_alloc(mp, pq_len + 2);
    if (!path_and_query) {
      _mem_free(mp, unix_path);
      return ccol_not_enough_memory;
    }
    path_and_query[0] = '/';
    memcpy(path_and_query + 1, pq, pq_len);
    path_and_query[pq_len + 1] = '\0';
  }

  int needed = snprintf(NULL, 0, "unix://%s", unix_path);
  if (needed < 0) {
    _mem_free(mp, unix_path);
    _mem_free(mp, path_and_query);
    return ccol_unexpected_failure;
  }
  char *origin_key = (char *)_mem_alloc(mp, (size_t)needed + 1);
  if (!origin_key) {
    _mem_free(mp, unix_path);
    _mem_free(mp, path_and_query);
    return ccol_not_enough_memory;
  }
  snprintf(origin_key, (size_t)needed + 1, "unix://%s", unix_path);

  out->is_https = false;
  out->is_ipv6 = false;
  out->is_unix = true;
  out->unix_socket_path = unix_path;
  out->host = NULL;
  out->port = 0;
  out->path_and_query = path_and_query;
  out->origin_key = origin_key;
  out->userinfo_authorization = NULL;
  return ccol_success;
}

/* Returns an owned "[host]" for an IPv6 literal, or a plain strdup
 * otherwise. Used wherever a host needs to round-trip through a URL string
 * (origin_key, a reconstructed redirect URL); connect()/TLS always use
 * chttp_url_t.host directly, which stays unbracketed. */
static char *_format_bracketed_host(ccol_memmgmt_procs_t *mp, const char *host,
                                    bool is_ipv6) {
  if (!is_ipv6) return ccol_strdup(mp, host);
  size_t hlen = strlen(host);
  char *out = (char *)_mem_alloc(mp, hlen + 3);
  if (!out) return NULL;
  out[0] = '[';
  memcpy(out + 1, host, hlen);
  out[hlen + 1] = ']';
  out[hlen + 2] = '\0';
  return out;
}

/*
 * URL parser: recognises "http://"/"https://", an optional "user:pass@"
 * userinfo prefix (also "user@" or ":pass@"; both syntactically valid RFC
 * 3986 forms) that is turned into a ready-to-send "Basic <base64>"
 * Authorization value, a plain reg-name/IPv4 host or a bracketed IPv6
 * literal ("[::1]"), an optional ":port", and a path/query with any
 * trailing "#fragment" discarded; fragments are never sent to a server
 * (RFC 3986 SS3.5), so silently forwarding one in the request line (the
 * previous behavior) was a real correctness bug, not a documented scope
 * choice.
 *
 * Not attempted: userinfo/hostname are not validated beyond basic syntax;
 * an implausible bracketed literal or reg-name is simply handed to
 * getaddrinfo()/inet_pton() at connect time and surfaces there as
 * ccol_http_host_resolution_failed, exactly like today's unvalidated
 * hostnames.
 *
 * Also recognises "http+unix://<percent-encoded-path>[/path][?query]" (see
 * _parse_chttp_unix_url); "https+unix://" is not recognised and falls
 * through to ccol_http_invalid_url below, same as any other unrecognised
 * scheme.
 */
static ccol_retval_t _parse_chttp_url(ccol_memmgmt_procs_t *mp, const char *url,
                                      chttp_url_t *out) {
  memset(out, 0, sizeof(*out));
  if (!url) return ccol_http_invalid_url;

  if (strncasecmp(url, "http+unix://", 12) == 0)
    return _parse_chttp_unix_url(mp, url + 12, out);

  bool https;
  const char *p;
  if (strncasecmp(url, "http://", 7) == 0) {
    https = false;
    p = url + 7;
  } else if (strncasecmp(url, "https://", 8) == 0) {
    https = true;
    p = url + 8;
  } else {
    return ccol_http_invalid_url;
  }

  /* Authority = [ userinfo "@" ] host [ ":" port ], terminated by the first
   * '/', '?', '#', or end of string. */
  const char *authority_end = p;
  while (*authority_end && *authority_end != '/' && *authority_end != '?' &&
         *authority_end != '#')
    authority_end++;

  /* Last unescaped '@' in the authority: tolerates an unescaped '@' inside a
   * lazily-encoded password, matching common real-world parser leniency. */
  const char *last_at = NULL;
  for (const char *s = p; s < authority_end; s++)
    if (*s == '@') last_at = s;

  char *userinfo_authorization = NULL;
  const char *host_scan_start = p;
  if (last_at) {
    /* Delimiters ('@', ':') are located in the RAW (still percent-encoded)
     * string, then each half is decoded independently; decoding first and
     * searching second would incorrectly split on a decoded '@'/':' that
     * was meant to be literal password content. */
    const char *colon = NULL;
    for (const char *s = p; s < last_at; s++)
      if (*s == ':') {
        colon = s;
        break;
      }
    const char *user_start = p;
    size_t user_len = colon ? (size_t)(colon - p) : (size_t)(last_at - p);
    const char *pass_start = colon ? colon + 1 : last_at;
    size_t pass_len = colon ? (size_t)(last_at - colon - 1) : 0;

    char *user_dec = NULL, *pass_dec = NULL;
    ccol_retval_t drv =
        _percent_decode_component(mp, user_start, user_len, &user_dec);
    if (drv == ccol_success)
      drv = _percent_decode_component(mp, pass_start, pass_len, &pass_dec);
    if (drv != ccol_success) {
      _mem_free(mp, user_dec);
      _mem_free(mp, pass_dec);
      return drv;
    }

    userinfo_authorization = chttp_basic_auth_mp(mp, user_dec, pass_dec);
    _mem_free(mp, user_dec);
    _mem_free(mp, pass_dec);
    if (!userinfo_authorization) return ccol_not_enough_memory;

    host_scan_start = last_at + 1;
  }

  bool is_ipv6 = false;
  const char *host_start;
  const char *q;
  if (*host_scan_start == '[') {
    const char *close = host_scan_start + 1;
    while (*close && *close != ']') close++;
    if (*close != ']') {
      _mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    char after = close[1];
    if (after != '\0' && after != ':' && after != '/' && after != '?' &&
        after != '#') {
      _mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    host_start = host_scan_start + 1;
    q = close; /* points at ']' */
    is_ipv6 = true;
  } else {
    host_start = host_scan_start;
    q = host_scan_start;
    while (*q && *q != ':' && *q != '/' && *q != '?' && *q != '#') q++;
  }
  size_t host_len = (size_t)(q - host_start);
  if (host_len == 0) {
    _mem_free(mp, userinfo_authorization);
    return ccol_http_invalid_url;
  }
  if (is_ipv6) q++; /* skip past ']' */

  uint16_t port = https ? 443 : 80;
  if (*q == ':') {
    q++;
    unsigned long pv = 0;
    size_t pd = 0;
    while (*q && isdigit((unsigned char)*q)) {
      pv = pv * 10 + (unsigned long)(*q - '0');
      if (pv > 65535) {
        _mem_free(mp, userinfo_authorization);
        return ccol_http_invalid_url;
      }
      q++;
      pd++;
    }
    if (pd == 0 || pv == 0) {
      _mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    port = (uint16_t)pv;
  }

  /* A fragment is a hard, unconditional delimiter; no escaping semantics
   * apply to '#' itself, since a fragment start is never quoted. */
  const char *pq = (*q && *q != '#') ? q : "/";
  size_t pq_raw_len = strlen(pq);
  const char *frag = memchr(pq, '#', pq_raw_len);
  size_t pq_len = frag ? (size_t)(frag - pq) : pq_raw_len;

  char *host = (char *)_mem_alloc(mp, host_len + 1);
  if (!host) {
    _mem_free(mp, userinfo_authorization);
    return ccol_not_enough_memory;
  }
  memcpy(host, host_start, host_len);
  host[host_len] = '\0';

  char *path_and_query;
  if (pq_len > 0 && pq[0] == '/') {
    path_and_query = (char *)_mem_alloc(mp, pq_len + 1);
    if (!path_and_query) {
      _mem_free(mp, host);
      _mem_free(mp, userinfo_authorization);
      return ccol_not_enough_memory;
    }
    memcpy(path_and_query, pq, pq_len);
    path_and_query[pq_len] = '\0';
  } else {
    /* pq is empty, or starts with '?' (query with no path component);
     * synthesise a leading '/'. */
    path_and_query = (char *)_mem_alloc(mp, pq_len + 2);
    if (!path_and_query) {
      _mem_free(mp, host);
      _mem_free(mp, userinfo_authorization);
      return ccol_not_enough_memory;
    }
    path_and_query[0] = '/';
    memcpy(path_and_query + 1, pq, pq_len);
    path_and_query[pq_len + 1] = '\0';
  }

  char *bracketed_host = _format_bracketed_host(mp, host, is_ipv6);
  if (!bracketed_host) {
    _mem_free(mp, host);
    _mem_free(mp, path_and_query);
    _mem_free(mp, userinfo_authorization);
    return ccol_not_enough_memory;
  }

  int needed = snprintf(NULL, 0, "%s://%s:%u", https ? "https" : "http",
                        bracketed_host, (unsigned)port);
  if (needed < 0) {
    _mem_free(mp, host);
    _mem_free(mp, path_and_query);
    _mem_free(mp, bracketed_host);
    _mem_free(mp, userinfo_authorization);
    return ccol_unexpected_failure;
  }
  char *origin_key = (char *)_mem_alloc(mp, (size_t)needed + 1);
  if (!origin_key) {
    _mem_free(mp, host);
    _mem_free(mp, path_and_query);
    _mem_free(mp, bracketed_host);
    _mem_free(mp, userinfo_authorization);
    return ccol_not_enough_memory;
  }
  snprintf(origin_key, (size_t)needed + 1, "%s://%s:%u",
           https ? "https" : "http", bracketed_host, (unsigned)port);
  _mem_free(mp, bracketed_host);

  out->is_https = https;
  out->is_ipv6 = is_ipv6;
  out->host = host;
  out->port = port;
  out->path_and_query = path_and_query;
  out->origin_key = origin_key;
  out->userinfo_authorization = userinfo_authorization;
  return ccol_success;
}

/* RFC 3986 SS5.2.4 "Remove Dot Segments". Operates on a path only; never
 * on a query string, since a literal ".."/"." inside query bytes must never
 * be reinterpreted as path navigation; callers strip/re-append any
 * "?query" before/after calling this. */
static char *_remove_dot_segments(ccol_memmgmt_procs_t *mp, const char *path) {
  size_t len = strlen(path);
  char *in = (char *)_mem_alloc(mp, len + 1);
  if (!in) return NULL;
  memcpy(in, path, len + 1);
  char *out = (char *)_mem_alloc(mp, len + 1);
  if (!out) {
    _mem_free(mp, in);
    return NULL;
  }
  size_t out_len = 0;
  char *p = in;
  while (*p) {
    if (strncmp(p, "../", 3) == 0) {
      p += 3;
    } else if (strncmp(p, "./", 2) == 0) {
      p += 2;
    } else if (strncmp(p, "/./", 3) == 0) {
      p += 2;
    } else if (strcmp(p, "/.") == 0) {
      p[1] = '\0';
    } else if (strncmp(p, "/../", 4) == 0) {
      p += 3;
      while (out_len > 0 && out[out_len - 1] != '/') out_len--;
      if (out_len > 0) out_len--;
    } else if (strcmp(p, "/..") == 0) {
      p[1] = '\0';
      while (out_len > 0 && out[out_len - 1] != '/') out_len--;
      if (out_len > 0) out_len--;
    } else if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) {
      p += strlen(p);
    } else {
      const char *seg_start = p;
      if (*p == '/') p++;
      while (*p && *p != '/') p++;
      size_t seg_len = (size_t)(p - seg_start);
      memcpy(out + out_len, seg_start, seg_len);
      out_len += seg_len;
    }
  }
  out[out_len] = '\0';
  _mem_free(mp, in);
  return out;
}

/* RFC 3986 SS5.3 "merge" (path component only; the caller splices the
 * reference's own query string back on afterward, once dot-segment removal
 * has run; see _remove_dot_segments' own comment for why). */
static char *_merge_ref_path(ccol_memmgmt_procs_t *mp,
                             const char *base_path_and_query,
                             const char *ref_path, size_t ref_path_len) {
  const char *base_query = strchr(base_path_and_query, '?');
  size_t base_path_len = base_query ? (size_t)(base_query - base_path_and_query)
                                    : strlen(base_path_and_query);

  size_t dir_len;
  if (ref_path_len == 0) {
    /* A query-only reference ("?x") reuses the base path verbatim. */
    dir_len = base_path_len;
  } else {
    const char *last_slash = NULL;
    for (size_t i = 0; i < base_path_len; i++)
      if (base_path_and_query[i] == '/') last_slash = &base_path_and_query[i];
    dir_len = last_slash ? (size_t)(last_slash - base_path_and_query) + 1 : 0;
  }

  char *out = (char *)_mem_alloc(mp, dir_len + ref_path_len + 1);
  if (!out) return NULL;
  memcpy(out, base_path_and_query, dir_len);
  memcpy(out + dir_len, ref_path, ref_path_len);
  out[dir_len + ref_path_len] = '\0';
  return out;
}

/*
 * Resolves a Location header against the current hop's URL per RFC 3986
 * SS5.2-5.3: absolute URLs, protocol-relative references ("//host/path"),
 * absolute-path references ("/foo"), and general relative-path references
 * ("foo", "../foo", "./foo", "?query") are all supported. Returns NULL
 * (caller treats as ccol_http_transfer_aborted) only for a NULL/empty
 * location; every other syntactically plausible Location value resolves
 * to *some* absolute URL, exactly like a real browser or curl.
 *
 * The result is always re-parsed by _parse_chttp_url on the next hop, so
 * this function does not need to know anything about userinfo/credential
 * carry-forward; that is handled by each tier's own hop loop.
 *
 * Dot-segment removal (RFC 3986 SS5.2.4) is applied to the path component
 * only; a query string is always split off first and re-appended verbatim
 * afterward, on every branch below that can produce one, so a query value
 * that happens to contain "/", "..", or "." bytes is never reinterpreted as
 * path navigation.
 */
static char *_resolve_redirect_url(ccol_memmgmt_procs_t *mp,
                                   const chttp_url_t *base,
                                   const char *location) {
  if (!location || !*location) return NULL;
  if (strncasecmp(location, "http://", 7) == 0 ||
      strncasecmp(location, "https://", 8) == 0 ||
      strncasecmp(location, "http+unix://", 12) == 0) {
    return ccol_strdup(mp, location);
  }

  const char *scheme = base->is_https ? "https" : "http";

  if (location[0] == '/' && location[1] == '/') {
    /* Protocol-relative reference: unambiguously names a (possibly
     * different) network host to redirect to, regardless of whether the
     * base was a unix-socket target; borrows the base's scheme (always
     * "http" for a unix base, since is_https is always false there). The
     * rest is already a well-formed authority+path for the next
     * _parse_chttp_url call. Not dot-segment-normalised here, exactly like
     * the fully absolute-URL case above isn't either. */
    int needed = snprintf(NULL, 0, "%s:%s", scheme, location);
    if (needed < 0) return NULL;
    char *out = (char *)_mem_alloc(mp, (size_t)needed + 1);
    if (!out) return NULL;
    snprintf(out, (size_t)needed + 1, "%s:%s", scheme, location);
    return out;
  }

  /* An absolute-path or relative-path reference against a unix-socket base
   * reconstructs "http+unix://<percent-encoded-path>" + the resolved path,
   * instead of "scheme://host:port" + path; there is no host/port to
   * reconstruct from for a unix target. */
  char *authority = base->is_unix
                        ? _percent_encode_unix_path(mp, base->unix_socket_path)
                        : _format_bracketed_host(mp, base->host, base->is_ipv6);
  if (!authority) return NULL;

  bool default_port =
      !base->is_unix && ((base->is_https && base->port == 443) ||
                         (!base->is_https && base->port == 80));

  /* Split R.query off of R.path ONCE, up front, shared by both branches
   * below: remove_dot_segments (RFC 3986 SS5.2.4) must operate on the path
   * component only, never on query bytes, since a literal ".."/"." inside
   * a query value must never be reinterpreted as path navigation (e.g. a
   * query containing "/../" would otherwise cause dot-segment removal to
   * walk backwards through, and delete, path segments it was never meant to
   * touch). ref_query (if any) is re-appended untouched after dot-segment
   * removal has run, for both branches. */
  const char *ref_query = strchr(location, '?');
  size_t ref_path_len =
      ref_query ? (size_t)(ref_query - location) : strlen(location);

  char *new_path = NULL;
  if (location[0] == '/') {
    /* Absolute-path reference: T.path = remove_dot_segments(R.path)
     * directly, no merge against the base path needed. */
    char *path_only = (char *)_mem_alloc(mp, ref_path_len + 1);
    if (path_only) {
      memcpy(path_only, location, ref_path_len);
      path_only[ref_path_len] = '\0';
      new_path = _remove_dot_segments(mp, path_only);
      _mem_free(mp, path_only);
    }
  } else {
    char *merged =
        _merge_ref_path(mp, base->path_and_query, location, ref_path_len);
    if (merged) {
      new_path = _remove_dot_segments(mp, merged);
      _mem_free(mp, merged);
    }
  }
  if (new_path && ref_query) {
    size_t path_len = strlen(new_path);
    size_t query_len = strlen(ref_query);
    char *with_query = (char *)_mem_alloc(mp, path_len + query_len + 1);
    if (!with_query) {
      _mem_free(mp, new_path);
      new_path = NULL;
    } else {
      memcpy(with_query, new_path, path_len);
      memcpy(with_query + path_len, ref_query, query_len);
      with_query[path_len + query_len] = '\0';
      _mem_free(mp, new_path);
      new_path = with_query;
    }
  }
  if (!new_path) {
    _mem_free(mp, authority);
    return NULL;
  }

  const char *out_scheme = base->is_unix ? "http+unix" : scheme;
  int needed;
  if (base->is_unix) {
    needed = snprintf(NULL, 0, "%s://%s%s", out_scheme, authority, new_path);
  } else if (default_port) {
    needed = snprintf(NULL, 0, "%s://%s%s", out_scheme, authority, new_path);
  } else {
    needed = snprintf(NULL, 0, "%s://%s:%u%s", out_scheme, authority,
                      (unsigned)base->port, new_path);
  }
  if (needed < 0) {
    _mem_free(mp, authority);
    _mem_free(mp, new_path);
    return NULL;
  }
  char *out = (char *)_mem_alloc(mp, (size_t)needed + 1);
  if (!out) {
    _mem_free(mp, authority);
    _mem_free(mp, new_path);
    return NULL;
  }
  if (base->is_unix || default_port) {
    snprintf(out, (size_t)needed + 1, "%s://%s%s", out_scheme, authority,
             new_path);
  } else {
    snprintf(out, (size_t)needed + 1, "%s://%s:%u%s", out_scheme, authority,
             (unsigned)base->port, new_path);
  }
  _mem_free(mp, authority);
  _mem_free(mp, new_path);
  return out;
}

/* White-box test helpers exposing _parse_chttp_url/_resolve_redirect_url;
 * chttp_url_t is a file-local type, so these flatten the result into out
 * params/a plain string. NULL mp means every result is plain-malloc'd (see
 * _mem_alloc); test code frees them with plain free(). Not part of the
 * public API; gated so these symbols do not leak into a production build
 * of libccollections.so, matching every other white-box helper in this
 * file. */
#ifdef RUNNING_UNIT_TESTS
ccol_retval_t _chttp_parse_url_for_tests(
    const char *url, bool *is_https_out, bool *is_ipv6_out, char **host_out,
    uint16_t *port_out, char **path_and_query_out, char **origin_key_out,
    char **userinfo_authorization_out, bool *is_unix_out,
    char **unix_socket_path_out) {
  ccol_memmgmt_procs_t *mp = NULL;
  chttp_url_t parsed;
  ccol_retval_t rv = _parse_chttp_url(mp, url, &parsed);
  if (rv != ccol_success) return rv;
  if (is_https_out) *is_https_out = parsed.is_https;
  if (is_ipv6_out) *is_ipv6_out = parsed.is_ipv6;
  if (port_out) *port_out = parsed.port;
  if (is_unix_out) *is_unix_out = parsed.is_unix;
  if (unix_socket_path_out)
    *unix_socket_path_out = parsed.unix_socket_path;
  else
    _mem_free(mp, parsed.unix_socket_path);
  if (host_out)
    *host_out = parsed.host;
  else
    _mem_free(mp, parsed.host);
  if (path_and_query_out)
    *path_and_query_out = parsed.path_and_query;
  else
    _mem_free(mp, parsed.path_and_query);
  if (origin_key_out)
    *origin_key_out = parsed.origin_key;
  else
    _mem_free(mp, parsed.origin_key);
  if (userinfo_authorization_out)
    *userinfo_authorization_out = parsed.userinfo_authorization;
  else
    _mem_free(mp, parsed.userinfo_authorization);
  return ccol_success;
}

char *_chttp_resolve_redirect_url_for_tests(const char *base_url,
                                            const char *location) {
  ccol_memmgmt_procs_t *mp = NULL;
  chttp_url_t base;
  ccol_retval_t rv = _parse_chttp_url(mp, base_url, &base);
  if (rv != ccol_success) return NULL;
  char *result = _resolve_redirect_url(mp, &base, location);
  _url_free(mp, &base);
  return result;
}
#endif /* RUNNING_UNIT_TESTS */

/* ========================================================================== */
/*                         REQUEST LIFECYCLE                                  */
/* ========================================================================== */

chttp_request_t *chttp_request_new_mp(chttp_method_t method, const char *url,
                                      const chttp_request_body_t *body,
                                      ccol_memmgmt_procs_t *mprocs,
                                      char **err_str) {
  if (!url) {
    if (err_str) *err_str = CCOL_ERR_STR("url must not be NULL");
    return NULL;
  }
  if (mprocs && !ccol_verify_memmgmt_procs(mprocs, err_str)) return NULL;

  ccol_memmgmt_procs_t *mp = NULL;
  if (mprocs) {
    mp = (ccol_memmgmt_procs_t *)mprocs->malloc(sizeof(ccol_memmgmt_procs_t));
    if (!mp) {
      if (err_str) *err_str = CCOL_ERR_STR("failed to allocate mprocs");
      return NULL;
    }
    mem_cpy(mp, mprocs, sizeof(ccol_memmgmt_procs_t));
  }

  chttp_request_t *req =
      (chttp_request_t *)_mem_alloc(mp, sizeof(chttp_request_t));
  if (!req) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate request");
    if (mp) mp->free(mp);
    return NULL;
  }
  memset(req, 0, sizeof(*req));
  req->method = method;
  req->_m_procs = mp;

  req->url = ccol_strdup(mp, url);
  if (!req->url) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to copy url");
    _mem_free(mp, req);
    if (mp) mp->free(mp);
    return NULL;
  }

  if (body && body->data && body->len > 0) {
    void *body_copy = _mem_alloc(mp, body->len);
    if (!body_copy) {
      if (err_str) *err_str = CCOL_ERR_STR("failed to copy body");
      _mem_free(mp, req->url);
      _mem_free(mp, req);
      if (mp) mp->free(mp);
      return NULL;
    }
    memcpy(body_copy, body->data, body->len);
    req->body.data = body_copy;
    req->body.len = body->len;

    if (body->content_type) {
      req->body.content_type = ccol_strdup(mp, body->content_type);
      if (!req->body.content_type) {
        if (err_str) *err_str = CCOL_ERR_STR("failed to copy content_type");
        _mem_free(mp, body_copy);
        _mem_free(mp, req->url);
        _mem_free(mp, req);
        if (mp) mp->free(mp);
        return NULL;
      }
    }
  }

  return req;
}

ccol_retval_t chttp_request_set_header(chttp_request_t *req, const char *name,
                                       const char *value) {
  if (!req || !name || !value) return ccol_invalid_args;

  if (!req->headers) {
    char *err = NULL;
    chmap hm = chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                                 ccol_string, req->_m_procs, NULL, &err);
    if (!hm) return ccol_not_enough_memory;
    req->headers = hm;
  }

  size_t nlen = strlen(name);
  char *lower = (char *)_mem_alloc(req->_m_procs, nlen + 1);
  if (!lower) return ccol_not_enough_memory;
  for (size_t i = 0; i <= nlen; i++)
    lower[i] = (char)tolower((unsigned char)name[i]);

  cmap_pair kp = {.ptr = lower, .size = nlen + 1};
  cmap_pair vp = {.ptr = (void *)value, .size = strlen(value) + 1};

  ccol_retval_t rv = chmap_insert_elem((chmap)req->headers, &kp, &vp);
  _mem_free(req->_m_procs, lower);
  if (rv == ccol_key_already_present) rv = ccol_success;
  return rv;
}

const char *chttp_request_get_header(const chttp_request_t *req,
                                     const char *name) {
  if (!req || !name || !req->headers) return NULL;

  size_t nlen = strlen(name);
  char *lower = (char *)_mem_alloc(req->_m_procs, nlen + 1);
  if (!lower) return NULL;
  for (size_t i = 0; i <= nlen; i++)
    lower[i] = (char)tolower((unsigned char)name[i]);

  cmap_pair kp = {.ptr = lower, .size = nlen + 1};
  cmap_pair *vp = NULL;
  ccol_retval_t rv = chmap_get_elem_ref((chmap)req->headers, &kp, &vp);
  _mem_free(req->_m_procs, lower);

  if (rv != ccol_success || !vp) return NULL;
  return (const char *)vp->ptr;
}

void chttp_request_free(chttp_request_t *req) {
  if (!req) return;
  ccol_memmgmt_procs_t *mp = req->_m_procs;
  _mem_free(mp, req->url);
  _mem_free(mp, (void *)req->body.data);
  _mem_free(mp, (void *)req->body.content_type);
  if (req->headers) __chmap_destroy((chmap)req->headers);
  _mem_free(mp, req);
  if (mp) mp->free(mp);
}

/* ========================================================================== */
/*                         DEADLINE HELPERS                                   */
/* ========================================================================== */

static chttp_deadline_t _deadline_make(long timeout_ms) {
  chttp_deadline_t d = {0};
  if (timeout_ms <= 0) return d; /* inactive: no limit */
  d.active = true;
  clock_gettime(CLOCK_MONOTONIC, &d.deadline);
  d.deadline.tv_sec += timeout_ms / 1000;
  d.deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
  if (d.deadline.tv_nsec >= 1000000000L) {
    d.deadline.tv_nsec -= 1000000000L;
    d.deadline.tv_sec += 1;
  }
  return d;
}

/*
 * Returns false if the deadline has already elapsed (caller should treat this
 * as an immediate ccol_timed_out). Otherwise sets *out_ms to the remaining
 * milliseconds, or -1 if there is no active deadline (block indefinitely).
 */
static bool _deadline_remaining_ms(const chttp_deadline_t *d, int *out_ms) {
  if (!d->active) {
    *out_ms = -1;
    return true;
  }
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  long ms = (long)(d->deadline.tv_sec - now.tv_sec) * 1000L +
            (d->deadline.tv_nsec - now.tv_nsec) / 1000000L;
  if (ms <= 0) return false;
  *out_ms = (ms > INT_MAX) ? INT_MAX : (int)ms;
  return true;
}

static int _combine_ms(int a, int b) {
  if (a < 0) return b;
  if (b < 0) return a;
  return (a < b) ? a : b;
}

/* Returns whichever of a/b elapses first ("inactive" == no limit, so it never
 * wins over an active one). Used to bound the Expect: 100-continue interim
 * wait by both its own CHTTP_100_CONTINUE_WAIT_MS budget and whatever is
 * left of the request's overall deadline, without needing a second parallel
 * remaining-ms computation the way _tcp_connect's connect/overall pair does
 * (that shape doesn't fit here since the wait lives inside a single
 * _chttp_read_message call, which only accepts one chttp_deadline_t). */
static chttp_deadline_t _deadline_earlier(chttp_deadline_t a,
                                          chttp_deadline_t b) {
  if (!a.active) return b;
  if (!b.active) return a;
  if (a.deadline.tv_sec != b.deadline.tv_sec)
    return (a.deadline.tv_sec < b.deadline.tv_sec) ? a : b;
  return (a.deadline.tv_nsec <= b.deadline.tv_nsec) ? a : b;
}

/* ========================================================================== */
/*                         LOW-LEVEL SOCKET / TLS I/O                         */
/* ========================================================================== */

/*
 * poll() can legitimately return -1/EINTR if a signal is delivered before any
 * fd becomes ready (more frequent under tools like valgrind that use signals
 * internally, but a real possibility in any process). Retry internally,
 * tracking elapsed time against the original timeout budget so repeated
 * interruptions cannot extend the caller's requested wait indefinitely.
 */
static ccol_retval_t _conn_wait(int fd, short events, int timeout_ms) {
  bool has_timeout = (timeout_ms >= 0);
  struct timespec start;
  if (has_timeout) clock_gettime(CLOCK_MONOTONIC, &start);
  int remaining = timeout_ms;

  for (;;) {
    struct pollfd pfd = {.fd = fd, .events = events, .revents = 0};
    int rc = poll(&pfd, 1, remaining);
    if (rc < 0) {
      if (errno == EINTR) {
        if (has_timeout) {
          struct timespec now;
          clock_gettime(CLOCK_MONOTONIC, &now);
          long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L +
                            (now.tv_nsec - start.tv_nsec) / 1000000L;
          remaining = (int)(timeout_ms - elapsed_ms);
          if (remaining <= 0) return ccol_timed_out;
        }
        continue;
      }
      return ccol_http_transfer_aborted;
    }
    if (rc == 0) return ccol_timed_out;
    if (pfd.revents & (POLLERR | POLLNVAL)) return ccol_http_connection_failed;
    return ccol_success;
  }
}

static ssize_t _conn_read(chttp_conn_t *c, void *buf, size_t len) {
  if (c->tls) {
    ssize_t n = ctls_conn_read(c->tls, buf, len);
    if (n < 0 && (errno == EAGAIN)) errno = EWOULDBLOCK;
    return n;
  }
  ssize_t n;
  do {
    n = recv(c->fd, buf, len, 0);
  } while (n < 0 && errno == EINTR);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) errno = EWOULDBLOCK;
  return n;
}

static ssize_t _conn_write(chttp_conn_t *c, const void *buf, size_t len) {
  if (c->tls) {
    ssize_t n = ctls_conn_write(c->tls, buf, len);
    if (n < 0 && (errno == EAGAIN)) errno = EWOULDBLOCK;
    return n;
  }
  ssize_t n;
  do {
    n = send(c->fd, buf, len, MSG_NOSIGNAL);
  } while (n < 0 && errno == EINTR);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) errno = EWOULDBLOCK;
  return n;
}

static ccol_retval_t _chttp_send_all(chttp_conn_t *conn, const char *data,
                                     size_t len, chttp_deadline_t *overall) {
  size_t sent = 0;
  while (sent < len) {
    int wait_ms;
    if (!_deadline_remaining_ms(overall, &wait_ms)) return ccol_timed_out;
    ccol_retval_t prv = _conn_wait(conn->fd, POLLOUT, wait_ms);
    if (prv != ccol_success) return prv;
    ssize_t n = _conn_write(conn, data + sent, len - sent);
    if (n < 0) {
      if (errno == EWOULDBLOCK) continue;
      return ccol_http_transfer_aborted;
    }
    if (n == 0) return ccol_http_transfer_aborted; /* peer closed mid-write */
    sent += (size_t)n;
  }
  return ccol_success;
}

/* Applies TCP_NODELAY to fd, best-effort (a failure here is never fatal to
 * the connection itself, just a missed latency optimisation). Closes a
 * pre-existing asymmetry: this Tier 1 connect path used to set no socket
 * options at all, while Tier 2's connect path already got TCP_NODELAY
 * internally; both tiers now apply it consistently via this one shared
 * helper. Meaningless for a unix domain socket (no TCP layer), so callers
 * only invoke this for an AF_INET/AF_INET6 connection. */
static void _apply_tcp_nodelay(int fd) {
  int one = 1;
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

/* Resolves host:port and connects, trying each address in turn. Combines the
 * connect-specific and overall request deadlines (whichever is tighter). */
static ccol_retval_t _tcp_connect(const char *host, uint16_t port,
                                  chttp_deadline_t *connect_dl,
                                  chttp_deadline_t *overall, int *fd_out) {
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = NULL;
  if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
    return ccol_http_host_resolution_failed;

  ccol_retval_t result = ccol_http_connection_failed;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    int fd =
        socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol);
    if (fd < 0) continue;

    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) {
      _apply_tcp_nodelay(fd);
      *fd_out = fd;
      result = ccol_success;
      break;
    }
    if (errno != EINPROGRESS) {
      close(fd);
      continue;
    }

    int wait_ms, overall_ms;
    if (!_deadline_remaining_ms(connect_dl, &wait_ms)) {
      close(fd);
      result = ccol_timed_out;
      break;
    }
    if (!_deadline_remaining_ms(overall, &overall_ms)) {
      close(fd);
      result = ccol_timed_out;
      break;
    }

    ccol_retval_t prv =
        _conn_wait(fd, POLLOUT, _combine_ms(wait_ms, overall_ms));
    if (prv == ccol_timed_out) {
      close(fd);
      result = ccol_timed_out;
      break;
    }
    if (prv != ccol_success) {
      close(fd);
      result = ccol_http_connection_failed;
      continue;
    }

    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 ||
        soerr != 0) {
      close(fd);
      result = ccol_http_connection_failed;
      continue;
    }

    _apply_tcp_nodelay(fd);
    *fd_out = fd;
    result = ccol_success;
    break;
  }

  freeaddrinfo(res);
  return result;
}

/* Connects to a unix domain stream socket at path, honouring the same
 * connect/overall deadlines _tcp_connect does. No TCP_NODELAY/DNS involved;
 * a unix domain connect() essentially never returns EINPROGRESS on Linux
 * (the accept queue is serviced synchronously in-kernel), but the
 * non-blocking + poll() dance is kept anyway for portability and to honour
 * the deadline even in that rare case. */
static ccol_retval_t _unix_connect(const char *path,
                                   chttp_deadline_t *connect_dl,
                                   chttp_deadline_t *overall, int *fd_out) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  size_t path_len = strlen(path);
  if (path_len >= sizeof(addr.sun_path)) return ccol_http_invalid_url;
  memcpy(addr.sun_path, path, path_len + 1);

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd < 0) return ccol_http_connection_failed;

  int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (rc == 0) {
    *fd_out = fd;
    return ccol_success;
  }
  if (errno != EINPROGRESS) {
    close(fd);
    return ccol_http_connection_failed;
  }

  int wait_ms, overall_ms;
  if (!_deadline_remaining_ms(connect_dl, &wait_ms)) {
    close(fd);
    return ccol_timed_out;
  }
  if (!_deadline_remaining_ms(overall, &overall_ms)) {
    close(fd);
    return ccol_timed_out;
  }
  ccol_retval_t prv = _conn_wait(fd, POLLOUT, _combine_ms(wait_ms, overall_ms));
  if (prv != ccol_success) {
    close(fd);
    return prv;
  }

  int soerr = 0;
  socklen_t slen = sizeof(soerr);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 || soerr != 0) {
    close(fd);
    return ccol_http_connection_failed;
  }

  *fd_out = fd;
  return ccol_success;
}

/* Drives the client-mode TLS handshake to completion, honouring both the
 * connect and overall deadlines (TLS handshake time is folded into the
 * connect timeout, matching curl's own behaviour). */
static ccol_retval_t _tls_handshake(chttp_conn_t *conn,
                                    chttp_deadline_t *connect_dl,
                                    chttp_deadline_t *overall) {
  for (;;) {
    ctls_handshake_result_t r = ctls_conn_handshake_step(conn->tls);
    if (r == CTLS_HANDSHAKE_DONE) return ccol_success;
    if (r == CTLS_HANDSHAKE_ERROR) {
      /* 0 == X509_V_OK by OpenSSL convention; ctls.h intentionally does not
       * expose OpenSSL headers to callers, so the raw value is compared
       * directly rather than via the X509_V_OK symbol. */
      long vr = ctls_conn_verify_result(conn->tls);
      return (vr != 0) ? ccol_http_tls_cert_verification_failed
                       : ccol_http_tls_handshake_failed;
    }

    short ev = (r == CTLS_HANDSHAKE_WANT_WRITE) ? POLLOUT : POLLIN;
    int wait_ms, overall_ms;
    if (!_deadline_remaining_ms(connect_dl, &wait_ms)) return ccol_timed_out;
    if (!_deadline_remaining_ms(overall, &overall_ms)) return ccol_timed_out;
    ccol_retval_t prv =
        _conn_wait(conn->fd, ev, _combine_ms(wait_ms, overall_ms));
    if (prv != ccol_success) return prv;
  }
}

static ccol_retval_t _conn_open(ccol_memmgmt_procs_t *mp,
                                const chttp_url_t *url, bool want_tls,
                                ctls_ctx_t *tls_ctx, bool verify_host,
                                chttp_deadline_t *connect_dl,
                                chttp_deadline_t *overall, chttp_conn_t *out) {
  memset(out, 0, sizeof(*out));
  out->fd = -1;

  int fd = -1;
  ccol_retval_t rv =
      url->is_unix
          ? _unix_connect(url->unix_socket_path, connect_dl, overall, &fd)
          : _tcp_connect(url->host, url->port, connect_dl, overall, &fd);
  if (rv != ccol_success) return rv;
  out->fd = fd;

  if (want_tls) {
    out->tls =
        ctls_conn_create_client(tls_ctx, fd, url->host, verify_host, NULL);
    if (!out->tls) {
      close(fd);
      out->fd = -1;
      return ccol_not_enough_memory;
    }
    rv = _tls_handshake(out, connect_dl, overall);
    if (rv != ccol_success) {
      ctls_conn_destroy(out->tls);
      close(fd);
      out->fd = -1;
      out->tls = NULL;
      return rv;
    }
  }

  out->origin_key = ccol_strdup(mp, url->origin_key);
  if (!out->origin_key) {
    if (out->tls) ctls_conn_destroy(out->tls);
    close(fd);
    out->fd = -1;
    out->tls = NULL;
    return ccol_not_enough_memory;
  }

  clock_gettime(CLOCK_MONOTONIC, &out->last_used);
  return ccol_success;
}

static void _conn_teardown(ccol_memmgmt_procs_t *mp, chttp_conn_t *c) {
  if (!c) return;
  if (c->tls) ctls_conn_destroy(c->tls);
  if (c->fd >= 0) close(c->fd);
  _mem_free(mp, c->origin_key);
  memset(c, 0, sizeof(*c));
  c->fd = -1;
}

/* ========================================================================== */
/*                         CONCURRENCY LIMITER                                */
/* ========================================================================== */

static size_t _resolve_pool_cap(size_t configured) {
  if (configured != 0) return configured;
  long np = sysconf(_SC_NPROCESSORS_ONLN);
  return (np > 0) ? (size_t)np : 1;
}

static ccol_retval_t _slot_acquire(struct chttpclient *cli) {
  mutex_lock(cli->lock);
  if (cli->destroying) {
    mutex_unlock(cli->lock);
    return ccol_not_permitted;
  }
  if (!cli->pool_initialized) {
    cli->pool_cap = _resolve_pool_cap(cli->configured_pool_size);
    cli->pool_initialized = true;
  }
  while (cli->in_flight_count >= cli->pool_cap && !cli->destroying)
    cond_var_wait(cli->available, cli->lock);
  if (cli->destroying) {
    mutex_unlock(cli->lock);
    return ccol_not_permitted;
  }
  cli->in_flight_count++;
  mutex_unlock(cli->lock);
  return ccol_success;
}

static void _slot_release(struct chttpclient *cli) {
  mutex_lock(cli->lock);
  cli->in_flight_count--;
  cond_var_broadcast(cli->available);
  mutex_unlock(cli->lock);
}

/* ========================================================================== */
/*                         KEEP-ALIVE IDLE POOL                               */
/* ========================================================================== */

/* chmap_entry is packed, so reading a stored pointer-sized value via a direct
 * cast is UB at -O3; use memcpy like cjson/cyaml do. */
static inline cvec _read_cvec(const void *src) {
  cvec v;
  memcpy(&v, src, sizeof(v));
  return v;
}

/*
 * Attempts to pop a usable idle connection for `origin_key` into *out.
 * Returns false if none is available. May pop and discard several stale/dead
 * candidates before finding a live one or exhausting the list; the age check
 * and liveness probe both happen OUTSIDE the client lock (no I/O while
 * holding it).
 */
static bool _idle_pool_take(struct chttpclient *cli, const char *origin_key,
                            chttp_conn_t *out) {
  for (;;) {
    bool got = false;
    mutex_lock(cli->lock);
    if (cli->idle_pools) {
      cmap_pair kp = {.ptr = (void *)origin_key,
                      .size = strlen(origin_key) + 1};
      cmap_pair *vp = NULL;
      if (chmap_get_elem_ref(cli->idle_pools, &kp, &vp) == ccol_success && vp) {
        cvec list = _read_cvec(vp->ptr);
        if (list && cvector_elem_count(list) > 0 &&
            cvector_pop_back(list, out) == ccol_success) {
          got = true;
          if (cli->idle_total_count > 0) cli->idle_total_count--;
        }
      }
    }
    mutex_unlock(cli->lock);
    if (!got) return false;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long age_ms = (now.tv_sec - out->last_used.tv_sec) * 1000L +
                  (now.tv_nsec - out->last_used.tv_nsec) / 1000000L;

    bool alive = false;
    if (age_ms <= CHTTP_IDLE_MAX_AGE_MS) {
      char probe;
      ssize_t pn = recv(out->fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
      alive = (pn < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }
    if (alive) return true;

    _conn_teardown(cli->m_procs, out);
    /* loop: try the next candidate (if any) for this origin */
  }
}

/*
 * Offers a still-good connection back to the idle pool, bounded by per-origin
 * and total caps. If it doesn't fit (caps hit, or the client is being
 * destroyed), the connection is simply closed instead; a lost optimisation
 * opportunity, never a correctness issue.
 */
static void _idle_pool_offer(struct chttpclient *cli, chttp_conn_t *c) {
  bool pooled = false;
  mutex_lock(cli->lock);
  if (!cli->destroying && cli->idle_pools &&
      cli->idle_total_count < CHTTP_MAX_IDLE_TOTAL) {
    cmap_pair kp = {.ptr = c->origin_key, .size = strlen(c->origin_key) + 1};
    cmap_pair *vp = NULL;
    cvec list = NULL;
    if (chmap_get_elem_ref(cli->idle_pools, &kp, &vp) == ccol_success && vp) {
      list = _read_cvec(vp->ptr);
    }
    if (!list) {
      char *cverr = NULL;
      list = cvector_create_full(sizeof(chttp_conn_t), cli->m_procs, &cverr);
      if (list) {
        cmap_pair vp2 = {.ptr = &list, .size = sizeof(list)};
        if (chmap_insert_elem(cli->idle_pools, &kp, &vp2) != ccol_success) {
          __cvector_destroy(list);
          list = NULL;
        }
      }
    }
    if (list && cvector_elem_count(list) < CHTTP_MAX_IDLE_PER_ORIGIN) {
      clock_gettime(CLOCK_MONOTONIC, &c->last_used);
      if (cvector_push_back(list, c) == ccol_success) {
        pooled = true;
        cli->idle_total_count++;
      }
    }
  }
  mutex_unlock(cli->lock);
  if (!pooled) _conn_teardown(cli->m_procs, c);
}

/* ========================================================================== */
/*                         REQUEST SERIALIZATION                              */
/* ========================================================================== */

/*
 * Return true if the headers map already contains a key that compares equal
 * to "content-type" case-insensitively. Scans all keys rather than doing a
 * map lookup so that borrowed maps (e.g. from chttp_run_query) with
 * mixed-case keys like "Content-Type" are also detected.
 */
static bool _map_has_content_type(chmap headers) {
  if (!headers) return false;
  cmap_iterator *it = chashmap_begin_iter(headers, NULL);
  for (; it; it = it->_next_fn(it)) {
    if (strcasecmp((const char *)it->key_pair->ptr, "content-type") == 0) {
      ccol_iter_destroy(it);
      return true;
    }
  }
  return false;
}

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  ccol_memmgmt_procs_t *mp;
  bool oom;
} chttp_outbuf_t;

static void _ob_append(chttp_outbuf_t *b, const char *data, size_t n) {
  if (b->oom) return;
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 1024;
    while (nc < b->len + n + 1) nc *= 2;
    char *nb = (char *)_mem_realloc(b->mp, b->buf, nc);
    if (!nb) {
      b->oom = true;
      return;
    }
    b->buf = nb;
    b->cap = nc;
  }
  memcpy(b->buf + b->len, data, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

static void _ob_append_cstr(chttp_outbuf_t *b, const char *s) {
  _ob_append(b, s, strlen(s));
}

/*
 * Serialises method line + headers + body into a single wire buffer.
 * Redirect-hop method/body substitution is the caller's responsibility (via
 * the method/body fields of `req`, which is a shallow per-hop view, not the
 * caller's original request object).
 */
static ccol_retval_t _serialize_request(ccol_memmgmt_procs_t *mp,
                                        const chttp_request_t *req,
                                        const chttp_url_t *url,
                                        const char *auto_authorization,
                                        char **out_buf, size_t *out_len) {
  chttp_outbuf_t ob = {.mp = mp};

  _ob_append_cstr(&ob, chttp_method_str(req->method));
  _ob_append(&ob, " ", 1);
  _ob_append_cstr(&ob, url->path_and_query);
  _ob_append_cstr(&ob, " HTTP/1.1\r\n");

  bool has_host = chttp_request_get_header(req, "host") != NULL;
  bool has_accept = chttp_request_get_header(req, "accept") != NULL;
  bool has_ua = chttp_request_get_header(req, "user-agent") != NULL;
  bool has_cl = chttp_request_get_header(req, "content-length") != NULL;
  bool has_ct = _map_has_content_type((chmap)req->headers);
  bool has_auth = chttp_request_get_header(req, "authorization") != NULL;

  if (!has_host) {
    if (url->is_unix) {
      /* A unix-domain-socket target has no real hostname/port to convey;
       * "localhost" matches curl's own --unix-socket default and is simple
       * and predictable for any server on the other end to expect (RFC 7230
       * SS5.4 still requires every HTTP/1.1 request to carry a Host header,
       * even though its value is meaningless here). */
      _ob_append_cstr(&ob, "host: localhost\r\n");
    } else {
      bool default_port = (url->is_https && url->port == 443) ||
                          (!url->is_https && url->port == 80);
      _ob_append_cstr(&ob, "host: ");
      if (url->is_ipv6) _ob_append(&ob, "[", 1);
      _ob_append_cstr(&ob, url->host);
      if (url->is_ipv6) _ob_append(&ob, "]", 1);
      if (!default_port) {
        char portbuf[16];
        int pn = snprintf(portbuf, sizeof(portbuf), ":%u", (unsigned)url->port);
        if (pn > 0) _ob_append(&ob, portbuf, (size_t)pn);
      }
      _ob_append(&ob, "\r\n", 2);
    }
  }
  if (!has_accept) _ob_append_cstr(&ob, "accept: */*\r\n");
  if (!has_ua)
    _ob_append_cstr(&ob, "user-agent: c_collections-chttpclient/1.0\r\n");
  if (!has_auth && auto_authorization) {
    _ob_append_cstr(&ob, "authorization: ");
    _ob_append_cstr(&ob, auto_authorization);
    _ob_append(&ob, "\r\n", 2);
  }

  if (req->headers) {
    cmap_iterator *it = chashmap_begin_iter((chmap)req->headers, NULL);
    for (; it; it = it->_next_fn(it)) {
      const char *name = (const char *)it->key_pair->ptr;
      const char *val = (const char *)it->val_pair->ptr;
      if (strcasecmp(name, "host") == 0) continue; /* already emitted above */
      _ob_append_cstr(&ob, name);
      _ob_append(&ob, ": ", 2);
      _ob_append_cstr(&ob, val);
      _ob_append(&ob, "\r\n", 2);
    }
  }

  if (!has_ct && req->body.data && req->body.len > 0 &&
      req->body.content_type) {
    _ob_append_cstr(&ob, "content-type: ");
    _ob_append_cstr(&ob, req->body.content_type);
    _ob_append(&ob, "\r\n", 2);
  }

  bool body_carrying_method =
      (req->method == CHTTP_POST || req->method == CHTTP_PUT ||
       req->method == CHTTP_PATCH);
  if (body_carrying_method && !has_cl) {
    char clbuf[48];
    int cln = snprintf(clbuf, sizeof(clbuf), "content-length: %zu\r\n",
                       req->body.data ? req->body.len : (size_t)0);
    if (cln > 0) _ob_append(&ob, clbuf, (size_t)cln);
  }

  bool has_expect = chttp_request_get_header(req, "expect") != NULL;
  if (!has_expect && req->expect_continue && body_carrying_method &&
      req->body.data && req->body.len > 0) {
    _ob_append_cstr(&ob, "expect: 100-continue\r\n");
  }

  _ob_append(&ob, "\r\n", 2);

  if (body_carrying_method && req->body.data && req->body.len > 0)
    _ob_append(&ob, (const char *)req->body.data, req->body.len);

  if (ob.oom) {
    _mem_free(mp, ob.buf);
    return ccol_not_enough_memory;
  }
  *out_buf = ob.buf;
  *out_len = ob.len;
  return ccol_success;
}

/* ========================================================================== */
/*                         CHTTP1_PARSER INTEGRATION                          */
/* ========================================================================== */

static size_t _sink_discard(const void *data, size_t len, void *ctx) {
  (void)data;
  (void)ctx;
  return len;
}

static size_t _sink_buffered(const void *data, size_t len, void *ctx) {
  chttp_bodybuf_t *bb = (chttp_bodybuf_t *)ctx;
  if (len == 0) return 0;
  if (bb->len + len + 1 > bb->cap) {
    size_t nc = bb->cap ? bb->cap * 2 : 4096;
    while (nc < bb->len + len + 1) nc *= 2;
    char *nb = (char *)_mem_realloc(bb->mp, bb->buf, nc);
    if (!nb) {
      bb->oom = true;
      return 0;
    }
    bb->buf = nb;
    bb->cap = nc;
  }
  memcpy(bb->buf + bb->len, data, len);
  bb->len += len;
  bb->buf[bb->len] = '\0';
  return len;
}

/* chttp1_parser hands a header/trailer line to this callback whole (name and
 * value already split and OWS-trimmed), so there is no accumulator state to
 * maintain here at all. name/value point into the parser's own internal
 * line buffer and are only valid for this call, so BOTH need a local,
 * NUL-terminated copy before use: name because it must be lower-cased, and
 * value because this codebase's cmap_pair convention for string values is
 * "size = strlen + 1" (chmap_insert_elem copies exactly that many bytes);
 * reading value_len+1 raw bytes directly out of the parser's own line
 * buffer would read one uncontrolled byte past the value itself, which is
 * not guaranteed to be '\0'. Both buffers are sized to CHTTP1_MAX_LINE_LEN+1,
 * more than enough for any substring of one line. */
static int _on_header(chttp1_parser_t *p, const char *name, size_t name_len,
                      const char *value, size_t value_len) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;

  char lower_name[CHTTP1_MAX_LINE_LEN + 1];
  for (size_t i = 0; i < name_len; i++)
    lower_name[i] = (char)tolower((unsigned char)name[i]);
  lower_name[name_len] = '\0';

  char value_copy[CHTTP1_MAX_LINE_LEN + 1];
  memcpy(value_copy, value, value_len);
  value_copy[value_len] = '\0';

  cmap_pair kp = {.ptr = lower_name, .size = name_len + 1};
  cmap_pair vp = {.ptr = value_copy, .size = value_len + 1};
  ccol_retval_t rv = chmap_insert_elem(ctx->headers, &kp, &vp);
  if (rv != ccol_success && rv != ccol_key_already_present) {
    ctx->error = true;
    return 1;
  }
  return 0;
}

static int _on_headers_complete(chttp1_parser_t *p) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  ctx->status_code = p->status_code;

  bool is_redirect_status = ctx->status_code == 301 ||
                            ctx->status_code == 302 ||
                            ctx->status_code == 303 ||
                            ctx->status_code == 307 || ctx->status_code == 308;
  if (is_redirect_status && ctx->redirects_still_allowed) {
    cmap_pair kp = {.ptr = (void *)"location", .size = sizeof("location")};
    cmap_pair *vp = NULL;
    if (chmap_get_elem_ref(ctx->headers, &kp, &vp) == ccol_success && vp) {
      ctx->location = ccol_strdup(ctx->mp, (const char *)vp->ptr);
      if (!ctx->location) {
        ctx->error = true;
        return -1; /* anything outside {0, 1} aborts with CHTTP1_USER */
      }
      ctx->will_redirect = true;
    }
  }

  ctx->sink_fn = ctx->will_redirect ? _sink_discard : ctx->requested_sink_fn;
  ctx->sink_ctx = ctx->will_redirect ? NULL : ctx->requested_sink_ctx;

  /* A HEAD response's Content-Length (if any) describes a body that was
   * never sent; this is the one case the parser cannot infer from the wire
   * on its own. */
  return ctx->is_head_request ? 1 : 0;
}

static int _on_body(chttp1_parser_t *p, const char *at, size_t len) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  size_t n = ctx->sink_fn ? ctx->sink_fn(at, len, ctx->sink_ctx) : len;
  if (n != len) {
    ctx->aborted = true;
    return 1;
  }
  return 0;
}

static int _on_message_complete(chttp1_parser_t *p) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  ctx->message_complete = true;
  return 0;
}

static struct {
  chttp1_settings_t settings;
  once_flag_t once;
} client_http1_settings_bundler = {0};

static void _init_chttp1_settings(void) {
  chttp1_settings_init(&client_http1_settings_bundler.settings);
  client_http1_settings_bundler.settings.on_header = _on_header;
  client_http1_settings_bundler.settings.on_headers_complete =
      _on_headers_complete;
  client_http1_settings_bundler.settings.on_body = _on_body;
  client_http1_settings_bundler.settings.on_message_complete =
      _on_message_complete;
}

static void _parse_ctx_free_fields(chttp_parse_ctx_t *ctx) {
  _mem_free(ctx->mp, ctx->location);
  if (ctx->headers) __chmap_destroy(ctx->headers);
  ctx->location = NULL;
  ctx->headers = NULL;
}

/* Resets ctx to parse a SECOND, logically distinct message on the same
 * connection: specifically, the real final response following a "100
 * Continue" interim response the same ctx was just used to parse. Mirrors
 * chttp_do_internal's own "fresh state per hop" convention (a fresh
 * chttp_parse_ctx_t per redirect hop) at the sub-hop granularity this one
 * connection's two-message exchange needs; the interim response's own
 * (rare, but legal) headers must never leak into the final response's
 * header map. is_head_request/redirects_still_allowed are left untouched
 * (properties of the request, not of any one parsed message);
 * requested_sink_fn/requested_sink_ctx are also left untouched (the
 * caller's real sink config); sink_fn/sink_ctx are cleared since
 * _on_headers_complete resolves them fresh for the message it's parsing. */
static ccol_retval_t _parse_ctx_reset_for_continue(chttp_parse_ctx_t *ctx) {
  _mem_free(ctx->mp, ctx->location);
  ctx->location = NULL;
  if (ctx->headers) __chmap_destroy(ctx->headers);
  char *herr = NULL;
  ctx->headers =
      chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_string, ctx->mp, NULL, &herr);
  ctx->will_redirect = false;
  ctx->message_complete = false;
  ctx->trailing_garbage = false;
  ctx->error = false;
  ctx->aborted = false;
  ctx->status_code = 0;
  ctx->sink_fn = NULL;
  ctx->sink_ctx = NULL;
  return ctx->headers ? ccol_success : ccol_not_enough_memory;
}

/*
 * Reads and parses exactly one HTTP/1.1 message from `conn`, starting with
 * whatever bytes are already available in `carry_in` (if any; fed to the
 * parser before ever touching the socket) and falling back to ordinary
 * deadline-bounded socket reads once carry_in is exhausted. On
 * ccol_success, *keep_alive_out reflects chttp1_parser's own keep-alive
 * bookkeeping (NOT yet downgraded for trailing garbage; see
 * _chttp_read_response_carry, the only caller that should be treating
 * leftover bytes as garbage in the first place).
 *
 * Unlike treating any bytes past the message boundary as trailing garbage
 * outright, this function reports them via leftover_out/leftover_len_out
 * (heap-allocated with pctx->mp; NULL/0 when there is none) instead;
 * needed so chttp_do_internal's Expect: 100-continue handling can carry a
 * fast server's real final-response bytes forward into a second parse, if
 * they happened to arrive in the same read as the "100 Continue" interim
 * status line. Every other caller has no second message to carry them into
 * and should treat a non-empty leftover exactly like the trailing garbage
 * it actually is; see _chttp_read_response_carry.
 *
 * *any_bytes_read_out is set to true the moment the first byte of the
 * message is actually received off the wire (carry_in bytes, having already
 * been read by the caller in an earlier call, do not count). Callers use
 * this to decide whether a failure is safe to silently retry against a
 * fresh connection (nothing has been parsed or handed to the caller yet)
 * versus one that must be surfaced (partial response already in flight,
 * possibly already streamed out to a user callback).
 */
static ccol_retval_t _chttp_read_message(
    chttp_conn_t *conn, chttp_parse_ctx_t *pctx, chttp_deadline_t *overall,
    const char *carry_in, size_t carry_in_len, bool *keep_alive_out,
    bool *any_bytes_read_out, char **leftover_out, size_t *leftover_len_out) {
  call_once(client_http1_settings_bundler.once, _init_chttp1_settings);

  chttp1_parser_t parser;
  chttp1_parser_init(&parser, &client_http1_settings_bundler.settings);
  parser.data = pctx;

  *leftover_out = NULL;
  *leftover_len_out = 0;

  if (carry_in_len > 0) {
    chttp1_errno_t err = chttp1_parser_execute(&parser, carry_in, carry_in_len);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(&parser);
      *keep_alive_out = chttp1_should_keep_alive(&parser);
      if (consumed < carry_in_len) {
        size_t rem = carry_in_len - consumed;
        char *lb = (char *)_mem_alloc(pctx->mp, rem);
        if (!lb) return ccol_not_enough_memory;
        memcpy(lb, carry_in + consumed, rem);
        *leftover_out = lb;
        *leftover_len_out = rem;
      }
      return ccol_success;
    }
    if (err == CHTTP1_USER) {
      return pctx->error ? ccol_not_enough_memory : ccol_http_transfer_aborted;
    }
    if (err != CHTTP1_OK) return ccol_http_transfer_aborted;
    /* else: message not yet complete, fall through to reading more */
  }

  char buf[8192];
  for (;;) {
    int wait_ms;
    if (!_deadline_remaining_ms(overall, &wait_ms)) return ccol_timed_out;
    ccol_retval_t prv = _conn_wait(conn->fd, POLLIN, wait_ms);
    if (prv != ccol_success) return prv;

    ssize_t n = _conn_read(conn, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EWOULDBLOCK) continue;
      return ccol_http_transfer_aborted;
    }
    if (n > 0) *any_bytes_read_out = true;
    if (n == 0) {
      /* CHTTP1_PAUSED here (not just CHTTP1_OK) is the expected outcome for
       * a valid EOF-delimited body (HTTP/1.0-style, or an explicit
       * Connection: close with no Content-Length/chunked framing): see
       * chttp1_parser_finish's own doc comment. Only CHTTP1_ERROR (a
       * genuinely truncated message) falls through to the aborted case. */
      chttp1_errno_t fe = chttp1_parser_finish(&parser);
      if ((fe != CHTTP1_OK && fe != CHTTP1_PAUSED) || !pctx->message_complete)
        return ccol_http_transfer_aborted;
      *keep_alive_out = false; /* peer closed; nothing left to reuse */
      return ccol_success;
    }

    chttp1_errno_t err = chttp1_parser_execute(&parser, buf, (size_t)n);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(&parser);
      *keep_alive_out = chttp1_should_keep_alive(&parser);
      if (consumed < (size_t)n) {
        size_t rem = (size_t)n - consumed;
        char *lb = (char *)_mem_alloc(pctx->mp, rem);
        if (!lb) return ccol_not_enough_memory;
        memcpy(lb, buf + consumed, rem);
        *leftover_out = lb;
        *leftover_len_out = rem;
      }
      return ccol_success;
    }
    if (err == CHTTP1_USER) {
      return pctx->error ? ccol_not_enough_memory : ccol_http_transfer_aborted;
    }
    if (err != CHTTP1_OK) return ccol_http_transfer_aborted;
    /* else: message not yet complete, need more data */
  }
}

/* _chttp_read_message, with any leftover bytes past the parsed message's own
 * boundary collapsed into the ordinary trailing-garbage handling every
 * caller except chttp_do_internal's Expect: 100-continue path actually
 * wants (downgrades keep_alive_out to false, exactly matching this
 * function's previous, inlined behavior before _chttp_read_message was
 * split out). carry_in/carry_in_len are forwarded unchanged. */
static ccol_retval_t _chttp_read_response_carry(
    chttp_conn_t *conn, chttp_parse_ctx_t *pctx, chttp_deadline_t *overall,
    const char *carry_in, size_t carry_in_len, bool *keep_alive_out,
    bool *any_bytes_read_out) {
  char *leftover = NULL;
  size_t leftover_len = 0;
  ccol_retval_t rv = _chttp_read_message(
      conn, pctx, overall, carry_in, carry_in_len, keep_alive_out,
      any_bytes_read_out, &leftover, &leftover_len);
  if (rv == ccol_success && leftover_len > 0) {
    pctx->trailing_garbage = true;
    *keep_alive_out = false;
  }
  _mem_free(pctx->mp, leftover);
  return rv;
}

/* Reads and parses exactly one HTTP/1.1 response from `conn`. See
 * _chttp_read_message's own doc comment for *keep_alive_out/
 * *any_bytes_read_out semantics (trailing-garbage-adjusted here, unlike that
 * lower-level function).
 */
static ccol_retval_t _chttp_read_response(chttp_conn_t *conn,
                                          chttp_parse_ctx_t *pctx,
                                          chttp_deadline_t *overall,
                                          bool *keep_alive_out,
                                          bool *any_bytes_read_out) {
  return _chttp_read_response_carry(conn, pctx, overall, NULL, 0,
                                    keep_alive_out, any_bytes_read_out);
}

/* ========================================================================== */
/*                         TLS CONTEXT MANAGEMENT                             */
/* ========================================================================== */

static bool _file_readable(const char *path) {
  return path && access(path, R_OK) == 0;
}

/*
 * Builds (or rebuilds) cli->tls_ctx from cli->tls. Called both at client
 * construction (default config) and from chttpclient_set_tls.
 *
 * ctls_ctx_cert_add/_trust report failure via an ordinary ccol_retval_t and
 * never touch the process's own lifetime (a missing/unreadable file is
 * always a recoverable error here, never a process abort). chttpclient_set_tls
 * must still be able to accept a syntactically valid but currently-nonexistent
 * path without crashing (callers may configure TLS well before ever making an
 * HTTPS request, or never make one at all); so the configured paths are
 * still validated with access() BEFORE ever calling into ctls. If they don't
 * check out, no context is built here and the failure is deferred to actual
 * connection time (see chttp_do_internal), where it surfaces as a normal
 * ccol_retval_t exactly as before. This pre-check is kept (rather than
 * relying solely on ctls_ctx_cert_add's own graceful failure) to preserve
 * this function's existing "always returns ccol_success, failure is always
 * deferred" contract unchanged.
 */
static ccol_retval_t _rebuild_tls_ctx_locked(struct chttpclient *cli) {
  if (cli->tls_ctx) {
    ctls_ctx_release(cli->tls_ctx);
    cli->tls_ctx = NULL;
  }
  cli->tls_ctx_usable = false;

  bool have_cert_pair = cli->tls.cert_path && cli->tls.key_path;
  if (have_cert_pair && (!_file_readable(cli->tls.cert_path) ||
                         !_file_readable(cli->tls.key_path))) {
    return ccol_success; /* deferred failure, see comment above */
  }
  if (cli->tls.ca_bundle_path && !_file_readable(cli->tls.ca_bundle_path)) {
    return ccol_success; /* deferred failure, see comment above */
  }

  ctls_ctx_t *ctx = ctls_ctx_new_mp(cli->m_procs, NULL);
  if (!ctx) return ccol_not_enough_memory;

  if (have_cert_pair) {
    if (ctls_ctx_cert_add(ctx, NULL, cli->tls.cert_path, cli->tls.key_path,
                          NULL, NULL) != ccol_success) {
      ctls_ctx_release(ctx);
      return ccol_success; /* deferred failure, see comment above */
    }
  }

  if (cli->tls.ca_bundle_path) {
    if (ctls_ctx_trust(ctx, cli->tls.ca_bundle_path, NULL) != ccol_success) {
      ctls_ctx_release(ctx);
      return ccol_success; /* deferred failure, see comment above */
    }
  } else if (cli->tls.verify_peer || cli->tls.verify_host) {
    /* verify_host implies verify_peer: hostname matching against a
     * certificate whose chain was never validated (SSL_VERIFY_NONE, no
     * trust store configured) gives no real security guarantee: the
     * certificate itself could be entirely attacker-forged. Without this,
     * a caller setting verify_peer=false, verify_host=true (plausible if
     * the two are read as independent toggles, which chttp_tls_config_t's
     * field comments now warn against) got a false sense of security: the
     * hostname check would run and "pass" against literally any
     * self-signed certificate for that hostname. */
    ctls_ctx_trust_system(ctx);
  }

  cli->tls_ctx = ctx;
  cli->tls_ctx_usable = true;
  return ccol_success;
}

/* ========================================================================== */
/*                    SHARED STATIC REACTOR (chttpclient's OWN engine)        */
/* ========================================================================== */

/*
 * One static, process-wide event_loop reactor shared by every chttpcli
 * instance in the process (including the lazily-created default client);
 * One of two separate, independent reactors (the other belongs to chttpserver;
 * see chttpserver.c's own identical lifecycle wrapper). chttpclient does not
 * share a process-wide singleton with chttpserver, so this lifecycle wrapper
 * needs no cross-module ordering at all, only ref-counting across Tier 2/3
 * callers; it mirrors chttpserver.c's own g_reactor
 * acquire/release/reaper-thread shape exactly, with chttpclient's own
 * additional resources (cli_engine_bundler.dns_pool, the deadline sweep)
 * layered on top and torn down in lockstep with it.
 */
static struct {
  event_loop reactor;
  size_t reactor_refs;
  mutex_t mutex;
  cond_var_t stopped_cv;
  once_flag_t once;
  bool stopping;
  thread_id_t reaper_thread;
  bool reaper_joinable;
  ccol_memmgmt_procs_t mprocs_storage;
  ccol_memmgmt_procs_t *mprocs;

  /* 0 = auto-detect via sysconf(_SC_NPROCESSORS_ONLN), this module's
   * original, still-default behavior. A positive value pins the reactor to
   * exactly that many OS threads instead; see
   * chttpcli_set_engine_num_reactor_threads's own doc comment. Baked into
   * the reactor at construction time, same "before first start, or after a
   * full stop" restriction as mprocs above. */
  size_t num_reactor_threads;
  /* The value actually passed to event_loop_create_with_mprocs the last time
   * the reactor was created (auto-detected or explicit); for test
   * instrumentation only, see
   * _chttpclient_engine_num_reactor_threads_for_tests below. */
  size_t last_resolved_num_reactor_threads;

  /* Diagnostics logger for chttpclient's own reactor-thread events (TLS
   * handshake failures, connect errors). NULL (the default) means diagnostics
   * are simply skipped; there is no default logger installed, since this
   * event_loop reactor has no internal logging of its own to forward. Guarded
   * by cli_engine_bundler.mutex purely against a torn pointer read/write racing
   * a concurrent chttpcli_set_engine_logger() call (clog itself is already
   * thread-safe for concurrent logging calls through one handle). */
  clog logger;

  /* Offloads the (potentially blocking) DNS-resolve-and-connect step off of
   * caller threads. Lives and dies with the reactor itself (created/destroyed
   * alongside it, guarded by cli_engine_bundler.mutex) rather than being a
   * separate lazy singleton, since nothing needs it once the reactor is down.
   */
  ctpool dns_pool;
} cli_engine_bundler = {0};

static ccol_retval_t _client_deadline_sweep_start(void);
static void _client_deadline_sweep_stop_and_join(void);

static void _client_engine_globals_init(void) {
  mutex_init(cli_engine_bundler.mutex);
  cond_var_init(cli_engine_bundler.stopped_cv);
  /* Belt-and-suspenders: Tier 1 already passes MSG_NOSIGNAL to every send(),
   * and Tier 2/3's own raw writes do the same (see _async_on_writable), so
   * this is not load-bearing the way chttpserver.c's identical call is for
   * its own worker-thread writes; but it costs nothing and protects any
   * future raw write path in this file from an unexpected process-killing
   * SIGPIPE regardless. */
  signal(SIGPIPE, SIG_IGN);
}

static clog _client_engine_logger_get(void) {
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  clog l = cli_engine_bundler.logger;
  mutex_unlock(cli_engine_bundler.mutex);
  return l;
}

static void _client_engine_join_reaper_if_needed_locked(void) {
  if (cli_engine_bundler.reaper_joinable) {
    thread_join(cli_engine_bundler.reaper_thread);
    cli_engine_bundler.reaper_joinable = false;
  }
}

/*
 * Runs on a freshly spawned thread (never inline on the calling thread that
 * dropped the last reference; that thread is routinely a reactor callback
 * thread itself, e.g. _async_on_error tearing down the last live ctx, and
 * must never block on joining the deadline sweep or draining
 * cli_engine_bundler.dns_pool). Tears down the deadline sweep and
 * cli_engine_bundler.dns_pool, destroys the reactor, then clears
 * cli_engine_bundler.stopping so a waiting acquirer can proceed.
 */
static void *_client_engine_reaper_fn(void *arg) {
  (void)arg;
  event_loop loop_to_destroy;
  mutex_lock(cli_engine_bundler.mutex);
  loop_to_destroy = cli_engine_bundler.reactor;
  mutex_unlock(cli_engine_bundler.mutex);

  /* Stop the deadline sweep before tearing down the reactor and DNS pool it
   * may still be referencing (a sweep tick closes a fd/removes a
   * registration directly; see _client_deadline_sweep_once). */
  _client_deadline_sweep_stop_and_join();
  if (loop_to_destroy) event_loop_destroy(loop_to_destroy);

  mutex_lock(cli_engine_bundler.mutex);
  ctpool_destroy(cli_engine_bundler.dns_pool); /* implicit drain shutdown */
  cli_engine_bundler.dns_pool = NULL;
  cli_engine_bundler.reactor = NULL;
  cli_engine_bundler.stopping = false;
  log_info(cli_engine_bundler.logger,
           "The http client reactor engine has been destroyed");
  clog old_logger = cli_engine_bundler.logger;
  cli_engine_bundler.logger = NULL;
  cond_var_broadcast(cli_engine_bundler.stopped_cv);
  mutex_unlock(cli_engine_bundler.mutex);
  if (old_logger) clog_close(old_logger);
  return NULL;
}

static void _client_engine_spawn_reaper(void) {
  thread_id_t reaper;
  if (thread_create(reaper, _client_engine_reaper_fn, NULL) != 0) {
    /* No safer fallback than running it inline (OOM-class failure); nothing
     * to join afterward since it already ran to completion synchronously. */
    _client_engine_reaper_fn(NULL);
    return;
  }
  mutex_lock(cli_engine_bundler.mutex);
  cli_engine_bundler.reaper_thread = reaper;
  cli_engine_bundler.reaper_joinable = true;
  mutex_unlock(cli_engine_bundler.mutex);
}

/*
 * Acquires a reference to chttpclient's own reactor, lazily creating it (and
 * cli_engine_bundler.dns_pool, and starting the deadline sweep) on the first
 * call. Subsequent calls just bump cli_engine_bundler.reactor_refs. Must be
 * paired with exactly one _client_engine_release() call.
 */
static ccol_retval_t _client_engine_acquire(void) {
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  while (cli_engine_bundler.stopping)
    cond_var_wait(cli_engine_bundler.stopped_cv, cli_engine_bundler.mutex);
  _client_engine_join_reaper_if_needed_locked();

  if (!cli_engine_bundler.reactor) {
    size_t nthreads = cli_engine_bundler.num_reactor_threads;
    if (nthreads == 0) {
      long cpus = sysconf(_SC_NPROCESSORS_ONLN);
      nthreads = (cpus > 0) ? (size_t)cpus : 1;
    }
    cli_engine_bundler.last_resolved_num_reactor_threads = nthreads;
    char *err = NULL;
    cli_engine_bundler.reactor = event_loop_create_with_mprocs(
        256, 4, nthreads, cli_engine_bundler.mprocs, &err);
    if (!cli_engine_bundler.reactor) {
      mutex_unlock(cli_engine_bundler.mutex);
      return ccol_not_enough_memory;
    }

    char *dns_err = NULL;
    cli_engine_bundler.dns_pool = create_cthread_pool(nthreads, 0, &dns_err);
    if (!cli_engine_bundler.dns_pool) {
      event_loop_destroy(cli_engine_bundler.reactor);
      cli_engine_bundler.reactor = NULL;
      mutex_unlock(cli_engine_bundler.mutex);
      return ccol_not_enough_memory;
    }

    if (_client_deadline_sweep_start() != ccol_success) {
      ctpool_destroy(cli_engine_bundler.dns_pool);
      cli_engine_bundler.dns_pool = NULL;
      event_loop_destroy(cli_engine_bundler.reactor);
      cli_engine_bundler.reactor = NULL;
      mutex_unlock(cli_engine_bundler.mutex);
      return ccol_unexpected_failure;
    }

    if (!cli_engine_bundler.logger) {
      cli_engine_bundler.logger =
          clog_open_fd_mp(2, CLOG_FATAL, cli_engine_bundler.mprocs);
      if (!cli_engine_bundler.logger) {
        mutex_unlock(cli_engine_bundler.mutex);
        return ccol_not_enough_memory;
      }
      clog_set_field(cli_engine_bundler.logger, "component",
                     "http-client-engine");
    }

    log_info(cli_engine_bundler.logger,
             "New http client reactor engine has been created");
  }
  cli_engine_bundler.reactor_refs++;
  mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

/*
 * Releases a reference acquired via _client_engine_acquire(). Once
 * cli_engine_bundler.reactor_refs returns to zero, hands the actual teardown
 * off to a freshly spawned reaper thread rather than performing it inline;
 * this is essential, not just a style choice, since this is routinely called
 * from inside a reactor callback thread itself (_async_on_error tearing down
 * the last live ctx), which must never block joining the deadline sweep or
 * draining cli_engine_bundler.dns_pool (a real hang, caught in testing, in the
 * single-owner predecessor of this exact design).
 */
static void _client_engine_release(void) {
  bool should_reap = false;
  mutex_lock(cli_engine_bundler.mutex);
  if (cli_engine_bundler.reactor_refs > 0) cli_engine_bundler.reactor_refs--;
  if (cli_engine_bundler.reactor_refs == 0 && cli_engine_bundler.reactor) {
    cli_engine_bundler.stopping = true;
    should_reap = true;
  }
  mutex_unlock(cli_engine_bundler.mutex);
  if (should_reap) _client_engine_spawn_reaper();
}

/*
 * Blocks until any in-flight reaper thread (see _client_engine_release) has
 * fully finished tearing this module's resources down. A no-op if not
 * currently stopping (including if not running at all, or running and
 * staying up because other Tier 2/3 references remain); this is
 * deliberately NOT "block until the engine eventually stops on its own"
 * (that would hang forever against a healthy, still-referenced engine);
 * callers use this only to wait for a teardown they know they just
 * triggered (releasing their own last reference) to actually finish.
 *
 * Gated behind RUNNING_UNIT_TESTS: unlike chttpsvr_engine_wait() on the
 * server side, this module exposes no public equivalent (the engine starts
 * and stops on its own as Tier 2/3 usage comes and goes, with no atexit
 * safety net needing to wait on it either; see the "SHARED STATIC
 * REACTOR" section above), so this primitive's only caller in a production
 * build would otherwise be none at all; it exists purely for
 * _chttpclient_engine_wait_for_quiescence_for_tests below.
 */
#ifdef RUNNING_UNIT_TESTS
static void _client_engine_wait_for_quiescence(void) {
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  while (cli_engine_bundler.stopping)
    cond_var_wait(cli_engine_bundler.stopped_cv, cli_engine_bundler.mutex);
  _client_engine_join_reaper_if_needed_locked();
  mutex_unlock(cli_engine_bundler.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

ccol_retval_t chttpcli_set_engine_logger(clog cl) {
  if (!cl) return ccol_invalid_args;
  clog derived = clog_derive(cl);
  if (!derived) return ccol_not_enough_memory;
  clog_set_field(derived, "component", "http-client-engine");
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  clog old = cli_engine_bundler.logger;
  cli_engine_bundler.logger = derived;
  mutex_unlock(cli_engine_bundler.mutex);
  if (old) clog_close(old);
  return ccol_success;
}

ccol_retval_t chttpcli_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp) {
  if (mp && (!mp->malloc || !mp->free || !mp->calloc || !mp->realloc))
    return ccol_invalid_args;
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  if (cli_engine_bundler.reactor) {
    mutex_unlock(cli_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  if (mp) {
    cli_engine_bundler.mprocs_storage = *mp;
    cli_engine_bundler.mprocs = &cli_engine_bundler.mprocs_storage;
  } else {
    cli_engine_bundler.mprocs = NULL;
  }
  mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

ccol_retval_t chttpcli_set_engine_num_reactor_threads(size_t num_threads) {
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  if (cli_engine_bundler.reactor) {
    mutex_unlock(cli_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  cli_engine_bundler.num_reactor_threads = num_threads;
  mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

/* ========================================================================== */
/*                    ASYNC CONNECTION STATE MACHINE (STEP A + B)             */
/* ========================================================================== */

/*
 * Tier 2/3 engine: a non-blocking HTTP or HTTPS request/response cycle per
 * connection, with redirect-following layered on top of a single-hop
 * pipeline, driven by chttpclient's own event_loop reactor (see the "SHARED
 * STATIC REACTOR" section above).
 *
 * TLS: uses the same reactor-agnostic, client-mode ctls API Tier 1 uses
 * (ctls_conn_create_client / ctls_conn_handshake_step / ctls_conn_read /
 * ctls_conn_write), driven from on_readable/on_writable instead of a
 * blocking poll() loop.
 *
 * No tls_lock is needed here: event_loop's own per-event_entry dispatch_lock
 * (see cthreadcomm's own documentation) guarantees that a registration's
 * callback is never invoked concurrently with itself, and, more strongly,
 * that both directions of one fd (on_readable/on_writable) are never
 * dispatched concurrently with each other either. That guarantee is exactly
 * what lets this module drive OpenSSL on one shared connection from either
 * callback with no additional locking of its own.
 *
 * Write-readiness re-arming needs no special-casing: event_loop is
 * level-triggered and keeps a registration live until explicitly removed
 * (event_loop_modify is used instead, purely to flip which direction(s) are
 * of interest), so there is no "forgot to re-arm a one-shot write interest"
 * window to guard against for raw (queue-bypassing) TLS writes.
 *
 * Connection identity is its raw fd plus the event_reg* returned by
 * event_loop_add for its current registration (NULL while none is active,
 * e.g. mid-connect via the DNS/connect pool, or while idle-pooled).
 * event_loop's own generation counter (event_loop_reg_generation) is
 * available for defensive bookkeeping but is not needed for correctness
 * here: event_loop's dispatch path already re-validates a registration's
 * liveness under its own lock at dispatch time, so a stale, already-fetched
 * epoll_wait batch entry for a since-removed registration is always a safe
 * no-op regardless.
 *
 * event_loop_remove only protects event_loop's OWN internal structures via
 * deferred/epoch-based freeing; it does NOT protect this module's OWN ctx
 * payload from a dispatch that was already in flight when the removal
 * happened. Every place that ends a connection therefore goes through one
 * of two small, explicit teardown helpers (_async_ctx_finish/
 * _async_idle_ctx_finish, defined after the idle pool section below) rather
 * than relying on any implicit "on_close eventually runs" guarantee: there
 * is no unconditional terminal callback under event_loop, so this module
 * must be explicit about ending a connection instead.
 *
 * Redirect-chain lifetime: a redirect chain spans multiple connections (one
 * chttp_async_ctx_t per hop, exactly mirroring Tier 1's "a fresh
 * chttp_parse_ctx_t per hop" comment), but must still fulfil the caller's
 * future exactly once and release exactly one engine reference for the whole
 * chain. chttp_async_chain_t is the small heap-allocated struct that
 * outlives any single hop's ctx to carry that shared state: the future, the
 * fulfilled-once guard, an owned deep copy of the original request's headers
 * and body, the pinned TLS context (constant across hops, exactly like
 * Tier 1's tls_ctx local), and a refcount tracking how many hops' ctx are
 * currently live. Every ctx teardown path releases one chain reference;
 * when the count reaches zero, _async_chain_release both frees the chain
 * and, via a fulfilled-guarded backstop, fulfils the future with a generic
 * error if nothing else already did.
 */

/* The ctpool_future's result is a chttpcli_async_result_t (public, declared
 * in chttpclient.h); built by _async_fulfill_chain below. */

typedef enum {
  CHTTP_ASYNC_CONNECTING,
  CHTTP_ASYNC_TLS_HANDSHAKING,
  CHTTP_ASYNC_WRITING,
  CHTTP_ASYNC_READING,
  CHTTP_ASYNC_IDLE, /* sitting in cli->idle_pools_async, not owned by any
                     * chain (ctx->chain == NULL); see the "ASYNC IDLE
                     * POOL" section below */
} chttp_async_state_t;

/*
 * Shared, per-request (not per-hop) state for a redirect chain. See the
 * file-level comment above for the ownership/lifetime rationale.
 */
typedef struct {
  ccol_memmgmt_procs_t *mp;
  struct chttpclient *cli; /* owning client; needed by _async_submit_hop to
                            * reach the async idle pool (cli->idle_pools_async)
                            * for both taking a reusable connection and
                            * offering one back */
  ctpool_future *future;   /* the caller's own reference is separate; see
                            * _chttp_do_async_internal's return value; this
                            * module only ever calls ctpool_future_fulfill on
                            * it (the producer-side reference) */

  chttpcli_write_fn write_fn; /* NULL for a buffered (chttpclient_do_async)
                               * request; non-NULL for a streaming
                               * (chttpclient_do_async_streaming) one. Wired
                               * into every hop's ctx->pctx.requested_sink_fn
                               * in _async_submit_hop/_async_retry_hop;
                               * constant across the whole chain, exactly
                               * like Tier 1's identical streaming/write_fn
                               * locals in chttp_do_internal. Called on
                               * whichever reactor thread is driving this
                               * hop's on_data callback; see this field's
                               * public documentation on
                               * chttpclient_do_async_streaming for the
                               * must-not-block contract that implies. */
  void *write_ctx;            /* passed verbatim to write_fn */

  chmap req_headers; /* owned deep copy of the original request's headers
                      * (chmap(char* -> char*)); re-sent unchanged on every
                      * hop, exactly like Tier 1's hop_req.headers. NULL if
                      * the original request had none. */
  void *body_data;   /* owned deep copy of the original request body bytes;
                      * NULL if the original request had none. Re-sent
                      * verbatim on 307/308 hops; a non-preserving redirect
                      * (301/302/303 with a non-HEAD method) drops it. */
  size_t body_len;
  char *body_content_type; /* owned copy; NULL if none */

  char *carried_auth;        /* auto-injected-from-userinfo Authorization
                              * value carried forward across hops, mirroring
                              * Tier 1's identical carried_auth local in
                              * chttp_do_internal; see that function's own
                              * comment for the full same-origin-carry /
                              * cross-origin-drop-permanently contract. NULL
                              * if no userinfo has been seen on this chain
                              * (yet, or ever). Mutated only from
                              * _async_submit_hop, at the same point the
                              * method/body downgrade decision already
                              * mutates this chain unguarded; see that
                              * function's own comment on why no additional
                              * locking is needed. */
  char *carried_auth_origin; /* origin_key the above was derived for */

  ctls_ctx_t *tls_ctx; /* pinned once (ctls_ctx_retain'd from cli->tls_ctx)
                        * for the whole chain, exactly like Tier 1's tls_ctx
                        * local; a redirect can hop between http and
                        * https, so this must survive every hop regardless
                        * of which scheme the chain started with. Released
                        * once via ctls_ctx_release when the chain is freed. */
  bool tls_ctx_usable;
  bool verify_host;

  long connect_timeout_ms; /* re-read fresh into ctx->connect_deadline at the
                            * start of every hop that actually connects (the
                            * reused-connection path never consults it, since
                            * it skips CONNECTING/TLS_HANDSHAKING entirely);
                            * see the "ASYNC DEADLINE SWEEP" section below. */
  chttp_deadline_t overall_deadline; /* computed once, here, for the whole
                                      * chain's lifetime; mirrors Tier 1's
                                      * overall_dl, computed once before its
                                      * hop loop rather than reset per hop. */

  mutex_t lock; /* guards fulfilled and refcount, both mutated from
                 * potentially concurrent hops' reactor callbacks (see the
                 * file-level comment: an old hop's on_close can fire
                 * concurrently with a new hop's on_data/on_ready once the
                 * redirect handoff has queued the next connection) */
  bool fulfilled;
  int refcount; /* number of currently-live per-hop ctx referencing this
                 * chain; reaches zero exactly once, when the last hop's
                 * teardown runs with no further hop having been queued to
                 * take over */
} chttp_async_chain_t;

/* Tagged (not anonymous) specifically so deadline_prev/deadline_next below
 * can self-reference the struct; an anonymous struct has no name to spell
 * a pointer to itself with. */
typedef struct chttp_async_ctx_s {
  ccol_memmgmt_procs_t *mp;
  struct chttpclient *cli; /* owning client; needed while idle
                            * (chain == NULL then) to reach
                            * cli->idle_pools_async, and while active to
                            * offer this connection back to the pool on a
                            * reusable completion */
  chttp_async_chain_t
      *_Atomic chain;        /* shared, whole-redirect-chain state;
                              * not owned by ctx; see _async_ctx_teardown.
                              * NULL while ctx is idle-pooled. _Atomic (like
                              * state/fd below) so the deadline sweep can
                              * read it without idle_lock; see that lock's
                              * own comment and the "ASYNC DEADLINE SWEEP"
                              * section for why individual atomic loads are
                              * sufficient there even though idle_lock's
                              * stronger *compound* (state+chain together)
                              * consistency guarantee remains necessary, and
                              * is left completely undisturbed, for every
                              * pre-existing idle_lock call site. */
  int hop;                   /* 0-based hop index of THIS connection */
  chttp_method_t cur_method; /* method used to build THIS hop's wire; the
                              * basis for deciding the next hop's method on
                              * redirect, exactly like Tier 1's cur_method
                              * loop variable */
  _Atomic chttp_async_state_t state;
  _Atomic int fd; /* -1 until the connect task obtains a real fd */
  _Atomic(event_reg *)
      reg; /* current event_loop registration, or NULL
            * while none is active (mid-connect via the DNS/connect
            * pool, or while idle-pooled with no direction of
            * interest yet needed). Set exactly once, by
            * _async_connect_task, right after event_loop_add
            * returns; and _Atomic specifically because that
            * assignment is NOT safe to treat as "invisible until a
            * dispatch could care": event_loop_add's own internal
            * registration goes live (dispatchable by another
            * reactor thread) as part of the call itself, which can
            * complete and hand a callback to a DIFFERENT thread
            * before this thread's own `ctx->reg = event_loop_add(
            * ...)` assignment has finished executing; especially
            * likely for a loopback connect, which is often already
            * writable the instant it's registered. A plain
            * (non-atomic) pointer here was a real, TSan-caught data
            * race (an earlier version of this comment's own claim
            * that this field is "never read lock-free" was simply
            * wrong, found only by running ThreadSanitizer against a
            * suite that kept intermittently failing
            * async_idle_pool.dead_connection_detected_and_retried,
            * not by re-reading this code and noticing it). Every
            * dispatch callback (_async_on_writable/_on_readable/
            * _on_error) treats a NULL read here as "registration
            * not yet published" and returns immediately without
            * touching anything else; this is self-healing rather
            * than a bug, since the underlying condition (a
            * writable/readable/errored fd) is level-triggered and
            * gets re-reported on the very next epoll_wait, by which
            * point the assignment has long since completed. */

  bool is_unix;
  char *unix_socket_path; /* owned copy; NULL unless is_unix */
  char *host;             /* owned copy; NULL if is_unix. port is passed
                           * separately */
  uint16_t port;
  char *origin_key;    /* owned copy, "scheme://host:port" or "unix://<path>";
                        * matches Tier 1's chttp_conn_t.origin_key; used to
                        * place/remove this ctx in cli->idle_pools_async */
  bool reused;         /* true if this hop's connection came from the idle
                        * pool rather than a fresh connect, for THIS
                        * attempt (a retry always resets this to false;
                        * see _async_retry_hop) */
  bool any_bytes_read; /* true once >=1 response byte has been read for the
                        * CURRENT attempt; gates the reused-connection
                        * dead-connection retry, exactly like Tier 1's
                        * identically-named any_bytes_read output param */
  struct timespec last_used; /* set when offered to the idle pool; used by
                              * _async_idle_pool_take's staleness check */
  char *wire; /* owned serialized request bytes; this module always retains
               * ownership and frees it once fully written (tracked via
               * wire_sent below), for both the plain and TLS path alike
               * (there is no queue to hand ownership off to instead).
               * Freeing it in _async_ctx_free is always safe (a no-op once
               * NULL). */
  size_t wire_len;
  size_t wire_sent; /* bytes of wire already written; ctls_conn_write/raw
                     * write(2) both have ordinary short-write semantics. */

  bool is_https;
  bool verify_host;
  _Atomic bool
      hop_completed; /* set once this hop's response has been
                      * fully parsed and either fulfilled or handed off to
                      * a redirect; guards the dispatch callbacks against
                      * re-running that (for _async_handle_redirect,
                      * non-idempotent) logic on a spurious extra
                      * readable/writable dispatch; see that check's own
                      * comment for why this can happen. Also used,
                      * together with reused/any_bytes_read/timed_out, by
                      * _async_ctx_finish (see the "ASYNC CONNECTION STATE
                      * MACHINE" section's own file-level comment) to
                      * decide, at every single connection-ending point,
                      * whether that ending must still fulfil the chain's
                      * future, retry, or neither (already handled by
                      * whichever code path called _async_ctx_finish in
                      * the first place). _Atomic (unlike a first version
                      * of this field, which assumed event_loop's own
                      * dispatch_lock was sufficient protection) because
                      * it is also written directly by _async_submit_hop's
                      * reused-connection path (ordinary application
                      * code running on whatever thread called
                      * chttpclient_do_async, not a dispatch callback, and
                      * therefore NOT covered by event_loop's dispatch_lock
                      * at all) while a concurrent dispatch for this
                      * same, already-registered ctx can legitimately be
                      * in flight at the same time. A real, TSan-caught
                      * data race, found chasing down an intermittent
                      * async_idle_pool.dead_connection_detected_and_
                      * retried failure. */
  ctls_conn_t *tls;  /* NULL until the connect succeeds and the handshake
                      * begins; NULL for plain HTTP. No separate lock guards
                      * this: see the file-level comment on why event_loop's
                      * own per-registration dispatch_lock already makes one
                      * unnecessary. */

  mutex_t idle_lock; /* Guards the state/chain pair specifically across the
                      * idle<->active transition. A connection popped out of
                      * cli->idle_pools_async by _async_idle_pool_take stays
                      * fully attached to the reactor the whole time (there
                      * is no way to "pause" polling for a single fd);
                      * removing it from the POOL's bookkeeping does nothing
                      * to stop the reactor from independently dispatching
                      * on_readable/on_writable/on_error for it on another
                      * thread the moment the peer sends something (or the
                      * connection dies) while _async_submit_hop is still in
                      * the middle of reconfiguring ctx->state away from
                      * CHTTP_ASYNC_IDLE and ctx->chain away from NULL for
                      * its new owner. Without this lock, a dispatch's
                      * IDLE-state guard could read a stale (already-popped
                      * but not-yet-reconfigured) ctx->state, fall through
                      * past the guard, and dereference ctx->chain while it
                      * is still NULL. Only ever held very briefly, around
                      * the handful of statements that flip state+chain in
                      * either direction, never across I/O. Deliberately NOT
                      * used by the deadline sweep (see the "ASYNC DEADLINE
                      * SWEEP" section for the lock-ordering hazard this
                      * would otherwise create, and the shutdown(fd)-based
                      * design that avoids it entirely): state/chain/fd/
                      * timed_out are instead _Atomic specifically so the
                      * sweep can read them lock-free; see those fields' own
                      * comments for why individual atomic reads (rather
                      * than this lock's stronger compound guarantee) are
                      * sufficient for the sweep's specific use, even though
                      * every OTHER consumer of state/chain (the dispatch
                      * callbacks, via _async_ctx_is_idle) still needs (and
                      * keeps getting) the full compound protection this
                      * lock alone provides. */

  chttp_deadline_t connect_deadline; /* Only meaningful while state is
                                      * CHTTP_ASYNC_CONNECTING or
                                      * CHTTP_ASYNC_TLS_HANDSHAKING; set
                                      * fresh (by the thread about to submit
                                      * this hop attempt) at the start of
                                      * every hop that actually connects
                                      * (never for a reused connection, which
                                      * skips both those states entirely).
                                      * Deliberately NOT _Atomic: it is
                                      * written exactly once, strictly
                                      * before _client_deadline_register is
                                      * called for this attempt; that
                                      * call's own mutex lock/unlock is
                                      * itself a release/acquire pair, so it
                                      * already guarantees the sweep (which
                                      * only ever observes a ctx AFTER
                                      * finding it via the registry) sees a
                                      * fully-initialised value with no
                                      * separate synchronisation needed. See
                                      * the "ASYNC DEADLINE SWEEP" section. */
  mutex_t deadline_lock; /* Guards overall_deadline below ONLY; a small,
                          * dedicated leaf lock, never held while trying to
                          * acquire idle_lock or client_deadline_bundle.mutex
                          * (so it introduces no new lock-ordering cycle with
                          * either), taken briefly by both the writer
                          * (_async_idle_pool_take/_async_submit_hop/
                          * _async_retry_hop, all of which may already be
                          * holding idle_lock at the point they need to
                          * write overall_deadline) and the reader (the
                          * deadline sweep, which already holds
                          * client_deadline_bundle.mutex for its whole registry
                          * walk). A first version of overall_deadline
                          * relied on ctx->chain's own _Atomic-ness as a
                          * publication barrier (matching connect_deadline's
                          * established pattern below) instead of a real
                          * lock; that reasoning turned out to be
                          * insufficient in practice for THIS field
                          * specifically (unlike connect_deadline, which is
                          * published exactly once via _client_deadline_
                          * register's own mutex, overall_deadline can be
                          * REWRITTEN on every idle-pool reuse cycle without
                          * a fresh _client_deadline_register call at all),
                          * caught as a real, reproducible TSan data race
                          * even after the ordering fix. */
  chttp_deadline_t overall_deadline; /* A ctx-local COPY of chain->
                                      * overall_deadline (immutable for the
                                      * chain's whole lifetime, computed once
                                      * in _async_chain_create), updated
                                      * (under deadline_lock) everywhere
                                      * ctx->chain is assigned to a live
                                      * chain (_async_submit_hop's fresh and
                                      * reused paths, _async_idle_pool_take,
                                      * _async_retry_hop). This copy exists
                                      * so the deadline sweep never needs to
                                      * dereference ctx->chain at all: an
                                      * earlier version read chain->
                                      * overall_deadline directly through
                                      * node->chain, a genuine TSan-caught
                                      * use-after-free, since nothing
                                      * prevented the chain's last reference
                                      * from being released (and the chain
                                      * freed) between the sweep's atomic
                                      * read of the pointer and its
                                      * subsequent dereference of it. The
                                      * sweep now only ever compares
                                      * node->chain against NULL directly
                                      * (a safe, non-dereferencing pointer
                                      * read, needing no lock) to decide
                                      * whether this field is meaningful
                                      * right now, and reads this field
                                      * itself under deadline_lock. */
  _Atomic bool timed_out;   /* Set by the deadline sweep, lock-free, right
                             * before it shuts this connection's fd down
                             * (see the "ASYNC DEADLINE SWEEP" section); lets
                             * the normal dispatch-driven teardown
                             * (_async_ctx_finish) report ccol_timed_out and
                             * skip the ordinary dead-connection retry
                             * (retrying past an already-blown deadline
                             * would only extend the overrun for no
                             * benefit). _Atomic for the same reason
                             * state/chain/fd are, above. */
  bool deadline_registered; /* True once this ctx has been linked into
                             * client_deadline_bundle.head at least once.
                             * Registration is idempotent and, once made,
                             * persists for ctx's whole lifetime (including
                             * every idle-pool cycle); see
                             * _client_deadline_register's own comment. */
  struct chttp_async_ctx_s *deadline_prev,
      *deadline_next; /* Intrusive
                       * doubly-linked membership in the process-wide deadline
                       * registry, guarded by client_deadline_bundle.mutex (a
                       * different lock than idle_lock above; see that
                       * global's own comment for the lock-ordering contract
                       * between the two). */

  chttp1_parser_t parser;
  chttp_parse_ctx_t pctx;
  chttp_bodybuf_t bb;
} chttp_async_ctx_t;

/* ========================================================================== */
/*                    ASYNC DEADLINE SWEEP (TIER 2)                           */
/* ========================================================================== */

/*
 * Enforces connect_timeout_ms/request_timeout_ms for Tier 2/3 as true
 * absolute wall-clock deadlines, mirroring Tier 1's _deadline_make/
 * _deadline_remaining_ms semantics exactly; including catching a
 * connection that never goes fully idle (e.g. a slow trickle of bytes just
 * before an activity-reset timeout would fire) but has still blown its
 * deadline. event_loop has no built-in timer/deadline mechanism at all, so
 * this is a small periodic-sweep thread, entirely separate from the
 * reactor's own threads and the DNS/connect ctpool; started/stopped
 * alongside them (see _client_deadline_sweep_start/_stop_and_join, called
 * from _client_engine_acquire/_client_engine_reaper_fn above).
 *
 * client_deadline_bundle.mutex serialises three things: (1) the intrusive
 * doubly-linked registry list itself (ctx->deadline_prev/deadline_next),
 * (2) the sweep thread's own sleep/wake condvar, and (3); the reason a
 * ctx is never explicitly unregistered except from within _async_ctx_free,
 * the single reliable point ctx memory is actually released; mutual
 * exclusion between the sweep reading a registered ctx's fields and any
 * other thread freeing that same ctx concurrently: _client_deadline_
 * unregister cannot complete (and therefore _async_ctx_free cannot proceed
 * to actually free ctx) while the sweep is still walking the registry with
 * this lock held.
 *
 * Registration (_client_deadline_register) is idempotent and, once made,
 * persists for a ctx's entire lifetime, including every idle-pool cycle it
 * goes through; a pooled/idle ctx just sits in the registry inertly (the
 * sweep's own state==CONNECTING/TLS_HANDSHAKING and chain!=NULL checks
 * naturally skip it while idle), which is simpler and just as correct as
 * explicitly unregistering on every idle-pool-offer and re-registering on
 * every reuse.
 *
 * Lock ordering: client_deadline_bundle.mutex is only ever taken OUTSIDE of any
 * ctx->idle_lock (never the reverse); _client_deadline_register/
 * _unregister are never called while idle_lock is held (see their call
 * sites), and the sweep itself never touches idle_lock at all (see
 * _client_deadline_sweep_once's own comment for how it avoids needing to).
 *
 * Neutering an expired connection: a raw fd carries no protection of its own
 * against being closed and reused by an unrelated connection between the
 * moment this sweep captures it and the moment it acts on it. Calling any
 * fd-based operation on a captured int after releasing a lock therefore
 * risks acting on a completely unrelated connection if that fd number was
 * closed and reused by a fresh connect() in the meantime. This sweep avoids
 * that hazard by construction rather than needing a generation check of its
 * own: every teardown path (_async_ctx_finish/_async_idle_ctx_finish,
 * defined after the idle pool section below) calls
 * _client_deadline_unregister (which requires this same lock) strictly
 * BEFORE closing ctx->fd, so any ctx still linked into this registry is
 * guaranteed to not have had its fd closed yet. This sweep therefore calls
 * shutdown(fd, SHUT_RDWR) directly, still holding client_deadline_bundle.mutex,
 * with no separate collect-then-act-outside-the-lock phase needed at all:
 * shutdown() is a plain kernel-level operation with no synchronous
 * application callback, so there is no reentrancy risk in calling it here.
 * shutdown() neuters both directions
 * of the connection (readable-with-error and writable-with-error on the
 * next epoll_wait) without this sweep ever touching ctx's own memory,
 * idle_lock, or its event_loop registration; the normal dispatch path
 * (_async_on_readable/_on_writable/_on_error) takes it from there once the
 * reactor observes it, exactly like any other organic connection failure.
 */
struct {
  mutex_t mutex;
  cond_var_t cond_var;
  /* client_deadline_bundle.mutex/_cv used to carry static
   * PTHREAD_MUTEX_INITIALIZER/PTHREAD_COND_INITIALIZER initializers; converted
   * to lazy, call_once-guarded runtime init (see
   * _client_deadline_init_globals), independent of the async engine pair's own
   * once-guard above, for the same reason: a non-pthread backend's equivalent
   * primitive may need real setup work a compile-time constant can't provide.
   * Every function below that touches either global calls
   * call_once(client_deadline_bundle.once, ...) as its first statement. */
  once_flag_t once;
  chttp_async_ctx_t *head;
  thread_id_t thread;
  bool stop;
} client_deadline_bundle = {0};

static void _client_deadline_init_globals(void) {
  mutex_init(client_deadline_bundle.mutex);
  cond_var_init(client_deadline_bundle.cond_var);
}

#define CHTTP_DEADLINE_SWEEP_INTERVAL_MS 100
/* Soft cap on how many expired connections one sweep tick shuts down.
 * Self-healing, not a hard limit: anything beyond this on one tick simply
 * stays registered (not yet marked timed_out, so it is picked up again) and
 * gets its shutdown() call from a later tick instead, at worst another
 * CHTTP_DEADLINE_SWEEP_INTERVAL_MS later per extra batch; a practically
 * irrelevant delay for the number of simultaneously-expiring connections it
 * would take to ever exceed this. */
#define CHTTP_DEADLINE_SWEEP_BATCH 256

/*
 * Adds ctx to the deadline registry if not already present. Must be called
 * with ctx->idle_lock NOT held (see the lock-ordering note above).
 */
static void _client_deadline_register(chttp_async_ctx_t *ctx) {
  call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  mutex_lock(client_deadline_bundle.mutex);
  if (!ctx->deadline_registered) {
    ctx->deadline_prev = NULL;
    ctx->deadline_next = client_deadline_bundle.head;
    if (client_deadline_bundle.head)
      client_deadline_bundle.head->deadline_prev = ctx;
    client_deadline_bundle.head = ctx;
    ctx->deadline_registered = true;
  }
  mutex_unlock(client_deadline_bundle.mutex);
}

/* Removes ctx from the deadline registry if present. Called exactly once,
 * as the first thing _async_ctx_free does; see that function's own
 * comment for why that is the single correct place for this. */
static void _client_deadline_unregister(chttp_async_ctx_t *ctx) {
  call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  mutex_lock(client_deadline_bundle.mutex);
  if (ctx->deadline_registered) {
    if (ctx->deadline_prev) {
      ctx->deadline_prev->deadline_next = ctx->deadline_next;
    } else {
      client_deadline_bundle.head = ctx->deadline_next;
    }
    if (ctx->deadline_next)
      ctx->deadline_next->deadline_prev = ctx->deadline_prev;
    ctx->deadline_prev = ctx->deadline_next = NULL;
    ctx->deadline_registered = false;
  }
  mutex_unlock(client_deadline_bundle.mutex);
}

/*
 * One sweep pass: walks the whole registry under client_deadline_bundle.mutex,
 * checking each ctx's connect_deadline (only while it is actually in a
 * connecting phase) and its chain's overall_deadline (whenever it has a
 * live chain at all), and shuts down the fd of any newly-expired connection
 * directly, inline, still holding the lock (see the section's own file-level
 * comment for why this is safe: no reentrancy risk, and no fd-reuse race).
 *
 * state/chain/fd/timed_out are read as plain (but _Atomic-qualified, so
 * individually race-free) loads here; deliberately NOT under the ctx's own
 * idle_lock (see the lock-ordering note above for why: some call paths
 * legitimately hold a ctx's idle_lock while also needing
 * client_deadline_bundle.mutex, e.g. _async_submit_hop's
 * reused-connection-write- failure branch calling _async_retry_hop ->
 * _client_deadline_register; this sweep must never acquire idle_lock itself
 * while holding client_deadline_bundle.mutex, or that becomes a classic AB-BA
 * lock-order inversion).
 *
 * This is safe without idle_lock's stronger *compound* (state-and-chain-
 * together) consistency guarantee specifically because of what this
 * function does with a possibly-torn snapshot: at both points idle_lock
 * actually protects (_async_idle_pool_take's idle->active transition and
 * _async_idle_pool_offer's active->idle transition), every reachable torn
 * combination of (state, chain) either skips both deadline checks (chain
 * NULL, or state not a connecting state) or evaluates a chain that is,
 * worst case, a moment away from being released but still guaranteed alive
 * (the chain reference is only actually dropped strictly after the
 * state/chain pair is updated); so the worst possible outcome of a torn
 * read here is shutting down a connection a few instructions before or
 * after it would otherwise have been cleanly pooled or hopped, i.e. a lost
 * optimisation opportunity, never an incorrect abort of a still-in-flight,
 * unrelated request. Every OTHER consumer of state/chain (the dispatch
 * callbacks, via _async_ctx_is_idle) has a stronger need for idle_lock's
 * full guarantee and keeps using it entirely unchanged.
 *
 * A ctx is marked ctx->timed_out before its fd is shut down, both to tell
 * the normal teardown path "this is a timeout, not an ordinary connection
 * failure" (report ccol_timed_out and skip the usual dead-connection retry;
 * retrying past an already-blown deadline would just extend the overrun)
 * and to make repeated sweep ticks idempotent against a ctx still mid-
 * teardown from an earlier tick's shutdown() call.
 */
static void _client_deadline_sweep_once(void) {
  call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  mutex_lock(client_deadline_bundle.mutex);
  chttp_async_ctx_t *node = client_deadline_bundle.head;
  size_t n_shutdown = 0;
  while (node && n_shutdown < CHTTP_DEADLINE_SWEEP_BATCH) {
    chttp_async_ctx_t *next = node->deadline_next;

    chttp_async_state_t state = node->state;
    /* Only ever compared against NULL (never dereferenced): see
     * node->overall_deadline's own field comment for why dereferencing a
     * bare chain pointer read here was a real, TSan-caught
     * use-after-free. */
    bool has_chain = (node->chain != NULL);
    int fd = node->fd;
    bool expired = false;
    if (!node->timed_out) {
      int ms;
      if ((state == CHTTP_ASYNC_CONNECTING ||
           state == CHTTP_ASYNC_TLS_HANDSHAKING) &&
          !_deadline_remaining_ms(&node->connect_deadline, &ms)) {
        expired = true;
      }
      if (!expired && has_chain) {
        /* See node->deadline_lock's own field comment: this dedicated leaf
         * lock is what makes reading overall_deadline here safe, since it
         * can be rewritten (not just published once) on every idle-pool
         * reuse cycle, unlike connect_deadline. */
        mutex_lock(node->deadline_lock);
        bool still_ok = _deadline_remaining_ms(&node->overall_deadline, &ms);
        mutex_unlock(node->deadline_lock);
        if (!still_ok) expired = true;
      }
      if (expired) node->timed_out = true;
    }

    if (expired) {
      if (fd >= 0) shutdown(fd, SHUT_RDWR);
      n_shutdown++;
    }
    node = next;
  }
  mutex_unlock(client_deadline_bundle.mutex);

  if (n_shutdown > 0) {
    clog el = _client_engine_logger_get();
    if (el)
      log_info(el, "deadline sweep shut down %zu connection(s)", n_shutdown);
  }
}

static void *_client_deadline_sweep_fn(void *arg) {
  (void)arg;
  call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  mutex_lock(client_deadline_bundle.mutex);
  while (!client_deadline_bundle.stop) {
    struct timespec wake;
    clock_gettime(CLOCK_MONOTONIC, &wake);
    wake.tv_nsec += CHTTP_DEADLINE_SWEEP_INTERVAL_MS * 1000000L;
    if (wake.tv_nsec >= 1000000000L) {
      wake.tv_nsec -= 1000000000L;
      wake.tv_sec += 1;
    }
    cond_var_timedwait(client_deadline_bundle.cond_var,
                       client_deadline_bundle.mutex, wake);
    if (client_deadline_bundle.stop) break;
    mutex_unlock(client_deadline_bundle.mutex);
    _client_deadline_sweep_once();
    mutex_lock(client_deadline_bundle.mutex);
  }
  mutex_unlock(client_deadline_bundle.mutex);
  return NULL;
}

/* Starts the sweep thread. Called from _client_engine_acquire, alongside
 * spawning the engine's own reactor thread; see that function for
 * rollback-on-failure handling. */
static ccol_retval_t _client_deadline_sweep_start(void) {
  client_deadline_bundle.stop = false;
  int rc = thread_create(client_deadline_bundle.thread,
                         _client_deadline_sweep_fn, NULL);
  return (rc == 0) ? ccol_success : ccol_unexpected_failure;
}

/* Signals and joins the sweep thread. Called from _client_engine_reaper_fn,
 * alongside joining the engine's own reactor thread; see that function's
 * own comment for why teardown always happens on a dedicated reaper thread,
 * never inline from a reactor callback. */
static void _client_deadline_sweep_stop_and_join(void) {
  call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  mutex_lock(client_deadline_bundle.mutex);
  client_deadline_bundle.stop = true;
  cond_var_broadcast(client_deadline_bundle.cond_var);
  mutex_unlock(client_deadline_bundle.mutex);
  thread_join(client_deadline_bundle.thread);
}

/* Deep-copies a chmap(char* -> char*) header map; used to give a redirect
 * chain its own copy of the original request's headers, since the caller's
 * chttp_request_t may be freed the moment chttpclient_do_async returns, long
 * before a later hop needs to re-serialise them. Returns NULL on OOM (input
 * NULL is not an error; it just means "no headers", and returns NULL too,
 * which is indistinguishable from an OOM failure taken alone; callers that
 * need to tell the two apart check the source map first, exactly like every
 * other call site in this file that treats "no headers" as normal). */
static chmap _clone_headers_map(ccol_memmgmt_procs_t *mp, chmap src) {
  char *err = NULL;
  chmap dst = chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                                ccol_string, mp, NULL, &err);
  if (!dst) return NULL;
  cmap_iterator *it = chashmap_begin_iter(src, NULL);
  for (; it; it = it->_next_fn(it)) {
    if (chmap_insert_elem(dst, it->key_pair, it->val_pair) != ccol_success) {
      ccol_iter_destroy(it);
      __chmap_destroy(dst);
      return NULL;
    }
  }
  return dst;
}

/*
 * Allocates the shared, whole-redirect-chain state (see the file-level
 * comment above chttp_async_chain_t). Takes ownership of tls_ctx on success
 * (released once via ctls_ctx_release when the chain is freed); on failure,
 * the caller still owns tls_ctx and must release it itself. req_headers/
 * body_data/body_content_type are NOT taken by reference: this function
 * deep-copies whatever it needs from them and never retains the originals.
 */
static chttp_async_chain_t *_async_chain_create(
    ccol_memmgmt_procs_t *mp, struct chttpclient *cli, ctpool_future *future,
    chmap req_headers, const void *body_data, size_t body_len,
    const char *body_content_type, ctls_ctx_t *tls_ctx, bool tls_ctx_usable,
    bool verify_host, long connect_timeout_ms, long request_timeout_ms,
    chttpcli_write_fn write_fn, void *write_ctx) {
  chttp_async_chain_t *chain =
      (chttp_async_chain_t *)_mem_calloc(mp, 1, sizeof(*chain));
  if (!chain) return NULL;
  if (mutex_init(chain->lock) != 0) {
    _mem_free(mp, chain);
    return NULL;
  }
  chain->mp = mp;
  chain->cli = cli;
  chain->future = future;
  chain->tls_ctx = tls_ctx;
  chain->tls_ctx_usable = tls_ctx_usable;
  chain->verify_host = verify_host;
  chain->connect_timeout_ms = connect_timeout_ms;
  chain->overall_deadline = _deadline_make(request_timeout_ms);
  chain->write_fn = write_fn;
  chain->write_ctx = write_ctx;

  if (req_headers) {
    chain->req_headers = _clone_headers_map(mp, req_headers);
    if (!chain->req_headers) goto fail;
  }
  if (body_data && body_len > 0) {
    chain->body_data = _mem_alloc(mp, body_len);
    if (!chain->body_data) goto fail;
    memcpy(chain->body_data, body_data, body_len);
    chain->body_len = body_len;
  }
  if (body_content_type) {
    chain->body_content_type = ccol_strdup(mp, body_content_type);
    if (!chain->body_content_type) goto fail;
  }

  /* This chain is now live and about to be handed to _async_submit_hop;
   * count it against cli's own async in-flight total (see that field's own
   * comment on struct chttpclient) so __chttpclient_destroy can wait for it.
   * Matched by exactly one decrement in _async_chain_release, once this
   * chain's refcount reaches zero. */
  mutex_lock(cli->async_count_lock);
  cli->async_in_flight_count++;
  mutex_unlock(cli->async_count_lock);

  return chain;

fail:
  if (chain->req_headers) __chmap_destroy(chain->req_headers);
  _mem_free(mp, chain->body_data);
  _mem_free(mp, chain->body_content_type);
  mutex_destroy(chain->lock);
  _mem_free(mp, chain);
  return NULL;
}

/* Forward declaration: needed by _async_chain_release's backstop, defined
 * further below once chttp_async_result_t's construction logic is in scope. */
static void _async_fulfill_chain(chttp_async_chain_t *chain, ccol_retval_t rv,
                                 chttpcli_response *resp);

/* Adds one live-hop reference to chain. Must be paired with exactly one
 * _async_chain_release call. Called with the chain already known to be
 * live (either freshly created with refcount 0 -> 1, or retained again by a
 * redirect handoff while the old hop's ctx is still alive). */
static void _async_chain_retain(chttp_async_chain_t *chain) {
  mutex_lock(chain->lock);
  chain->refcount++;
  mutex_unlock(chain->lock);
}

/*
 * Releases one live-hop reference. When the count reaches zero; meaning no
 * further hop was ever queued to take over from the last one torn down;
 * this is the single reliable point to both fulfil the future as a backstop
 * (a no-op if some hop already fulfilled it, via the same fulfilled guard
 * _async_fulfill_chain checks) and free the chain itself, including
 * releasing the one engine reference held for the whole chain's lifetime.
 */
static void _async_chain_release(chttp_async_chain_t *chain) {
  if (!chain) return;
  mutex_lock(chain->lock);
  int remaining = --chain->refcount;
  mutex_unlock(chain->lock);
  if (remaining > 0) return;

  struct chttpclient *cli = chain->cli;

  _async_fulfill_chain(chain, ccol_http_transfer_aborted, NULL);
  if (chain->tls_ctx) ctls_ctx_release(chain->tls_ctx);
  if (chain->req_headers) __chmap_destroy(chain->req_headers);
  _mem_free(chain->mp, chain->body_data);
  _mem_free(chain->mp, chain->body_content_type);
  _mem_free(chain->mp, chain->carried_auth);
  _mem_free(chain->mp, chain->carried_auth_origin);
  mutex_destroy(chain->lock);
  _mem_free(chain->mp, chain);
  _client_engine_release();

  /* Matches the increment in _async_chain_create; see cli's own
   * async_in_flight_count field comment for why this uses a dedicated leaf
   * lock rather than cli->lock (this function runs from inside event_loop
   * dispatch callbacks as often as from a safe synchronous context). */
  mutex_lock(cli->async_count_lock);
  if (cli->async_in_flight_count > 0) cli->async_in_flight_count--;
  if (cli->async_in_flight_count == 0)
    cond_var_broadcast(cli->async_count_drained);
  mutex_unlock(cli->async_count_lock);
}

static chttp_async_ctx_t *_async_ctx_create(ccol_memmgmt_procs_t *mp) {
  chttp_async_ctx_t *ctx =
      (chttp_async_ctx_t *)_mem_calloc(mp, 1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->mp = mp;
  ctx->fd = -1;
  if (mutex_init(ctx->idle_lock) != 0) {
    _mem_free(mp, ctx);
    return NULL;
  }
  if (mutex_init(ctx->deadline_lock) != 0) {
    mutex_destroy(ctx->idle_lock);
    _mem_free(mp, ctx);
    return NULL;
  }
  return ctx;
}

/*
 * The single reliable place ctx (a single hop's connection state) is freed;
 * called from _async_ctx_teardown (via _async_ctx_finish/
 * _async_idle_ctx_finish, the two explicit terminal-teardown helpers below
 * that give this module its own guaranteed-exactly-once teardown point; see
 * the "ASYNC CONNECTION STATE MACHINE" section's file-level comment for why
 * event_loop needs this to be explicit rather than implicit) or, for
 * failures that occur before a connection is ever registered with the
 * reactor, directly by the failing code path. Every field is safe to
 * free/destroy unconditionally: fields whose ownership was transferred
 * elsewhere (headers/bb.buf -> a successfully built response) are set
 * NULL/cleared at the transfer site, and _mem_free/_parse_ctx_free_fields/a
 * NULL tls/reg/fd are all no-ops. Does NOT touch ctx->chain; that is
 * shared, whole-chain state; see _async_ctx_teardown, which pairs this with
 * the matching chain release.
 *
 * event_loop_remove and close() are both called here, unconditionally and
 * idempotently safe, rather than requiring every caller to have already
 * done so: this mirrors ctx->tls/ctx->wire's own "always safe to free here
 * regardless of what the caller already did" treatment. _client_deadline_
 * unregister is called FIRST, strictly before close(ctx->fd): this ordering
 * is load-bearing, not incidental; see the "ASYNC DEADLINE SWEEP"
 * section's own comment for why the deadline sweep's fd-based shutdown()
 * call is only safe from an fd-reuse race because every teardown path
 * unregisters from that registry before its fd can be closed and
 * potentially reused by an unrelated connection.
 */
static void _async_ctx_free(chttp_async_ctx_t *ctx) {
  if (!ctx) return;
  /* Brief acquire/release, not held across anything below: guarantees
   * _async_connect_task's own post-event_loop_add window (see its own
   * comment, right after this same idle_lock is taken there) has fully
   * completed before this function proceeds to free ctx out from under it.
   * Every other _async_ctx_free/_async_ctx_teardown call site already
   * releases idle_lock (if it was even held) before calling in here (see
   * the call-site audit in this function's own doc comment above), so this
   * cannot self-deadlock against any existing caller. */
  mutex_lock(ctx->idle_lock);
  mutex_unlock(ctx->idle_lock);
  _client_deadline_unregister(ctx); /* no-op if never registered; MUST run
                                     * before the close() below */
  if (ctx->reg) event_loop_remove(cli_engine_bundler.reactor, ctx->reg);
  if (ctx->tls) ctls_conn_destroy(ctx->tls);
  if (ctx->fd >= 0) close(ctx->fd);
  mutex_destroy(ctx->idle_lock);
  mutex_destroy(ctx->deadline_lock);
  _mem_free(ctx->mp, ctx->wire);
  _mem_free(ctx->mp, ctx->unix_socket_path);
  _mem_free(ctx->mp, ctx->host);
  _mem_free(ctx->mp, ctx->origin_key);
  _parse_ctx_free_fields(&ctx->pctx);
  _mem_free(ctx->mp, ctx->bb.buf);
  _mem_free(ctx->mp, ctx);
}

/* Frees a single hop's per-connection state and releases its chain
 * reference; the standard way every terminal code path for a ctx (on_close,
 * or an early failure before fio_attach ever ran) ends. Captures chain into
 * a local first since _async_ctx_free frees ctx itself. */
static void _async_ctx_teardown(chttp_async_ctx_t *ctx) {
  chttp_async_chain_t *chain = ctx->chain;
  _async_ctx_free(ctx);
  _async_chain_release(chain);
}

/*
 * Delivers a terminal result to the caller's future exactly once (guarded by
 * chain->fulfilled, under chain->lock; see that field's comment for why
 * this must be lock-protected rather than a plain bool now that a redirect
 * chain can have two hops' callbacks running concurrently); safe to call
 * from multiple exit paths (e.g. an error path followed by
 * _async_chain_release's backstop call) since only the first call has any
 * effect. On the (rare) allocation failure building the result struct
 * itself, still fulfills with NULL rather than leaving the caller's
 * ctpool_future_get blocked forever.
 */
static void _async_fulfill_chain(chttp_async_chain_t *chain, ccol_retval_t rv,
                                 chttpcli_response *resp) {
  mutex_lock(chain->lock);
  bool already = chain->fulfilled;
  chain->fulfilled = true;
  mutex_unlock(chain->lock);
  if (already) {
    if (resp) {
      if (resp->headers) __chmap_destroy(resp->headers);
      _mem_free(chain->mp, resp->body);
      _mem_free(chain->mp, resp);
    }
    return;
  }
  chttpcli_async_result_t *result =
      (chttpcli_async_result_t *)_mem_calloc(chain->mp, 1, sizeof(*result));
  if (!result) {
    ctpool_future_fulfill(chain->future, NULL);
    if (resp) {
      if (resp->headers) __chmap_destroy(resp->headers);
      _mem_free(chain->mp, resp->body);
      _mem_free(chain->mp, resp);
    }
    return;
  }
  result->rv = rv;
  result->resp = resp;
  result->_m_procs = chain->mp;
  ctpool_future_fulfill(chain->future, result);
}

static void _async_fulfill(chttp_async_ctx_t *ctx, ccol_retval_t rv,
                           chttpcli_response *resp) {
  /* Marks this ctx terminal so _async_on_readable/_async_on_writable's own
   * top-of-function guard skips any further work for it; see
   * ctx->hop_completed's field comment for why a stray extra callback
   * invocation after this point must be a no-op rather than re-running
   * (non-idempotent) logic like _async_handle_redirect or a second
   * ctls_conn_handshake_step call on an already-failed handshake. */
  ctx->hop_completed = true;
  _async_fulfill_chain(ctx->chain, rv, resp);
}

/* Builds the chttpcli_response from the completed parse (mirrors Tier 1's
 * own response-building code at the tail of chttp_do_internal). Transfers
 * ownership of ctx->pctx.headers/ctx->bb.buf out of ctx (nulling them there)
 *; called BEFORE _async_finish_connection's idle-pool-offer path, which
 * would otherwise free those exact same fields while resetting ctx for its
 * idle life; see _async_on_data's CHTTP1_PAUSED handling for why the ordering
 * (build response, then finish the connection, then actually fulfil) matters
 * on its own terms too.
 *
 * For a streaming request (ctx->chain->write_fn set), body bytes were
 * already delivered to the caller's callback as they arrived off the wire
 * (see _async_submit_hop/_async_retry_hop's sink wiring); ctx->bb was
 * never used as the sink at all, so there is nothing to transfer into
 * resp->body, which stays NULL/0 exactly like Tier 1's
 * chttpclient_do_streaming leaves chttpcli_response.body. Headers ARE still
 * parsed internally (redirect detection needs them regardless of sink), but
 * are freed here rather than exposed, matching chttpclient_do_streaming's
 * documented "response headers are not accessible via this path". */
static chttpcli_response *_async_build_response(chttp_async_ctx_t *ctx) {
  chttpcli_response *resp =
      (chttpcli_response *)_mem_calloc(ctx->mp, 1, sizeof(*resp));
  if (!resp) return NULL;
  resp->_m_procs = ctx->mp;
  resp->status_code = ctx->pctx.status_code;
  if (ctx->chain->write_fn) {
    _parse_ctx_free_fields(&ctx->pctx);
  } else {
    resp->body = ctx->bb.buf;
    resp->body_len = ctx->bb.len;
    resp->headers = ctx->pctx.headers;
    ctx->pctx.headers = NULL; /* ownership transferred to resp */
    ctx->bb.buf = NULL;       /* ownership transferred to resp */
  }
  return resp;
}

/* Builds the response and fulfills with it in one step; used by the eof-
 * driven completion path, which never pools its connection (a peer that
 * closes the connection to signal end-of-body is, by definition, not
 * offering keep-alive), so there is no ordering hazard with
 * _async_finish_connection to worry about here. */
static void _async_fulfill_success(chttp_async_ctx_t *ctx) {
  chttpcli_response *resp = _async_build_response(ctx);
  _async_fulfill(ctx, resp ? ccol_success : ccol_not_enough_memory, resp);
}

/* ========================================================================== */
/*                         ASYNC IDLE POOL (TIER 2)                           */
/* ========================================================================== */

/*
 * Tier 2's own keep-alive idle pool: chmap(char *origin_key -> cvec of
 * chttp_async_ctx_t*), scoped per chttpcli (cli->idle_pools_async), mirroring
 * Tier 1's idle_pools/_idle_pool_take/_idle_pool_offer in shape and policy
 * (same CHTTP_MAX_IDLE_PER_ORIGIN/CHTTP_MAX_IDLE_TOTAL/CHTTP_IDLE_MAX_AGE_MS
 * caps) but necessarily different in mechanism: a pooled connection here is
 * still attached to the shared reactor (there is no way to detach a live
 * connection's registration without closing it), so pooling a ctx means
 * transitioning it to CHTTP_ASYNC_IDLE and letting the dispatch callbacks'
 * own IDLE-state branches keep driving it; any activity
 * (or the natural EOF/hangup a dead connection eventually produces) is
 * treated as "no longer usable" and torn down through the exact same
 * on_close path a normal failed connection would use, just entered from a
 * different state. Because there is no way to synchronously peek a reactor-
 * owned fd the way Tier 1's MSG_PEEK liveness probe does, a connection that
 * dies in the narrow window between being popped out of the pool and
 * actually being reused is instead caught by the reused/any_bytes_read
 * retry-once mechanism at the point of use (see _async_retry_hop);
 * together these two mechanisms give Tier 2 the same effective guarantee
 * Tier 1's probe-then-retry combination does.
 *
 * A pooled ctx holds its OWN engine reference (acquired in
 * _async_idle_pool_offer, released in whichever of _async_idle_pool_take's
 * reuse path or the IDLE-state on_close path claims it next); separate
 * from any chain's, since the chain that produced this connection has
 * already been (or is about to be) fully fulfilled and torn down, but the
 * pooled connection itself must keep the engine alive for as long as it
 * sits there.
 */

/* Decrements idle_total_count_async and, if it just reached zero, wakes
 * anyone (namely __chttpclient_destroy) waiting on idle_async_drained for
 * every pooled connection to finish tearing down. Must be called with
 * cli->lock held. */
static void _async_idle_count_dec_locked(struct chttpclient *cli) {
  if (cli->idle_total_count_async > 0) cli->idle_total_count_async--;
  if (cli->idle_total_count_async == 0)
    cond_var_broadcast(cli->idle_async_drained);
}

/* Removes `target` from its origin's idle list if it's still there (swap-
 * with-last, since cvector only supports push_back/pop_back) and decrements
 * the count. A no-op (returns false) if target isn't found; e.g. it was
 * already popped out by _async_idle_pool_take's own staleness eviction just
 * before its natural death was ALSO detected via the IDLE-state on_data/
 * on_close path; both sides are safe to call this unconditionally. Must be
 * called with cli->lock held. */
/* Deliberately does NOT call _async_idle_count_dec_locked: that decrement
 * (and the idle_async_drained broadcast it may trigger once the count
 * reaches zero) is the caller's own responsibility, deferred until the ctx
 * has ACTUALLY been freed. An earlier version of this function decremented
 * right here, at removal time -- which let __chttpclient_destroy observe
 * the count reach zero and proceed to free cli (and cli->m_procs) while
 * _async_idle_ctx_finish, the only caller of this function, was still
 * mid-teardown on a reactor worker thread, reading that same freed
 * cli->m_procs through ctx->mp inside its own _async_ctx_free call: a real
 * use-after-free, caught by ThreadSanitizer (not valgrind) via a stress
 * test that cycles many connections through real, server-initiated death
 * fast enough to make the window land. See _async_idle_ctx_finish's own
 * comment for the corrected ordering. */
static bool _async_idle_remove_locked(struct chttpclient *cli,
                                      chttp_async_ctx_t *target) {
  if (!cli->idle_pools_async || !target->origin_key) return false;
  cmap_pair kp = {.ptr = target->origin_key,
                  .size = strlen(target->origin_key) + 1};
  cmap_pair *vp = NULL;
  if (chmap_get_elem_ref(cli->idle_pools_async, &kp, &vp) != ccol_success ||
      !vp)
    return false;
  cvec list = _read_cvec(vp->ptr);
  if (!list) return false;
  size_t n = cvector_elem_count(list);
  for (size_t i = 0; i < n; i++) {
    chttp_async_ctx_t **slot = (chttp_async_ctx_t **)cvector_at(list, i);
    if (*slot != target) continue;
    chttp_async_ctx_t *last = NULL;
    cvector_pop_back(list, &last); /* removes the last element; may shrink
                                    * the backing array, so any pointer into
                                    * it taken before this call is stale */
    if (i < cvector_elem_count(list)) {
      chttp_async_ctx_t **slot2 = (chttp_async_ctx_t **)cvector_at(list, i);
      *slot2 = last;
    }
    return true;
  }
  return false;
}

/*
 * Attempts to pop a usable idle connection for `origin_key`. Returns true
 * and fills *out on success. May pop and discard several stale candidates
 * (age only; see the file-level comment above for why there is no
 * liveness probe here) before finding a fresh one or exhausting the list.
 *
 * IMPORTANT; return contract: on a true return, ctx->idle_lock is left
 * LOCKED. The caller (_async_submit_hop) must keep it held for as long as
 * it takes to finish reconfiguring ctx for the new hop and attempting the
 * write, only unlocking once ctx is fully consistent again (all fields set,
 * write attempted). This is not a stylistic choice: popping a candidate out
 * of the pool's cvec makes it un-findable by a concurrent _async_idle_
 * pool_take, but does nothing to stop the reactor from independently
 * dispatching a readable/writable/error callback for its connection on
 * another thread at any moment; the connection stays fully attached to the
 * reactor for as long as it sat in the pool, exactly like every other race
 * described in this section.
 * Without holding idle_lock across the WHOLE reconfiguration (not just the
 * state/chain pair, as an earlier version of this function attempted), a
 * concurrent on_close could see ctx in a half-reconfigured state; state
 * already flipped off CHTTP_ASYNC_IDLE, but hop/pctx/wire/etc. still being
 * written by this thread; and tear ctx down (freeing it, destroying
 * idle_lock itself) while this function's caller is still using it: a real,
 * caught-in-development use-after-free, distinct from (and deeper than) the
 * earlier state/chain torn-write bug. _async_ctx_is_idle (used by on_data/
 * on_ready/on_close) takes the same lock, so any of them racing this
 * function simply blocks until the caller releases it, by which point ctx
 * is fully self-consistent one way or the other.
 *
 * A discarded stale candidate is torn down via the SAME normal active-hop
 * teardown path every other failed hop uses (chain retained just for this,
 * hop_completed forced true to skip the reused-retry check, which does not
 * apply; this was never a real request attempt) rather than freed
 * directly here, for the identical reason: a concurrent, independently
 * triggered dispatch for its fd may already be racing this function and
 * must find a live, consistent ctx if it gets there first.
 */

/* Forward declarations: defined further below (after the "ASYNC IDLE POOL"
 * section, alongside the TLS/plain write helpers that also need them), but
 * _async_idle_pool_take's stale-candidate eviction needs _async_ctx_finish
 * already. */
static void _async_ctx_finish(chttp_async_ctx_t *ctx);
static void _async_retry_hop(chttp_async_ctx_t *ctx);

static bool _async_idle_pool_take(struct chttpclient *cli,
                                  const char *origin_key,
                                  chttp_async_chain_t *chain,
                                  chttp_async_ctx_t **out) {
  for (;;) {
    chttp_async_ctx_t *ctx = NULL;
    mutex_lock(cli->lock);
    if (cli->idle_pools_async) {
      cmap_pair kp = {.ptr = (void *)origin_key,
                      .size = strlen(origin_key) + 1};
      cmap_pair *vp = NULL;
      if (chmap_get_elem_ref(cli->idle_pools_async, &kp, &vp) == ccol_success &&
          vp) {
        cvec list = _read_cvec(vp->ptr);
        if (list && cvector_elem_count(list) > 0 &&
            cvector_pop_back(list, &ctx) == ccol_success) {
          _async_idle_count_dec_locked(cli);
        }
      }
    }
    mutex_unlock(cli->lock);
    if (!ctx) return false;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long age_ms = (now.tv_sec - ctx->last_used.tv_sec) * 1000L +
                  (now.tv_nsec - ctx->last_used.tv_nsec) / 1000000L;
    if (age_ms <= CHTTP_IDLE_MAX_AGE_MS) {
      mutex_lock(ctx->idle_lock); /* left locked; see contract above */
      /* See ctx->deadline_lock's own field comment for why this dedicated
       * leaf lock (not idle_lock, which the deadline sweep deliberately
       * never takes) is what makes this safe for the sweep to read
       * concurrently. */
      mutex_lock(ctx->deadline_lock);
      ctx->overall_deadline = chain->overall_deadline;
      mutex_unlock(ctx->deadline_lock);
      ctx->chain = chain;
      ctx->state = CHTTP_ASYNC_WRITING;
      *out = ctx;
      return true;
    }

    _async_chain_retain(chain); /* throwaway; balanced by the normal
                                 * teardown _async_ctx_finish runs below */
    mutex_lock(ctx->idle_lock);
    mutex_lock(ctx->deadline_lock);
    ctx->overall_deadline = chain->overall_deadline;
    mutex_unlock(ctx->deadline_lock);
    ctx->chain = chain;
    ctx->state = CHTTP_ASYNC_WRITING; /* just needs to be non-IDLE */
    ctx->hop_completed = true;
    ctx->reused = false;
    mutex_unlock(ctx->idle_lock);
    _async_ctx_finish(ctx); /* hop_completed is already true, so this is a
                             * plain teardown, mirroring the same active-hop
                             * teardown path any other failed hop uses */
    /* _async_ctx_finish/_async_ctx_teardown only release the chain
     * reference; they know nothing about the SEPARATE engine reference
     * _async_idle_pool_offer acquired for this ctx while it sat in the idle
     * pool (ctx->chain is NULL/unrelated at that time). Every other exit
     * from the idle pool (a successful reuse in _async_submit_hop, or
     * organic death via _async_idle_ctx_finish) explicitly releases that
     * reference; this staleness-eviction path was missing the matching
     * release, permanently leaking one engine reference per aged-out
     * connection. */
    _client_engine_release();
    /* loop: try the next candidate (if any) for this origin */
  }
}

/*
 * Offers a still-good, keep-alive-eligible connection back to cli's async
 * idle pool, bounded by the same per-origin/total caps Tier 1 uses. Returns
 * true if pooled (ownership of the connection, and this ctx's memory,
 * transfers to the pool; the caller must not touch ctx again) or false if
 * it doesn't fit (caps hit, client destroying, or the idle-slot engine
 * reference couldn't be acquired); in which case ctx is left COMPLETELY
 * UNTOUCHED (still attached to its original chain, in whatever state it was
 * in on entry) and the caller is expected to fall back to closing it
 * normally, exactly like Tier 1's identical "a lost optimisation
 * opportunity, never a correctness issue" comment.
 *
 * On success, detaches ctx from its original chain, releasing the one chain
 * reference this hop was holding for it (the hop is over; the chain no
 * longer owns this connection, the idle pool does); forgetting this
 * release was a real, valgrind-caught reference leak during this feature's
 * own development, since a pooled connection never goes through the normal
 * _async_ctx_teardown path (which is the only OTHER place that releases a
 * ctx's chain reference) until it is later reused or evicted.
 *
 * The capacity check (is there room in the pool for this origin, creating
 * the per-origin list on demand if needed) happens FIRST, entirely under
 * cli->lock, deciding definitively whether this offer can succeed before
 * touching ctx AT ALL. Only once that is certain does this function reset
 * ctx to a clean idle state (clearing the per-hop parse/body/wire buffers
 * and marking it CHTTP_ASYNC_IDLE) and insert it; both still under the
 * SAME cli->lock critical section, so no other thread can pop from the list
 * in between. This ordering matters for two independent reasons: (1) ctx
 * must never become visible/poppable via _async_idle_pool_take while only
 * partially reset; _async_idle_pool_take unconditionally overwrites
 * whatever it finds without waiting for anything, so a concurrent take()
 * starting to reconfigure ctx for a brand new hop while this function was
 * still freeing/resetting those exact same fields for the OLD hop would be
 * a genuine concurrent-access data race on the fields themselves, not just
 * a logical inconsistency; caught during this feature's own development;
 * (2) the caller's "ctx untouched on failure" contract above would
 * otherwise be violated by a version of this function that resets ctx
 * speculatively before knowing whether the pool has room.
 */
static bool _async_idle_pool_offer(chttp_async_ctx_t *ctx) {
  struct chttpclient *cli = ctx->cli;

  /* This ctx is about to belong to the idle pool, not any chain; it needs
   * its own engine reference to keep the reactor alive while it sits here,
   * independent of whichever chain's lifetime just ended. Acquired before
   * anything else so a failure here need not unwind any pool state at all. */
  if (_client_engine_acquire() != ccol_success) return false;

  cvec list = NULL;
  bool has_room = false;
  mutex_lock(cli->lock);
  if (!cli->destroying && cli->idle_pools_async &&
      cli->idle_total_count_async < CHTTP_MAX_IDLE_TOTAL) {
    cmap_pair kp = {.ptr = ctx->origin_key,
                    .size = strlen(ctx->origin_key) + 1};
    cmap_pair *vp = NULL;
    if (chmap_get_elem_ref(cli->idle_pools_async, &kp, &vp) == ccol_success &&
        vp) {
      list = _read_cvec(vp->ptr);
    }
    if (!list) {
      char *cverr = NULL;
      list = cvector_create_full(sizeof(chttp_async_ctx_t *), cli->m_procs,
                                 &cverr);
      if (list) {
        cmap_pair vp2 = {.ptr = &list, .size = sizeof(list)};
        if (chmap_insert_elem(cli->idle_pools_async, &kp, &vp2) !=
            ccol_success) {
          __cvector_destroy(list);
          list = NULL;
        }
      }
    }
    has_room = (list && cvector_elem_count(list) < CHTTP_MAX_IDLE_PER_ORIGIN);
  }
  if (!has_room) {
    mutex_unlock(cli->lock);
    _client_engine_release();
    return false; /* ctx untouched; caller falls back to a normal close */
  }

  /* Committed: this ctx WILL leave chain and become idle-pooled. Capture
   * the chain reference to release below, then reset ctx while STILL
   * holding cli->lock (see the file-level comment above for why). */
  chttp_async_chain_t *old_chain = ctx->chain;

  /* _parse_ctx_free_fields only frees+nulls cur_field/cur_value/location/
   * headers; every other field (cur_field_cap/cur_value_cap in
   * particular) is left stale. That has always been safe at its other call
   * sites (_async_ctx_free, right before the whole ctx is freed; and Tier
   * 1's per-hop chttp_parse_ctx_t, a fresh stack struct every hop) because
   * the struct itself is discarded immediately after. Here it is NOT
   * discarded (ctx is about to be reused for a future hop) so a stale,
   * non-zero cur_field_cap paired with a freshly-NULLed cur_field would
   * make _accum_append's very next call skip its realloc (capacity already
   * "big enough") and memcpy into a NULL buffer. The explicit memset below
   * (after _parse_ctx_free_fields has freed everything that needs freeing)
   * restores the same all-zero state a freshly _mem_calloc'd ctx already
   * has, which every other user of chttp_parse_ctx_t implicitly relies on. */
  _parse_ctx_free_fields(&ctx->pctx);
  memset(&ctx->pctx, 0, sizeof(ctx->pctx));
  _mem_free(ctx->mp, ctx->bb.buf);
  ctx->bb.buf = NULL;
  ctx->bb.len = ctx->bb.cap = 0;
  _mem_free(ctx->mp, ctx->wire);
  ctx->wire = NULL;
  ctx->wire_len = ctx->wire_sent = 0;
  ctx->hop_completed = false;
  ctx->reused = false;
  ctx->any_bytes_read = false;
  /* ctx->chain and ctx->state flip together, under idle_lock; see that
   * field's comment for why an unsynchronised pair of writes here could be
   * observed torn (state already IDLE, chain not yet NULL, or vice versa)
   * by a concurrent on_data/on_ready/on_close dispatch on another thread. */
  mutex_lock(ctx->idle_lock);
  ctx->chain = NULL;
  ctx->state = CHTTP_ASYNC_IDLE;
  mutex_unlock(ctx->idle_lock);

  clock_gettime(CLOCK_MONOTONIC, &ctx->last_used);
  bool pushed = (cvector_push_back(list, &ctx) == ccol_success);
  if (pushed) cli->idle_total_count_async++;
  mutex_unlock(cli->lock);

  if (!pushed) {
    /* Vanishingly rare (OOM inside cvector_push_back itself, despite room
     * being confirmed available above); ctx has already been fully
     * detached from old_chain and reset to an idle shape, so it can no
     * longer be handed back to the caller as "just fio_close it as a
     * normal active ctx" the way the has_room==false path above can. Tear
     * it down directly instead. */
    _async_chain_release(old_chain);
    _async_ctx_free(ctx);
    _client_engine_release();
    return false;
  }

  _async_chain_release(old_chain);
  return true;
}

/*
 * Called once a hop's response has been fully consumed, before deciding
 * whether to redirect or fulfil; offers the connection back to cli's idle
 * pool if `reusable`, otherwise closes it. Mirrors Tier 1's identical
 * reusable/_idle_pool_offer dance in chttp_do_internal, applied uniformly
 * regardless of whether this hop turns out to be a redirect or the final
 * response (see _async_handle_redirect and _async_on_data's CHTTP1_PAUSED
 * handling, both of which call this before doing anything else with the
 * connection).
 */
/*
 * This module's own guaranteed-exactly-once teardown point for an ACTIVE
 * (non-idle-pooled) ctx: every place that ends such a connection (a hard
 * error, a timeout, an explicit close after a successful/redirect
 * completion that wasn't pooled) calls this exactly once, since event_loop
 * has no unconditional terminal callback of its own to rely on instead
 * (see the "ASYNC CONNECTION STATE MACHINE" section's file-level comment).
 *
 * Deliberately does NOT pre-emptively fulfil with a generic error before
 * checking retry-eligibility: doing so would mark ctx->hop_completed and
 * incorrectly skip a legitimate reused-connection retry. If neither
 * timed_out nor the reused-retry case applies, this function fulfils
 * nothing itself; _async_ctx_teardown's chain release below carries its own
 * backstop (guarded by chain->fulfilled) that fulfils with the generic
 * ccol_http_transfer_aborted exactly once, the same value every caller of
 * this function that doesn't need a more specific code already relied on.
 */
static void _async_ctx_finish(chttp_async_ctx_t *ctx) {
  if (!ctx->hop_completed) {
    if (ctx->timed_out) { /* _Atomic; plain read is already race-free */
      _async_fulfill(ctx, ccol_timed_out, NULL);
    } else if (ctx->reused && !ctx->any_bytes_read) {
      _async_retry_hop(ctx); /* marks ctx->hop_completed = true itself */
    }
  }
  _async_ctx_teardown(ctx);
}

/*
 * This module's own teardown point for an IDLE-pooled ctx: removes it from
 * the pool and frees it. It holds its own engine reference (acquired in
 * _async_idle_pool_offer), not any chain's, since ctx->chain is NULL while
 * idle.
 *
 * Only actually tears ctx down if THIS call is the one that successfully
 * removes it from the pool's cvec (cli->lock is the true, sole arbiter of
 * exclusive ownership here; see _async_idle_remove_locked's own bool
 * return). If the removal finds nothing (already removed by a concurrent
 * caller), this returns immediately without touching ctx again.
 *
 * This is load-bearing, not a defensive nicety: with N reactor threads
 * sharing one epoll instance (no EPOLLEXCLUSIVE-style dedup), a genuinely
 * dead connection's EOF condition is PERSISTENT (unlike a one-shot
 * readable-data event, every subsequent recv()/peek on an already-closed
 * fd keeps reporting EOF, not EWOULDBLOCK), so two sequential stale
 * dispatches for the same idle ctx can both legitimately observe real EOF
 * via the peek in _async_on_readable's idle branch and both decide to
 * evict it. An earlier version of this function unconditionally called
 * _async_ctx_free/_client_engine_release regardless of whether the removal
 * above actually found anything (the old comment even said "a no-op if
 * some other path already removed it" while the code below it was NOT,
 * in fact, a no-op); a real double-free, caught via valgrind and a
 * flaky-test repro loop against async_idle_pool.dead_connection_detected_
 * and_retried specifically because that test's second request is exactly
 * what makes the first request's pooled connection genuinely, persistently
 * dead (the server closes its end), rather than merely racing a stale-but-
 * harmless spurious dispatch.
 *
 * idle_total_count_async is decremented (and idle_async_drained possibly
 * broadcast) only in a THIRD, separate locked section, after both
 * _async_ctx_free and _client_engine_release have already fully run below,
 * not folded into the removal step above. __chttpclient_destroy treats the
 * count reaching zero as its signal that every pooled connection has
 * genuinely finished tearing down before it proceeds to free cli (and
 * cli->m_procs, which ctx->mp still points at); decrementing at removal
 * time let that signal fire while this function's own _async_ctx_free call
 * was still using cli->m_procs on this thread, a real use-after-free
 * ThreadSanitizer caught that valgrind alone had not (see
 * _async_idle_remove_locked's own comment for the full account).
 */
static void _async_idle_ctx_finish(chttp_async_ctx_t *ctx) {
  struct chttpclient *cli = ctx->cli;
  mutex_lock(cli->lock);
  bool removed = _async_idle_remove_locked(cli, ctx);
  mutex_unlock(cli->lock);
  if (!removed) return;
  _async_ctx_free(ctx);
  _client_engine_release();
  mutex_lock(cli->lock);
  _async_idle_count_dec_locked(cli);
  mutex_unlock(cli->lock);
}

static void _async_finish_connection(chttp_async_ctx_t *ctx, bool reusable) {
  if (reusable && ctx->origin_key && _async_idle_pool_offer(ctx)) return;
  _async_ctx_finish(ctx);
}

/* Forward declaration: _async_on_readable (below) calls this on a reused
 * connection that turns out to be dead before any response byte was read;
 * the full definition (after _async_connect_task, which it needs to queue
 * the replacement attempt) comes later in this file, alongside
 * _async_submit_hop. */
static void _async_retry_hop(chttp_async_ctx_t *ctx);

/* Forward declaration: _async_tls_advance (below) calls this once the
 * handshake completes, before its own definition later in this file. */
static void _async_tls_try_write(chttp_async_ctx_t *ctx);

/*
 * Drives the client TLS handshake one step at a time from either
 * _async_on_readable or _async_on_writable, whichever fires next (this
 * ctx's single registration is flipped between read/write direction via
 * event_loop_modify as the handshake's own WANT_READ/WANT_WRITE demands).
 * No locking needed around ctx->tls: event_loop's own per-registration
 * dispatch_lock already guarantees this ctx's read and write direction are
 * never dispatched concurrently with each other (see the file-level comment
 * above for why this replaces the old tls_lock entirely).
 */
static void _async_tls_advance(chttp_async_ctx_t *ctx) {
  ctls_handshake_result_t r = ctls_conn_handshake_step(ctx->tls);
  if (r == CTLS_HANDSHAKE_DONE) {
    ctx->state = CHTTP_ASYNC_WRITING;
    if (event_loop_modify(cli_engine_bundler.reactor, ctx->reg,
                          ccol_select_write) != ccol_success) {
      _async_ctx_finish(ctx);
      return;
    }
    _async_tls_try_write(ctx);
    return;
  }
  if (r == CTLS_HANDSHAKE_ERROR) {
    /* 0 == X509_V_OK by OpenSSL convention; ctls.h intentionally does not
     * expose OpenSSL headers to callers, so the raw value is compared
     * directly rather than via the X509_V_OK symbol (mirrors Tier 1's
     * _tls_handshake). Never retry-eligible regardless of ctx->reused: a
     * handshake failure is a TLS-level problem, not evidence the pooled
     * connection had merely gone stale. */
    long vr = ctls_conn_verify_result(ctx->tls);
    _async_fulfill(ctx,
                   vr != 0 ? ccol_http_tls_cert_verification_failed
                           : ccol_http_tls_handshake_failed,
                   NULL);
    _async_ctx_finish(ctx);
    return;
  }
  ccol_select_dir want =
      (r == CTLS_HANDSHAKE_WANT_WRITE) ? ccol_select_write : ccol_select_read;
  event_loop_modify(cli_engine_bundler.reactor, ctx->reg, want);
}

/*
 * Writes as much of ctx->wire[ctx->wire_sent..] as ctls_conn_write will
 * currently accept, tracking partial progress (ordinary short-write
 * semantics, not an error). Called once right after the handshake completes
 * and again from _async_on_writable each time write interest fires.
 */
static void _async_tls_try_write(chttp_async_ctx_t *ctx) {
  while (ctx->wire_sent < ctx->wire_len) {
    ssize_t n = ctls_conn_write(ctx->tls, ctx->wire + ctx->wire_sent,
                                ctx->wire_len - ctx->wire_sent);
    if (n > 0) {
      ctx->wire_sent += (size_t)n;
      continue;
    }
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) return;
    /* n == 0 or a hard error: the connection is dead. Retry-eligibility is
     * checked inside _async_ctx_finish itself; no pre-emptive fulfil here
     * (see that function's own comment for why). */
    _async_ctx_finish(ctx);
    return;
  }
  /* A REUSED ctx's wire buffer must survive a "successful" write (accepted
   * by the local socket buffer, which does NOT guarantee the peer is
   * actually still alive to respond); see _async_retry_hop, which can run
   * against THIS ctx later if the subsequent read side discovers the
   * connection was already dead, and needs the ORIGINAL request bytes to
   * resend on a fresh connection. Freeing wire here unconditionally was a
   * real, reproducible bug (not just the earlier NULL/stale-wire_len crash
   * this comment used to describe): once that crash was fixed by also
   * zeroing wire_len/wire_sent alongside wire, _async_retry_hop's transfer
   * of a NULL wire + wire_len==0 into the retry ctx silently made the retry
   * send ZERO bytes (its own write loop's `wire_sent < wire_len` check is
   * immediately false), so the retry's connection sat open while the real
   * server-side mock waited out its own 5-second read timeout before
   * closing it; caught via async_idle_pool.dead_connection_detected_and_
   * retried failing intermittently under full-suite load (never in
   * isolation, since it needs enough concurrent connections for a write to
   * a reused-but-already-peer-closed socket to actually succeed locally
   * before the read side notices), root-caused with targeted stderr tracing
   * on both the client and the test mock server after ThreadSanitizer
   * confirmed no data race was involved. A fresh (non-reused) ctx is never
   * retried (see _async_ctx_finish's own reused-only check), so its wire is
   * still freed eagerly here, exactly as before; only the reused case needs
   * to keep it alive, until either _async_retry_hop's own transfer, a
   * successful _async_idle_pool_offer (which resets wire unconditionally
   * for the next hop), or _async_ctx_free's own unconditional free at
   * teardown claims it. */
  if (!ctx->reused) {
    _mem_free(ctx->mp, ctx->wire);
    ctx->wire = NULL;
    ctx->wire_len = ctx->wire_sent = 0;
  }
  ctx->state = CHTTP_ASYNC_READING;
  if (event_loop_modify(cli_engine_bundler.reactor, ctx->reg,
                        ccol_select_read) != ccol_success) {
    _async_ctx_finish(ctx);
  }
}

/*
 * Plain-HTTP counterpart to _async_tls_try_write: writes as much of
 * ctx->wire[ctx->wire_sent..] as a raw, non-blocking send() will currently
 * accept. Shared by _async_on_writable's WRITING branch and
 * _async_submit_hop's reused-connection write attempt, so the exact same
 * partial-write/EWOULDBLOCK/hard-failure handling is not duplicated in
 * three places.
 */
static void _async_plain_try_write(chttp_async_ctx_t *ctx) {
  while (ctx->wire_sent < ctx->wire_len) {
    ssize_t n = send(ctx->fd, ctx->wire + ctx->wire_sent,
                     ctx->wire_len - ctx->wire_sent, MSG_NOSIGNAL);
    if (n > 0) {
      ctx->wire_sent += (size_t)n;
      continue;
    }
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) return;
    _async_ctx_finish(ctx);
    return;
  }
  /* See _async_tls_try_write's identical comment for the full history: a
   * REUSED ctx must keep its wire buffer alive past a "successful" write,
   * since _async_retry_hop needs the original request bytes to resend if
   * the read side then discovers the peer was already dead; only a fresh
   * (never-retried) ctx frees it here eagerly. */
  if (!ctx->reused) {
    _mem_free(ctx->mp, ctx->wire);
    ctx->wire = NULL;
    ctx->wire_len = ctx->wire_sent = 0;
  }
  ctx->state = CHTTP_ASYNC_READING;
  if (event_loop_modify(cli_engine_bundler.reactor, ctx->reg,
                        ccol_select_read) != ccol_success) {
    _async_ctx_finish(ctx);
  }
}

/* Forward declaration: _async_on_readable's redirect-detection branches
 * (below) call this; its full definition comes after _async_connect_task,
 * which it needs in order to queue the next hop. */
static void _async_handle_redirect(chttp_async_ctx_t *ctx, bool reusable);

/* Reads ctx->state under ctx->idle_lock; see that field's comment for why
 * a plain unlocked read is not safe here specifically. */
static bool _async_ctx_is_idle(chttp_async_ctx_t *ctx) {
  mutex_lock(ctx->idle_lock);
  bool idle = (ctx->state == CHTTP_ASYNC_IDLE);
  mutex_unlock(ctx->idle_lock);
  return idle;
}

static void _async_on_writable(event_loop loop, ccol_selectable *sel,
                               void *arg) {
  (void)loop;
  (void)sel;
  chttp_async_ctx_t *ctx = (chttp_async_ctx_t *)arg;
  /* See ctx->hop_completed's field comment: a stray dispatch can still
   * arrive after this ctx already reached a terminal outcome, and must not
   * re-run any of the state-transition logic below. An idle-pooled ctx's
   * registration only ever carries read direction (see the "ASYNC IDLE
   * POOL" section), so on_writable structurally cannot fire for one; no
   * idle check is needed here (unlike on_readable/on_error). */
  if (ctx->hop_completed) return;
  /* See ctx->reg's own field comment: a dispatch for this ctx's very first
   * (write-direction) registration can legitimately arrive before
   * _async_connect_task's own `ctx->reg = event_loop_add(...)` assignment
   * has finished, especially for a loopback connect that's often already
   * writable the instant it's registered. Nothing else has happened yet in
   * that window (no read, no write), so simply returning is always safe;
   * the same level-triggered condition is reported again on the very next
   * epoll_wait, by which point ctx->reg is set. */
  if (!ctx->reg) return;

  if (ctx->state == CHTTP_ASYNC_CONNECTING) {
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (getsockopt(ctx->fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 ||
        soerr != 0) {
      _async_fulfill(ctx, ccol_http_connection_failed, NULL);
      _async_ctx_finish(ctx);
      return;
    }
    if (!ctx->is_unix) _apply_tcp_nodelay(ctx->fd);

    if (ctx->is_https) {
      /* Configured cert/key/ca path(s) not being readable is deferred all
       * the way to here (mirroring Tier 1's own _rebuild_tls_ctx_locked
       * comment) rather than failing at chttpclient_set_tls time; but that
       * deferred failure is checked synchronously in
       * _chttp_do_async_internal (tls_ctx_usable), before any connection is
       * even opened, so reaching here means TLS is genuinely usable. */
      ctx->tls = ctls_conn_create_client(ctx->chain->tls_ctx, ctx->fd,
                                         ctx->host, ctx->verify_host, NULL);
      if (!ctx->tls) {
        _async_fulfill(ctx, ccol_not_enough_memory, NULL);
        _async_ctx_finish(ctx);
        return;
      }
      ctx->state = CHTTP_ASYNC_TLS_HANDSHAKING;
      _async_tls_advance(ctx);
      return;
    }

    ctx->state = CHTTP_ASYNC_WRITING;
    /* Falls through to the WRITING branch below to attempt the first write
     * immediately (the fd is already known writable right now), rather than
     * waiting for a separate on_writable dispatch. */
  }

  if (ctx->state == CHTTP_ASYNC_TLS_HANDSHAKING) {
    _async_tls_advance(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_WRITING) {
    if (ctx->tls) {
      _async_tls_try_write(ctx);
      return;
    }
    _async_plain_try_write(ctx);
    return;
  }
  /* CHTTP_ASYNC_READING: nothing of ours is pending to flush; a stray
   * on_writable here is a no-op. */
}

static void _async_on_readable(event_loop loop, ccol_selectable *sel,
                               void *arg) {
  (void)loop;
  (void)sel;
  chttp_async_ctx_t *ctx = (chttp_async_ctx_t *)arg;

  /* This hop already reached a terminal outcome (fulfilled (including a
   * TLS handshake failure detected mid-handshake) or handed off to a
   * redirect hop) via a previous dispatch for this same ctx; a spurious
   * extra on_readable can still arrive afterward (e.g. every route in the
   * test server's mock sends "Connection: close" and closes its end right
   * after writing the response, so the peer's EOF can be observed in a
   * SEPARATE dispatch from the one that already consumed the response
   * bytes and hit CHTTP1_PAUSED). Re-running the completion logic would be
   * harmless for a plain fulfill (guarded by chain->fulfilled) or a repeat
   * ctls_conn_handshake_step call on an already-failed handshake (which
   * just returns another, ignored, error), but _async_handle_redirect is
   * NOT idempotent; it unconditionally queues another hop every time it
   * runs, so without this guard a single hop could fan out into multiple
   * redirect chains sharing the same chain state, corrupting its refcount
   * bookkeeping. Checked before the TLS_HANDSHAKING branch too, since a
   * handshake failure detected by one dispatch must stop a second, racing
   * dispatch from re-entering _async_tls_advance. event_loop's own
   * per-registration dispatch_lock already serialises every dispatch for
   * this ctx's single registration against itself, so a plain bool is
   * sufficient here; no additional locking needed. */
  if (ctx->hop_completed) return;
  /* See ctx->reg's own field comment for why this can legitimately be NULL
   * on a very early dispatch, and why simply returning is always safe. */
  if (!ctx->reg) return;

  if (_async_ctx_is_idle(ctx)) {
    /* With N reactor threads all calling epoll_wait on one shared epoll
     * instance (no EPOLLEXCLUSIVE-style dedup), a single underlying
     * readiness event can legitimately produce more than one sequential
     * dispatch for the same fd: e.g. a response that arrives in two TCP
     * segments can have both segments' readiness independently observed by
     * two different reactor threads' own epoll_wait calls before either has
     * had a chance to actually drain the socket, so the first dispatch
     * fully reads and processes the response (transitioning this ctx to
     * IDLE and pooling it) and a second, already-in-flight dispatch for the
     * very same original event still follows afterward. With N reactor
     * threads able to dispatch concurrently, a bare dispatch here is NOT
     * sufficient evidence that the connection is actually dead or has
     * genuinely unexpected data; an actual non-blocking read is required
     * to tell a stale, already-handled readiness apart from real activity.
     * Found via targeted stderr tracing after a real, reproducible failure
     * in async_idle_pool.sequential_requests_reuse_connection (pooled
     * connections were being spuriously evicted moments after being
     * offered), not assumed from code inspection alone. */
    char peek_buf[1];
    ssize_t pn;
    if (ctx->tls) {
      pn = ctls_conn_read(ctx->tls, peek_buf, sizeof(peek_buf));
    } else {
      do {
        pn = recv(ctx->fd, peek_buf, sizeof(peek_buf), 0);
      } while (pn < 0 && errno == EINTR);
    }
    if (pn < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
      /* Nothing actually available: this was a stale/duplicate dispatch for
       * an event already fully handled by another thread. The connection
       * remains genuinely idle and pooled; nothing to do. */
      return;
    }
    /* pn == 0 (peer closed), pn > 0 (unexpected data), or a hard error: the
     * connection is genuinely no longer safely reusable. */
    _async_idle_ctx_finish(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_TLS_HANDSHAKING) {
    _async_tls_advance(ctx);
    return;
  }

  char buf[8192];
  ssize_t n;
  bool eof;

  if (ctx->tls) {
    n = ctls_conn_read(ctx->tls, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) return;
      eof = true;
    } else {
      eof = (n == 0);
    }
  } else {
    do {
      n = recv(ctx->fd, buf, sizeof(buf), 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) return;
    eof = (n <= 0);
  }
  if (!eof) ctx->any_bytes_read = true;

  if (eof) {
    /* Mirrors Tier 1's own n==0/EOF handling in _chttp_read_response: a
     * clean chttp1_parser_finish with a complete message is valid for
     * responses that signal their end via connection-close rather than
     * Content-Length/chunked framing. A reused connection that produces
     * this before any response byte came back is Tier 1's retry-once
     * scenario, checked BEFORE marking hop_completed/fulfilling; the whole
     * point is that nothing has failed for the caller yet.
     *
     * fe == CHTTP1_PAUSED (not just CHTTP1_OK) is the EXPECTED outcome for a
     * valid EOF-delimited body; see chttp1_parser_finish's own doc comment
     * and Tier 1's identical comment in _chttp_read_response. */
    chttp1_errno_t fe = chttp1_parser_finish(&ctx->parser);
    bool ok = ((fe == CHTTP1_OK || fe == CHTTP1_PAUSED) &&
               ctx->pctx.message_complete);
    if (!ok) {
      /* _async_ctx_finish itself checks reused/any_bytes_read and retries
       * if eligible; no pre-emptive fulfil here (see its own comment). */
      _async_ctx_finish(ctx);
      return;
    }
    ctx->hop_completed = true;
    if (ctx->pctx.will_redirect) {
      /* Peer-closed (rather than Content-Length/chunked) framing is never
       * keep-alive eligible; matches Tier 1's _chttp_read_response, which
       * hard-codes *keep_alive_out = false for this exact case. */
      _async_handle_redirect(ctx, false);
    } else {
      _async_fulfill_success(ctx);
      _async_ctx_finish(ctx);
    }
    return;
  }

  chttp1_errno_t err = chttp1_parser_execute(&ctx->parser, buf, (size_t)n);
  if (err == CHTTP1_PAUSED) {
    /* Message complete; the parser intentionally pauses right after, exactly
     * like Tier 1's own CHTTP1_PAUSED handling. */
    ctx->hop_completed = true;
    size_t consumed = chttp1_parser_consumed(&ctx->parser);
    if (consumed < (size_t)n) ctx->pctx.trailing_garbage = true;
    bool keep_alive =
        chttp1_should_keep_alive(&ctx->parser) && !ctx->pctx.trailing_garbage;

    if (ctx->pctx.will_redirect) {
      _async_handle_redirect(ctx, keep_alive); /* closes/pools the connection */
    } else {
      /* Capture chain (with a temporary extra retain; see
       * _async_handle_redirect's identical one for why: _async_finish_
       * connection below can trigger a concurrent teardown of THIS ctx's
       * own chain reference on another reactor thread, which could free
       * chain before the _async_fulfill_chain call below runs if nothing
       * else were holding it) and build the response BEFORE calling
       * _async_finish_connection: when keep_alive is true, that call can
       * successfully offer ctx to the idle pool, which resets ctx->chain to
       * NULL and ctx->pctx/ctx->bb for reuse (see _async_idle_pool_offer)
       * as part of a normal, expected, successful outcome; so both
       * ctx->chain and ctx->pctx/ctx->bb must be captured/extracted first,
       * or a NULL ctx->chain would crash the fulfil below (or the response
       * would be built from already-cleared fields). Finishing the
       * connection before fulfilling also matters independently:
       * fulfilling can unblock the caller immediately (e.g. a
       * ctpool_future_get on another thread), and if that caller then
       * destroys cli, _async_finish_connection's idle-pool-offer path
       * touching cli->lock afterward would race a use-after-free. */
      chttp_async_chain_t *chain = ctx->chain;
      _async_chain_retain(chain);
      chttpcli_response *resp = _async_build_response(ctx);
      _async_finish_connection(ctx, keep_alive);
      _async_fulfill_chain(chain, resp ? ccol_success : ccol_not_enough_memory,
                           resp);
      _async_chain_release(chain);
    }
    return;
  }
  if (err == CHTTP1_USER) {
    ctx->hop_completed = true;
    _async_fulfill(
        ctx,
        ctx->pctx.error ? ccol_not_enough_memory : ccol_http_transfer_aborted,
        NULL);
    _async_ctx_finish(ctx);
    return;
  }
  if (err != CHTTP1_OK) {
    ctx->hop_completed = true;
    _async_fulfill(ctx, ccol_http_transfer_aborted, NULL);
    _async_ctx_finish(ctx);
    return;
  }
  /* else: message not yet complete, wait for more on_readable */
}

static void _async_on_error(event_loop loop, ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)sel;
  chttp_async_ctx_t *ctx = (chttp_async_ctx_t *)arg;
  if (ctx->hop_completed) return;
  /* See ctx->reg's own field comment for why this can legitimately be NULL
   * on a very early dispatch (the fd erroring out essentially immediately
   * after being registered), and why simply returning is always safe: an
   * fd-level error condition is persistent at the OS level, so a later
   * dispatch (once ctx->reg is visible) will observe the same error. */
  if (!ctx->reg) return;
  if (_async_ctx_is_idle(ctx)) {
    _async_idle_ctx_finish(ctx);
    return;
  }
  /* An fd-level error with no more specific diagnosis available; rely on
   * _async_ctx_finish's own retry-check-then-backstop logic exactly like
   * the analogous cases above, rather than pre-emptively fulfilling and
   * accidentally disabling a legitimate reused-connection retry. */
  _async_ctx_finish(ctx);
}

/*
 * Runs on a cli_engine_bundler.dns_pool worker: resolves DNS (TCP) or builds
 * the sockaddr_un (unix), then issues ONE non-blocking connect() and registers
 * the resulting fd with the reactor for write-readiness; never blocks the
 * worker waiting for the connect to actually complete. This division of
 * labor (synchronous DNS resolution plus a single non-blocking connect
 * attempt, on an offload thread, rather than blocking the worker through the
 * entire connect) is deliberate: tying up this fixed-size worker pool for an
 * entire connect's duration would regress throughput under high concurrency,
 * whereas registering for write-readiness lets a single epoll instance
 * cheaply track thousands of pending connects. For TCP, only the first
 * candidate address that either connects immediately or returns EINPROGRESS
 * is used; a candidate that fails outright falls through to the next
 * address. Never touches the caller's thread.
 */
static void _async_connect_task(void *arg) {
  chttp_async_ctx_t *ctx = (chttp_async_ctx_t *)arg;
  int fd = -1;

  if (ctx->is_unix) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(ctx->unix_socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
      _async_fulfill(ctx, ccol_http_invalid_url, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
    memcpy(addr.sun_path, ctx->unix_socket_path, path_len + 1);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      _async_fulfill(ctx, ccol_http_connection_failed, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 &&
        errno != EINPROGRESS) {
      close(fd);
      _async_fulfill(ctx, ccol_http_connection_failed, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
  } else {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)ctx->port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(ctx->host, port_str, &hints, &res) != 0 || !res) {
      _async_fulfill(ctx, ccol_http_host_resolution_failed, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
      int cand = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK,
                        ai->ai_protocol);
      if (cand < 0) continue;
      if (connect(cand, ai->ai_addr, ai->ai_addrlen) == 0 ||
          errno == EINPROGRESS) {
        fd = cand;
        break;
      }
      close(cand);
    }
    freeaddrinfo(res);
    if (fd < 0) {
      _async_fulfill(ctx, ccol_http_connection_failed, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
  }

  /* ctx->fd/state are _Atomic (see their own field comments), so these
   * plain assignments are already race-free against the deadline sweep; an
   * independent thread, registered against this ctx since before it was
   * ever submitted here (see _async_submit_hop), which can run at any time,
   * including concurrently with this exact assignment. This also covers the
   * case where connect_timeout_ms already expired while this task merely
   * sat queued on cli_engine_bundler.dns_pool (e.g. a saturated pool): the
   * sweep cannot shut down an fd that doesn't exist yet, so on that path it can
   * only mark ctx->timed_out and wait; checked here, the moment a real fd
   * finally exists. */
  ctx->fd = fd;
  ctx->state = CHTTP_ASYNC_CONNECTING;

  if (ctx->timed_out) {
    /* Never registered with the reactor, so nothing else could act on this
     * fd yet; close it directly and report the timeout ourselves. */
    close(fd);
    ctx->fd = -1;
    _async_fulfill(ctx, ccol_timed_out, NULL);
    _async_ctx_teardown(ctx);
    return;
  }

  char *err = NULL;
  event_handlers_t handlers = {
      .on_readable = _async_on_readable,
      .on_writable = _async_on_writable,
      .on_error = _async_on_error,
  };
  /* idle_lock, held across both the event_loop_add call and this thread's
   * own read-back of ctx->reg right after: the moment event_loop_add makes
   * this registration live, a reactor thread is free to dispatch it (a
   * loopback connect is very often already writable immediately), drive the
   * whole hop to completion, and reach _async_ctx_free; racing this
   * thread's own still-in-flight "ctx->reg = ..." write and the "if
   * (!ctx->reg)" read right after it, a genuine write/read race on ctx->reg
   * itself (caught by ThreadSanitizer, not by inspection). _async_ctx_free
   * takes the same lock, briefly, as its very first action, which is enough
   * to guarantee this window has fully completed (ctx->reg published, and
   * this function has stopped touching ctx) before free() can proceed; no
   * dispatch path needs idle_lock to observe ctx->reg itself (see its own
   * field comment: a NULL read there is already handled as a harmless,
   * self-healing no-op), so this adds no new contention on that side. */
  mutex_lock(ctx->idle_lock);
  ctx->reg = event_loop_add(cli_engine_bundler.reactor,
                            selectable_from_fd(fd, ccol_select_write), handlers,
                            ctx, &err);
  bool reg_failed = !ctx->reg;
  mutex_unlock(ctx->idle_lock);
  if (reg_failed) {
    _async_fulfill(ctx, ccol_not_enough_memory, NULL);
    _async_ctx_teardown(ctx);
  }
}

/*
 * Called when a REUSED connection turns out to be dead before any response
 * byte was read (write failure, immediate EOF, or an error detected by the
 * reactor); mirrors Tier 1's identical "retry exactly once against a
 * brand-new connection" liveness-probe-failure recovery in
 * chttp_do_internal.
 *
 * Rather than reusing old_ctx's own struct in place for the new attempt
 * (which would require neutralising its dispatch callbacks to guard against
 * a dispatch already in flight for the OLD registration: such a dispatch
 * could otherwise arrive and observe ctx fields that have since been
 * repurposed for the NEW attempt; and that neutralisation is itself unsafe
 * if some OTHER, independent teardown was already mid-flight when we tried
 * it), this allocates a fresh ctx and transfers just what the retry needs
 * (the already-serialized wire bytes, host/port, origin_key, the empty response
 * headers map, and hop metadata) out of old_ctx, nulling those fields there
 * so old_ctx's own upcoming normal teardown (via its caller, exactly as if
 * this were an ordinary failure) doesn't double-free them. old_ctx is left
 * otherwise untouched and continues through its NORMAL teardown path
 * afterward; this function does not free it or touch its chain reference;
 * only ctx->hop_completed is set, both to prevent old_ctx's own on_data/
 * on_close from re-triggering this a second time and because, from
 * old_ctx's own perspective, it genuinely has reached a terminal outcome.
 *
 * Retains a SECOND chain reference for the new ctx (old_ctx keeps its own
 * until its own teardown releases it); retaining before old_ctx's release
 * can possibly happen guarantees the chain's refcount never dips to zero
 * prematurely during the handoff, exactly like a redirect hop's own
 * retain-before-release ordering.
 */
static void _async_retry_hop(chttp_async_ctx_t *old_ctx) {
  old_ctx->hop_completed = true;
  chttp_async_chain_t *chain = old_ctx->chain;

  _async_chain_retain(chain);

  chttp_async_ctx_t *ctx = _async_ctx_create(chain->mp);
  if (!ctx) {
    _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    _async_chain_release(chain);
    return;
  }

  /* ctx is a brand-new, still-private object here (not yet registered with
   * the deadline registry or event_loop), so no concurrent reader could
   * possibly race this write regardless; deadline_lock is still used, for
   * uniformity with every other write site (see its own field comment). */
  mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  mutex_unlock(ctx->deadline_lock);
  ctx->chain = chain;
  ctx->cli = old_ctx->cli;
  ctx->hop = old_ctx->hop;
  ctx->cur_method = old_ctx->cur_method;
  ctx->is_https = old_ctx->is_https;
  ctx->verify_host = old_ctx->verify_host;

  ctx->is_unix = old_ctx->is_unix;
  ctx->unix_socket_path = old_ctx->unix_socket_path;
  old_ctx->unix_socket_path = NULL;
  ctx->host = old_ctx->host;
  old_ctx->host = NULL;
  ctx->port = old_ctx->port;
  ctx->wire = old_ctx->wire;
  old_ctx->wire = NULL;
  ctx->wire_len = old_ctx->wire_len;
  ctx->origin_key = old_ctx->origin_key;
  old_ctx->origin_key = NULL;

  ctx->pctx.mp = chain->mp;
  ctx->pctx.is_head_request = old_ctx->pctx.is_head_request;
  ctx->pctx.redirects_still_allowed = old_ctx->pctx.redirects_still_allowed;
  /* Streaming (chain->write_fn set) delivers body bytes straight to the
   * caller's callback; buffered uses ctx->bb; see _async_build_response's
   * own comment for why this must be consistent with what it later does. */
  ctx->pctx.requested_sink_fn =
      chain->write_fn ? chain->write_fn : _sink_buffered;
  ctx->pctx.requested_sink_ctx = chain->write_fn ? chain->write_ctx : &ctx->bb;
  ctx->pctx.headers = old_ctx->pctx.headers; /* empty; nothing was ever
                                              * parsed into it, since retry
                                              * requires !any_bytes_read */
  old_ctx->pctx.headers = NULL;
  ctx->bb.mp = chain->mp;

  ctx->reused = false; /* the retry itself is a fresh connection */
  ctx->any_bytes_read = false;
  /* A retry always connects fresh (see above), so it needs its own
   * connect_deadline exactly like _async_submit_hop's fresh path; the
   * chain's overall_deadline is unaffected (it was never per-hop) and
   * continues to apply unchanged across the retry. */
  ctx->connect_deadline = _deadline_make(chain->connect_timeout_ms);

  call_once(client_http1_settings_bundler.once, _init_chttp1_settings);
  chttp1_parser_init(&ctx->parser, &client_http1_settings_bundler.settings);
  ctx->parser.data = &ctx->pctx;

  /* Registered BEFORE submitting; see _async_submit_hop's identical
   * comment for why (a worker could otherwise run this hop to completion
   * and free ctx before this thread registers it). */
  _client_deadline_register(ctx);

  ccol_retval_t sr = ctpool_submit(cli_engine_bundler.dns_pool,
                                   _async_connect_task, ctx, NULL);
  if (sr != ccol_success) {
    _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    _async_ctx_free(ctx); /* unregisters ctx too */
    _async_chain_release(chain);
  }
}

/*
 * Prepares and submits one hop of a redirect chain; used for both the very
 * first hop (from _chttp_do_async_internal) and every subsequent redirect
 * hop (from _async_handle_redirect). Retains one chain reference on success
 * (released when this hop's ctx is eventually torn down); on any failure,
 * that retain is released again internally (net effect: no refcount change)
 * and the chain's future is fulfilled with a specific error code before
 * returning, so callers never need their own fallback fulfil-on-failure
 * logic here; only _async_chain_release's generic backstop remains as a
 * true last resort for paths that can't be reached with a more specific
 * error (e.g. an already-in-flight connection dying).
 *
 * body_data/body_len/body_content_type describe THIS hop's body (usually
 * chain->body_data/len/content_type verbatim, or all-empty once a
 * non-preserving redirect has rewritten the method to GET); passed
 * explicitly rather than always read from chain because the caller (Tier 1
 * loop equivalent) is the one that knows, from the previous hop's status
 * code, whether to preserve or drop them.
 *
 * Returns true if the hop was successfully queued.
 */

/*
 * Handles a setup failure (headers-map allocation, request serialisation, or
 * origin_key allocation) that occurs AFTER a reused connection has already
 * been popped from the idle pool; i.e. ctx->fd is a live, registered
 * connection, not a not-yet-connected fresh ctx, and ctx->idle_lock is still
 * held per _async_idle_pool_take's return contract. Report the error, mark
 * the ctx terminal (both because it already has been, and to stop
 * _async_ctx_finish's retry-check from queuing a pointless retry of what is
 * an allocation failure, not a dead connection), release idle_lock (this
 * function is always the last thing _async_submit_hop does with ctx on this
 * path), and let the connection tear down normally via _async_ctx_teardown.
 * A fresh (not yet connected) ctx never had idle_lock locked and has no such
 * attachment to worry about, and is simply freed directly, exactly like
 * every other pre-connect failure path.
 */
static void _async_submit_hop_fail(chttp_async_ctx_t *ctx, ccol_retval_t rv) {
  chttp_async_chain_t *chain = ctx->chain;
  _async_fulfill_chain(chain, rv, NULL);
  if (ctx->reused) {
    ctx->hop_completed = true;
    mutex_unlock(ctx->idle_lock);
    _async_ctx_teardown(ctx);
    /* _async_ctx_teardown only releases ctx's chain reference; it knows
     * nothing about the SEPARATE engine reference _async_idle_pool_offer
     * acquired for this ctx while it sat in the idle pool (same reasoning
     * as _async_idle_pool_take's own staleness-eviction branch above, which
     * has this exact fix already). This is a fourth exit from the idle
     * pool this reference must be released on, alongside a successful
     * reuse (below in _async_submit_hop), organic idle-connection death
     * (_async_idle_ctx_finish), and staleness eviction
     * (_async_idle_pool_take); missing it here leaked one engine reference
     * per reused-connection setup failure (an allocation or serialisation
     * failure occurring after a pooled connection was already popped). */
    _client_engine_release();
  } else {
    _async_ctx_free(ctx);
    _async_chain_release(chain);
  }
}

static bool _async_submit_hop(chttp_async_chain_t *chain, const char *url_str,
                              chttp_method_t method, const void *body_data,
                              size_t body_len, const char *body_content_type,
                              int hop) {
  _async_chain_retain(chain);

  chttp_url_t url;
  ccol_retval_t prv = _parse_chttp_url(chain->mp, url_str, &url);
  if (prv != ccol_success) {
    _async_fulfill_chain(chain, prv, NULL);
    _async_chain_release(chain);
    return false;
  }
  if (url.is_https && !chain->tls_ctx_usable) {
    /* Mirrors Tier 1's identical per-hop check: the configured cert/key/ca
     * path(s) were not readable at set_tls time. */
    _url_free(chain->mp, &url);
    _async_fulfill_chain(chain, ccol_http_tls_handshake_failed, NULL);
    _async_chain_release(chain);
    return false;
  }

  /* Auto-injected-from-userinfo Authorization carry-forward; mirrors Tier
   * 1's identical carried_auth/carried_auth_origin locals in
   * chttp_do_internal exactly (same-origin carry, permanent cross-origin
   * drop). Safe to mutate chain->carried_auth* here without any additional
   * locking: _async_submit_hop runs strictly sequentially per chain (one
   * hop's setup always completes (including this mutation) before the
   * next hop is ever submitted), the same invariant every other unguarded
   * chain-field mutation in this file's redirect machinery already relies
   * on (e.g. the method/body downgrade decision in _async_handle_redirect).
   */
  const char *effective_auth = NULL;
  if (url.userinfo_authorization) {
    effective_auth = url.userinfo_authorization;
  } else if (chain->carried_auth &&
             strcmp(chain->carried_auth_origin, url.origin_key) == 0) {
    effective_auth = chain->carried_auth;
  }
  if (url.userinfo_authorization) {
    char *na = ccol_strdup(chain->mp, url.userinfo_authorization);
    char *no = ccol_strdup(chain->mp, url.origin_key);
    if (!na || !no) {
      _mem_free(chain->mp, na);
      _mem_free(chain->mp, no);
      _url_free(chain->mp, &url);
      _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
      _async_chain_release(chain);
      return false;
    }
    _mem_free(chain->mp, chain->carried_auth);
    _mem_free(chain->mp, chain->carried_auth_origin);
    chain->carried_auth = na;
    chain->carried_auth_origin = no;
  } else if (chain->carried_auth &&
             strcmp(chain->carried_auth_origin, url.origin_key) != 0) {
    _mem_free(chain->mp, chain->carried_auth);
    _mem_free(chain->mp, chain->carried_auth_origin);
    chain->carried_auth = NULL;
    chain->carried_auth_origin = NULL;
  }

  chttp_async_ctx_t *ctx = NULL;
  bool reused = _async_idle_pool_take(chain->cli, url.origin_key, chain, &ctx);
  if (!reused) {
    ctx = _async_ctx_create(chain->mp);
    if (!ctx) {
      _url_free(chain->mp, &url);
      _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
      _async_chain_release(chain);
      return false;
    }
    ctx->cli = chain->cli;
    /* Only a fresh connection actually goes through CHTTP_ASYNC_CONNECTING/
     * TLS_HANDSHAKING; a reused one skips straight to WRITING below, so
     * connect_deadline is simply never consulted for it (see the "ASYNC
     * DEADLINE SWEEP" section). */
    ctx->connect_deadline = _deadline_make(chain->connect_timeout_ms);
  }
  /* For the reused case, _async_idle_pool_take has already set ctx->chain,
   * ctx->overall_deadline, and ctx->state (to CHTTP_ASYNC_WRITING) under
   * ctx->idle_lock, and returned with that lock STILL HELD; see its own
   * doc comment for why. This function must keep it held for everything
   * below, only releasing it once the write attempt (success or failure)
   * at the bottom of this function has actually happened; ctx is not safe
   * for any concurrent dispatch to touch until then. The re-assignment
   * below is a harmless no-op for the reused case (already set to the
   * same values) and the only assignment for the fresh case; still goes
   * through deadline_lock regardless (see its own field comment), since
   * for the reused case this ctx may already be registered with the
   * deadline sweep from an earlier fresh connect. */
  mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  mutex_unlock(ctx->deadline_lock);
  ctx->chain = chain;
  ctx->hop = hop;
  ctx->cur_method = method;
  ctx->is_https = url.is_https;
  ctx->verify_host = chain->verify_host;
  ctx->reused = reused;
  ctx->any_bytes_read = false;
  ctx->hop_completed = false;

  ctx->pctx.mp = chain->mp;
  ctx->pctx.is_head_request = (method == CHTTP_HEAD);
  ctx->pctx.redirects_still_allowed = (hop < CHTTP_MAX_REDIRECTS);
  /* Streaming (chain->write_fn set) delivers body bytes straight to the
   * caller's callback; buffered uses ctx->bb; see _async_build_response's
   * own comment for why this must be consistent with what it later does. */
  ctx->pctx.requested_sink_fn =
      chain->write_fn ? chain->write_fn : _sink_buffered;
  ctx->pctx.requested_sink_ctx = chain->write_fn ? chain->write_ctx : &ctx->bb;
  ctx->bb.mp = chain->mp;

  char *herr = NULL;
  ctx->pctx.headers =
      chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_string, chain->mp, NULL, &herr);
  if (!ctx->pctx.headers) {
    _url_free(chain->mp, &url);
    _async_submit_hop_fail(ctx, ccol_not_enough_memory);
    return false;
  }

  chttp_request_t hop_req;
  memset(&hop_req, 0, sizeof(hop_req));
  hop_req.method = method;
  hop_req.headers = chain->req_headers;
  hop_req.body.data = body_data;
  hop_req.body.len = body_len;
  hop_req.body.content_type = body_content_type;

  prv = _serialize_request(chain->mp, &hop_req, &url, effective_auth,
                           &ctx->wire, &ctx->wire_len);
  if (prv != ccol_success) {
    _url_free(chain->mp, &url);
    _async_submit_hop_fail(ctx, prv);
    return false;
  }

  if (!ctx->origin_key) {
    /* Always set except on the reused path, where it already carries the
     * origin this connection was pooled under (identical to url.origin_key
     * by construction; _async_idle_pool_take only ever returns a
     * connection filed under the exact origin_key being looked up). */
    ctx->origin_key = ccol_strdup(chain->mp, url.origin_key);
    if (!ctx->origin_key) {
      _url_free(chain->mp, &url);
      _async_submit_hop_fail(ctx, ccol_not_enough_memory);
      return false;
    }
  }

  if (!reused) {
    ctx->is_unix = url.is_unix;
    if (url.is_unix) {
      ctx->unix_socket_path = ccol_strdup(chain->mp, url.unix_socket_path);
      if (!ctx->unix_socket_path) {
        _url_free(chain->mp, &url);
        _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
        _async_ctx_teardown(ctx);
        return false;
      }
    } else {
      ctx->host = ccol_strdup(chain->mp, url.host);
      ctx->port = url.port;
      if (!ctx->host) {
        _url_free(chain->mp, &url);
        _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
        _async_ctx_teardown(ctx);
        return false;
      }
    }
  }
  _url_free(chain->mp, &url);

  call_once(client_http1_settings_bundler.once, _init_chttp1_settings);
  chttp1_parser_init(&ctx->parser, &client_http1_settings_bundler.settings);
  ctx->parser.data = &ctx->pctx;

  if (reused) {
    /* Already connected (and, if HTTPS, already handshaked); skip
     * CONNECTING/TLS_HANDSHAKING entirely. ctx->state was already moved to
     * CHTTP_ASYNC_WRITING (under idle_lock, together with ctx->chain) above,
     * before any of the other per-hop fields below were touched. The
     * registration's direction must be flipped from read (its steady
     * idle-pooled state) to write; the actual write attempt is deliberately
     * NOT made here, on this (non-reactor) calling thread; it is left
     * entirely to _async_on_writable's own dispatch, exactly like a fresh
     * connection's first write already works. This is not just simpler; it
     * is required for correctness: the moment event_loop_modify flips this
     * registration to write interest, an already-running reactor thread is
     * free to dispatch _async_on_writable for it concurrently with this
     * calling thread (event_loop's own guarantee only prevents two
     * dispatches of the same registration from running concurrently with
     * EACH OTHER; it says nothing about a non-callback caller like this
     * function racing a dispatch it just made possible). An earlier version
     * of this function attempted the write here directly, which both raced
     * that concurrent dispatch and separately failed to transition
     * ctx->state/direction to READING on a fully-completed write; a real,
     * reproducible hang in async_idle_pool.sequential_requests_reuse_
     * connection, found via gdb thread backtraces on the hung process
     * rather than assumed from code inspection alone. */
    if (event_loop_modify(cli_engine_bundler.reactor, ctx->reg,
                          ccol_select_write) != ccol_success) {
      _async_retry_hop(ctx);
      mutex_unlock(ctx->idle_lock);
      _async_ctx_teardown(ctx);
      return true;
    }
    /* ctx is fully consistent again (every per-hop field above is already
     * set, and the registration now correctly reflects WRITING), so
     * idle_lock can finally be released; any concurrent dispatch that was
     * blocked waiting for it now proceeds against a coherent ctx, and (since
     * the fd is almost certainly already writable, being freshly reused) may
     * do so essentially immediately. */
    mutex_unlock(ctx->idle_lock);
    /* This ctx's connection now belongs to the chain, not the idle pool;
     * release the engine reference it was holding while pooled (the
     * chain's own single reference, held for its whole lifetime, covers it
     * from here on). No deadline (re-)registration needed here: this ctx
     * was already registered back when it was first created on its very
     * first (fresh) hop attempt, and registration persists across every
     * idle-pool cycle since; see _client_deadline_register's own comment. */
    _client_engine_release();
    return true;
  }

  /* Registered BEFORE submitting, not after: once ctpool_submit hands ctx to
   * a worker, that worker can connect, run the whole hop to completion, and
   * free ctx before this thread would otherwise get a chance to register it
   *; registering first guarantees ctx is already safely in the registry
   * for the entire time any other thread can possibly touch it. */
  _client_deadline_register(ctx);

  ccol_retval_t sr = ctpool_submit(cli_engine_bundler.dns_pool,
                                   _async_connect_task, ctx, NULL);
  if (sr != ccol_success) {
    _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    _async_ctx_teardown(ctx); /* unregisters ctx too, via _async_ctx_free */
    return false;
  }
  return true;
}

/*
 * Called once a hop's response has fully parsed as a redirect (pctx.
 * will_redirect). Resolves the Location header against this hop's URL,
 * applies Tier 1's identical method/body preservation rules (307/308 keep
 * the method and body; any other redirect status downgrades to GET and
 * drops the body unless the current method is already HEAD), finishes this
 * hop's connection (offering it to the idle pool if `reusable`, otherwise
 * closing it; see _async_finish_connection), and only THEN queues the
 * next hop.
 *
 * That order is deliberate and load-bearing, not cosmetic, for the close
 * case specifically: _async_ctx_free's close(fd) call runs synchronously,
 * on THIS thread, before _async_finish_connection returns. The next hop's
 * connect() call runs on a completely different ctpool worker thread and,
 * on a loopback redirect chain that opens and closes a fresh fd every hop
 * in quick succession, can be handed back the EXACT SAME fd number the
 * kernel just freed; but only once that worker thread actually gets to
 * run, which cannot happen until ctpool_submit for the next hop is called.
 * Finishing the connection unconditionally FIRST (so the old fd is fully
 * closed, and its event_loop registration fully removed, before
 * _async_submit_hop ever calls ctpool_submit for the next hop) guarantees
 * there is no window where the new hop's socket() could receive the old
 * fd's number while it might still be registered with the reactor; when the
 * connection is pooled instead, no fd is freed at all, so this ordering
 * costs nothing there either.
 *
 * Whether the handoff to the next hop succeeds or not, this connection's
 * job is done either way; on failure, _async_submit_hop has already
 * fulfilled the future with a specific error (or, for a Location that fails
 * to resolve, this function never creates a next hop at all and the
 * chain's refcount is unaffected); either way, this ctx's own teardown
 * (inside _async_finish_connection) is the one guaranteed to notice, via
 * _async_chain_release's backstop, that nothing fulfilled the future, and
 * do so itself with a generic error.
 */
static void _async_handle_redirect(chttp_async_ctx_t *ctx, bool reusable) {
  chttp_async_chain_t *chain = ctx->chain;
  /* Temporary, extra reference: _async_finish_connection below can trigger
   * (via _async_ctx_finish's teardown, or a successful idle-pool-offer's
   * own explicit release) a full teardown of THIS ctx's chain reference on
   * a DIFFERENT, concurrently-running reactor thread; if that happened to
   * be the last live reference, chain would be freed while this function
   * is still using its local `chain` pointer below (in _async_submit_hop's
   * argument and _mem_free(chain->mp, ...)). This is the same class of "a
   * concurrent teardown can free something a synchronous caller still
   * needs" race documented throughout this section, just one level higher
   * than the ctx-local ones. Retaining here first, and releasing only once
   * this function is completely done with `chain`, guarantees it never
   * hits zero out from under us regardless of how fast a concurrent
   * teardown races. */
  _async_chain_retain(chain);

  chttp_url_t base;
  memset(&base, 0, sizeof(base));
  base.is_https = ctx->is_https;
  base.is_unix = ctx->is_unix;
  base.unix_socket_path = ctx->unix_socket_path;
  base.host = ctx->host;
  base.port = ctx->port;

  char *next_url = _resolve_redirect_url(chain->mp, &base, ctx->pctx.location);
  bool preserve =
      (ctx->pctx.status_code == 307 || ctx->pctx.status_code == 308);
  chttp_method_t next_method = ctx->cur_method;
  const void *next_body_data = chain->body_data;
  size_t next_body_len = chain->body_len;
  const char *next_body_ct = chain->body_content_type;
  if (!preserve && ctx->cur_method != CHTTP_HEAD) {
    next_method = CHTTP_GET;
    next_body_data = NULL;
    next_body_len = 0;
    next_body_ct = NULL;
  }
  /* Captured BEFORE _async_finish_connection, not after: that call can
   * trigger (via a concurrent reactor thread's dispatch) the normal
   * teardown of THIS ctx itself (freeing it), so ctx->hop must not be read
   * afterward. The temporary chain retain above only protects `chain`; it
   * does nothing for ctx, which is never safe to touch once its own
   * connection has been handed to _async_finish_connection. */
  int next_hop = ctx->hop + 1;

  _async_finish_connection(ctx, reusable);

  if (next_url) {
    _async_submit_hop(chain, next_url, next_method, next_body_data,
                      next_body_len, next_body_ct, next_hop);
    _mem_free(chain->mp, next_url);
  }
  _async_chain_release(chain); /* release the temporary ref taken above */
}

/*
 * Shared pre-flight validation for Tier 2 (_chttp_do_async_internal) and
 * Tier 3 (chttpclient_do_pooled/_streaming): validates cli/req, parses
 * req->url into *url_out (caller must _url_free it on ccol_success), and
 * checks TLS usability; the same three checks Tier 1's chttp_do_internal
 * performs, with the exact same result codes (ccol_invalid_args,
 * whatever _parse_chttp_url returns, ccol_http_tls_handshake_failed).
 *
 * Tier 1 is NOT refactored to call this: its equivalent checks are woven
 * into the per-hop loop of chttp_do_internal, re-run fresh on every hop
 * (a redirect can change URL/scheme hop to hop, so there's no single
 * upfront check to extract there the way Tier 2/3 have; they only ever
 * need this once, before a chain/future exists at all). Tier 1 already
 * returns fully specific ccol_retval_t codes natively, so extracting its
 * inline logic would touch already-hardened, already-shipped code for no
 * functional benefit.
 *
 * This exists specifically so Tier 3 does not have to accept
 * chttpclient_do_async/_streaming's collapsed "NULL for any pre-queue
 * failure"; it can call this directly first and return the specific
 * code, matching Tier 1's error granularity for exactly the two failure
 * classes (bad URL, TLS unusable) that can be detected before a request is
 * ever queued.
 */
static ccol_retval_t _chttp_async_preflight_check(chttpcli cli,
                                                  const chttp_request_t *req,
                                                  chttp_url_t *url_out) {
  if (!cli || !req) return ccol_invalid_args;

  ccol_retval_t prv = _parse_chttp_url(cli->m_procs, req->url, url_out);
  if (prv != ccol_success) return prv;

  mutex_lock(cli->lock);
  bool tls_ctx_usable = cli->tls_ctx_usable;
  mutex_unlock(cli->lock);

  if (url_out->is_https && !tls_ctx_usable) {
    /* Configured cert/key/ca path(s) were not readable at set_tls time;
     * that failure was deferred here rather than aborting the process (see
     * _rebuild_tls_ctx_locked). Mirrors Tier 1's identical check and its
     * choice of error code for it. */
    _url_free(cli->m_procs, url_out);
    return ccol_http_tls_handshake_failed;
  }
  return ccol_success;
}

/*
 * Internal entry point: submits req for asynchronous execution against cli,
 * returning a future the caller must eventually pair with exactly one
 * ctpool_future_free (after an optional ctpool_future_get/_done). Returns
 * NULL if the request could not even be queued (bad arguments, invalid URL,
 * TLS unusable, OOM, or the engine failing to start); mirrors
 * ctpool_submit_future's own "NULL on failure" convention. Once a future has
 * been created, every subsequent failure (including one on a later redirect
 * hop) instead fulfils that future with a specific error and still returns
 * it, exactly like Tier 1 returns a ccol_retval_t instead of aborting
 * silently. Every queued hop is unconditionally freed, and the whole chain's
 * one engine reference released, once the redirect chain reaches a terminal
 * connection state (see chttp_async_chain_t's file-level comment).
 */
static ctpool_future *_chttp_do_async_internal(chttpcli cli,
                                               const chttp_request_t *req,
                                               chttpcli_write_fn write_fn,
                                               void *write_ctx) {
  chttp_url_t url;
  if (_chttp_async_preflight_check(cli, req, &url) != ccol_success) return NULL;
  ccol_memmgmt_procs_t *mp = cli->m_procs;
  _url_free(mp, &url); /* only needed for the pre-check above;
                        * _async_submit_hop re-parses req->url itself */

  /* Read and pin the client's TLS context under its lock, exactly like
   * Tier 1's chttp_do_internal does; ctls_ctx_retain pins it against a
   * concurrent chttpclient_set_tls freeing/rebuilding it while this request
   * is still using it. Pinned unconditionally (not just for an https first
   * hop) since a redirect chain can hop between http and https, exactly
   * like Tier 1's own tls_ctx local; the pinned reference is released once
   * by _async_chain_release (via ctls_ctx_release) when the whole chain's
   * last hop is torn down. */
  ctls_ctx_t *tls_ctx;
  bool tls_ctx_usable;
  bool verify_host;
  long connect_timeout_ms;
  long request_timeout_ms;
  mutex_lock(cli->lock);
  tls_ctx = cli->tls_ctx;
  tls_ctx_usable = cli->tls_ctx_usable;
  verify_host = cli->tls.verify_host;
  connect_timeout_ms = cli->connect_timeout_ms;
  request_timeout_ms = cli->request_timeout_ms;
  if (tls_ctx) ctls_ctx_retain(tls_ctx);
  mutex_unlock(cli->lock);

  if (_client_engine_acquire() != ccol_success) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    return NULL;
  }

  char *ferr = NULL;
  ctpool_future *future = ctpool_future_create_detached(&ferr);
  if (!future) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    _client_engine_release();
    return NULL;
  }
  /* From here on a future exists and is always returned to the caller (who
   * owns pairing it with exactly one ctpool_future_free); any subsequent
   * failure fulfils it with a specific error instead of returning NULL,
   * exactly mirroring this function's previous, single-hop behaviour on a
   * ctpool_submit failure. Captured into a local now: once hop 0 is queued
   * below, a worker may run the request to completion and free the chain
   * via _async_chain_release before this function's own thread runs another
   * instruction; reading chain->future afterward would be a
   * use-after-free. */
  ctpool_future *f = future;

  chttp_async_chain_t *chain = _async_chain_create(
      mp, cli, future, (chmap)req->headers, req->body.data, req->body.len,
      req->body.content_type, tls_ctx, tls_ctx_usable, verify_host,
      connect_timeout_ms, request_timeout_ms, write_fn, write_ctx);
  if (!chain) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    ctpool_future_fulfill(future, NULL); /* producer side; caller's own ref
                                          * is released via its eventual
                                          * ctpool_future_free(f) call */
    _client_engine_release();
    return f;
  }
  _async_submit_hop(chain, req->url, req->method, req->body.data, req->body.len,
                    req->body.content_type, 0);
  return f;
}

/* White-box test helpers exposing internal engine state. Not part of the
 * public API; gated so these symbols do not leak into a production build
 * of libccollections.so (the surrounding engine code itself is no longer
 * gated now that chttpclient_do_async/_streaming are real public callers,
 * but these five functions exist purely for test instrumentation). */
#ifdef RUNNING_UNIT_TESTS
int _chttpclient_engine_ref_count_for_tests(void) {
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  int n = (int)cli_engine_bundler.reactor_refs;
  mutex_unlock(cli_engine_bundler.mutex);
  return n;
}

bool _chttpclient_engine_running_for_tests(void) {
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  bool running = (cli_engine_bundler.reactor != NULL);
  mutex_unlock(cli_engine_bundler.mutex);
  return running;
}

ccol_retval_t _chttpclient_engine_acquire_for_tests(void) {
  return _client_engine_acquire();
}

void _chttpclient_engine_release_for_tests(void) { _client_engine_release(); }

void _chttpclient_engine_wait_for_quiescence_for_tests(void) {
  _client_engine_wait_for_quiescence();
}

size_t _chttpclient_engine_num_reactor_threads_for_tests(void) {
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  size_t n = cli_engine_bundler.last_resolved_num_reactor_threads;
  mutex_unlock(cli_engine_bundler.mutex);
  return n;
}

/* Rewrites every currently-pooled Tier 2/3 idle connection's last_used
 * timestamp far enough into the past to make _async_idle_pool_take's own
 * CHTTP_IDLE_MAX_AGE_MS staleness check treat it as aged-out on the very
 * next pop, without a test actually waiting out the real 60-second window.
 * Test-only: exists purely to make the staleness-eviction path in
 * _async_idle_pool_take deterministically reachable. */
void _chttpclient_force_async_idle_stale_for_tests(chttpcli cli) {
  mutex_lock(cli->lock);
  if (cli->idle_pools_async) {
    cmap_iterator *it = chashmap_begin_iter(cli->idle_pools_async, NULL);
    for (; it; it = it->_next_fn(it)) {
      cvec list = _read_cvec(it->val_pair->ptr);
      if (!list) continue;
      size_t n = cvector_elem_count(list);
      for (size_t i = 0; i < n; i++) {
        chttp_async_ctx_t *actx = *(chttp_async_ctx_t **)cvector_at(list, i);
        actx->last_used.tv_sec -= (CHTTP_IDLE_MAX_AGE_MS / 1000L) + 5;
      }
    }
  }
  mutex_unlock(cli->lock);
}

/* Reads cli->idle_total_count_async: how many connections are currently
 * sitting in Tier 2/3's async idle pool, across every origin. Test-only:
 * lets a test verify live pool membership directly instead of inferring it
 * indirectly, needed to state an ordinal-independent invariant ("whenever
 * this pool is empty, the engine's ref count contributed by it must be
 * zero too") that holds regardless of exactly which internal allocation an
 * injected OOM failure happens to land on. */
size_t _chttpclient_async_idle_total_count_for_tests(chttpcli cli) {
  mutex_lock(cli->lock);
  size_t n = cli->idle_total_count_async;
  mutex_unlock(cli->lock);
  return n;
}
#endif /* RUNNING_UNIT_TESTS */

ctpool_future *chttpclient_do_async(chttpcli cli, const chttp_request_t *req) {
  return _chttp_do_async_internal(cli, req, NULL, NULL);
}

ctpool_future *chttpclient_do_async_streaming(chttpcli cli,
                                              const chttp_request_t *req,
                                              chttpcli_write_fn write_fn,
                                              void *write_ctx) {
  if (!write_fn) return NULL;
  return _chttp_do_async_internal(cli, req, write_fn, write_ctx);
}

chttpcli_async_result_t *chttpclient_async_result_get(ctpool_future *f) {
  return (chttpcli_async_result_t *)ctpool_future_get(f);
}

void chttpclient_async_result_free(chttpcli_async_result_t *result) {
  if (!result) return;
  _mem_free(result->_m_procs, result);
}

/* ========================================================================== */
/*                    POOLED-SYNC API (TIER 3)                                */
/* ========================================================================== */

/*
 * Both functions below are thin wrappers: submit via Tier 2, block on the
 * future, unwrap the result into the exact same ccol_retval_t/resp_out (or
 * status_code_out) shape chttpclient_do/chttpclient_do_streaming use, then
 * free the future and its result before returning; the caller never sees
 * ctpool_future or chttpcli_async_result_t at all. _chttp_async_preflight_
 * check runs first specifically so a bad URL or unusable TLS config is
 * reported with the same specific code chttpclient_do would use, rather
 * than chttpclient_do_async/_streaming's collapsed NULL for that case (see
 * that helper's own comment). Any OTHER pre-queue failure (OOM, the engine
 * failing to start) still collapses to ccol_unexpected_failure; Tier 1
 * does not distinguish those cases with any more granularity either.
 */

ccol_retval_t chttpclient_do_pooled(chttpcli cli, const chttp_request_t *req,
                                    chttpcli_response **resp_out) {
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;

  chttp_url_t url;
  ccol_retval_t prv = _chttp_async_preflight_check(cli, req, &url);
  if (prv != ccol_success) return prv;
  _url_free(cli->m_procs, &url);

  ctpool_future *f = chttpclient_do_async(cli, req);
  if (!f) return ccol_unexpected_failure;

  chttpcli_async_result_t *result = chttpclient_async_result_get(f);
  if (!result) {
    ctpool_future_free(f);
    return ccol_unexpected_failure;
  }

  ccol_retval_t rv = result->rv;
  *resp_out = result->resp;
  chttpclient_async_result_free(result);
  ctpool_future_free(f);
  return rv;
}

ccol_retval_t chttpclient_do_pooled_streaming(chttpcli cli,
                                              const chttp_request_t *req,
                                              chttpcli_write_fn write_fn,
                                              void *write_ctx,
                                              int *status_code_out) {
  if (!write_fn) return ccol_invalid_args;

  chttp_url_t url;
  ccol_retval_t prv = _chttp_async_preflight_check(cli, req, &url);
  if (prv != ccol_success) return prv;
  _url_free(cli->m_procs, &url);

  ctpool_future *f =
      chttpclient_do_async_streaming(cli, req, write_fn, write_ctx);
  if (!f) return ccol_unexpected_failure;

  chttpcli_async_result_t *result = chttpclient_async_result_get(f);
  if (!result) {
    ctpool_future_free(f);
    return ccol_unexpected_failure;
  }

  ccol_retval_t rv = result->rv;
  if (result->resp) {
    /* Streaming's chttpcli_response always exists internally (its body/
     * headers just stay NULL; see _async_build_response) purely so
     * status_code has somewhere to travel through the single
     * chttpcli_async_result_t.resp channel; this function's own public
     * contract has no resp_out at all (mirrors chttpclient_do_streaming
     * exactly), so it is unwrapped and freed here, never exposed. */
    if (rv == ccol_success && status_code_out) {
      *status_code_out = result->resp->status_code;
    }
    chttpclient_resp_free(result->resp);
  }
  chttpclient_async_result_free(result);
  ctpool_future_free(f);
  return rv;
}

/* ========================================================================== */
/*                         CLIENT CONSTRUCTORS                                */
/* ========================================================================== */

chttpcli create_chttpclient_mp(ccol_memmgmt_procs_t *mprocs, char **err_str) {
  if (mprocs && !ccol_verify_memmgmt_procs(mprocs, err_str)) return NULL;

  ccol_memmgmt_procs_t *mp = NULL;
  if (mprocs) {
    mp = (ccol_memmgmt_procs_t *)mprocs->malloc(sizeof(ccol_memmgmt_procs_t));
    if (!mp) {
      if (err_str) *err_str = CCOL_ERR_STR("failed to allocate mprocs");
      return NULL;
    }
    mem_cpy(mp, mprocs, sizeof(ccol_memmgmt_procs_t));
  }

  struct chttpclient *cli =
      (struct chttpclient *)_mem_calloc(mp, 1, sizeof(struct chttpclient));
  if (!cli) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate client");
    if (mp) mp->free(mp);
    return NULL;
  }

  cli->m_procs = mp;
  cli->tls = CHTTP_TLS_DEFAULT;
  mutex_init(cli->lock);
  cond_var_init(cli->available);
  cond_var_init(cli->idle_async_drained);
  mutex_init(cli->async_count_lock);
  cond_var_init(cli->async_count_drained);

  char *herr = NULL;
  cli->idle_pools =
      chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_pointer, mp, NULL, &herr);
  if (!cli->idle_pools) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate idle pool map");
    mutex_destroy(cli->lock);
    cond_var_destroy(cli->available);
    cond_var_destroy(cli->idle_async_drained);
    mutex_destroy(cli->async_count_lock);
    cond_var_destroy(cli->async_count_drained);
    _mem_free(mp, cli);
    if (mp) mp->free(mp);
    return NULL;
  }

  cli->idle_pools_async =
      chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_pointer, mp, NULL, &herr);
  if (!cli->idle_pools_async) {
    if (err_str)
      *err_str = CCOL_ERR_STR("failed to allocate async idle pool map");
    __chmap_destroy(cli->idle_pools);
    mutex_destroy(cli->lock);
    cond_var_destroy(cli->available);
    cond_var_destroy(cli->idle_async_drained);
    mutex_destroy(cli->async_count_lock);
    cond_var_destroy(cli->async_count_drained);
    _mem_free(mp, cli);
    if (mp) mp->free(mp);
    return NULL;
  }

  if (_rebuild_tls_ctx_locked(cli) != ccol_success) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to build default TLS context");
    __chmap_destroy(cli->idle_pools);
    __chmap_destroy(cli->idle_pools_async);
    mutex_destroy(cli->lock);
    cond_var_destroy(cli->available);
    cond_var_destroy(cli->idle_async_drained);
    mutex_destroy(cli->async_count_lock);
    cond_var_destroy(cli->async_count_drained);
    _mem_free(mp, cli);
    if (mp) mp->free(mp);
    return NULL;
  }

  return cli;
}

/* ========================================================================== */
/*                         CLIENT CONFIGURATION                               */
/* ========================================================================== */

ccol_retval_t chttpclient_set_pool_size(chttpcli cli, size_t n) {
  if (!cli) return ccol_invalid_args;
  mutex_lock(cli->lock);
  cli->configured_pool_size = n;
  cli->pool_cap = _resolve_pool_cap(n);
  cli->pool_initialized = true;
  cond_var_broadcast(cli->available);
  mutex_unlock(cli->lock);
  return ccol_success;
}

ccol_retval_t chttpclient_set_connect_timeout(chttpcli cli, long ms) {
  if (!cli) return ccol_invalid_args;
  mutex_lock(cli->lock);
  cli->connect_timeout_ms = ms;
  mutex_unlock(cli->lock);
  return ccol_success;
}

ccol_retval_t chttpclient_set_request_timeout(chttpcli cli, long ms) {
  if (!cli) return ccol_invalid_args;
  mutex_lock(cli->lock);
  cli->request_timeout_ms = ms;
  mutex_unlock(cli->lock);
  return ccol_success;
}

ccol_retval_t chttpclient_set_tls(chttpcli cli, const chttp_tls_config_t *tls) {
  if (!cli) return ccol_invalid_args;
  mutex_lock(cli->lock);

  _mem_free(cli->m_procs, cli->owned_cert_path);
  _mem_free(cli->m_procs, cli->owned_key_path);
  _mem_free(cli->m_procs, cli->owned_ca_bundle_path);
  cli->owned_cert_path = cli->owned_key_path = cli->owned_ca_bundle_path = NULL;

  if (!tls) {
    cli->tls = CHTTP_TLS_DEFAULT;
    ccol_retval_t rv = _rebuild_tls_ctx_locked(cli);
    mutex_unlock(cli->lock);
    return rv;
  }

  if (tls->cert_path) {
    cli->owned_cert_path = ccol_strdup(cli->m_procs, tls->cert_path);
    if (!cli->owned_cert_path) goto oom;
  }
  if (tls->key_path) {
    cli->owned_key_path = ccol_strdup(cli->m_procs, tls->key_path);
    if (!cli->owned_key_path) goto oom;
  }
  if (tls->ca_bundle_path) {
    cli->owned_ca_bundle_path = ccol_strdup(cli->m_procs, tls->ca_bundle_path);
    if (!cli->owned_ca_bundle_path) goto oom;
  }

  cli->tls = *tls;
  cli->tls.cert_path = cli->owned_cert_path;
  cli->tls.key_path = cli->owned_key_path;
  cli->tls.ca_bundle_path = cli->owned_ca_bundle_path;

  {
    ccol_retval_t rv = _rebuild_tls_ctx_locked(cli);
    mutex_unlock(cli->lock);
    return rv;
  }

oom:
  _mem_free(cli->m_procs, cli->owned_cert_path);
  _mem_free(cli->m_procs, cli->owned_key_path);
  _mem_free(cli->m_procs, cli->owned_ca_bundle_path);
  cli->owned_cert_path = cli->owned_key_path = cli->owned_ca_bundle_path = NULL;
  cli->tls = CHTTP_TLS_DEFAULT;
  mutex_unlock(cli->lock);
  return ccol_not_enough_memory;
}

/* ========================================================================== */
/*                         CLIENT DESTRUCTION                                 */
/* ========================================================================== */

void __chttpclient_destroy(chttpcli cli) {
  if (!cli) return;

  /* Defensive: if the caller passed the handle chttp_default_client()
   * returns (its own doc comment invites passing it to chttpclient_set_*,
   * and nothing stops a caller from also passing it here), clear the
   * singleton's own copy of this pointer first. Without this,
   * default_client_bundler.client would keep pointing at memory this call
   * is about to free -- both handed straight back out by any later
   * chttp_default_client()/chttp_do()/chttp_get() call in this process (a
   * use-after-free, since default_client_bundler.once never re-fires to
   * rebuild it), and destroyed a second time by this file's own
   * process-exit destructor, an unconditional double-free. A no-op
   * (compare-and-swap fails harmlessly) for any client actually created via
   * create_chttpclient/_mp, which can never equal this singleton's pointer. */
  chttpcli expected = cli;
  atomic_compare_exchange_strong(&default_client_bundler.client, &expected,
                                 NULL);

  mutex_lock(cli->lock);
  cli->destroying = true;
  cond_var_broadcast(cli->available);
  while (cli->in_flight_count > 0) cond_var_wait(cli->available, cli->lock);
  mutex_unlock(cli->lock);

  /* Wait for every ACTIVE (not yet idle-pooled) Tier 2/3 async chain
   * created for this client to finish before touching anything else below:
   * an in-flight chain can still be connecting/handshaking/writing/reading
   * on a reactor or DNS/connect-pool thread, dereferencing chain->cli/
   * ctx->cli (cli->lock, cli->idle_pools_async, cli->m_procs) at essentially
   * any point until it either tears down or joins the idle pool this
   * function drains further below. Without this wait, a caller doing
   * `f = chttpclient_do_async(cli, req); chttpclient_destroy(cli);` would
   * free cli out from under a still-in-flight request; see this field's own
   * comment on struct chttpclient for why a dedicated lock/condvar is used
   * here rather than cli->lock/available. This must run before the idle-pool
   * draining below: an active chain completing while this wait is still in
   * progress is exactly what is expected to feed fresh entries into that
   * pool, which the idle-pool draining logic then cleans up. */
  mutex_lock(cli->async_count_lock);
  while (cli->async_in_flight_count > 0)
    cond_var_wait(cli->async_count_drained, cli->async_count_lock);
  mutex_unlock(cli->async_count_lock);

  if (cli->idle_pools) {
    cmap_iterator *it = chashmap_begin_iter(cli->idle_pools, NULL);
    for (; it; it = it->_next_fn(it)) {
      cvec list = _read_cvec(it->val_pair->ptr);
      if (list) {
        chttp_conn_t c;
        while (cvector_elem_count(list) > 0 &&
               cvector_pop_back(list, &c) == ccol_success)
          _conn_teardown(cli->m_procs, &c);
        __cvector_destroy(list);
      }
    }
    __chmap_destroy(cli->idle_pools);
  }

  /* Tier 2's own idle pool (see the "ASYNC IDLE POOL" section earlier in
   * this file). Shut down every currently pooled connection's fd; each
   * one's own IDLE-state dispatch (_async_idle_ctx_finish) removes it from
   * the pool and frees it (releasing its idle-held engine reference)
   * asynchronously once the reactor observes it; wait for
   * idle_total_count_async to reach zero before proceeding, since cli is
   * about to be freed below and those deferred teardowns read
   * cli->lock/cli->idle_pools_async. shutdown() (rather than closing the fd
   * directly here) is safe to call while still holding cli->lock, exactly
   * like the deadline sweep's identical use of it: it has no synchronous
   * application-level callback of its own, so there is no
   * reentrancy/lock-order hazard in calling it from inside this loop. */
  mutex_lock(cli->lock);
  if (cli->idle_pools_async) {
    cmap_iterator *ait = chashmap_begin_iter(cli->idle_pools_async, NULL);
    for (; ait; ait = ait->_next_fn(ait)) {
      cvec list = _read_cvec(ait->val_pair->ptr);
      if (!list) continue;
      size_t n = cvector_elem_count(list);
      for (size_t i = 0; i < n; i++) {
        chttp_async_ctx_t *actx = *(chttp_async_ctx_t **)cvector_at(list, i);
        int afd = actx->fd;
        if (afd >= 0) shutdown(afd, SHUT_RDWR);
      }
    }
  }
  while (cli->idle_total_count_async > 0)
    cond_var_wait(cli->idle_async_drained, cli->lock);
  mutex_unlock(cli->lock);

  if (cli->idle_pools_async) {
    cmap_iterator *it = chashmap_begin_iter(cli->idle_pools_async, NULL);
    for (; it; it = it->_next_fn(it)) {
      cvec list = _read_cvec(it->val_pair->ptr);
      if (list) __cvector_destroy(list);
    }
    __chmap_destroy(cli->idle_pools_async);
  }

  if (cli->tls_ctx) ctls_ctx_release(cli->tls_ctx);

  mutex_destroy(cli->lock);
  cond_var_destroy(cli->available);
  cond_var_destroy(cli->idle_async_drained);
  mutex_destroy(cli->async_count_lock);
  cond_var_destroy(cli->async_count_drained);

  ccol_memmgmt_procs_t *mp = cli->m_procs;
  _mem_free(mp, cli->owned_cert_path);
  _mem_free(mp, cli->owned_key_path);
  _mem_free(mp, cli->owned_ca_bundle_path);
  _mem_free(mp, cli);
  if (mp) mp->free(mp);
}

/* ========================================================================== */
/*                         REQUEST EXECUTION                                  */
/* ========================================================================== */

/*
 * Sends `wire` (the fully serialized request, `wire_len` bytes, with the
 * body (if any) appended verbatim at the end, exactly `body_len` bytes)
 * and reads the response, honoring chttp_request_t.expect_continue when
 * `use_100_continue` is true (the caller has already confirmed this hop
 * genuinely has a body to hold back: body_carrying_method && body.data &&
 * body.len > 0, matching _serialize_request's own condition for having
 * emitted the "expect: 100-continue" header in the first place).
 *
 * Non-100-continue case: unchanged single-shot send-then-read, exactly what
 * this logic looked like before this function existed.
 *
 * 100-continue case: sends only the header portion first, then waits up to
 * CHTTP_100_CONTINUE_WAIT_MS (bounded by whatever is left of `overall`) for
 * either:
 *   - a "100 Continue" interim response: pctx is reset (a fresh header map,
 *     matching this codebase's "fresh state per message" convention; the
 *     interim response's own headers must never leak into the final one),
 *     the body is sent, and the real final response is read, carrying
 *     forward any bytes a fast/optimistic server already sent past the
 *     interim message's own boundary in the same read (see
 *     _chttp_read_message's own doc comment for why this can happen and why
 *     it must not be silently dropped as garbage);
 *   - the server answering directly without a "100 Continue" at all (RFC
 *     7231 SS5.1.1 explicitly permits this, e.g. to reject a request
 *     without wanting the body); that response IS the final response, and
 *     the body is never sent;
 *   - a timeout: the body is sent anyway and the final response is read
 *     normally, matching curl's own CURLOPT_EXPECT_100_TIMEOUT_MS behavior.
 */
static ccol_retval_t _chttp_send_and_read(
    chttp_conn_t *conn, const char *wire, size_t wire_len, size_t body_len,
    bool use_100_continue, chttp_deadline_t *overall, chttp_parse_ctx_t *pctx,
    bool *keep_alive_out, bool *any_bytes_read_out) {
  if (!use_100_continue) {
    ccol_retval_t prv = _chttp_send_all(conn, wire, wire_len, overall);
    if (prv != ccol_success) return prv;
    return _chttp_read_response(conn, pctx, overall, keep_alive_out,
                                any_bytes_read_out);
  }

  size_t header_len = wire_len - body_len;
  ccol_retval_t prv = _chttp_send_all(conn, wire, header_len, overall);
  if (prv != ccol_success) return prv;

  chttp_deadline_t continue_dl = _deadline_make(CHTTP_100_CONTINUE_WAIT_MS);
  chttp_deadline_t wait_dl = _deadline_earlier(continue_dl, *overall);

  char *leftover = NULL;
  size_t leftover_len = 0;
  prv = _chttp_read_message(conn, pctx, &wait_dl, NULL, 0, keep_alive_out,
                            any_bytes_read_out, &leftover, &leftover_len);
  if (prv == ccol_timed_out) {
    /* No interim response within the wait window; but the abandoned
     * interim parse attempt may still have left partial state on pctx (a
     * status line parsed without ever reaching CHTTP1_PAUSED, in the
     * pathological case of a server splitting even the interim response's
     * own few bytes across multiple slow writes); reset before reusing pctx
     * for the real, unrelated final response, exactly as the "100 Continue
     * seen" branch below already does. */
    ccol_retval_t rrv = _parse_ctx_reset_for_continue(pctx);
    if (rrv != ccol_success) return rrv;
    prv = _chttp_send_all(conn, wire + header_len, body_len, overall);
    if (prv != ccol_success) return prv;
    /* The abandoned interim read above may have already set
     * *any_bytes_read_out true (a partial "100 Con..." fragment arrived
     * before the wait window expired); that must not leak into the
     * unrelated final response read below, or a genuinely failed final read
     * on a reused connection would be mistaken for "some response bytes
     * already handed to the caller" and skip the safe retry-once fallback.
     * Mirrors the identical reset the "100 Continue seen" branch below
     * already does before its own final read. */
    *any_bytes_read_out = false;
    return _chttp_read_response(conn, pctx, overall, keep_alive_out,
                                any_bytes_read_out);
  }
  if (prv != ccol_success) {
    _mem_free(pctx->mp, leftover);
    return prv;
  }

  if (pctx->status_code != 100) {
    /* Server answered directly; this already IS the final response and the
     * body must never be sent. Any bytes past its own boundary are genuine
     * trailing garbage (nothing legitimate can follow a final response on a
     * connection whose body was never sent). */
    if (leftover_len > 0) {
      pctx->trailing_garbage = true;
      *keep_alive_out = false;
    }
    _mem_free(pctx->mp, leftover);
    return ccol_success;
  }

  prv = _parse_ctx_reset_for_continue(pctx);
  if (prv != ccol_success) {
    _mem_free(pctx->mp, leftover);
    return prv;
  }

  prv = _chttp_send_all(conn, wire + header_len, body_len, overall);
  if (prv != ccol_success) {
    _mem_free(pctx->mp, leftover);
    return prv;
  }

  *any_bytes_read_out = false;
  prv = _chttp_read_response_carry(conn, pctx, overall, leftover, leftover_len,
                                   keep_alive_out, any_bytes_read_out);
  _mem_free(pctx->mp, leftover);
  return prv;
}

static ccol_retval_t chttp_do_internal(chttpcli cli, const chttp_request_t *req,
                                       bool streaming,
                                       chttpcli_write_fn user_write_fn,
                                       void *user_write_ctx,
                                       chttpcli_response **resp_out,
                                       int *status_code_out) {
  if (!cli || !req) return ccol_invalid_args;
  if (streaming && !user_write_fn) return ccol_invalid_args;
  if (!streaming && !resp_out) return ccol_invalid_args;

  ccol_retval_t rv = _slot_acquire(cli);
  if (rv != ccol_success) return rv;

  ccol_memmgmt_procs_t *mp = cli->m_procs;

  long connect_timeout_ms, request_timeout_ms;
  chttp_tls_config_t tls_cfg;
  ctls_ctx_t *tls_ctx;
  bool tls_ctx_usable;
  mutex_lock(cli->lock);
  connect_timeout_ms = cli->connect_timeout_ms;
  request_timeout_ms = cli->request_timeout_ms;
  tls_cfg = cli->tls;
  tls_ctx = cli->tls_ctx;
  tls_ctx_usable = cli->tls_ctx_usable;
  if (tls_ctx)
    ctls_ctx_retain(
        tls_ctx); /* pin: a concurrent set_tls must not free this under us */
  mutex_unlock(cli->lock);

  chttp_deadline_t overall_dl = _deadline_make(request_timeout_ms);

  char *cur_url = ccol_strdup(mp, req->url);
  if (!cur_url) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    _slot_release(cli);
    return ccol_not_enough_memory;
  }

  chttp_request_body_t empty_body = CHTTP_NO_BODY;
  chttp_method_t cur_method = req->method;
  chttp_request_body_t cur_body = req->body;

  /* Auto-injected-from-userinfo Authorization, carried across hops as long
   * as the origin (scheme+host+port) doesn't change; dropped permanently
   * (curl's default, non "--location-trusted" behavior) the first time it
   * does, and never re-acquired even if a later hop circles back to the
   * original origin. A caller-supplied Authorization header is completely
   * unaffected by any of this; see _serialize_request's has_auth check. */
  char *carried_auth = NULL;
  char *carried_auth_origin = NULL;

  ccol_retval_t result = ccol_http_too_many_redirects;

  for (int hop = 0; hop <= CHTTP_MAX_REDIRECTS; hop++) {
    chttp_url_t url;
    ccol_retval_t prv = _parse_chttp_url(mp, cur_url, &url);
    if (prv != ccol_success) {
      result = prv;
      break;
    }

    if (url.is_https && !tls_ctx_usable) {
      /* Configured cert/key/ca path(s) were not readable at set_tls time;
       * that failure was deferred here rather than aborting the process
       * (see _rebuild_tls_ctx_locked). */
      _url_free(mp, &url);
      result = ccol_http_tls_handshake_failed;
      break;
    }

    const char *effective_auth = NULL;
    if (url.userinfo_authorization) {
      effective_auth = url.userinfo_authorization;
    } else if (carried_auth &&
               strcmp(carried_auth_origin, url.origin_key) == 0) {
      effective_auth = carried_auth;
    }
    if (url.userinfo_authorization) {
      char *na = ccol_strdup(mp, url.userinfo_authorization);
      char *no = ccol_strdup(mp, url.origin_key);
      if (!na || !no) {
        _mem_free(mp, na);
        _mem_free(mp, no);
        _url_free(mp, &url);
        result = ccol_not_enough_memory;
        break;
      }
      _mem_free(mp, carried_auth);
      _mem_free(mp, carried_auth_origin);
      carried_auth = na;
      carried_auth_origin = no;
    } else if (carried_auth &&
               strcmp(carried_auth_origin, url.origin_key) != 0) {
      _mem_free(mp, carried_auth);
      _mem_free(mp, carried_auth_origin);
      carried_auth = NULL;
      carried_auth_origin = NULL;
    }

    chttp_request_t hop_req = *req;
    hop_req.method = cur_method;
    hop_req.body = cur_body;

    char *wire = NULL;
    size_t wire_len = 0;
    prv = _serialize_request(mp, &hop_req, &url, effective_auth, &wire,
                             &wire_len);
    if (prv != ccol_success) {
      _url_free(mp, &url);
      result = prv;
      break;
    }

    chttp_deadline_t connect_dl = _deadline_make(connect_timeout_ms);

    chttp_conn_t conn;
    bool reused = _idle_pool_take(cli, url.origin_key, &conn);
    if (!reused) {
      prv = _conn_open(mp, &url, url.is_https, tls_ctx, tls_cfg.verify_host,
                       &connect_dl, &overall_dl, &conn);
      if (prv != ccol_success) {
        _mem_free(mp, wire);
        _url_free(mp, &url);
        result = prv;
        break;
      }
    }

    chttp_parse_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.mp = mp;
    pctx.is_head_request = (cur_method == CHTTP_HEAD);
    pctx.redirects_still_allowed = (hop < CHTTP_MAX_REDIRECTS);

    chttp_bodybuf_t bb;
    memset(&bb, 0, sizeof(bb));
    bb.mp = mp;
    pctx.requested_sink_fn = streaming ? user_write_fn : _sink_buffered;
    pctx.requested_sink_ctx = streaming ? user_write_ctx : &bb;

    char *herr = NULL;
    pctx.headers = chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                     ccol_string, ccol_string, mp, NULL, &herr);
    if (!pctx.headers) {
      _conn_teardown(mp, &conn);
      _url_free(mp, &url);
      result = ccol_not_enough_memory;
      break;
    }

    bool body_carrying_method =
        (cur_method == CHTTP_POST || cur_method == CHTTP_PUT ||
         cur_method == CHTTP_PATCH);
    bool use_100_continue = req->expect_continue && body_carrying_method &&
                            cur_body.data && cur_body.len > 0;

    bool keep_alive = false;
    bool any_bytes_read = false;
    prv = _chttp_send_and_read(&conn, wire, wire_len, cur_body.len,
                               use_100_continue, &overall_dl, &pctx,
                               &keep_alive, &any_bytes_read);
    if (prv != ccol_success && reused && !any_bytes_read) {
      /* The reused connection may have died between our liveness probe and
       * this attempt; either the write silently succeeded into the local
       * send buffer before the peer's close became visible, or the read
       * never produced a single byte. Either way nothing has been parsed or
       * handed to the caller yet, so it is safe to retry exactly once
       * against a brand-new connection. */
      _conn_teardown(mp, &conn);
      prv = _conn_open(mp, &url, url.is_https, tls_ctx, tls_cfg.verify_host,
                       &connect_dl, &overall_dl, &conn);
      if (prv == ccol_success) {
        reused = false;
        prv = _chttp_send_and_read(&conn, wire, wire_len, cur_body.len,
                                   use_100_continue, &overall_dl, &pctx,
                                   &keep_alive, &any_bytes_read);
      }
    }
    _mem_free(mp, wire);
    if (prv != ccol_success) {
      _conn_teardown(mp, &conn);
      _parse_ctx_free_fields(&pctx);
      _mem_free(mp, bb.buf);
      _url_free(mp, &url);
      result = prv;
      break;
    }

    bool reusable = keep_alive && !pctx.trailing_garbage;
    if (reusable) {
      _idle_pool_offer(cli, &conn);
    } else {
      _conn_teardown(mp, &conn);
    }

    if (pctx.will_redirect) {
      char *next_url = _resolve_redirect_url(mp, &url, pctx.location);
      bool preserve = (pctx.status_code == 307 || pctx.status_code == 308);
      _parse_ctx_free_fields(&pctx);
      _mem_free(mp, bb.buf);
      _url_free(mp, &url);
      _mem_free(mp, cur_url);
      /* NULLed immediately after freeing (not just reassigned on the
       * success path below): a redirect status with an empty or otherwise
       * unresolvable Location header (RFC 3986 SS5.2/5.3 resolution
       * failure, not just OOM -- _resolve_redirect_url's very first check
       * is `if (!location || !*location) return NULL;`, trivially
       * reachable via a plain server response, no malformed input needed)
       * makes next_url NULL and falls through to the post-loop cleanup's
       * own _mem_free(mp, cur_url) below with this pointer still holding
       * the just-freed value; a real, remotely-triggerable double-free
       * caught by clang's static analyzer, not by any dynamic test (no
       * existing mock route sends a redirect with an empty Location). */
      cur_url = NULL;

      if (!next_url) {
        result = ccol_http_transfer_aborted;
        break;
      }
      cur_url = next_url;
      if (!preserve && cur_method != CHTTP_HEAD) {
        cur_method = CHTTP_GET;
        cur_body = empty_body;
      }
      continue;
    }

    /* Final hop. */
    if (streaming) {
      if (status_code_out) *status_code_out = pctx.status_code;
      _parse_ctx_free_fields(&pctx);
      _url_free(mp, &url);
      result = ccol_success;
      break;
    }

    chttpcli_response *resp =
        (chttpcli_response *)_mem_calloc(mp, 1, sizeof(chttpcli_response));
    if (!resp) {
      _parse_ctx_free_fields(&pctx);
      _mem_free(mp, bb.buf);
      _url_free(mp, &url);
      result = ccol_not_enough_memory;
      break;
    }
    resp->_m_procs = mp;
    resp->status_code = pctx.status_code;
    resp->body = bb.buf;
    resp->body_len = bb.len;
    resp->headers = pctx.headers;
    pctx.headers = NULL; /* ownership transferred to resp */
    _parse_ctx_free_fields(&pctx);
    _url_free(mp, &url);
    *resp_out = resp;
    result = ccol_success;
    break;
  }

  _mem_free(mp, cur_url);
  _mem_free(mp, carried_auth);
  _mem_free(mp, carried_auth_origin);
  if (tls_ctx) ctls_ctx_release(tls_ctx);
  _slot_release(cli);
  return result;
}

ccol_retval_t chttpclient_do(chttpcli cli, const chttp_request_t *req,
                             chttpcli_response **resp_out) {
  return chttp_do_internal(cli, req, false, NULL, NULL, resp_out, NULL);
}

ccol_retval_t chttpclient_do_streaming(chttpcli cli, const chttp_request_t *req,
                                       chttpcli_write_fn write_fn,
                                       void *write_ctx, int *status_code_out) {
  return chttp_do_internal(cli, req, true, write_fn, write_ctx, NULL,
                           status_code_out);
}

/* ========================================================================== */
/*                         DEFAULT CLIENT                                     */
/* ========================================================================== */

static void _init_default_client(void) {
  atomic_store(&default_client_bundler.client, create_chttpclient(NULL));
}

chttpcli chttp_default_client(void) {
  call_once(default_client_bundler.once, _init_default_client);
  return atomic_load(&default_client_bundler.client);
}

__attribute__((destructor)) static void _cleanup_default_client(void) {
  /* Clear first, then destroy: __chttpclient_destroy's own defensive
   * compare-and-swap (see its doc comment) would otherwise race this
   * function's own read of the pointer in the vanishingly unlikely case
   * another thread is concurrently destroying the same handle at process
   * exit; clearing here first makes that CAS in __chttpclient_destroy a
   * guaranteed no-op instead of a second racing writer. */
  chttpcli cli = atomic_exchange(&default_client_bundler.client, NULL);
  if (cli) __chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                         CONVENIENCE API                                    */
/* ========================================================================== */

ccol_retval_t chttp_do(const chttp_request_t *req,
                       chttpcli_response **resp_out) {
  chttpcli cli = chttp_default_client();
  if (!cli) return ccol_unexpected_failure;
  return chttpclient_do(cli, req, resp_out);
}

ccol_retval_t chttp_run_query(chttp_method_t method, const char *url,
                              const chttp_request_body_t *body, chmap headers,
                              chttpcli_response **resp_out) {
  if (!url || !resp_out) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_new(method, url, body, NULL);
  if (!req) return ccol_not_enough_memory;

  req->headers = headers;
  ccol_retval_t rv = chttp_do(req, resp_out);
  req->headers =
      NULL; /* headers not owned by req; do not let free destroy it */
  chttp_request_free(req);
  return rv;
}

ccol_retval_t chttp_get(const char *url, chttpcli_response **resp_out) {
  if (!url || !resp_out) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  if (!req) return ccol_not_enough_memory;
  ccol_retval_t rv = chttp_do(req, resp_out);
  chttp_request_free(req);
  return rv;
}

ccol_retval_t chttp_post(const char *url, const chttp_request_body_t *body,
                         chttpcli_response **resp_out) {
  if (!url || !resp_out) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, body, NULL);
  if (!req) return ccol_not_enough_memory;
  ccol_retval_t rv = chttp_do(req, resp_out);
  chttp_request_free(req);
  return rv;
}

ccol_retval_t chttp_put(const char *url, const chttp_request_body_t *body,
                        chttpcli_response **resp_out) {
  if (!url || !resp_out) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_new(CHTTP_PUT, url, body, NULL);
  if (!req) return ccol_not_enough_memory;
  ccol_retval_t rv = chttp_do(req, resp_out);
  chttp_request_free(req);
  return rv;
}

ccol_retval_t chttp_delete(const char *url, chttpcli_response **resp_out) {
  if (!url || !resp_out) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_new(CHTTP_DELETE, url, NULL, NULL);
  if (!req) return ccol_not_enough_memory;
  ccol_retval_t rv = chttp_do(req, resp_out);
  chttp_request_free(req);
  return rv;
}

ccol_retval_t chttp_patch(const char *url, const chttp_request_body_t *body,
                          chttpcli_response **resp_out) {
  if (!url || !resp_out) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_new(CHTTP_PATCH, url, body, NULL);
  if (!req) return ccol_not_enough_memory;
  ccol_retval_t rv = chttp_do(req, resp_out);
  chttp_request_free(req);
  return rv;
}

/* ========================================================================== */
/*                         RESPONSE API                                       */
/* ========================================================================== */

const char *chttpclient_resp_header(const chttpcli_response *resp,
                                    const char *name) {
  if (!resp || !name || !resp->headers) return NULL;

  size_t nlen = strlen(name);
  char *lower = (char *)_mem_alloc(resp->_m_procs, nlen + 1);
  if (!lower) return NULL;
  for (size_t i = 0; i <= nlen; i++)
    lower[i] = (char)tolower((unsigned char)name[i]);

  cmap_pair kp = {.ptr = lower, .size = nlen + 1};
  cmap_pair *vp = NULL;
  ccol_retval_t rv = chmap_get_elem_ref((chmap)resp->headers, &kp, &vp);
  _mem_free(resp->_m_procs, lower);

  if (rv != ccol_success || !vp) return NULL;
  return (const char *)vp->ptr;
}

void chttpclient_resp_free(chttpcli_response *resp) {
  if (!resp) return;
  ccol_memmgmt_procs_t *mp = resp->_m_procs;
  _mem_free(mp, resp->body);
  if (resp->headers) __chmap_destroy((chmap)resp->headers);
  _mem_free(mp, resp);
  /* mp is NOT freed: it is owned by the client, not the response. */
}
