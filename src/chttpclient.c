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

#include <cfio_engine.h>
#include <chashmap.h>
#include <chttpclient.h>
#include <cthreadpool.h>
#include <ctype.h>
#include <cvector.h>
#include <errno.h>
#include <fio.h>
#include <fio_tls.h>
#include <limits.h>
#include <llhttp.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ========================================================================== */
/*                         CONSTANTS                                          */
/* ========================================================================== */

#define CHTTP_MAX_REDIRECTS 50
#define CHTTP_MAX_IDLE_PER_ORIGIN 4
#define CHTTP_MAX_IDLE_TOTAL 32
#define CHTTP_IDLE_MAX_AGE_MS 60000L

/* ========================================================================== */
/*                         INTERNAL TYPES                                     */
/* ========================================================================== */

/* Parsed request-target. All fields are owned copies (mp-allocated). */
typedef struct {
  bool is_https;
  char *host;
  uint16_t port;
  char *path_and_query;
  char
      *origin_key; /* "scheme://host:port"; used for SNI and idle-pool keying */
} chttp_url_t;

/* Absolute-time deadline helper; `active == false` means "no limit". */
typedef struct {
  bool active;
  struct timespec deadline;
} chttp_deadline_t;

/* A single (possibly pooled) connection. Always stack/value-resident -- never
 * individually heap-allocated -- so it can be stored by value in the idle
 * pool's cvector without an extra allocation layer. */
typedef struct {
  int fd;
  fio_tls_connection_s *tls; /* NULL for plain HTTP */
  char *origin_key;          /* owned copy, matches chttp_url_t.origin_key */
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
 * headers never leak into the final response" fall out naturally -- there is
 * no shared, reset-in-place header map to leak from. */
typedef struct {
  ccol_memmgmt_procs_t *mp;
  chmap headers; /* chmap(char* -> char*); owned until transferred/destroyed */

  char *cur_field;
  size_t cur_field_len, cur_field_cap;
  char *cur_value;
  size_t cur_value_len, cur_value_cap;

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
   * attached to the shared async reactor) -- see the "ASYNC IDLE POOL"
   * section further down for the full design. idle_async_drained is
   * broadcast whenever idle_total_count_async reaches 0, so
   * __chttpclient_destroy can wait for any in-flight idle-connection
   * teardowns (triggered by its own drain, or already in progress from a
   * natural death) to actually finish before freeing this struct out from
   * under them. */
  chmap idle_pools_async;
  size_t idle_total_count_async;
  cond_var_t idle_async_drained;

  long connect_timeout_ms;
  long request_timeout_ms;
  chttp_tls_config_t tls;
  char *owned_cert_path;
  char *owned_key_path;
  char *owned_ca_bundle_path;
  fio_tls_s *tls_ctx;  /* rebuilt whenever chttpclient_set_tls is called */
  bool tls_ctx_usable; /* false if the configured cert/key/ca paths are not
                          readable -- see _rebuild_tls_ctx_locked */

  ccol_memmgmt_procs_t *m_procs;
};

/* ========================================================================== */
/*                         GLOBAL STATE                                       */
/* ========================================================================== */

static chttpcli g_default_client = NULL;
static pthread_once_t g_default_client_once = PTHREAD_ONCE_INIT;

static llhttp_settings_t g_llhttp_settings;
static pthread_once_t g_llhttp_settings_once = PTHREAD_ONCE_INIT;

/* ========================================================================== */
/*                         URL PARSING                                        */
/* ========================================================================== */

static void _url_free(ccol_memmgmt_procs_t *mp, chttp_url_t *u) {
  if (!u) return;
  _mem_free(mp, u->host);
  _mem_free(mp, u->path_and_query);
  _mem_free(mp, u->origin_key);
  memset(u, 0, sizeof(*u));
}

/*
 * Deliberately scoped-down URL parser: recognises "http://"/"https://" only,
 * no userinfo ("user:pass@host"), no fragment handling (fragments are never
 * sent to a server so they are simply left attached to the query if present).
 * This mirrors the surface this module has ever actually needed -- it is not
 * meant to be a general-purpose URL parser.
 */
static ccol_retval_t _parse_chttp_url(ccol_memmgmt_procs_t *mp, const char *url,
                                      chttp_url_t *out) {
  memset(out, 0, sizeof(*out));
  if (!url) return ccol_http_invalid_url;

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

  const char *host_start = p;
  const char *q = p;
  while (*q && *q != ':' && *q != '/' && *q != '?') q++;
  size_t host_len = (size_t)(q - host_start);
  if (host_len == 0) return ccol_http_invalid_url;

  uint16_t port = https ? 443 : 80;
  if (*q == ':') {
    q++;
    unsigned long pv = 0;
    size_t pd = 0;
    while (*q && isdigit((unsigned char)*q)) {
      pv = pv * 10 + (unsigned long)(*q - '0');
      if (pv > 65535) return ccol_http_invalid_url;
      q++;
      pd++;
    }
    if (pd == 0 || pv == 0) return ccol_http_invalid_url;
    port = (uint16_t)pv;
  }

  const char *pq = *q ? q : "/";

  char *host = (char *)_mem_alloc(mp, host_len + 1);
  if (!host) return ccol_not_enough_memory;
  memcpy(host, host_start, host_len);
  host[host_len] = '\0';

  char *path_and_query;
  if (*pq == '/') {
    size_t pq_len = strlen(pq);
    path_and_query = (char *)_mem_alloc(mp, pq_len + 1);
    if (!path_and_query) {
      _mem_free(mp, host);
      return ccol_not_enough_memory;
    }
    memcpy(path_and_query, pq, pq_len + 1);
  } else {
    /* pq starts with '?' (query with no path component); synthesise '/'. */
    size_t pq_len = strlen(pq);
    path_and_query = (char *)_mem_alloc(mp, pq_len + 2);
    if (!path_and_query) {
      _mem_free(mp, host);
      return ccol_not_enough_memory;
    }
    path_and_query[0] = '/';
    memcpy(path_and_query + 1, pq, pq_len + 1);
  }

  int needed = snprintf(NULL, 0, "%s://%s:%u", https ? "https" : "http", host,
                        (unsigned)port);
  if (needed < 0) {
    _mem_free(mp, host);
    _mem_free(mp, path_and_query);
    return ccol_unexpected_failure;
  }
  char *origin_key = (char *)_mem_alloc(mp, (size_t)needed + 1);
  if (!origin_key) {
    _mem_free(mp, host);
    _mem_free(mp, path_and_query);
    return ccol_not_enough_memory;
  }
  snprintf(origin_key, (size_t)needed + 1, "%s://%s:%u",
           https ? "https" : "http", host, (unsigned)port);

  out->is_https = https;
  out->host = host;
  out->port = port;
  out->path_and_query = path_and_query;
  out->origin_key = origin_key;
  return ccol_success;
}

/*
 * Resolves a Location header against the current hop's URL. Supports
 * absolute URLs and root-relative paths ("/foo") only -- general relative
 * resolution (e.g. "../x") is a deliberate, documented scope reduction; no
 * test or known caller requires it, and it is a large surface to replicate
 * faithfully. Returns NULL (caller treats as ccol_http_transfer_aborted) for
 * anything else.
 */
static char *_resolve_redirect_url(ccol_memmgmt_procs_t *mp,
                                   const chttp_url_t *base,
                                   const char *location) {
  if (!location || !*location) return NULL;
  if (strncasecmp(location, "http://", 7) == 0 ||
      strncasecmp(location, "https://", 8) == 0) {
    return ccol_strdup(mp, location);
  }
  if (location[0] != '/') return NULL;

  bool default_port = (base->is_https && base->port == 443) ||
                      (!base->is_https && base->port == 80);
  const char *scheme = base->is_https ? "https" : "http";
  int needed =
      default_port
          ? snprintf(NULL, 0, "%s://%s%s", scheme, base->host, location)
          : snprintf(NULL, 0, "%s://%s:%u%s", scheme, base->host,
                     (unsigned)base->port, location);
  if (needed < 0) return NULL;
  char *out = (char *)_mem_alloc(mp, (size_t)needed + 1);
  if (!out) return NULL;
  if (default_port) {
    snprintf(out, (size_t)needed + 1, "%s://%s%s", scheme, base->host,
             location);
  } else {
    snprintf(out, (size_t)needed + 1, "%s://%s:%u%s", scheme, base->host,
             (unsigned)base->port, location);
  }
  return out;
}

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
  if (c->tls) return fio_tls_connection_read(c->tls, buf, len);
  ssize_t n;
  do {
    n = recv(c->fd, buf, len, 0);
  } while (n < 0 && errno == EINTR);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) errno = EWOULDBLOCK;
  return n;
}

static ssize_t _conn_write(chttp_conn_t *c, const void *buf, size_t len) {
  if (c->tls) return fio_tls_connection_write(c->tls, buf, len);
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

    *fd_out = fd;
    result = ccol_success;
    break;
  }

  freeaddrinfo(res);
  return result;
}

/* Drives the client-mode TLS handshake to completion, honouring both the
 * connect and overall deadlines (TLS handshake time is folded into the
 * connect timeout, matching curl's own behaviour). */
static ccol_retval_t _tls_handshake(chttp_conn_t *conn,
                                    chttp_deadline_t *connect_dl,
                                    chttp_deadline_t *overall) {
  for (;;) {
    fio_tls_handshake_result_e r = fio_tls_client_handshake_step(conn->tls);
    if (r == FIO_TLS_HANDSHAKE_DONE) return ccol_success;
    if (r == FIO_TLS_HANDSHAKE_ERROR) {
      /* 0 == X509_V_OK by OpenSSL convention; fio_tls.h intentionally does not
       * expose OpenSSL headers to callers, so the raw value is compared
       * directly rather than via the X509_V_OK symbol. */
      long vr = fio_tls_connection_verify_result(conn->tls);
      return (vr != 0) ? ccol_http_tls_cert_verification_failed
                       : ccol_http_tls_handshake_failed;
    }

    short ev = (r == FIO_TLS_HANDSHAKE_WANT_WRITE) ? POLLOUT : POLLIN;
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
                                fio_tls_s *tls_ctx, bool verify_host,
                                chttp_deadline_t *connect_dl,
                                chttp_deadline_t *overall, chttp_conn_t *out) {
  memset(out, 0, sizeof(*out));
  out->fd = -1;

  int fd = -1;
  ccol_retval_t rv =
      _tcp_connect(url->host, url->port, connect_dl, overall, &fd);
  if (rv != ccol_success) return rv;
  out->fd = fd;

  if (want_tls) {
    out->tls =
        fio_tls_connect_create(tls_ctx, fd, url->host, verify_host ? 1 : 0);
    if (!out->tls) {
      close(fd);
      out->fd = -1;
      return ccol_not_enough_memory;
    }
    rv = _tls_handshake(out, connect_dl, overall);
    if (rv != ccol_success) {
      fio_tls_connection_destroy(out->tls);
      close(fd);
      out->fd = -1;
      out->tls = NULL;
      return rv;
    }
  }

  out->origin_key = ccol_strdup(mp, url->origin_key);
  if (!out->origin_key) {
    if (out->tls) fio_tls_connection_destroy(out->tls);
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
  if (c->tls) fio_tls_connection_destroy(c->tls);
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
 * destroyed), the connection is simply closed instead -- a lost optimisation
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
                                        const chttp_url_t *url, char **out_buf,
                                        size_t *out_len) {
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

  if (!has_host) {
    bool default_port = (url->is_https && url->port == 443) ||
                        (!url->is_https && url->port == 80);
    _ob_append_cstr(&ob, "host: ");
    _ob_append_cstr(&ob, url->host);
    if (!default_port) {
      char portbuf[16];
      int pn = snprintf(portbuf, sizeof(portbuf), ":%u", (unsigned)url->port);
      if (pn > 0) _ob_append(&ob, portbuf, (size_t)pn);
    }
    _ob_append(&ob, "\r\n", 2);
  }
  if (!has_accept) _ob_append_cstr(&ob, "accept: */*\r\n");
  if (!has_ua)
    _ob_append_cstr(&ob, "user-agent: c_collections-chttpclient/1.0\r\n");

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
/*                         LLHTTP INTEGRATION                                 */
/* ========================================================================== */

static bool _accum_append(ccol_memmgmt_procs_t *mp, char **buf, size_t *len,
                          size_t *cap, const char *data, size_t n) {
  if (*len + n + 1 > *cap) {
    size_t nc = *cap ? *cap * 2 : 64;
    while (nc < *len + n + 1) nc *= 2;
    char *nb = (char *)_mem_realloc(mp, *buf, nc);
    if (!nb) return false;
    *buf = nb;
    *cap = nc;
  }
  memcpy(*buf + *len, data, n);
  *len += n;
  (*buf)[*len] = '\0';
  return true;
}

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

static int _on_header_field(llhttp_t *p, const char *at, size_t len) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  if (!_accum_append(ctx->mp, &ctx->cur_field, &ctx->cur_field_len,
                     &ctx->cur_field_cap, at, len)) {
    ctx->error = true;
    return HPE_USER;
  }
  return 0;
}

static int _on_header_value(llhttp_t *p, const char *at, size_t len) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  if (!_accum_append(ctx->mp, &ctx->cur_value, &ctx->cur_value_len,
                     &ctx->cur_value_cap, at, len)) {
    ctx->error = true;
    return HPE_USER;
  }
  return 0;
}

static int _on_header_value_complete(llhttp_t *p) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  if (ctx->cur_field_len == 0) return 0;

  for (size_t i = 0; i < ctx->cur_field_len; i++)
    ctx->cur_field[i] = (char)tolower((unsigned char)ctx->cur_field[i]);

  cmap_pair kp = {.ptr = ctx->cur_field, .size = ctx->cur_field_len + 1};
  cmap_pair vp = {.ptr = ctx->cur_value ? ctx->cur_value : (void *)"",
                  .size = (ctx->cur_value ? ctx->cur_value_len : 0) + 1};
  ccol_retval_t rv = chmap_insert_elem(ctx->headers, &kp, &vp);
  if (rv != ccol_success && rv != ccol_key_already_present) {
    ctx->error = true;
    return HPE_USER;
  }

  ctx->cur_field_len = 0;
  ctx->cur_value_len = 0;
  if (ctx->cur_field) ctx->cur_field[0] = '\0';
  if (ctx->cur_value) ctx->cur_value[0] = '\0';
  return 0;
}

static int _on_headers_complete(llhttp_t *p) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  ctx->status_code = (int)p->status_code;

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
        return HPE_USER;
      }
      ctx->will_redirect = true;
    }
  }

  ctx->sink_fn = ctx->will_redirect ? _sink_discard : ctx->requested_sink_fn;
  ctx->sink_ctx = ctx->will_redirect ? NULL : ctx->requested_sink_ctx;

  /* A HEAD response's Content-Length (if any) describes a body that was
   * never sent -- this is the one case llhttp cannot infer from the wire. */
  return ctx->is_head_request ? 1 : 0;
}

