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
#include <chttpclient.h>
#include <ctype.h>
#include <cvector.h>
#include <errno.h>
#include <fio_tls.h>
#include <limits.h>
#include <llhttp.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
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
 */
static ccol_retval_t _chttp_read_response(chttp_conn_t *conn,
                                          chttp_parse_ctx_t *pctx,
                                          chttp_deadline_t *overall,
                                          bool *keep_alive_out) {
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

  char *herr = NULL;
  cli->idle_pools =
      chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_pointer, mp, NULL, &herr);
  if (!cli->idle_pools) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate idle pool map");
    mutex_destroy(cli->lock);
    cond_var_destroy(cli->available);
    _mem_free(mp, cli);
    if (mp) mp->free(mp);
    return NULL;
  }

  if (_rebuild_tls_ctx_locked(cli) != ccol_success) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to build default TLS context");
    __chmap_destroy(cli->idle_pools);
    mutex_destroy(cli->lock);
    cond_var_destroy(cli->available);
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

  if (cli->tls_ctx) fio_tls_destroy(cli->tls_ctx);

  mutex_destroy(cli->lock);
  cond_var_destroy(cli->available);

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

    prv = _chttp_send_all(&conn, wire, wire_len, &overall_dl);
    if (prv != ccol_success && reused) {
      /* The reused connection may have died between our liveness probe and
       * this write; retry exactly once against a brand-new connection. */
      _conn_teardown(mp, &conn);
      prv = _conn_open(mp, &url, url.is_https, tls_ctx, tls_cfg.verify_host,
                       &connect_dl, &overall_dl, &conn);
      if (prv == ccol_success) {
        reused = false;
        prv = _chttp_send_all(&conn, wire, wire_len, &overall_dl);
      }
    }
    _mem_free(mp, wire);
    if (prv != ccol_success) {
      _conn_teardown(mp, &conn);
      _url_free(mp, &url);
      result = prv;
      break;
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
    prv = _chttp_read_response(&conn, &pctx, &overall_dl, &keep_alive);
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