static int _on_body(llhttp_t *p, const char *at, size_t len) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  size_t n = ctx->sink_fn ? ctx->sink_fn(at, len, ctx->sink_ctx) : len;
  if (n != len) {
    ctx->aborted = true;
    return HPE_USER;
  }
  return 0;
}

static int _on_message_complete(llhttp_t *p) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  ctx->message_complete = true;
  /* Pause so the caller can find out exactly how many bytes of the last read
   * chunk belonged to this message (llhttp_get_error_pos), rather than
   * silently continuing to parse a hypothetical next pipelined message that
   * this client never solicited. */
  return HPE_PAUSED;
}

static void _init_llhttp_settings(void) {
  llhttp_settings_init(&g_llhttp_settings);
  g_llhttp_settings.on_header_field = _on_header_field;
  g_llhttp_settings.on_header_value = _on_header_value;
  g_llhttp_settings.on_header_value_complete = _on_header_value_complete;
  g_llhttp_settings.on_headers_complete = _on_headers_complete;
  g_llhttp_settings.on_body = _on_body;
  g_llhttp_settings.on_message_complete = _on_message_complete;
}

static void _parse_ctx_free_fields(chttp_parse_ctx_t *ctx) {
  _mem_free(ctx->mp, ctx->cur_field);
  _mem_free(ctx->mp, ctx->cur_value);
  _mem_free(ctx->mp, ctx->location);
  if (ctx->headers) __chmap_destroy(ctx->headers);
  ctx->cur_field = ctx->cur_value = ctx->location = NULL;
  ctx->headers = NULL;
}

/*
 * Reads and parses exactly one HTTP/1.1 response from `conn`. On
 * ccol_success, *keep_alive_out reflects whether the connection may be
 * reused for a subsequent request (llhttp's own bookkeeping, further gated
 * by "no trailing garbage after the message boundary").
 *
 * *any_bytes_read_out is set to true the moment the first byte of the
 * response is actually received off the wire. Callers use this to decide
 * whether a failure is safe to silently retry against a fresh connection
 * (nothing has been parsed or handed to the caller yet) versus one that
 * must be surfaced (partial response already in flight, possibly already
 * streamed out to a user callback).
 */
static ccol_retval_t _chttp_read_response(chttp_conn_t *conn,
                                          chttp_parse_ctx_t *pctx,
                                          chttp_deadline_t *overall,
                                          bool *keep_alive_out,
                                          bool *any_bytes_read_out) {
  pthread_once(&g_llhttp_settings_once, _init_llhttp_settings);

  llhttp_t parser;
  llhttp_init(&parser, HTTP_RESPONSE, &g_llhttp_settings);
  parser.data = pctx;

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
      llhttp_errno_t fe = llhttp_finish(&parser);
      if (fe != HPE_OK || !pctx->message_complete)
        return ccol_http_transfer_aborted;
      *keep_alive_out = false; /* peer closed; nothing left to reuse */
      return ccol_success;
    }

    llhttp_errno_t err = llhttp_execute(&parser, buf, (size_t)n);
    if (err == HPE_PAUSED) {
      const char *pos = llhttp_get_error_pos(&parser);
      size_t consumed = (size_t)(pos - buf);
      if (consumed < (size_t)n) pctx->trailing_garbage = true;
      *keep_alive_out =
          llhttp_should_keep_alive(&parser) && !pctx->trailing_garbage;
      return ccol_success;
    }
    if (err == HPE_USER) {
      return pctx->error ? ccol_not_enough_memory : ccol_http_transfer_aborted;
    }
    if (err != HPE_OK) return ccol_http_transfer_aborted;
    /* else: message not yet complete, need more data */
  }
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
 * fio_tls_new(NULL, cert_path, key_path, NULL) mirrors chttpserver.c's own
 * calling convention exactly (2nd arg = certificate file, 3rd = private key
 * file) -- this is the proven-working order, verified end-to-end against a
 * real TLS handshake.
 *
 * IMPORTANT: facio's fio_tls_cert_add/fio_tls_trust call FIO_LOG_FATAL+exit()
 * on a missing/unreadable file -- a hard process abort, not a recoverable
 * error (this is why tests/chttpserver_tls is a separate, isolated binary).
 * chttpclient_set_tls must be able to accept a syntactically valid but
 * currently-nonexistent path without crashing the process (callers may
 * configure TLS well before ever making an HTTPS request, or never make one
 * at all) -- exactly like this module's own regression test expects. So the
 * configured paths are validated with access() BEFORE ever calling into
 * facio; if they don't check out, no context is built here and the failure
 * is deferred to actual connection time (see chttp_do_internal), where it
 * surfaces as a normal ccol_retval_t instead of aborting the process.
 */
static ccol_retval_t _rebuild_tls_ctx_locked(struct chttpclient *cli) {
  if (cli->tls_ctx) {
    fio_tls_destroy(cli->tls_ctx);
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

  fio_tls_s *ctx = fio_tls_new(NULL, have_cert_pair ? cli->tls.cert_path : NULL,
                               have_cert_pair ? cli->tls.key_path : NULL, NULL);
  if (!ctx) return ccol_not_enough_memory;

  if (cli->tls.ca_bundle_path) {
    fio_tls_trust(ctx, cli->tls.ca_bundle_path);
  } else if (cli->tls.verify_peer) {
    fio_tls_trust_system(ctx);
  }

  cli->tls_ctx = ctx;
  cli->tls_ctx_usable = true;
  return ccol_success;
}

/* ========================================================================== */
/*                         ASYNC ENGINE (TIER 2/3)                            */
/* ========================================================================== */

/*
 * chttpclient's async engine (chttpclient_do_async and the pooled-sync
 * wrappers built on top of it) is driven by a small, fixed-size pool of
 * facio reactor threads. The raw reactor itself (fio_start/fio_stop and the
 * one-time facio global init) is NOT owned by this module: facio's fio_data
 * is a process-wide singleton (see fio.c) -- there can only be one
 * fio_start() reactor running per process -- so that lifecycle is owned by
 * the shared src/cfio_engine.c module and acquired/released via
 * _cfio_engine_acquire()/_cfio_engine_release(), the exact same calls
 * chttpserver.c's chttpsvr_start()/__chttpsvr_destroy() make. This is what
 * allows chttpserver and chttpclient's async engine (Tier 2/3) to run
 * simultaneously in the same process -- previously a hard, documented
 * limitation (only one of the two could ever be running at a time), lifted
 * by this shared module.
 *
 * What remains here, under g_client_async_mutex, is purely chttpclient's own
 * bookkeeping layered on top of that shared reference: g_client_async_users
 * (how many outstanding Tier 2/3 callers currently need the engine) and this
 * module's own auxiliary resources that have nothing to do with chttpserver
 * -- g_client_dns_pool (offloads fio_socket()'s synchronous DNS resolution
 * off of caller threads) and the reactor-owned deadline sweep
 * (connect_timeout_ms/request_timeout_ms enforcement). Exactly one
 * _cfio_engine_acquire()/_cfio_engine_release() pair is made per
 * g_client_async_users 0->1/1->0 transition, regardless of how many
 * individual Tier 2/3 requests are in flight -- chttpclient is just one of
 * (up to) two users of the shared reactor, not a re-implementation of its
 * ref-counting.
 */
/* Offloads fio_socket()'s synchronous DNS resolution off of caller threads.
 * Lives and dies with this module's own async-user tracking (created/
 * destroyed alongside it, guarded by g_client_async_mutex) rather than being
 * a separate lazy singleton, since nothing needs it once no Tier 2/3 caller
 * is outstanding. */
static ctpool g_client_dns_pool = NULL;
static pthread_mutex_t g_client_async_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Signalled once this module's own reaper thread (see
 * _client_engine_reaper_fn) finishes tearing its resources down
 * (g_client_async_stopping -> false). */
static pthread_cond_t g_client_async_stopped_cv = PTHREAD_COND_INITIALIZER;
/* True while g_client_dns_pool/the deadline sweep are up (i.e. this module
 * currently holds its one shared-engine reference). */
static bool g_client_async_running = false;
/* True from the moment the last user reference is released until this
 * module's own reaper thread has fully finished tearing its resources down
 * (released the shared engine reference, joined the deadline sweep,
 * destroyed g_client_dns_pool). While true, a concurrent acquire must wait
 * rather than starting fresh or touching the dying instance's resources --
 * see _client_engine_acquire's wait loop. */
static bool g_client_async_stopping = false;
/* Number of chttpcli instances (or, more precisely, outstanding Tier 2/3
 * users) currently relying on this module's async resources. Torn down when
 * this reaches 0. */
static int g_client_async_users = 0;
/* Handle of the most recently spawned reaper thread, and whether it still
 * needs joining. Not detached -- see
 * _client_engine_join_reaper_if_needed_locked's comment for why an explicit
 * join, not just its stopped_cv broadcast, is required for a caller to be
 * able to rely on this module's teardown having fully completed at the OS
 * level (mirrors src/cfio_engine.c's identical reaper-joining pattern). */
static pthread_t g_client_reaper_thread;
static bool g_client_reaper_joinable = false;

/* Forward declaration: defined below (near _client_engine_wait_for_
 * quiescence, which it also calls), but must be registered via atexit only
 * after this module's own first-ever successful _cfio_engine_acquire() call
 * -- see _client_register_atexit_safety_net's own comment for why this
 * exact ordering matters. */
static void _client_engine_atexit_safety_net(void);
static pthread_once_t _client_atexit_once = PTHREAD_ONCE_INIT;
static void _client_register_atexit_safety_net(void) {
  atexit(_client_engine_atexit_safety_net);
}
/* Forward declarations: the reactor-owned deadline sweep (connect_timeout_ms/
 * request_timeout_ms enforcement for Tier 2/3) is defined later, in the
 * "ASYNC DEADLINE SWEEP" section after chttp_async_ctx_t/chttp_async_chain_t
 * are declared -- the sweep needs to inspect those types. It must still be
 * started/stopped in lockstep with this module's own async-user tracking
 * here. */
static ccol_retval_t _client_deadline_sweep_start(void);
static void _client_deadline_sweep_stop_and_join(void);

/*
 * Runs on a freshly spawned thread (never inline on the calling thread --
 * see _client_engine_release's comment for why that distinction is
 * essential) to perform the actual teardown of this module's own
 * resources: release this module's one shared-engine reference, wait for
 * any resulting shared-reactor teardown to finish (a no-op if chttpserver
 * still holds its own reference and the shared reactor stays up), then stop
 * the deadline sweep and destroy g_client_dns_pool, and finally clear
 * g_client_async_stopping so a waiting acquirer can proceed.
 *
 * Releasing the shared reference (and waiting for it) before stopping the
 * deadline sweep, rather than after, mirrors this module's pre-unification
 * ordering (stop the reactor, then the sweep) for the case where this
 * release is what brings the shared engine down; when it isn't (chttpserver
 * still has references), _cfio_engine_wait_for_quiescence() returns
 * immediately and the sweep is stopped right away regardless -- safe either
 * way, since by the time g_client_async_users has reached 0 there are no
 * ctxs left registered with the sweep for it to act on (see
 * _client_deadline_register's own comment for why a registered ctx always
 * implies a live async-user reference is held for it).
 */
static void *_client_engine_reaper_fn(void *arg) {
  (void)arg;
  _cfio_engine_release();
  _cfio_engine_wait_for_quiescence();
  _client_deadline_sweep_stop_and_join();
  pthread_mutex_lock(&g_client_async_mutex);
  ctpool_destroy(g_client_dns_pool); /* implicit drain shutdown; see
                                      * __ctpool_destroy */
  g_client_dns_pool = NULL;
  g_client_async_stopping = false;
  pthread_cond_broadcast(&g_client_async_stopped_cv);
  pthread_mutex_unlock(&g_client_async_mutex);
  return NULL;
}

/*
 * Joins the previous reaper thread if one is still outstanding. Must be
 * called with g_client_async_mutex already held, and is safe to block while
 * holding it: by the time g_client_reaper_joinable is observed true here,
 * the reaper has already broadcast g_client_async_stopped_cv and is on its
 * way to returning -- it never touches g_client_async_mutex again after that
 * broadcast, so this pthread_join cannot deadlock against it, and it returns
 * in practice almost immediately.
 *
 * Holding the mutex across the join also correctly serialises concurrent
 * callers: pthread_join-ing the same target from more than one thread
 * simultaneously is undefined behavior per POSIX, so only the first caller
 * to observe g_client_reaper_joinable may actually join -- every other
 * concurrent caller simply blocks on the mutex itself until that join (and
 * the flag clear) is done.
 *
 * Without this, a caller relying on _client_engine_wait_for_quiescence to
 * mean "this module's own resources are now fully gone at the OS level"
 * could proceed (e.g. straight into process exit) microseconds before the
 * reaper thread had actually finished its own glibc-internal thread-exit
 * bookkeeping -- observed in practice as an intermittent valgrind "possibly
 * lost" report for that thread's TLS/stack allocation. Mirrors
 * src/cfio_engine.c's _cfio_engine_join_reaper_if_needed_locked exactly.
 */
static void _client_engine_join_reaper_if_needed_locked(void) {
  if (g_client_reaper_joinable) {
    pthread_join(g_client_reaper_thread, NULL);
    g_client_reaper_joinable = false;
  }
}

/*
 * Acquires a reference to this module's async resources, bringing up
 * g_client_dns_pool and the deadline sweep and acquiring this module's one
 * shared-engine reference on the first call (blocking until the shared
 * reactor's event loop is confirmed running -- see cfio_engine.h).
 * Subsequent calls just bump g_client_async_users. Must be paired with
 * exactly one _client_engine_release() call.
 */
static ccol_retval_t _client_engine_acquire(void) {
  pthread_mutex_lock(&g_client_async_mutex);

  /* A previous instance may still be mid-teardown on this module's own
   * reaper thread (see _client_engine_release). Reusing g_client_dns_pool
   * while that's in flight would be a use-after-free, so wait for it to
   * fully finish before deciding whether to start fresh. */
  while (g_client_async_stopping) {
    pthread_cond_wait(&g_client_async_stopped_cv, &g_client_async_mutex);
  }
  _client_engine_join_reaper_if_needed_locked();

  if (!g_client_async_running) {
    long raw = sysconf(_SC_NPROCESSORS_ONLN);
    if (raw < 1) raw = 1;
    int16_t nthreads = (raw > INT16_MAX) ? INT16_MAX : (int16_t)raw;

    /* fio_socket() resolves DNS synchronously (see fio_tcp_socket in
     * fio.c) -- offloaded here so chttpclient_do_async's caller thread never
     * blocks on it. Created before acquiring the shared engine reference so
     * a failure here needs no rollback of anything else. */
    char *dns_err = NULL;
    g_client_dns_pool = create_cthread_pool((size_t)nthreads, 0, &dns_err);
    if (!g_client_dns_pool) {
      pthread_mutex_unlock(&g_client_async_mutex);
      return ccol_not_enough_memory;
    }

    /* Started before acquiring the shared reactor reference: if that
     * acquire then fails, the rollback below needs the sweep thread already
     * running so it can be torn down uniformly via
     * _client_deadline_sweep_stop_and_join. */
    if (_client_deadline_sweep_start() != ccol_success) {
      ctpool_destroy(g_client_dns_pool);
      g_client_dns_pool = NULL;
      pthread_mutex_unlock(&g_client_async_mutex);
      return ccol_unexpected_failure;
    }

    ccol_retval_t engine_rc = _cfio_engine_acquire();
    if (engine_rc != ccol_success) {
      _client_deadline_sweep_stop_and_join();
      ctpool_destroy(g_client_dns_pool);
      g_client_dns_pool = NULL;
      pthread_mutex_unlock(&g_client_async_mutex);
      return engine_rc;
    }

    /* Registered only once, ever, per process -- and only after this
     * module's own first successful _cfio_engine_acquire() call, which
     * itself guarantees (via cfio_engine.c's own pthread_once) that the
     * shared module's atexit(fio_lib_destroy) and
     * atexit(_cfio_engine_atexit_safety_net) registrations have ALREADY
     * happened by this point, regardless of whether chttpclient or
     * chttpserver was the first caller into the shared engine process-wide.
     * atexit runs handlers in reverse registration order, so registering
     * ours strictly after those two guarantees this module's own cleanup
     * (which gracefully releases its shared reference and tears down its
     * own DNS pool/deadline sweep) always runs BEFORE the shared module's
     * forced stop-everything safety net -- which matters when this process
     * exits with genuine in-flight chttpclient async work still open: this
     * module's atexit handler gets first refusal to wind things down on its
     * own terms, rather than having the shared reactor rippped out from
     * under a still-registered deadline-sweep ctx by the shared module's own
     * safety net running first. */
    pthread_once(&_client_atexit_once, _client_register_atexit_safety_net);

    g_client_async_running = true;
  }

  g_client_async_users++;
  pthread_mutex_unlock(&g_client_async_mutex);
  return ccol_success;
}

/*
 * Releases a reference acquired via _client_engine_acquire(). Once
 * g_client_async_users returns to zero, hands the actual teardown off to a
 * freshly spawned, detached reaper thread rather than performing it inline.
 *
 * This is essential, not just a style choice: _client_engine_release() is
 * routinely called from inside a facio protocol callback (_async_on_close),
 * i.e. from a thread that facio itself owns as part of the very reactor
 * this module holds a reference to. Even though _cfio_engine_release() itself
 * is safe to call from such a context (see cfio_engine.h), this module's OWN
 * teardown work (_client_deadline_sweep_stop_and_join joins a real thread;
 * ctpool_destroy drains and joins ctpool workers) must not block a facio
 * callback thread either -- so all of it is pushed onto the same kind of
 * detached reaper thread cfio_engine.c itself uses (this was caught by
 * async_step_a.post_echoes_body hanging in testing, in the single-owner
 * predecessor of this exact design).
 *
 * g_client_async_running is cleared here (synchronously, before spawning the
 * reaper) so no concurrent acquire can be satisfied by the now-dying
 * instance; g_client_async_stopping is set alongside it so a concurrent
 * acquire waits for the reaper to fully finish rather than possibly
 * touching g_client_dns_pool mid-teardown.
 */
/* Spawns the reaper thread (see _client_engine_reaper_fn's own comment).
 * Shared by _client_engine_release and _client_engine_atexit_safety_net --
 * both reach the same "something must tear this module's resources down
 * now" decision, just via different triggers (ref count hitting 0 vs.
 * process exit). */
static void _client_engine_spawn_reaper(void) {
  pthread_t reaper;
  if (pthread_create(&reaper, NULL, _client_engine_reaper_fn, NULL) != 0) {
    /* Could not spawn the reaper (OOM-class failure). No safer fallback
     * exists than doing it inline; this reintroduces the deadlock/blocking
     * risk documented on _client_engine_release only in this already-
     * degenerate case. Nothing to join afterward since it already ran to
     * completion synchronously. */
    _client_engine_reaper_fn(NULL);
    return;
  }
  pthread_mutex_lock(&g_client_async_mutex);
  g_client_reaper_thread = reaper;
  g_client_reaper_joinable = true;
  pthread_mutex_unlock(&g_client_async_mutex);
}

static void _client_engine_release(void) {
  bool should_reap = false;
  pthread_mutex_lock(&g_client_async_mutex);
  if (g_client_async_users > 0) g_client_async_users--;
  if (g_client_async_users == 0 && g_client_async_running) {
    g_client_async_running = false;
    g_client_async_stopping = true;
    should_reap = true;
  }
  pthread_mutex_unlock(&g_client_async_mutex);

  if (should_reap) _client_engine_spawn_reaper();
}

/*
 * Blocks until any in-flight reaper thread (see _client_engine_release) has
 * fully finished tearing this module's own resources down. A no-op if not
 * currently stopping (including if not running at all, or running and
 * staying up because other user references remain).
 *
 * _client_engine_release() deliberately does not block the calling thread
 * itself -- it is routinely called from inside a facio callback (on_close),
 * where blocking would be unsafe/undesirable for the reasons documented on
 * that function. That makes teardown asynchronous from every caller's point
 * of view, including this module's own atexit-registered safety net: without
 * a way to wait for a just-triggered teardown to actually finish, process
 * exit can race a still-running reaper thread (a crash caught by valgrind
 * during testing, since each test tears its engine fully down after a
 * single request rather than keeping it warm the way a real long-lived
 * application would). Callers that need a guaranteed-quiescent state --
 * this test suite between requests, or eventually a real application at
 * shutdown -- must call this explicitly, mirroring chttpserver.c's own
 * chttpsvr_engine_wait() for the identical class of problem on the server
 * side.
 */
static void _client_engine_wait_for_quiescence(void) {
  pthread_mutex_lock(&g_client_async_mutex);
  while (g_client_async_stopping) {
    pthread_cond_wait(&g_client_async_stopped_cv, &g_client_async_mutex);
  }
  _client_engine_join_reaper_if_needed_locked();
  pthread_mutex_unlock(&g_client_async_mutex);
}

/*
 * Process-exit safety net: registered (once, via _client_atexit_once) only
 * after this module's own first successful _cfio_engine_acquire() call --
 * see _client_engine_acquire's own comment on the pthread_once call site for
 * why that exact ordering is what guarantees this always runs *before* the
 * shared module's own forced-stop safety net and fio_lib_destroy, regardless
 * of which module (chttpclient or chttpserver) happened to bring the shared
 * engine up first in this process.
 *
 * _client_engine_wait_for_quiescence alone only protects a caller who
 * *knows* to call it once no more async work is outstanding. Nothing forces
 * that discipline: an application can exit (or, as happened developing this
 * module's own tests, a test can abort via a failed assertion) while
 * requests are still genuinely in flight -- user count still nonzero, no
 * release ever triggered a reaper. Without this function, the shared
 * engine's own atexit safety net would force the reactor down while this
 * module's deadline-sweep thread and DNS pool are potentially still
 * registered against it, and neither would ever be cleanly torn down
 * (a leak at best, a use-after-free at worst).
 *
 * If still running at process-exit time, forces g_client_async_users to 0
 * (there is no meaningful outstanding caller left to wait for) and triggers
 * the same stop-and-reap path _client_engine_release uses, then waits for
 * it -- covering both "we just triggered a stop" and "a concurrent ordinary
 * release already triggered one that hadn't finished yet".
 */
static void _client_engine_atexit_safety_net(void) {
  bool should_reap = false;
  pthread_mutex_lock(&g_client_async_mutex);
  if (g_client_async_running) {
    g_client_async_users = 0;
    g_client_async_running = false;
    g_client_async_stopping = true;
    should_reap = true;
  }
  pthread_mutex_unlock(&g_client_async_mutex);

  if (should_reap) _client_engine_spawn_reaper();
  _client_engine_wait_for_quiescence();
}

/* ========================================================================== */
/*                    ASYNC CONNECTION STATE MACHINE (STEP A + B)             */
/* ========================================================================== */

/*
 * Tier 2/3 engine: a non-blocking HTTP or HTTPS request/response cycle per
 * connection, now with redirect-following (Step B) layered on top of the
 * original single-hop pipeline (Step A). There is still no idle-pool reuse
 * yet -- every hop opens, then always closes, its own fresh connection; that
 * remains a separate, later increment. Per-request connect/request timeouts
 * are also not yet enforced here -- see the "reactor-owned timer/cancellation
 * handling" roadmap item.
 *
 * TLS: uses the same reactor-independent, client-mode TLS API Tier 1 uses
 * (fio_tls_connect_create / fio_tls_client_handshake_step /
 * fio_tls_connection_read / fio_tls_connection_write), driven from
 * on_data/on_ready instead of a blocking poll() loop. Those calls write and
 * read straight through OpenSSL's BIO layer on the raw fd, bypassing
 * facio's fio_write2/fio_read data path entirely -- see
 * _async_tls_try_write's comment for what that implies for write-readiness
 * notification (fio_force_write_rearm).
 *
 * Redirect-chain lifetime (Step B): a redirect chain spans multiple
 * connections (one chttp_async_ctx_t per hop, exactly mirroring Tier 1's "a
 * fresh chttp_parse_ctx_t per hop" comment), but must still fulfil the
 * caller's future exactly once and release exactly one engine reference for
 * the whole chain. chttp_async_chain_t is the small heap-allocated struct
 * that outlives any single hop's ctx to carry that shared state: the future,
 * the fulfilled-once guard, an owned deep copy of the original request's
 * headers and body (the original chttp_request_t may be freed by the caller
 * the moment chttpclient_do_async returns -- long before a redirect hop two
 * or three requests later would need to re-serialise them), the pinned TLS
 * context (constant across hops, exactly like Tier 1's tls_ctx local), and a
 * refcount tracking how many hops' ctx are currently live. Every ctx
 * teardown path (_async_ctx_teardown) releases one chain reference; when the
 * count reaches zero -- meaning no further hop was ever queued to take over
 * -- _async_chain_release both frees the chain and, via a fulfilled-guarded
 * backstop, fulfils the future with a generic error if nothing else already
 * did (mirroring _async_on_close's old per-ctx backstop, now centralised so
 * it naturally does the right thing regardless of which hop's ctx happens to
 * be the last one torn down).
 */

/* The ctpool_future's result is a chttpcli_async_result_t (public, declared
 * in chttpclient.h) -- built by _async_fulfill_chain below. */

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
  ctpool_future *future;   /* the caller's own reference is separate -- see
                            * _chttp_do_async_internal's return value; this
                            * module only ever calls ctpool_future_fulfill on
                            * it (the producer-side reference) */

  chttpcli_write_fn write_fn; /* NULL for a buffered (chttpclient_do_async)
                               * request; non-NULL for a streaming
                               * (chttpclient_do_async_streaming) one. Wired
                               * into every hop's ctx->pctx.requested_sink_fn
                               * in _async_submit_hop/_async_retry_hop --
                               * constant across the whole chain, exactly
                               * like Tier 1's identical streaming/write_fn
                               * locals in chttp_do_internal. Called on
                               * whichever reactor thread is driving this
                               * hop's on_data callback -- see this field's
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

  fio_tls_s *tls_ctx; /* pinned once (fio_tls_dup'd from cli->tls_ctx) for
                       * the whole chain, exactly like Tier 1's tls_ctx
                       * local -- a redirect can hop between http and
                       * https, so this must survive every hop regardless
                       * of which scheme the chain started with. Released
                       * once via fio_tls_destroy when the chain is freed. */
  bool tls_ctx_usable;
  bool verify_host;

  long connect_timeout_ms; /* re-read fresh into ctx->connect_deadline at the
                            * start of every hop that actually connects (the
                            * reused-connection path never consults it, since
                            * it skips CONNECTING/TLS_HANDSHAKING entirely) --
                            * see the "ASYNC DEADLINE SWEEP" section below. */
  chttp_deadline_t overall_deadline; /* computed once, here, for the whole
                                      * chain's lifetime -- mirrors Tier 1's
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
 * can self-reference the struct -- an anonymous struct has no name to spell
 * a pointer to itself with. */
typedef struct chttp_async_ctx_s {
  fio_protocol_s protocol; /* MUST be the first member: on_data/on_ready/
                            * on_close receive a fio_protocol_s* that is cast
                            * straight back to chttp_async_ctx_t*. */
  ccol_memmgmt_procs_t *mp;
  struct chttpclient *cli; /* owning client -- needed while idle
                            * (chain == NULL then) to reach
                            * cli->idle_pools_async, and while active to
                            * offer this connection back to the pool on a
                            * reusable completion */
  chttp_async_chain_t
      *_Atomic chain;        /* shared, whole-redirect-chain state;
                              * not owned by ctx -- see _async_ctx_teardown.
                              * NULL while ctx is idle-pooled. _Atomic (like
                              * state/uuid below) so the deadline sweep can
                              * read it without idle_lock -- see that lock's
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
  _Atomic intptr_t uuid;

  char *host; /* owned copy; port is passed separately */
  uint16_t port;
  char *origin_key;    /* owned copy, "scheme://host:port" -- matches Tier 1's
                        * chttp_conn_t.origin_key; used to place/remove this
                        * ctx in cli->idle_pools_async */
  bool reused;         /* true if this hop's connection came from the idle
                        * pool rather than a fresh connect, for THIS
                        * attempt (a retry always resets this to false --
                        * see _async_retry_hop) */
  bool any_bytes_read; /* true once >=1 response byte has been read for the
                        * CURRENT attempt; gates the reused-connection
                        * dead-connection retry, exactly like Tier 1's
                        * identically-named any_bytes_read output param */
  struct timespec last_used; /* set when offered to the idle pool; used by
                              * _async_idle_pool_take's staleness check */
  char *wire; /* owned serialized request bytes. Plain HTTP: ownership
               * transfers to fio_write2 once sent (set NULL after). TLS:
               * this module retains ownership and frees it once fully
               * written via fio_tls_connection_write (see wire_sent) --
               * either way, freeing it in _async_ctx_free is always safe
               * (a no-op once NULL). */
  size_t wire_len;
  size_t wire_sent; /* TLS only: bytes of wire already handed to
                     * fio_tls_connection_write -- that call has raw write(2)
                     * semantics (short writes are normal), unlike fio_write2
                     * which tracks this internally for the plain path. */

  bool is_https;
  bool verify_host;
  bool hop_completed; /* set once this hop's response has been fully parsed
                       * and either fulfilled or handed off to a redirect --
                       * guards _async_on_data against re-running that (for
                       * _async_handle_redirect, non-idempotent) logic on a
                       * spurious extra on_data call; see that check's own
                       * comment for why this can happen. */
  fio_tls_connection_s *tls; /* NULL until the TCP connect succeeds and the
                              * handshake begins; NULL for plain HTTP */
  mutex_t tls_lock;          /* Guards every access to tls: facio's on_data and
                              * on_ready callbacks for the SAME connection are only
                              * mutually exclusive with themselves (they take
                              * different internal lock types -- FIO_PR_LOCK_TASK vs
                              * FIO_PR_LOCK_WRITE -- see deferred_on_data/
                              * deferred_on_ready_usr in fio.c), NOT with each other.
                              * Both _async_on_data and _async_on_ready can call into
                              * _async_tls_advance for the same ctx, so two reactor
                              * threads can end up calling
                              * fio_tls_client_handshake_step/fio_tls_connection_read/
                              * fio_tls_connection_write on the same underlying SSL*
                              * concurrently without this lock -- a real data race on
                              * OpenSSL's internal BIO state (use-after-free, caught
                              * by valgrind: BIO_free from one thread racing
                              * BIO_copy_next_retry from another on the same freed
                              * block). Unused (left zero-initialised) for plain HTTP
                              * connections, which never touch tls at all. */

  mutex_t idle_lock; /* Guards the state/chain pair specifically across the
                      * idle<->active transition. A connection popped out of
                      * cli->idle_pools_async by _async_idle_pool_take stays
                      * fully attached to the reactor the whole time (there
                      * is no way to "pause" polling for a single uuid) --
                      * removing it from the POOL's bookkeeping does nothing
                      * to stop facio from independently dispatching
                      * on_data/on_ready/on_close for it on another thread
                      * the moment the peer sends something (or the
                      * connection dies) while _async_submit_hop is still in
                      * the middle of reconfiguring ctx->state away from
                      * CHTTP_ASYNC_IDLE and ctx->chain away from NULL for
                      * its new owner. Without this lock, on_data's
                      * IDLE-state guard could read a stale (already-popped
                      * but not-yet-reconfigured) ctx->state, fall through
                      * past the guard, and dereference ctx->chain while it
                      * is still NULL -- caught by a real SIGSEGV in
                      * _async_fulfill_chain during this feature's own
                      * development. Only ever held very briefly, around the
                      * handful of statements that flip state+chain in
                      * either direction, never across I/O. Deliberately NOT
                      * used by the deadline sweep (see g_client_deadline_
                      * lock's own comment in the "ASYNC DEADLINE SWEEP"
                      * section): the sweep runs on an independent thread and
                      * needs to read state/chain/uuid/timed_out from
                      * *every* registered ctx on every tick, which taking
                      * this lock for would create a genuine lock-order
                      * inversion against call sites that call into the
                      * deadline registry (_client_deadline_register/
                      * _unregister) while already holding this exact lock
                      * (e.g. _async_submit_hop's reused-connection-write-
                      * failure path calling _async_retry_hop) -- caught as
                      * a real deadlock (gdb: sweep thread blocked on this
                      * lock while holding g_client_deadline_lock, main
                      * thread blocked on g_client_deadline_lock while
                      * holding this lock) during this feature's own
                      * development. state/chain/uuid/timed_out are instead
                      * _Atomic specifically so the sweep can read them
                      * lock-free; see those fields' own comments for why
                      * individual atomic reads (rather than this lock's
                      * stronger compound guarantee) are sufficient for the
                      * sweep's specific use, even though every OTHER
                      * consumer of state/chain (on_data/on_ready/on_close,
                      * via _async_ctx_is_idle) still needs -- and keeps
                      * getting -- the full compound protection this lock
                      * alone provides. */

  chttp_deadline_t connect_deadline; /* Only meaningful while state is
                                      * CHTTP_ASYNC_CONNECTING or
                                      * CHTTP_ASYNC_TLS_HANDSHAKING -- set
                                      * fresh (by the thread about to submit
                                      * this hop attempt) at the start of
                                      * every hop that actually connects
                                      * (never for a reused connection, which
                                      * skips both those states entirely).
                                      * Deliberately NOT _Atomic: it is
                                      * written exactly once, strictly
                                      * before _client_deadline_register is
                                      * called for this attempt -- that
                                      * call's own mutex lock/unlock is
                                      * itself a release/acquire pair, so it
                                      * already guarantees the sweep (which
                                      * only ever observes a ctx AFTER
                                      * finding it via the registry) sees a
                                      * fully-initialised value with no
                                      * separate synchronisation needed. See
                                      * the "ASYNC DEADLINE SWEEP" section. */
  _Atomic bool timed_out;   /* Set by the deadline sweep, lock-free, right
                             * before it force-closes this connection -- lets
                             * _async_on_close report ccol_timed_out and skip
                             * the ordinary dead-connection retry (retrying
                             * past an already-blown deadline would only
                             * extend the overrun for no benefit). _Atomic for
                             * the same reason state/chain/uuid are, above. */
  bool deadline_registered; /* True once this ctx has been linked into
                             * g_client_deadline_head at least once.
                             * Registration is idempotent and, once made,
                             * persists for ctx's whole lifetime (including
                             * every idle-pool cycle) -- see
                             * _client_deadline_register's own comment. */
  struct chttp_async_ctx_s *deadline_prev,
      *deadline_next; /* Intrusive
                       * doubly-linked membership in the process-wide deadline
                       * registry, guarded by g_client_deadline_lock (a
                       * different lock than idle_lock above -- see that
                       * global's own comment for the lock-ordering contract
                       * between the two). */

  llhttp_t parser;
  chttp_parse_ctx_t pctx;
  chttp_bodybuf_t bb;
} chttp_async_ctx_t;

/* ========================================================================== */
/*                    ASYNC DEADLINE SWEEP (TIER 2)                           */
/* ========================================================================== */

/*
 * Enforces connect_timeout_ms/request_timeout_ms for Tier 2/3 as true
 * absolute wall-clock deadlines, mirroring Tier 1's _deadline_make/
 * _deadline_remaining_ms semantics exactly -- including catching a
 * connection that never goes fully idle (e.g. a slow trickle of bytes just
 * before an activity-reset timeout would fire) but has still blown its
 * deadline. facio's own built-in per-connection timer (fio_timeout_set + a
 * .ping callback fired by fio_review_timeout) is activity-reset only and
 * cannot express that, so this is a genuinely new, small periodic-sweep
 * mechanism, entirely separate from the engine's fio_start() reactor thread
 * and the DNS/connect ctpool -- started/stopped alongside them (see
 * _client_deadline_sweep_start/_stop_and_join, called from
 * _client_engine_acquire/_client_engine_reaper_fn above).
 *
 * g_client_deadline_lock serialises three things: (1) the intrusive
 * doubly-linked registry list itself (ctx->deadline_prev/deadline_next),
 * (2) the sweep thread's own sleep/wake condvar, and (3) -- the reason a
 * ctx is never explicitly unregistered except from within _async_ctx_free,
 * the single reliable point ctx memory is actually released -- mutual
 * exclusion between the sweep reading a registered ctx's fields and any
 * other thread freeing that same ctx concurrently: _client_deadline_
 * unregister cannot complete (and therefore _async_ctx_free cannot proceed
 * to actually free ctx) while the sweep is still walking the registry with
 * this lock held.
 *
 * Registration (_client_deadline_register) is idempotent and, once made,
 * persists for a ctx's entire lifetime, including every idle-pool cycle it
 * goes through -- a pooled/idle ctx just sits in the registry inertly (the
 * sweep's own state==CONNECTING/TLS_HANDSHAKING and chain!=NULL checks
 * naturally skip it while idle), which is simpler and just as correct as
 * explicitly unregistering on every idle-pool-offer and re-registering on
 * every reuse.
 *
 * Lock ordering: g_client_deadline_lock is only ever taken OUTSIDE of any
 * ctx->idle_lock (never the reverse) -- _client_deadline_register/
 * _unregister are never called while idle_lock is held (see their call
 * sites), and the sweep itself takes idle_lock only after already holding
 * g_client_deadline_lock. This one-directional order is what makes nesting
 * the two safe.
 */
static pthread_mutex_t g_client_deadline_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_client_deadline_cv = PTHREAD_COND_INITIALIZER;
static chttp_async_ctx_t *g_client_deadline_head = NULL;
static pthread_t g_client_deadline_thread;
static bool g_client_deadline_stop = false;

#define CHTTP_DEADLINE_SWEEP_INTERVAL_MS 100
/* Soft cap on how many expired connections one sweep tick force-closes.
 * Self-healing, not a hard limit: anything beyond this on one tick simply
 * stays registered (not yet marked timed_out, so it is picked up again) and
 * gets its fio_force_close call from a later tick instead, at worst another
 * CHTTP_DEADLINE_SWEEP_INTERVAL_MS later per extra batch -- a practically
 * irrelevant delay for the number of simultaneously-expiring connections it
 * would take to ever exceed this. */
#define CHTTP_DEADLINE_SWEEP_BATCH 256

/*
 * Adds ctx to the deadline registry if not already present. Must be called
 * with ctx->idle_lock NOT held (see the lock-ordering note above).
 */
static void _client_deadline_register(chttp_async_ctx_t *ctx) {
  pthread_mutex_lock(&g_client_deadline_lock);
  if (!ctx->deadline_registered) {
    ctx->deadline_prev = NULL;
    ctx->deadline_next = g_client_deadline_head;
    if (g_client_deadline_head) g_client_deadline_head->deadline_prev = ctx;
    g_client_deadline_head = ctx;
    ctx->deadline_registered = true;
  }
  pthread_mutex_unlock(&g_client_deadline_lock);
}

/* Removes ctx from the deadline registry if present. Called exactly once,
 * as the first thing _async_ctx_free does -- see that function's own
 * comment for why that is the single correct place for this. */
static void _client_deadline_unregister(chttp_async_ctx_t *ctx) {
  pthread_mutex_lock(&g_client_deadline_lock);
  if (ctx->deadline_registered) {
    if (ctx->deadline_prev) {
      ctx->deadline_prev->deadline_next = ctx->deadline_next;
    } else {
      g_client_deadline_head = ctx->deadline_next;
    }
    if (ctx->deadline_next)
      ctx->deadline_next->deadline_prev = ctx->deadline_prev;
    ctx->deadline_prev = ctx->deadline_next = NULL;
    ctx->deadline_registered = false;
  }
  pthread_mutex_unlock(&g_client_deadline_lock);
}

/*
 * One sweep pass: walks the whole registry under g_client_deadline_lock,
 * checking each ctx's connect_deadline (only while it is actually in a
 * connecting phase) and its chain's overall_deadline (whenever it has a
 * live chain at all), collecting the uuids of newly-expired connections
 * into a small stack buffer. fio_force_close is deliberately called AFTER
 * releasing g_client_deadline_lock, once the whole walk is done: calling it
 * while still holding the lock risks a same-thread reentrant deadlock,
 * since fio_force_close can synchronously invoke this same ctx's on_close
 * callback (the same general hazard documented on _async_handle_redirect's
 * fd-reuse-race fix elsewhere in this file), which calls _async_ctx_free ->
 * _client_deadline_unregister -> tries to re-lock this exact mutex on the
 * exact same thread.
 *
 * state/chain/uuid/timed_out are read as plain (but _Atomic-qualified, so
 * individually race-free) loads here -- deliberately NOT under the ctx's
 * own idle_lock. An earlier version of this function did take idle_lock
 * here, which produced a real deadlock during development: some call paths
 * (_async_submit_hop's reused-connection-write-failure branch, calling
 * _async_retry_hop -> _client_deadline_register) legitimately need to hold
 * a ctx's idle_lock while also needing g_client_deadline_lock, and this
 * function held g_client_deadline_lock while trying to acquire a (possibly
 * different) ctx's idle_lock -- classic AB-BA lock-order inversion, caught
 * live via gdb (sweep thread blocked on some ctx's idle_lock while holding
 * g_client_deadline_lock; the submitting thread blocked on
 * g_client_deadline_lock while holding that same ctx's idle_lock).
 *
 * This is safe without idle_lock's stronger *compound* (state-and-chain-
 * together) consistency guarantee specifically because of what this
 * function does with a possibly-torn snapshot: at both points idle_lock
 * actually protects (_async_idle_pool_take's idle->active transition and
 * _async_idle_pool_offer's active->idle transition), every reachable torn
 * combination of (state, chain) either skips both deadline checks (chain
 * NULL, or state not a connecting state) or evaluates a chain that is,
 * worst case, a moment away from being released but still guaranteed
 * alive (the chain reference is only actually dropped strictly after the
 * state/chain pair is updated) -- so the worst possible outcome of a torn
 * read here is force-closing a connection a few instructions before or
 * after it would otherwise have been cleanly pooled or hopped, i.e. a lost
 * optimisation opportunity, never an incorrect abort of a still-in-flight,
 * unrelated request. Every OTHER consumer of state/chain (on_data/on_ready/
 * on_close, via _async_ctx_is_idle) has a stronger need for idle_lock's
 * full guarantee and keeps using it entirely unchanged.
 *
 * A ctx is marked ctx->timed_out before being queued for force-close, both
 * to tell _async_on_close "this is a timeout, not an ordinary connection
 * failure" (it reports ccol_timed_out and skips the usual dead-connection
 * retry -- retrying past an already-blown deadline would just extend the
 * overrun) and to make repeated sweep ticks idempotent against a ctx that
 * is still mid-teardown from an earlier tick's force-close.
 */
static void _client_deadline_sweep_once(void) {
  intptr_t to_close[CHTTP_DEADLINE_SWEEP_BATCH];
  size_t n_to_close = 0;

  pthread_mutex_lock(&g_client_deadline_lock);
  chttp_async_ctx_t *node = g_client_deadline_head;
  while (node && n_to_close < CHTTP_DEADLINE_SWEEP_BATCH) {
    chttp_async_ctx_t *next = node->deadline_next;

    chttp_async_state_t state = node->state;
    chttp_async_chain_t *chain = node->chain;
    intptr_t uuid = node->uuid;
    bool expired = false;
    if (!node->timed_out) {
      int ms;
      if ((state == CHTTP_ASYNC_CONNECTING ||
           state == CHTTP_ASYNC_TLS_HANDSHAKING) &&
          !_deadline_remaining_ms(&node->connect_deadline, &ms)) {
        expired = true;
      }
      if (!expired && chain &&
          !_deadline_remaining_ms(&chain->overall_deadline, &ms)) {
        expired = true;
      }
      if (expired) node->timed_out = true;
    }

    if (expired && uuid >= 0) to_close[n_to_close++] = uuid;
    node = next;
  }
  pthread_mutex_unlock(&g_client_deadline_lock);

  for (size_t i = 0; i < n_to_close; i++) fio_force_close(to_close[i]);
}

static void *_client_deadline_sweep_fn(void *arg) {
  (void)arg;
  pthread_mutex_lock(&g_client_deadline_lock);
  while (!g_client_deadline_stop) {
    struct timespec wake;
    clock_gettime(CLOCK_MONOTONIC, &wake);
    wake.tv_nsec += CHTTP_DEADLINE_SWEEP_INTERVAL_MS * 1000000L;
    if (wake.tv_nsec >= 1000000000L) {
      wake.tv_nsec -= 1000000000L;
      wake.tv_sec += 1;
    }
    pthread_cond_timedwait(&g_client_deadline_cv, &g_client_deadline_lock,
                           &wake);
    if (g_client_deadline_stop) break;
    pthread_mutex_unlock(&g_client_deadline_lock);
    _client_deadline_sweep_once();
    pthread_mutex_lock(&g_client_deadline_lock);
  }
  pthread_mutex_unlock(&g_client_deadline_lock);
  return NULL;
}

/* Starts the sweep thread. Called from _client_engine_acquire, alongside
 * spawning the engine's own reactor thread -- see that function for
 * rollback-on-failure handling. */
static ccol_retval_t _client_deadline_sweep_start(void) {
  g_client_deadline_stop = false;
  int rc = pthread_create(&g_client_deadline_thread, NULL,
                          _client_deadline_sweep_fn, NULL);
  return (rc == 0) ? ccol_success : ccol_unexpected_failure;
}

/* Signals and joins the sweep thread. Called from _client_engine_reaper_fn,
 * alongside joining the engine's own reactor thread -- see that function's
 * own comment for why teardown always happens on a dedicated reaper thread,
 * never inline from a facio callback. */
static void _client_deadline_sweep_stop_and_join(void) {
  pthread_mutex_lock(&g_client_deadline_lock);
  g_client_deadline_stop = true;
  pthread_cond_broadcast(&g_client_deadline_cv);
  pthread_mutex_unlock(&g_client_deadline_lock);
  pthread_join(g_client_deadline_thread, NULL);
}

/* Deep-copies a chmap(char* -> char*) header map -- used to give a redirect
 * chain its own copy of the original request's headers, since the caller's
 * chttp_request_t may be freed the moment chttpclient_do_async returns, long
 * before a later hop needs to re-serialise them. Returns NULL on OOM (input
 * NULL is not an error -- it just means "no headers", and returns NULL too,
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
 * (released once via fio_tls_destroy when the chain is freed) -- on failure,
 * the caller still owns tls_ctx and must release it itself. req_headers/
 * body_data/body_content_type are NOT taken by reference: this function
 * deep-copies whatever it needs from them and never retains the originals.
 */
static chttp_async_chain_t *_async_chain_create(
    ccol_memmgmt_procs_t *mp, struct chttpclient *cli, ctpool_future *future,
    chmap req_headers, const void *body_data, size_t body_len,
    const char *body_content_type, fio_tls_s *tls_ctx, bool tls_ctx_usable,
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
 * Releases one live-hop reference. When the count reaches zero -- meaning no
 * further hop was ever queued to take over from the last one torn down --
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

  _async_fulfill_chain(chain, ccol_http_transfer_aborted, NULL);
  if (chain->tls_ctx) fio_tls_destroy(chain->tls_ctx);
  if (chain->req_headers) __chmap_destroy(chain->req_headers);
  _mem_free(chain->mp, chain->body_data);
  _mem_free(chain->mp, chain->body_content_type);
  mutex_destroy(chain->lock);
  _mem_free(chain->mp, chain);
  _client_engine_release();
}

static chttp_async_ctx_t *_async_ctx_create(ccol_memmgmt_procs_t *mp) {
  chttp_async_ctx_t *ctx =
      (chttp_async_ctx_t *)_mem_calloc(mp, 1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->mp = mp;
  ctx->uuid = -1;
  if (mutex_init(ctx->tls_lock) != 0) {
    _mem_free(mp, ctx);
    return NULL;
  }
  if (mutex_init(ctx->idle_lock) != 0) {
    mutex_destroy(ctx->tls_lock);
    _mem_free(mp, ctx);
    return NULL;
  }
  return ctx;
}

/* The single reliable place ctx (a single hop's connection state) is freed
 * -- called from on_close (the guaranteed-exactly-once terminal callback for
 * any attached protocol) or, for failures that occur before fio_attach ever
 * runs (so on_close will never fire for this ctx), directly by the failing
 * code path. Every field is safe to free/destroy unconditionally: fields
 * whose ownership was transferred elsewhere (wire -> fio_write2, headers/
 * bb.buf -> a successfully built response) are set NULL/cleared at the
 * transfer site, and _mem_free/_parse_ctx_free_fields/a NULL tls are all
 * no-ops. Does NOT touch ctx->chain -- that is shared, whole-chain state;
 * see _async_ctx_teardown, which pairs this with the matching chain release. */
static void _async_ctx_free(chttp_async_ctx_t *ctx) {
  if (!ctx) return;
  _client_deadline_unregister(ctx); /* no-op if never registered */
  if (ctx->tls) fio_tls_connection_destroy(ctx->tls);
  mutex_destroy(ctx->tls_lock);
  mutex_destroy(ctx->idle_lock);
  _mem_free(ctx->mp, ctx->wire);
  _mem_free(ctx->mp, ctx->host);
  _mem_free(ctx->mp, ctx->origin_key);
  _parse_ctx_free_fields(&ctx->pctx);
  _mem_free(ctx->mp, ctx->bb.buf);
  _mem_free(ctx->mp, ctx);
}

/* Frees a single hop's per-connection state and releases its chain
 * reference -- the standard way every terminal code path for a ctx (on_close,
 * or an early failure before fio_attach ever ran) ends. Captures chain into
 * a local first since _async_ctx_free frees ctx itself. */
static void _async_ctx_teardown(chttp_async_ctx_t *ctx) {
  chttp_async_chain_t *chain = ctx->chain;
  _async_ctx_free(ctx);
  _async_chain_release(chain);
}

/*
 * Delivers a terminal result to the caller's future exactly once (guarded by
 * chain->fulfilled, under chain->lock -- see that field's comment for why
 * this must be lock-protected rather than a plain bool now that a redirect
 * chain can have two hops' callbacks running concurrently) -- safe to call
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
  /* Marks this ctx terminal so _async_on_data/_async_on_ready's own
   * top-of-function guard skips any further work for it -- see
   * ctx->hop_completed's field comment for why a stray extra callback
   * invocation after this point must be a no-op rather than re-running
   * (non-idempotent) logic like _async_handle_redirect or a second
   * fio_tls_client_handshake_step call on an already-failed handshake. */
  ctx->hop_completed = true;
  _async_fulfill_chain(ctx->chain, rv, resp);
}

/* Builds the chttpcli_response from the completed parse (mirrors Tier 1's
 * own response-building code at the tail of chttp_do_internal). Transfers
 * ownership of ctx->pctx.headers/ctx->bb.buf out of ctx (nulling them there)
 * -- called BEFORE _async_finish_connection's idle-pool-offer path, which
 * would otherwise free those exact same fields while resetting ctx for its
 * idle life; see _async_on_data's HPE_PAUSED handling for why the ordering
 * (build response, then finish the connection, then actually fulfil) matters
 * on its own terms too.
 *
 * For a streaming request (ctx->chain->write_fn set), body bytes were
 * already delivered to the caller's callback as they arrived off the wire
 * (see _async_submit_hop/_async_retry_hop's sink wiring) -- ctx->bb was
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

/* Builds the response and fulfills with it in one step -- used by the eof-
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
 * still attached to the shared reactor (there is no way to "detach" a live
 * uuid in facio without closing it), so pooling a ctx means transitioning it
 * to CHTTP_ASYNC_IDLE and letting _async_on_data/_async_on_ready/
 * _async_on_close's own IDLE-state branches keep driving it -- any activity
 * (or the natural EOF/hangup a dead connection eventually produces) is
 * treated as "no longer usable" and torn down through the exact same
 * on_close path a normal failed connection would use, just entered from a
 * different state. Because there is no way to synchronously peek a reactor-
 * owned fd the way Tier 1's MSG_PEEK liveness probe does, a connection that
 * dies in the narrow window between being popped out of the pool and
 * actually being reused is instead caught by the reused/any_bytes_read
 * retry-once mechanism at the point of use (see _async_retry_hop) --
 * together these two mechanisms give Tier 2 the same effective guarantee
 * Tier 1's probe-then-retry combination does.
 *
 * A pooled ctx holds its OWN engine reference (acquired in
 * _async_idle_pool_offer, released in whichever of _async_idle_pool_take's
 * reuse path or the IDLE-state on_close path claims it next) -- separate
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
 * the count. A no-op (returns false) if target isn't found -- e.g. it was
 * already popped out by _async_idle_pool_take's own staleness eviction just
 * before its natural death was ALSO detected via the IDLE-state on_data/
 * on_close path; both sides are safe to call this unconditionally. Must be
 * called with cli->lock held. */
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
    _async_idle_count_dec_locked(cli);
    return true;
  }
  return false;
}

/*
 * Attempts to pop a usable idle connection for `origin_key`. Returns true
 * and fills *out on success. May pop and discard several stale candidates
 * (age only -- see the file-level comment above for why there is no
 * liveness probe here) before finding a fresh one or exhausting the list.
 *
 * IMPORTANT -- return contract: on a true return, ctx->idle_lock is left
 * LOCKED. The caller (_async_submit_hop) must keep it held for as long as
 * it takes to finish reconfiguring ctx for the new hop and attempting the
 * write, only unlocking once ctx is fully consistent again (all fields set,
 * write attempted). This is not a stylistic choice: popping a candidate out
 * of the pool's cvec makes it un-findable by a concurrent _async_idle_
 * pool_take, but does nothing to stop facio from independently dispatching
 * on_data/on_ready/on_close for its uuid on another thread at any moment --
 * the connection stays fully attached to the reactor for as long as it sat
 * in the pool, exactly like every other race described in this section.
 * Without holding idle_lock across the WHOLE reconfiguration (not just the
 * state/chain pair, as an earlier version of this function attempted), a
 * concurrent on_close could see ctx in a half-reconfigured state -- state
 * already flipped off CHTTP_ASYNC_IDLE, but hop/pctx/wire/etc. still being
 * written by this thread -- and tear ctx down (freeing it, destroying
 * idle_lock itself) while this function's caller is still using it: a real,
 * caught-in-development use-after-free, distinct from (and deeper than) the
 * earlier state/chain torn-write bug. _async_ctx_is_idle (used by on_data/
 * on_ready/on_close) takes the same lock, so any of them racing this
 * function simply blocks until the caller releases it, by which point ctx
 * is fully self-consistent one way or the other.
 *
 * A discarded stale candidate is torn down via the SAME normal active-hop
 * on_close path every other failed hop uses (chain retained just for this,
 * hop_completed forced true to skip the reused-retry check, which does not
 * apply -- this was never a real request attempt) rather than freed
 * directly here, for the identical reason: a concurrent, independently
 * triggered on_close for its uuid may already be racing this function and
 * must find a live, consistent ctx if it gets there first.
 */
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
      mutex_lock(ctx->idle_lock); /* left locked -- see contract above */
      ctx->chain = chain;
      ctx->state = CHTTP_ASYNC_WRITING;
      *out = ctx;
      return true;
    }

    _async_chain_retain(chain); /* throwaway -- balanced by the normal
                                 * teardown this candidate's own on_close
                                 * runs once force-closed below */
    mutex_lock(ctx->idle_lock);
    ctx->chain = chain;
    ctx->state = CHTTP_ASYNC_WRITING; /* just needs to be non-IDLE */
    ctx->hop_completed = true;
    ctx->reused = false;
    mutex_unlock(ctx->idle_lock);
    if (ctx->uuid >= 0) fio_force_close(ctx->uuid);
    /* loop: try the next candidate (if any) for this origin */
  }
}

/*
 * Offers a still-good, keep-alive-eligible connection back to cli's async
 * idle pool, bounded by the same per-origin/total caps Tier 1 uses. Returns
 * true if pooled (ownership of the connection, and this ctx's memory,
 * transfers to the pool -- the caller must not touch ctx again) or false if
 * it doesn't fit (caps hit, client destroying, or the idle-slot engine
 * reference couldn't be acquired) -- in which case ctx is left COMPLETELY
 * UNTOUCHED (still attached to its original chain, in whatever state it was
 * in on entry) and the caller is expected to fall back to closing it
 * normally, exactly like Tier 1's identical "a lost optimisation
 * opportunity, never a correctness issue" comment.
 *
 * On success, detaches ctx from its original chain, releasing the one chain
 * reference this hop was holding for it (the hop is over; the chain no
 * longer owns this connection, the idle pool does) -- forgetting this
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
 * and marking it CHTTP_ASYNC_IDLE) and insert it -- both still under the
 * SAME cli->lock critical section, so no other thread can pop from the list
 * in between. This ordering matters for two independent reasons: (1) ctx
 * must never become visible/poppable via _async_idle_pool_take while only
 * partially reset -- _async_idle_pool_take unconditionally overwrites
 * whatever it finds without waiting for anything, so a concurrent take()
 * starting to reconfigure ctx for a brand new hop while this function was
 * still freeing/resetting those exact same fields for the OLD hop would be
 * a genuine concurrent-access data race on the fields themselves, not just
 * a logical inconsistency -- caught during this feature's own development;
 * (2) the caller's "ctx untouched on failure" contract above would
 * otherwise be violated by a version of this function that resets ctx
 * speculatively before knowing whether the pool has room.
 */
static bool _async_idle_pool_offer(chttp_async_ctx_t *ctx) {
  struct chttpclient *cli = ctx->cli;

  /* This ctx is about to belong to the idle pool, not any chain -- it needs
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
    return false; /* ctx untouched -- caller falls back to a normal close */
  }

  /* Committed: this ctx WILL leave chain and become idle-pooled. Capture
   * the chain reference to release below, then reset ctx while STILL
   * holding cli->lock (see the file-level comment above for why). */
  chttp_async_chain_t *old_chain = ctx->chain;

  /* _parse_ctx_free_fields only frees+nulls cur_field/cur_value/location/
   * headers -- every other field (cur_field_cap/cur_value_cap in
   * particular) is left stale. That has always been safe at its other call
   * sites (_async_ctx_free, right before the whole ctx is freed; and Tier
   * 1's per-hop chttp_parse_ctx_t, a fresh stack struct every hop) because
   * the struct itself is discarded immediately after. Here it is NOT
   * discarded -- ctx is about to be reused for a future hop -- so a stale,
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
  /* ctx->chain and ctx->state flip together, under idle_lock -- see that
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
     * being confirmed available above) -- ctx has already been fully
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
 * whether to redirect or fulfil -- offers the connection back to cli's idle
 * pool if `reusable`, otherwise closes it. Mirrors Tier 1's identical
 * reusable/_idle_pool_offer dance in chttp_do_internal, applied uniformly
 * regardless of whether this hop turns out to be a redirect or the final
 * response (see _async_handle_redirect and _async_on_data's HPE_PAUSED
 * handling, both of which call this before doing anything else with the
 * connection).
 */
static void _async_finish_connection(chttp_async_ctx_t *ctx, intptr_t uuid,
                                     bool reusable) {
  if (reusable && ctx->origin_key && _async_idle_pool_offer(ctx)) return;
  fio_close(uuid);
}

/* Forward declaration: _async_on_data/_async_on_close (below) call this on a
 * reused connection that turns out to be dead before any response byte was
 * read; the full definition (after _async_connect_task, which it needs to
 * queue the replacement attempt) comes later in this file, alongside
 * _async_submit_hop. */
static void _async_retry_hop(chttp_async_ctx_t *ctx);

/* Forward declaration: _async_tls_advance (below) calls this once the
 * handshake completes, before its own definition later in this file. */
static void _async_tls_try_write(chttp_async_ctx_t *ctx, intptr_t uuid);

/*
 * Drives the client TLS handshake one step at a time from either on_data or
 * on_ready, whichever fires next.
 *
 * Explicitly re-arms write interest on WANT_WRITE via fio_force_write_rearm:
 * the raw OpenSSL calls inside fio_tls_client_handshake_step read/write the
 * fd directly through the BIO layer, bypassing fio_write2's own packet-queue
 * (and its accompanying re-arm-on_ready-only-if-still-pending logic)
 * entirely -- so nothing else would ever schedule a follow-up on_ready.
 * WANT_READ needs no equivalent call: deferred_on_data (fio.c) re-arms read
 * interest after every on_data invocation unconditionally, regardless of
 * what this function does.
 */
static void _async_tls_advance(chttp_async_ctx_t *ctx, intptr_t uuid) {
  mutex_lock(ctx->tls_lock);
  fio_tls_handshake_result_e r = fio_tls_client_handshake_step(ctx->tls);
  mutex_unlock(ctx->tls_lock);
  if (r == FIO_TLS_HANDSHAKE_DONE) {
    ctx->state = CHTTP_ASYNC_WRITING;
    _async_tls_try_write(ctx, uuid);
    return;
  }
  if (r == FIO_TLS_HANDSHAKE_ERROR) {
    /* 0 == X509_V_OK by OpenSSL convention; fio_tls.h intentionally does not
     * expose OpenSSL headers to callers, so the raw value is compared
     * directly rather than via the X509_V_OK symbol (mirrors Tier 1's
     * _tls_handshake). */
    mutex_lock(ctx->tls_lock);
    long vr = fio_tls_connection_verify_result(ctx->tls);
    mutex_unlock(ctx->tls_lock);
    _async_fulfill(ctx,
                   vr != 0 ? ccol_http_tls_cert_verification_failed
                           : ccol_http_tls_handshake_failed,
                   NULL);
    fio_force_close(uuid);
    return;
  }
  if (r == FIO_TLS_HANDSHAKE_WANT_WRITE) fio_force_write_rearm(uuid);
  /* FIO_TLS_HANDSHAKE_WANT_READ: nothing further needed, see comment above. */
}

/*
 * Writes as much of ctx->wire[ctx->wire_sent..] as the raw TLS layer will
 * currently accept, tracking partial progress -- fio_tls_connection_write
 * has raw write(2) semantics (a short write is normal, not an error),
 * unlike fio_write2 which tracks this internally for the plain-HTTP path.
 * Called once right after the handshake completes and again from on_ready
 * each time write interest re-arms (see fio_force_write_rearm above).
 */
static void _async_tls_try_write(chttp_async_ctx_t *ctx, intptr_t uuid) {
  while (ctx->wire_sent < ctx->wire_len) {
    mutex_lock(ctx->tls_lock);
    ssize_t n = fio_tls_connection_write(ctx->tls, ctx->wire + ctx->wire_sent,
                                         ctx->wire_len - ctx->wire_sent);
    int werrno = errno; /* captured before unlock, which -- while extremely
                         * unlikely to touch errno itself -- must not be
                         * allowed to intervene between the OpenSSL call and
                         * this read */
    mutex_unlock(ctx->tls_lock);
    if (n > 0) {
      ctx->wire_sent += (size_t)n;
      continue;
    }
    if (n < 0 && (werrno == EWOULDBLOCK || werrno == EAGAIN)) {
      fio_force_write_rearm(uuid);
      return;
    }
    /* n == 0 or a hard error: the connection is dead. facio has no idea --
     * this was a raw OpenSSL call bypassing its own write path -- so close
     * it explicitly, matching the TLS read-side EOF/error handling below.
     * A reused connection that dies before any response byte came back is
     * exactly Tier 1's retry-once scenario -- queue a fresh attempt for the
     * same hop instead of failing the whole chain over it. */
    if (ctx->reused && !ctx->any_bytes_read) {
      _async_retry_hop(ctx);
    } else {
      _async_fulfill(ctx, ccol_http_transfer_aborted, NULL);
    }
    fio_force_close(uuid);
    return;
  }
  /* Fully sent -- this module owns ctx->wire for TLS connections (unlike the
   * plain-HTTP path, where fio_write2 takes ownership), so free it here. */
  _mem_free(ctx->mp, ctx->wire);
  ctx->wire = NULL;
  ctx->state = CHTTP_ASYNC_READING;
}

/* Forward declaration: _async_on_data's redirect-detection branches (below)
 * call this; its full definition comes after _async_connect_task, which it
 * needs in order to queue the next hop. */
static void _async_handle_redirect(chttp_async_ctx_t *ctx, intptr_t uuid,
                                   bool reusable);

/* Reads ctx->state under ctx->idle_lock -- see that field's comment for why
 * a plain unlocked read is not safe here specifically. */
static bool _async_ctx_is_idle(chttp_async_ctx_t *ctx) {
  mutex_lock(ctx->idle_lock);
  bool idle = (ctx->state == CHTTP_ASYNC_IDLE);
  mutex_unlock(ctx->idle_lock);
  return idle;
}

static void _async_on_ready(intptr_t uuid, fio_protocol_s *pr) {
  chttp_async_ctx_t *ctx = (chttp_async_ctx_t *)pr;
  /* See ctx->hop_completed's field comment: a stray on_ready can still
   * arrive after this ctx already reached a terminal outcome (e.g. a
   * connect failure or TLS handshake error detected by on_data), and must
   * not re-run any of the state-transition logic below. */
  if (ctx->hop_completed) return;

  if (_async_ctx_is_idle(ctx)) {
    /* Nothing of ours should ever be pending to flush on an idle pooled
     * connection -- a stray on_ready here means facio has something to
     * report about it regardless (e.g. a write-readiness event racing a
     * hangup), which this connection has no active request to make sense
     * of. Treat it the same as idle-state on_data: no longer usable. */
    fio_force_close(uuid);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_CONNECTING) {
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (getsockopt(fio_uuid2fd(uuid), SOL_SOCKET, SO_ERROR, &soerr, &slen) !=
            0 ||
        soerr != 0) {
      _async_fulfill(ctx, ccol_http_connection_failed, NULL);
      fio_force_close(uuid);
      return;
    }

    if (ctx->is_https) {
      /* Configured cert/key/ca path(s) not being readable is deferred all
       * the way to here (mirroring Tier 1's own _rebuild_tls_ctx_locked
       * comment) rather than failing at chttpclient_set_tls time -- but that
       * deferred failure is checked synchronously in
       * _chttp_do_async_internal (tls_ctx_usable), before any connection is
       * even opened, so reaching here means TLS is genuinely usable. */
      mutex_lock(ctx->tls_lock);
      ctx->tls = fio_tls_connect_create(ctx->chain->tls_ctx, fio_uuid2fd(uuid),
                                        ctx->host, ctx->verify_host ? 1 : 0);
      mutex_unlock(ctx->tls_lock);
      if (!ctx->tls) {
        _async_fulfill(ctx, ccol_not_enough_memory, NULL);
        fio_force_close(uuid);
        return;
      }
      ctx->state = CHTTP_ASYNC_TLS_HANDSHAKING;
      _async_tls_advance(ctx, uuid);
      return;
    }

    /* Plain HTTP: hand the serialized request to facio's write buffer.
     * Uses the COPYING fio_write (not fio_write2's ownership-transferring
     * .data.buffer mode) so ctx->wire stays valid and owned by ctx the
     * whole time, exactly like the TLS path already retains it until
     * wire_sent==wire_len -- required so a dead-reused-connection retry
     * (_async_retry_hop) always has the already-serialized bytes on hand to
     * resend verbatim, mirroring Tier 1's identical retry-with-the-same-
     * wire-buffer behaviour. ctx->wire is freed normally by _async_ctx_free
     * once this ctx is torn down. */
    ctx->state = CHTTP_ASYNC_WRITING;
    if (fio_write(uuid, ctx->wire, ctx->wire_len) == -1) {
      _async_fulfill(ctx, ccol_not_enough_memory, NULL);
      fio_force_close(uuid);
    }
    return;
  }

  if (ctx->state == CHTTP_ASYNC_TLS_HANDSHAKING) {
    _async_tls_advance(ctx, uuid);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_WRITING) {
    if (ctx->tls) {
      _async_tls_try_write(ctx, uuid);
      return;
    }
    /* Plain HTTP: on_ready fires once all pending fio_write calls are
     * flushed -- the request has now been fully sent. */
    ctx->state = CHTTP_ASYNC_READING;
    return;
  }
  /* CHTTP_ASYNC_READING: nothing of ours is pending to flush; a stray
   * on_ready here is a no-op. */
}

static void _async_on_data(intptr_t uuid, fio_protocol_s *pr) {
  chttp_async_ctx_t *ctx = (chttp_async_ctx_t *)pr;

  /* This hop already reached a terminal outcome (fulfilled -- including a
   * TLS handshake failure detected mid-handshake -- or handed off to a
   * redirect hop) via a previous on_data/on_ready invocation for this same
   * uuid; a spurious extra on_data can still arrive afterward (e.g. every
   * route in the test server's mock sends "Connection: close" and closes
   * its end right after writing the response, so the peer's EOF can be
   * observed in a SEPARATE on_data callback from the one that already
   * consumed the response bytes and hit HPE_PAUSED, racing our own
   * subsequent fio_close/fio_force_close call). Re-running the completion
   * logic would be harmless for a plain fulfill (guarded by
   * chain->fulfilled) or a repeat fio_tls_client_handshake_step call on an
   * already-failed handshake (which just returns another, ignored, error),
   * but _async_handle_redirect is NOT idempotent -- it unconditionally
   * queues another hop every time it runs, so without this guard a single
   * hop could fan out into multiple redirect chains sharing the same chain
   * state, corrupting its refcount bookkeeping and, in the worst case
   * observed while stress-testing the CHTTP_MAX_REDIRECTS cap, permanently
   * wedging the caller's ctpool_future_get in a way that never gets
   * fulfilled. Checked before the TLS_HANDSHAKING branch too, since a
   * handshake failure detected by one on_data call must stop a second,
   * racing on_data call from re-entering _async_tls_advance. on_data
   * invocations for the same uuid are serialised by facio itself (see
   * tls_lock's comment on FIO_PR_LOCK_TASK), so a plain bool is sufficient
   * here -- no additional locking needed. */
  if (ctx->hop_completed) return;

  if (_async_ctx_is_idle(ctx)) {
    /* Any activity (or the EOF/error a dead connection eventually produces)
     * on an idle pooled connection means it's no longer safely reusable --
     * force-close it; the IDLE branch of _async_on_close finishes the
     * actual pool removal + teardown once that close completes. */
    fio_force_close(uuid);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_TLS_HANDSHAKING) {
    _async_tls_advance(ctx, uuid);
    return;
  }

  char buf[8192];
  ssize_t n;
  bool eof;

  if (ctx->tls) {
    /* fio_tls_connection_read has raw read(2) semantics: >0 = data, 0 =
     * clean EOF, -1 with EWOULDBLOCK/EAGAIN = try again once readable,
     * anything else = a hard error -- distinct from fio_read's convention
     * below, and (like the write side) never observed by facio itself, so
     * this function must force the close explicitly on EOF/error. Guarded
     * by tls_lock: see that field's comment for why on_data and on_ready
     * can otherwise race on the same SSL* from separate threads. */
    mutex_lock(ctx->tls_lock);
    n = fio_tls_connection_read(ctx->tls, buf, sizeof(buf));
    int rerrno = errno; /* captured before unlock; see _async_tls_try_write's
                         * identical precaution */
    mutex_unlock(ctx->tls_lock);
    if (n < 0) {
      if (rerrno == EWOULDBLOCK || rerrno == EAGAIN)
        return; /* wait; read
                 * interest is
                 * auto-rearmed
                 */
      eof = true;
    } else {
      eof = (n == 0);
    }
  } else {
    n = fio_read(uuid, buf, sizeof(buf));
    if (n == 0) return; /* nothing available right now; wait for next on_data */
    eof = (n < 0);      /* facio has already force-closed the connection itself
                         * (fatal error or EOF) when fio_read returns -1. */
  }
  if (!eof) ctx->any_bytes_read = true;

  if (eof) {
    /* Mirrors Tier 1's own n==0/EOF handling in _chttp_read_response: a
     * clean llhttp_finish with a complete message is valid for responses
     * that signal their end via connection-close rather than
     * Content-Length/chunked framing. A reused connection that produces
     * this before any response byte came back is Tier 1's retry-once
     * scenario, checked BEFORE marking hop_completed/fulfilling -- the
     * whole point is that nothing has failed for the caller yet. */
    llhttp_errno_t fe = llhttp_finish(&ctx->parser);
    bool ok = (fe == HPE_OK && ctx->pctx.message_complete);
    if (!ok && ctx->reused && !ctx->any_bytes_read) {
      _async_retry_hop(ctx); /* marks ctx->hop_completed = true itself */
      /* Plain HTTP: facio already force-closed the connection as part of
       * fio_read's own EOF/error handling; nothing further to do. TLS: this
       * was a raw OpenSSL call facio never saw, so close it explicitly. */
      if (ctx->tls) fio_force_close(uuid);
      return;
    }
    ctx->hop_completed = true;
    if (!ok) {
      _async_fulfill(ctx, ccol_http_transfer_aborted, NULL);
      if (ctx->tls) fio_force_close(uuid);
    } else if (ctx->pctx.will_redirect) {
      /* Peer-closed (rather than Content-Length/chunked) framing is never
       * keep-alive eligible -- matches Tier 1's _chttp_read_response, which
       * hard-codes *keep_alive_out = false for this exact case. */
      _async_handle_redirect(ctx, uuid, false); /* closes uuid either way */
    } else {
      _async_fulfill_success(ctx);
      if (ctx->tls) fio_force_close(uuid);
    }
    return; /* on_close runs the actual teardown either way. */
  }

  llhttp_errno_t err = llhttp_execute(&ctx->parser, buf, (size_t)n);
  if (err == HPE_PAUSED) {
    /* Message complete; llhttp intentionally pauses right after, exactly
     * like Tier 1's own HPE_PAUSED handling. */
    ctx->hop_completed = true;
    const char *pos = llhttp_get_error_pos(&ctx->parser);
    size_t consumed = (size_t)(pos - buf);
    if (consumed < (size_t)n) ctx->pctx.trailing_garbage = true;
    bool keep_alive =
        llhttp_should_keep_alive(&ctx->parser) && !ctx->pctx.trailing_garbage;

    if (ctx->pctx.will_redirect) {
      _async_handle_redirect(ctx, uuid, keep_alive); /* closes/pools uuid */
    } else {
      /* Capture chain (with a temporary extra retain -- see
       * _async_handle_redirect's identical one for why: _async_finish_
       * connection below can trigger a concurrent teardown of THIS ctx's
       * own chain reference on another reactor thread, which could free
       * chain before the _async_fulfill_chain call below runs if nothing
       * else were holding it) and build the response BEFORE calling
       * _async_finish_connection: when keep_alive is true, that call can
       * successfully offer ctx to the idle pool, which resets ctx->chain to
       * NULL and ctx->pctx/ctx->bb for reuse (see _async_idle_pool_offer)
       * as part of a normal, expected, successful outcome -- so both
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
      _async_finish_connection(ctx, uuid, keep_alive);
      _async_fulfill_chain(chain, resp ? ccol_success : ccol_not_enough_memory,
                           resp);
      _async_chain_release(chain);
    }
    return;
  }
  if (err == HPE_USER) {
    ctx->hop_completed = true;
    _async_fulfill(
        ctx,
        ctx->pctx.error ? ccol_not_enough_memory : ccol_http_transfer_aborted,
        NULL);
    fio_force_close(uuid);
    return;
  }
  if (err != HPE_OK) {
    ctx->hop_completed = true;
    _async_fulfill(ctx, ccol_http_transfer_aborted, NULL);
    fio_force_close(uuid);
    return;
  }
  /* else: message not yet complete, wait for more on_data */
}

static void _async_on_close(intptr_t uuid, fio_protocol_s *pr) {
  (void)uuid;
  chttp_async_ctx_t *ctx = (chttp_async_ctx_t *)pr;

  if (_async_ctx_is_idle(ctx)) {
    /* This pooled connection died (or was force-closed above by the
     * IDLE-state on_data/on_ready branches, or by _async_idle_pool_take's
     * staleness eviction). Remove it from the pool if it's still listed
     * there (a no-op if some other path already removed it -- see
     * _async_idle_remove_locked's own comment) and free it; it holds its
     * own engine reference (acquired in _async_idle_pool_offer), not any
     * chain's, since ctx->chain is NULL while idle. */
    struct chttpclient *cli = ctx->cli;
    mutex_lock(cli->lock);
    _async_idle_remove_locked(cli, ctx);
    mutex_unlock(cli->lock);
    _async_ctx_free(ctx);
    _client_engine_release();
    return;
  }

  if (!ctx->hop_completed) {
    if (ctx->timed_out) { /* _Atomic -- plain read is already race-free */
      /* The deadline sweep force-closed this connection because
       * connect_timeout_ms/request_timeout_ms was exceeded -- report the
       * specific timeout error rather than retrying (retrying past an
       * already-blown deadline would only extend the overrun) or falling
       * through to the generic chain-release backstop error below. */
      _async_fulfill(ctx, ccol_timed_out, NULL);
    } else if (ctx->reused && !ctx->any_bytes_read) {
      /* The reused connection died (write failure, or a hangup facio itself
       * detected -- e.g. a pure HUP with nothing left to read, dispatched
       * straight to on_close without ever going through on_data) before any
       * response byte came back: exactly Tier 1's retry-once scenario. */
      _async_retry_hop(ctx); /* marks ctx->hop_completed = true itself */
    }
  }

  /* Frees this hop's connection state and releases its chain reference.
   * _async_chain_release's own backstop (guarded by chain->fulfilled)
   * guarantees the future is eventually fulfilled exactly once even along
   * paths that didn't already call _async_fulfill (e.g. a connection error
   * signalled by facio itself rather than detected by one of our own
   * callbacks) -- but only once this was truly the LAST live hop, i.e. no
   * redirect handoff queued a next one that is still in flight. */
  _async_ctx_teardown(ctx);
}

/* Runs on a g_client_dns_pool worker: resolves DNS and issues the
 * non-blocking connect() (fio_socket does both synchronously), then hands
 * the connection off to the reactor via fio_attach. Never touches the
 * caller's thread. */
static void _async_connect_task(void *arg) {
  chttp_async_ctx_t *ctx = (chttp_async_ctx_t *)arg;

  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)ctx->port);

  intptr_t uuid = fio_socket(ctx->host, port_str, 0);
  if (uuid == -1) {
    _async_fulfill(ctx, ccol_http_connection_failed, NULL);
    _async_ctx_teardown(ctx);
    return;
  }

  /* ctx->uuid/state are _Atomic (see their own field comments), so these
   * plain assignments are already race-free against the deadline sweep --
   * an independent thread, registered against this ctx since before it was
   * ever submitted here (see _async_submit_hop), which can run at any time,
   * including concurrently with this exact assignment. This also covers the
   * case where connect_timeout_ms already expired while this task merely
   * sat queued on g_client_dns_pool (e.g. a saturated pool): the sweep
   * cannot force-close a uuid that doesn't exist yet, so on that path it
   * can only mark ctx->timed_out and wait -- checked here, the moment a
   * uuid finally exists. */
  ctx->uuid = uuid;
  ctx->state = CHTTP_ASYNC_CONNECTING;

  if (ctx->timed_out) {
    /* Never attached, so no protocol/on_close exists for it yet -- close
     * the bare uuid directly and report the timeout ourselves. */
    fio_force_close(uuid);
    _async_fulfill(ctx, ccol_timed_out, NULL);
    _async_ctx_teardown(ctx);
    return;
  }

  ctx->protocol.on_data = _async_on_data;
  ctx->protocol.on_ready = _async_on_ready;
  ctx->protocol.on_close = _async_on_close;
  /* From here on the reactor drives everything via the protocol callbacks.
   * fio_attach's own contract guarantees on_close eventually fires exactly
   * once -- "On error, the new protocol's on_close callback will be called
   * immediately" -- making it the single reliable point where ctx is freed
   * and this hop's chain reference (acquired when this ctx was created) is
   * released, regardless of how this connection ends. */
  fio_attach(uuid, &ctx->protocol);
}

/*
 * Called when a REUSED connection turns out to be dead before any response
 * byte was read (write failure, immediate EOF, or a facio-detected close) --
 * mirrors Tier 1's identical "retry exactly once against a brand-new
 * connection" liveness-probe-failure recovery in chttp_do_internal.
 *
 * Rather than reusing old_ctx's own struct in place for the new attempt
 * (which would require neutralising its protocol callbacks to guard against
 * a stale deferred on_close -- facio's deferred_on_close reads
 * protocol->on_close at DISPATCH time, not at schedule time, so a close
 * already scheduled against the OLD connection before we got here could
 * otherwise fire against whatever the NEW attempt's state has become by the
 * time it actually runs -- and that neutralisation is itself unsafe if some
 * OTHER, independent close was already mid-flight when we tried it), this
 * allocates a fresh ctx and transfers just what the retry needs (the
 * already-serialized wire bytes, host/port, origin_key, the empty response
 * headers map, and hop metadata) out of old_ctx, nulling those fields there
 * so old_ctx's own upcoming normal teardown (via its caller, exactly as if
 * this were an ordinary failure) doesn't double-free them. old_ctx is left
 * otherwise untouched and continues through its NORMAL teardown path
 * afterward -- this function does not free it or touch its chain reference;
 * only ctx->hop_completed is set, both to prevent old_ctx's own on_data/
 * on_close from re-triggering this a second time and because, from
 * old_ctx's own perspective, it genuinely has reached a terminal outcome.
 *
 * Retains a SECOND chain reference for the new ctx (old_ctx keeps its own
 * until its own teardown releases it) -- retaining before old_ctx's release
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

  ctx->chain = chain;
  ctx->cli = old_ctx->cli;
  ctx->hop = old_ctx->hop;
  ctx->cur_method = old_ctx->cur_method;
  ctx->is_https = old_ctx->is_https;
  ctx->verify_host = old_ctx->verify_host;

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
   * caller's callback; buffered uses ctx->bb -- see _async_build_response's
   * own comment for why this must be consistent with what it later does. */
  ctx->pctx.requested_sink_fn =
      chain->write_fn ? chain->write_fn : _sink_buffered;
  ctx->pctx.requested_sink_ctx = chain->write_fn ? chain->write_ctx : &ctx->bb;
  ctx->pctx.headers = old_ctx->pctx.headers; /* empty -- nothing was ever
                                              * parsed into it, since retry
                                              * requires !any_bytes_read */
  old_ctx->pctx.headers = NULL;
  ctx->bb.mp = chain->mp;

  ctx->reused = false; /* the retry itself is a fresh connection */
  ctx->any_bytes_read = false;
  /* A retry always connects fresh (see above), so it needs its own
   * connect_deadline exactly like _async_submit_hop's fresh path -- the
   * chain's overall_deadline is unaffected (it was never per-hop) and
   * continues to apply unchanged across the retry. */
  ctx->connect_deadline = _deadline_make(chain->connect_timeout_ms);

  pthread_once(&g_llhttp_settings_once, _init_llhttp_settings);
  llhttp_init(&ctx->parser, HTTP_RESPONSE, &g_llhttp_settings);
  ctx->parser.data = &ctx->pctx;

  /* Registered BEFORE submitting -- see _async_submit_hop's identical
   * comment for why (a worker could otherwise run this hop to completion
   * and free ctx before this thread registers it). */
  _client_deadline_register(ctx);

  ccol_retval_t sr =
      ctpool_submit(g_client_dns_pool, _async_connect_task, ctx, NULL);
  if (sr != ccol_success) {
    _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    _async_ctx_free(ctx); /* unregisters ctx too */
    _async_chain_release(chain);
  }
}

/*
 * Prepares and submits one hop of a redirect chain -- used for both the very
 * first hop (from _chttp_do_async_internal) and every subsequent redirect
 * hop (from _async_handle_redirect). Retains one chain reference on success
 * (released when this hop's ctx is eventually torn down); on any failure,
 * that retain is released again internally (net effect: no refcount change)
 * and the chain's future is fulfilled with a specific error code before
 * returning, so callers never need their own fallback fulfil-on-failure
 * logic here -- only _async_chain_release's generic backstop remains as a
 * true last resort for paths that can't be reached with a more specific
 * error (e.g. an already-in-flight connection dying).
 *
 * body_data/body_len/body_content_type describe THIS hop's body (usually
 * chain->body_data/len/content_type verbatim, or all-empty once a
 * non-preserving redirect has rewritten the method to GET) -- passed
 * explicitly rather than always read from chain because the caller (Tier 1
 * loop equivalent) is the one that knows, from the previous hop's status
 * code, whether to preserve or drop them.
 *
 * Returns true if the hop was successfully queued.
 */

/*
 * Handles a setup failure (headers-map allocation, request serialisation, or
 * origin_key allocation) that occurs AFTER a reused connection has already
 * been popped from the idle pool -- i.e. ctx->uuid is a live, attached
 * connection, not a not-yet-connected fresh ctx, and ctx->idle_lock is still
 * held per _async_idle_pool_take's return contract. Freeing ctx directly
 * (via _async_ctx_teardown, as a fresh-connect failure at this same point
 * safely would) would leave that still-attached connection's protocol
 * pointing at freed memory, a use-after-free the moment facio next touches
 * the uuid. Instead: report the error, mark the ctx terminal (both because
 * it already has been, and to stop on_close's retry-check from queuing a
 * pointless retry of what is an allocation failure, not a dead connection),
 * release idle_lock (this function is always the last thing _async_submit_
 * hop does with ctx on this path), and let the connection's own normal
 * on_close teardown run. A fresh (not yet connected) ctx never had
 * idle_lock locked and has no such attachment to worry about, and is simply
 * freed directly, exactly like every other pre-connect failure path.
 */
static void _async_submit_hop_fail(chttp_async_ctx_t *ctx, ccol_retval_t rv) {
  chttp_async_chain_t *chain = ctx->chain;
  _async_fulfill_chain(chain, rv, NULL);
  if (ctx->reused) {
    ctx->hop_completed = true;
    mutex_unlock(ctx->idle_lock);
    fio_force_close(ctx->uuid);
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
     * TLS_HANDSHAKING -- a reused one skips straight to WRITING below, so
     * connect_deadline is simply never consulted for it (see the "ASYNC
     * DEADLINE SWEEP" section). */
    ctx->connect_deadline = _deadline_make(chain->connect_timeout_ms);
  }
  /* For the reused case, _async_idle_pool_take has already set ctx->chain
   * and ctx->state (to CHTTP_ASYNC_WRITING) under ctx->idle_lock, and
   * returned with that lock STILL HELD -- see its own doc comment for why.
   * This function must keep it held for everything below, only releasing it
   * once the write attempt (success or failure) at the bottom of this
   * function has actually happened -- ctx is not safe for any concurrent
   * on_data/on_ready/on_close to touch until then. */
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
   * caller's callback; buffered uses ctx->bb -- see _async_build_response's
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

  prv =
      _serialize_request(chain->mp, &hop_req, &url, &ctx->wire, &ctx->wire_len);
  if (prv != ccol_success) {
    _url_free(chain->mp, &url);
    _async_submit_hop_fail(ctx, prv);
    return false;
  }

  if (!ctx->origin_key) {
    /* Always set except on the reused path, where it already carries the
     * origin this connection was pooled under (identical to url.origin_key
     * by construction -- _async_idle_pool_take only ever returns a
     * connection filed under the exact origin_key being looked up). */
    ctx->origin_key = ccol_strdup(chain->mp, url.origin_key);
    if (!ctx->origin_key) {
      _url_free(chain->mp, &url);
      _async_submit_hop_fail(ctx, ccol_not_enough_memory);
      return false;
    }
  }

  if (!reused) {
    ctx->host = ccol_strdup(chain->mp, url.host);
    ctx->port = url.port;
    if (!ctx->host) {
      _url_free(chain->mp, &url);
      _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
      _async_ctx_teardown(ctx);
      return false;
    }
  }
  _url_free(chain->mp, &url);

  pthread_once(&g_llhttp_settings_once, _init_llhttp_settings);
  llhttp_init(&ctx->parser, HTTP_RESPONSE, &g_llhttp_settings);
  ctx->parser.data = &ctx->pctx;

  if (reused) {
    /* Already connected (and, if HTTPS, already handshaked) -- skip
     * CONNECTING/TLS_HANDSHAKING entirely and attempt the write directly,
     * on whichever thread called _async_submit_hop (the caller's own
     * thread for hop 0, or a reactor thread for a redirect hand-off);
     * fio_tls_connection_write/fio_write are both non-blocking regardless
     * of calling thread. ctx->state was already moved to
     * CHTTP_ASYNC_WRITING (under idle_lock, together with ctx->chain)
     * above, before any of the other per-hop fields below were touched. */
    if (ctx->tls) {
      _async_tls_try_write(ctx, ctx->uuid);
    } else if (fio_write(ctx->uuid, ctx->wire, ctx->wire_len) == -1) {
      /* fio_write2_fn (which fio_write calls after its own copy succeeds)
       * returns -1 both for a real OOM AND for an already-invalid uuid --
       * exactly what a reused connection that died while pooled looks like.
       * ctx->reused is always true and ctx->any_bytes_read always false
       * here (this whole branch only runs for a freshly reused, not-yet-
       * written-to connection), so this is unconditionally Tier 1's
       * retry-once scenario -- retrying is harmless even in the rare real-
       * OOM case, since the fresh connection's own first write attempt
       * (ctx->reused == false there) reports ccol_not_enough_memory
       * normally if it fails again. */
      _async_retry_hop(ctx);
      mutex_unlock(ctx->idle_lock);
      fio_force_close(ctx->uuid);
      return true; /* the hop WAS queued (onto an existing connection); its
                    * outcome (retried, or reported) is handled the normal
                    * way, through on_close */
    }
    /* Write attempted (successfully, or via _async_tls_try_write's own
     * retry-on-failure) -- ctx is fully consistent again, so idle_lock can
     * finally be released; any concurrent on_data/on_ready/on_close that
     * was blocked waiting for it now proceeds against a coherent ctx. */
    mutex_unlock(ctx->idle_lock);
    /* This ctx's connection now belongs to the chain, not the idle pool --
     * release the engine reference it was holding while pooled (the
     * chain's own single reference, held for its whole lifetime, covers it
     * from here on). No deadline (re-)registration needed here: this ctx
     * was already registered back when it was first created on its very
     * first (fresh) hop attempt, and registration persists across every
     * idle-pool cycle since -- see _client_deadline_register's own comment.
     * Registering again here would also be unsafe, not just redundant: the
     * moment idle_lock is released, a concurrent on_data/on_ready/on_close
     * on another reactor thread is free to run this hop to completion and
     * free ctx before this thread got a chance to touch it again. */
    _client_engine_release();
    return true;
  }

  /* Registered BEFORE submitting, not after: once ctpool_submit hands ctx to
   * a worker, that worker can connect, run the whole hop to completion, and
   * free ctx before this thread would otherwise get a chance to register it
   * -- registering first guarantees ctx is already safely in the registry
   * for the entire time any other thread can possibly touch it. */
  _client_deadline_register(ctx);

  ccol_retval_t sr =
      ctpool_submit(g_client_dns_pool, _async_connect_task, ctx, NULL);
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
 * closing it -- see _async_finish_connection), and only THEN queues the
 * next hop.
 *
 * That order is deliberate and load-bearing, not cosmetic, for the close
 * case specifically: fio_close (when nothing is still queued to write,
 * always true here since the response was already fully read) synchronously
 * runs fio_force_close -> fio_clear_fd -> close(fd) -> fio_poll_remove_fd on
 * THIS thread before returning. The next hop's fio_socket() call runs on a
 * completely different ctpool worker thread and, on a loopback redirect
 * chain that opens and closes a fresh fd every hop in quick succession, can
 * be handed back the EXACT SAME fd number the kernel just freed. Submitting
 * the next hop first (as an earlier version of this function did) left a
 * window where that worker thread's fio_socket()+fio_attach() could run
 * concurrently with -- and potentially interleave its own epoll_ctl(ADD)
 * around -- this thread's still-in-progress fio_poll_remove_fd's
 * epoll_ctl(DEL) for the SAME fd number, silently leaving the new
 * connection registered nowhere in epoll: fio_attach would appear to
 * succeed (traced -- see git history of this file for the diagnostic
 * session) but no on_ready/on_data callback would ever follow, permanently
 * wedging that hop (and, transitively, the whole chain's future) since
 * nothing else would ever move it forward. Finishing the connection
 * unconditionally FIRST guarantees the old fd's epoll registration is fully
 * torn down (when it's actually being closed) before the new socket() call
 * that might reuse its fd number even has a chance to run; when the
 * connection is pooled instead, no fd is freed at all, so this ordering
 * costs nothing there either.
 *
 * Whether the handoff to the next hop succeeds or not, this connection's
 * job is done either way; on failure, _async_submit_hop has already
 * fulfilled the future with a specific error (or, for a Location that fails
 * to resolve, this function never creates a next hop at all and the
 * chain's refcount is unaffected) -- either way, this ctx's own upcoming
 * on_close is the one guaranteed to notice, via _async_chain_release's
 * backstop, that nothing fulfilled the future, and do so itself with a
 * generic error.
 */
static void _async_handle_redirect(chttp_async_ctx_t *ctx, intptr_t uuid,
                                   bool reusable) {
  chttp_async_chain_t *chain = ctx->chain;
  /* Temporary, extra reference: _async_finish_connection below can trigger
   * (via fio_close's deferred on_close, or a successful idle-pool-offer's
   * own explicit release) a full teardown of THIS ctx's chain reference on
   * a DIFFERENT, concurrently-running reactor thread -- if that happened to
   * be the last live reference, chain would be freed while this function
   * is still using its local `chain` pointer below (in _async_submit_hop's
   * argument and _mem_free(chain->mp, ...)). This is the same class of "a
   * concurrent teardown can free something a synchronous caller still
   * needs" race documented throughout this section, just one level higher
   * than the ctx-local ones -- caught by a real SIGSEGV in
   * _async_fulfill_chain during this feature's own development, since
   * _async_submit_hop's own retain (previously relied on to keep chain
   * alive for the next hop) only happens AFTER _async_finish_connection has
   * already returned, too late to close this specific window. Retaining
   * here first, and releasing only once this function is completely done
   * with `chain`, guarantees it never hits zero out from under us
   * regardless of how fast a concurrent teardown races. */
  _async_chain_retain(chain);

  chttp_url_t base;
  memset(&base, 0, sizeof(base));
  base.is_https = ctx->is_https;
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
   * trigger (via fio_close's deferred on_close, on a concurrent reactor
   * thread) the normal teardown of THIS ctx itself -- freeing it -- so
   * ctx->hop must not be read afterward. The temporary chain retain above
   * only protects `chain`; it does nothing for ctx, which is never safe to
   * touch once its own connection has been handed to _async_finish_
   * connection. A stale read here (caught as heap corruption -- a later,
   * unrelated free() aborting with "free(): invalid size" -- during this
   * feature's own development) is exactly the kind of bug that can surface
   * far away from its actual cause. */
  int next_hop = ctx->hop + 1;

  _async_finish_connection(ctx, uuid, reusable);

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
 * checks TLS usability -- the same three checks Tier 1's chttp_do_internal
 * performs, with the exact same result codes (ccol_invalid_args,
 * whatever _parse_chttp_url returns, ccol_http_tls_handshake_failed).
 *
 * Tier 1 is NOT refactored to call this: its equivalent checks are woven
 * into the per-hop loop of chttp_do_internal, re-run fresh on every hop
 * (a redirect can change URL/scheme hop to hop, so there's no single
 * upfront check to extract there the way Tier 2/3 have -- they only ever
 * need this once, before a chain/future exists at all). Tier 1 already
 * returns fully specific ccol_retval_t codes natively, so extracting its
 * inline logic would touch already-hardened, already-shipped code for no
 * functional benefit.
 *
 * This exists specifically so Tier 3 does not have to accept
 * chttpclient_do_async/_streaming's collapsed "NULL for any pre-queue
 * failure" -- it can call this directly first and return the specific
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
 * TLS unusable, OOM, or the engine failing to start) -- mirrors
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
   * Tier 1's chttp_do_internal does -- fio_tls_dup pins it against a
   * concurrent chttpclient_set_tls freeing/rebuilding it while this request
   * is still using it. Pinned unconditionally (not just for an https first
   * hop) since a redirect chain can hop between http and https, exactly
   * like Tier 1's own tls_ctx local; the pinned reference is released once
   * by _async_chain_release (via fio_tls_destroy) when the whole chain's
   * last hop is torn down. */
  fio_tls_s *tls_ctx;
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
  if (tls_ctx) fio_tls_dup(tls_ctx);
  mutex_unlock(cli->lock);

  if (_client_engine_acquire() != ccol_success) {
    if (tls_ctx) fio_tls_destroy(tls_ctx);
    return NULL;
  }

  char *ferr = NULL;
  ctpool_future *future = ctpool_future_create_detached(&ferr);
  if (!future) {
    if (tls_ctx) fio_tls_destroy(tls_ctx);
    _client_engine_release();
    return NULL;
  }
  /* From here on a future exists and is always returned to the caller (who
   * owns pairing it with exactly one ctpool_future_free) -- any subsequent
   * failure fulfils it with a specific error instead of returning NULL,
   * exactly mirroring this function's previous, single-hop behaviour on a
   * ctpool_submit failure. Captured into a local now: once hop 0 is queued
   * below, a worker may run the request to completion and free the chain
   * via _async_chain_release before this function's own thread runs another
   * instruction -- reading chain->future afterward would be a
   * use-after-free. */
  ctpool_future *f = future;

  chttp_async_chain_t *chain = _async_chain_create(
      mp, cli, future, (chmap)req->headers, req->body.data, req->body.len,
      req->body.content_type, tls_ctx, tls_ctx_usable, verify_host,
      connect_timeout_ms, request_timeout_ms, write_fn, write_ctx);
  if (!chain) {
    if (tls_ctx) fio_tls_destroy(tls_ctx);
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
 * public API -- gated so these symbols do not leak into a production build
 * of libccollections.so (the surrounding engine code itself is no longer
 * gated now that chttpclient_do_async/_streaming are real public callers,
 * but these five functions exist purely for test instrumentation). */
#ifdef RUNNING_UNIT_TESTS
int _chttpclient_engine_ref_count_for_tests(void) {
  pthread_mutex_lock(&g_client_async_mutex);
  int n = g_client_async_users;
  pthread_mutex_unlock(&g_client_async_mutex);
  return n;
}

bool _chttpclient_engine_running_for_tests(void) {
  pthread_mutex_lock(&g_client_async_mutex);
  bool running = g_client_async_running;
  pthread_mutex_unlock(&g_client_async_mutex);
  return running;
}

ccol_retval_t _chttpclient_engine_acquire_for_tests(void) {
  return _client_engine_acquire();
}

void _chttpclient_engine_release_for_tests(void) { _client_engine_release(); }

void _chttpclient_engine_wait_for_quiescence_for_tests(void) {
  _client_engine_wait_for_quiescence();
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
 * free the future and its result before returning -- the caller never sees
 * ctpool_future or chttpcli_async_result_t at all. _chttp_async_preflight_
 * check runs first specifically so a bad URL or unusable TLS config is
 * reported with the same specific code chttpclient_do would use, rather
 * than chttpclient_do_async/_streaming's collapsed NULL for that case (see
 * that helper's own comment). Any OTHER pre-queue failure (OOM, the engine
 * failing to start) still collapses to ccol_unexpected_failure -- Tier 1
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
     * headers just stay NULL -- see _async_build_response) purely so
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

  char *herr = NULL;
  cli->idle_pools =
      chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_pointer, mp, NULL, &herr);
  if (!cli->idle_pools) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate idle pool map");
    mutex_destroy(cli->lock);
    cond_var_destroy(cli->available);
    cond_var_destroy(cli->idle_async_drained);
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

  mutex_lock(cli->lock);
  cli->destroying = true;
  cond_var_broadcast(cli->available);
  while (cli->in_flight_count > 0) cond_var_wait(cli->available, cli->lock);
  mutex_unlock(cli->lock);

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
   * this file). Force-close every currently pooled connection; each one's
   * own IDLE-state on_close removes it from the pool and frees it
   * (releasing its idle-held engine reference) asynchronously -- wait for
   * idle_total_count_async to reach zero before proceeding, since cli is
   * about to be freed below and those deferred teardowns read
   * cli->lock/cli->idle_pools_async. */
  mutex_lock(cli->lock);
  if (cli->idle_pools_async) {
    cmap_iterator *ait = chashmap_begin_iter(cli->idle_pools_async, NULL);
    for (; ait; ait = ait->_next_fn(ait)) {
      cvec list = _read_cvec(ait->val_pair->ptr);
      if (!list) continue;
      size_t n = cvector_elem_count(list);
      for (size_t i = 0; i < n; i++) {
        chttp_async_ctx_t *actx = *(chttp_async_ctx_t **)cvector_at(list, i);
        if (actx->uuid >= 0) fio_force_close(actx->uuid);
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

  if (cli->tls_ctx) fio_tls_destroy(cli->tls_ctx);

  mutex_destroy(cli->lock);
  cond_var_destroy(cli->available);
  cond_var_destroy(cli->idle_async_drained);

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
  fio_tls_s *tls_ctx;
  bool tls_ctx_usable;
  mutex_lock(cli->lock);
  connect_timeout_ms = cli->connect_timeout_ms;
  request_timeout_ms = cli->request_timeout_ms;
  tls_cfg = cli->tls;
  tls_ctx = cli->tls_ctx;
  tls_ctx_usable = cli->tls_ctx_usable;
  if (tls_ctx)
    fio_tls_dup(
        tls_ctx); /* pin: a concurrent set_tls must not free this under us */
  mutex_unlock(cli->lock);

  chttp_deadline_t overall_dl = _deadline_make(request_timeout_ms);

  char *cur_url = ccol_strdup(mp, req->url);
  if (!cur_url) {
    if (tls_ctx) fio_tls_destroy(tls_ctx);
    _slot_release(cli);
    return ccol_not_enough_memory;
  }

  chttp_request_body_t empty_body = CHTTP_NO_BODY;
  chttp_method_t cur_method = req->method;
  chttp_request_body_t cur_body = req->body;

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

    chttp_request_t hop_req = *req;
    hop_req.method = cur_method;
    hop_req.body = cur_body;

    char *wire = NULL;
    size_t wire_len = 0;
    prv = _serialize_request(mp, &hop_req, &url, &wire, &wire_len);
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

    bool keep_alive = false;
    bool any_bytes_read = false;
    prv = _chttp_send_all(&conn, wire, wire_len, &overall_dl);
    if (prv == ccol_success) {
      prv = _chttp_read_response(&conn, &pctx, &overall_dl, &keep_alive,
                                 &any_bytes_read);
    }
    if (prv != ccol_success && reused && !any_bytes_read) {
      /* The reused connection may have died between our liveness probe and
       * this attempt -- either the write silently succeeded into the local
       * send buffer before the peer's close became visible, or the read
       * never produced a single byte. Either way nothing has been parsed or
       * handed to the caller yet, so it is safe to retry exactly once
       * against a brand-new connection. */
      _conn_teardown(mp, &conn);
      prv = _conn_open(mp, &url, url.is_https, tls_ctx, tls_cfg.verify_host,
                       &connect_dl, &overall_dl, &conn);
      if (prv == ccol_success) {
        reused = false;
        prv = _chttp_send_all(&conn, wire, wire_len, &overall_dl);
        if (prv == ccol_success) {
          any_bytes_read = false;
          prv = _chttp_read_response(&conn, &pctx, &overall_dl, &keep_alive,
                                     &any_bytes_read);
        }
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
  if (tls_ctx) fio_tls_destroy(tls_ctx);
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
  g_default_client = create_chttpclient(NULL);
}

chttpcli chttp_default_client(void) {
  pthread_once(&g_default_client_once, _init_default_client);
  return g_default_client;
}

__attribute__((destructor)) static void _cleanup_default_client(void) {
  if (g_default_client) {
    __chttpclient_destroy(g_default_client);
    g_default_client = NULL;
  }
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
