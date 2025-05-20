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
#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ========================================================================== */
/*                         INTERNAL TYPES                                     */
/* ========================================================================== */

typedef struct {
  CURL *handle;
  bool in_use;
} chttpcli_slot_t;

struct chttpclient {
  mutex_t lock;
  cond_var_t available;
  chttpcli_slot_t *slots;
  size_t pool_cap;
  size_t pool_allocated;
  size_t in_flight_count;
  bool pool_initialized;
  bool destroying;
  size_t configured_pool_size;
  long connect_timeout_ms;
  long request_timeout_ms;
  chttp_tls_config_t tls;
  char *owned_cert_path;
  char *owned_key_path;
  char *owned_ca_bundle_path;
  ccol_memmgmt_procs_t *m_procs;
};

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  bool oom;
  ccol_memmgmt_procs_t *m_procs;
} body_buf_t;

typedef struct {
  chmap headers;
  ccol_memmgmt_procs_t *m_procs;
  bool error;
} header_ctx_t;

typedef struct {
  chttpcli_write_fn fn;
  void *ctx;
} stream_ctx_t;

typedef struct {
  const char *data;
  size_t pos;
  size_t len;
} read_buf_t;

/* ========================================================================== */
/*                         GLOBAL STATE                                       */
/* ========================================================================== */

static chttpcli g_default_client = NULL;
static pthread_once_t g_default_client_once = PTHREAD_ONCE_INIT;

/* ========================================================================== */
/*                         CONSTRUCTOR / DESTRUCTOR                           */
/* ========================================================================== */

/* Lazy curl init: runs exactly once on the first chttpclient_create call.
 * _curl_initialized gates the destructor so curl_global_cleanup is a no-op
 * in processes that never create an HTTP client.
 * Destructor ordering: _curl_global_cleanup is defined FIRST so it runs
 * SECOND; _cleanup_default_client is defined SECOND so it runs FIRST.
 * This guarantees the default client is torn down before curl_global_cleanup
 * is invoked -- same ordering as before, now simply guarded by the flag. */
static pthread_once_t _curl_once = PTHREAD_ONCE_INIT;
static atomic_bool _curl_initialized = false;

static void _do_curl_global_init(void) {
  curl_global_init(CURL_GLOBAL_ALL);
  atomic_store(&_curl_initialized, true);
}

__attribute__((destructor)) static void _curl_global_cleanup(void) {
  if (!atomic_load(&_curl_initialized)) return;
  curl_global_cleanup();
}

/* ========================================================================== */
/*                         POOL HELPERS                                       */
/* ========================================================================== */

static ccol_retval_t _pool_init_locked(struct chttpclient *cli) {
  size_t n = cli->configured_pool_size;
  if (n == 0) {
    long np = sysconf(_SC_NPROCESSORS_ONLN);
    n = (np > 0) ? (size_t)np : 1;
  }

  cli->slots =
      (chttpcli_slot_t *)_mem_calloc(cli->m_procs, n, sizeof(chttpcli_slot_t));
  if (!cli->slots) return ccol_not_enough_memory;

  cli->pool_cap = n;
  cli->pool_allocated = n;
  cli->pool_initialized = true;
  return ccol_success;
}

static ccol_retval_t _pool_acquire(struct chttpclient *cli,
                                   size_t *slot_idx_out, CURL **handle_out) {
  mutex_lock(cli->lock);

  /* Fast-path: reject immediately if the client is already being torn down,
   * before we waste time initialising the pool. */
  if (cli->destroying) {
    mutex_unlock(cli->lock);
    return ccol_not_permitted;
  }

  if (!cli->pool_initialized) {
    ccol_retval_t rv = _pool_init_locked(cli);
    if (rv != ccol_success) {
      mutex_unlock(cli->lock);
      return rv;
    }
  }

  size_t idx = (size_t)-1;
  while (true) {
    if (cli->destroying) {
      mutex_unlock(cli->lock);
      return ccol_not_permitted;
    }
    for (size_t i = 0; i < cli->pool_cap; i++) {
      if (!cli->slots[i].in_use) {
        idx = i;
        break;
      }
    }
    if (idx != (size_t)-1) break;
    cond_var_wait(cli->available, cli->lock);
  }

  if (!cli->slots[idx].handle) {
    cli->slots[idx].handle = curl_easy_init();
    if (!cli->slots[idx].handle) {
      mutex_unlock(cli->lock);
      return ccol_not_enough_memory;
    }
  }

  cli->slots[idx].in_use = true;
  cli->in_flight_count++;
  *handle_out = cli->slots[idx].handle;
  *slot_idx_out = idx;
  mutex_unlock(cli->lock);
  return ccol_success;
}

/*
 * _pool_release_slot: marks the slot as no longer in use and wakes up
 * threads waiting to acquire a slot.  Does NOT decrement in_flight_count so
 * that a concurrent __chttpclient_destroy cannot free cli or its m_procs
 * before the caller has finished using them.
 */
static void _pool_release_slot(struct chttpclient *cli, size_t slot_idx) {
  mutex_lock(cli->lock);
  if (slot_idx < cli->pool_cap) {
    cli->slots[slot_idx].in_use = false;
    cond_var_signal(cli->available);
  } else {
    /* Exiled slot: pool was shrunk while this request was in flight. */
    curl_easy_cleanup(cli->slots[slot_idx].handle);
    cli->slots[slot_idx].handle = NULL;
    cli->slots[slot_idx].in_use = false;
    /* No cond_var_signal here: this index is beyond pool_cap and cannot be
     * reused by a waiting thread, so waking them would be spurious. */
  }
  mutex_unlock(cli->lock);
}

/*
 * _pool_finish: decrements in_flight_count.  Call this only AFTER all
 * post-request cleanup that touches cli->m_procs (or memory allocated from
 * it) is complete.  After this returns, cli and its m_procs may have been
 * freed by a concurrent __chttpclient_destroy; do not access either.
 */
static void _pool_finish(struct chttpclient *cli) {
  mutex_lock(cli->lock);
  cli->in_flight_count--;
  if (cli->in_flight_count == 0) cond_var_broadcast(cli->available);
  mutex_unlock(cli->lock);
}

/* Combined helper for call sites that do no post-release cleanup. */
static void _pool_release(struct chttpclient *cli, size_t slot_idx) {
  _pool_release_slot(cli, slot_idx);
  _pool_finish(cli);
}

/* ========================================================================== */
/*                         CURL CALLBACKS                                     */
/* ========================================================================== */

static size_t _write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
  body_buf_t *bb = (body_buf_t *)ud;
  size_t len = size * nmemb;
  if (len == 0) return 0;

  if (bb->len + len + 1 > bb->cap) {
    size_t new_cap = bb->cap ? bb->cap * 2 : 4096;
    while (new_cap < bb->len + len + 1) new_cap *= 2;
    char *nb = (char *)_mem_realloc(bb->m_procs, bb->buf, new_cap);
    if (!nb) {
      bb->oom = true;
      return 0;
    }
    bb->buf = nb;
    bb->cap = new_cap;
  }

  memcpy(bb->buf + bb->len, ptr, len);
  bb->len += len;
  bb->buf[bb->len] = '\0';
  return len;
}

static size_t _header_cb(char *buf, size_t size, size_t nmemb, void *ud) {
  header_ctx_t *ctx = (header_ctx_t *)ud;
  size_t total = size * nmemb;

  /* New response starting (redirect or initial): clear accumulated headers so
   * intermediate redirect headers do not leak into the final response map. */
  if (total >= 5 && memcmp(buf, "HTTP/", 5) == 0) {
    chmap_reset(ctx->headers, 0);
    return total;
  }
  /* Skip HTTP/2 pseudo-headers. */
  if (total > 0 && buf[0] == ':') return total;
  /* Skip empty CRLF line. */
  if (total <= 2) return total;

  /* Find colon separator. */
  const char *colon = (const char *)memchr(buf, ':', total);
  if (!colon) return total;

  size_t name_len = (size_t)(colon - buf);
  if (name_len == 0 || name_len >= 256) return total;

  /* Lowercase the header name into a stack buffer. */
  char name_buf[256];
  for (size_t i = 0; i < name_len; i++)
    name_buf[i] = (char)tolower((unsigned char)buf[i]);
  name_buf[name_len] = '\0';

  /* Strip leading whitespace from value, trailing CRLF. */
  const char *val_start = colon + 1;
  const char *val_end = buf + total;
  while (val_start < val_end && (*val_start == ' ' || *val_start == '\t'))
    val_start++;
  while (val_end > val_start &&
         (*(val_end - 1) == '\r' || *(val_end - 1) == '\n'))
    val_end--;

  size_t val_len = (size_t)(val_end - val_start);

  /* Allocate a NUL-terminated value buffer. */
  char *val_buf = (char *)_mem_alloc(ctx->m_procs, val_len + 1);
  if (!val_buf) {
    ctx->error = true;
    return 0;
  }
  memcpy(val_buf, val_start, val_len);
  val_buf[val_len] = '\0';

  cmap_pair kp = {.ptr = name_buf, .size = name_len + 1};
  cmap_pair vp = {.ptr = val_buf, .size = val_len + 1};

  ccol_retval_t rv = chmap_insert_elem(ctx->headers, &kp, &vp);
  _mem_free(ctx->m_procs, val_buf);

  if (rv != ccol_success && rv != ccol_key_already_present) {
    ctx->error = true;
    return 0;
  }

  return total;
}

static size_t _stream_write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
  stream_ctx_t *sc = (stream_ctx_t *)ud;
  size_t len = size * nmemb;
  return sc->fn(ptr, len, sc->ctx);
}

static size_t _read_cb(char *buf, size_t size, size_t nmemb, void *ud) {
  read_buf_t *rb = (read_buf_t *)ud;
  size_t avail = rb->len - rb->pos;
  size_t n = size * nmemb < avail ? size * nmemb : avail;
  if (n == 0) return 0;
  memcpy(buf, rb->data + rb->pos, n);
  rb->pos += n;
  return n;
}

/* ========================================================================== */
/*                         CURL CONFIGURATION HELPERS                         */
/* ========================================================================== */

static void _set_method(CURL *h, const chttp_request_t *req, read_buf_t *rb) {
  switch (req->method) {
    case CHTTP_GET:
      curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
      break;
    case CHTTP_HEAD:
      curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
      break;
    case CHTTP_POST:
      curl_easy_setopt(h, CURLOPT_POST, 1L);
      if (req->body.data && req->body.len > 0) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, (const char *)req->body.data);
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE,
                         (curl_off_t)req->body.len);
      } else {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)0);
      }
      break;
    case CHTTP_PUT:
      curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
      rb->data = (req->body.data && req->body.len > 0)
                     ? (const char *)req->body.data
                     : NULL;
      rb->len = rb->data ? req->body.len : 0;
      rb->pos = 0;
      if (rb->data) {
        curl_easy_setopt(h, CURLOPT_READFUNCTION, _read_cb);
        curl_easy_setopt(h, CURLOPT_READDATA, rb);
      }
      curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, (curl_off_t)rb->len);
      break;
    case CHTTP_PATCH:
      curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "PATCH");
      if (req->body.data && req->body.len > 0) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, (const char *)req->body.data);
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE,
                         (curl_off_t)req->body.len);
      } else {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)0);
      }
      break;
    case CHTTP_DELETE:
      curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");
      break;
    case CHTTP_OPTIONS:
      curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "OPTIONS");
      break;
    default:
      curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
      break;
  }
}

/*
 * Return true if the headers map already contains a key that compares equal
 * to "content-type" case-insensitively.  This intentionally scans all keys
 * rather than doing a map lookup so that borrowed maps (e.g. from
 * chttp_run_query) with mixed-case keys like "Content-Type" are also detected.
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

/*
 * Build a curl_slist from request headers and optionally inject Content-Type
 * from the body.  The returned slist is owned by the caller and must be freed
 * with curl_slist_free_all.  Returns NULL if there are no headers to send.
 */
static struct curl_slist *_build_req_headers(const chttp_request_t *req,
                                             ccol_memmgmt_procs_t *mp,
                                             bool *oom_out) {
  *oom_out = false;
  struct curl_slist *list = NULL;

  if (req->headers) {
    cmap_iterator *it = chashmap_begin_iter((chmap)req->headers, NULL);
    for (; it; it = it->_next_fn(it)) {
      const char *name = (const char *)it->key_pair->ptr;
      const char *val = (const char *)it->val_pair->ptr;
      size_t nlen = strlen(name);
      size_t vlen = strlen(val);
      /* name + ": " + val + NUL */
      size_t hlen = nlen + 2 + vlen + 1;
      char *hdr = (char *)_mem_alloc(mp, hlen);
      if (!hdr) {
        ccol_iter_destroy(it);
        curl_slist_free_all(list);
        *oom_out = true;
        return NULL;
      }
      memcpy(hdr, name, nlen);
      hdr[nlen] = ':';
      hdr[nlen + 1] = ' ';
      memcpy(hdr + nlen + 2, val, vlen);
      hdr[hlen - 1] = '\0';

      struct curl_slist *nl = curl_slist_append(list, hdr);
      _mem_free(mp, hdr);
      if (!nl) {
        ccol_iter_destroy(it);
        curl_slist_free_all(list);
        *oom_out = true;
        return NULL;
      }
      list = nl;
    }
  }

  /* Auto-inject Content-Type from body if not already set. */
  if (req->body.data && req->body.len > 0 && req->body.content_type) {
    if (!_map_has_content_type((chmap)req->headers)) {
      size_t ct_len = strlen(req->body.content_type);
      size_t hlen = sizeof("content-type: ") - 1 + ct_len + 1;
      char *hdr = (char *)_mem_alloc(mp, hlen);
      if (!hdr) {
        curl_slist_free_all(list);
        *oom_out = true;
        return NULL;
      }
      memcpy(hdr, "content-type: ", sizeof("content-type: ") - 1);
      memcpy(hdr + sizeof("content-type: ") - 1, req->body.content_type,
             ct_len);
      hdr[hlen - 1] = '\0';
      struct curl_slist *nl = curl_slist_append(list, hdr);
      _mem_free(mp, hdr);
      if (!nl) {
        curl_slist_free_all(list);
        *oom_out = true;
        return NULL;
      }
      list = nl;
    }
  }

  /* PATCH now uses CURLOPT_POSTFIELDS, which causes libcurl to auto-inject
   * "Content-Type: application/x-www-form-urlencoded".  Suppress it whenever
   * neither body.content_type nor an explicit header provided a Content-Type,
   * so that a PATCH without a content-type sends none to the server. */
  if (req->method == CHTTP_PATCH) {
    bool patch_has_ct =
        (req->body.data && req->body.len > 0 && req->body.content_type) ||
        _map_has_content_type((chmap)req->headers);
    if (!patch_has_ct) {
      struct curl_slist *nl = curl_slist_append(list, "Content-Type:");
      if (!nl) {
        curl_slist_free_all(list);
        *oom_out = true;
        return NULL;
      }
      list = nl;
    }
  }

  return list;
}

static ccol_retval_t _map_curl_error(CURLcode rc) {
  switch (rc) {
    case CURLE_OK:
      return ccol_success;
    case CURLE_OPERATION_TIMEDOUT:
      return ccol_timed_out;
    case CURLE_OUT_OF_MEMORY:
      return ccol_not_enough_memory;

    /* ---- invalid URL / unsupported scheme ---------------------------------
     */
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_URL_MALFORMAT:
      return ccol_http_invalid_url;

    /* ---- DNS resolution failure ------------------------------------------ */
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
      return ccol_http_host_resolution_failed;

    /* ---- TCP / transport connection failure --------------------------------
     */
    case CURLE_COULDNT_CONNECT:
    case CURLE_INTERFACE_FAILED: /* local socket binding failed */
#if CURL_AT_LEAST_VERSION(7, 30, 0)
    case CURLE_NO_CONNECTION_AVAILABLE:
#endif
#if CURL_AT_LEAST_VERSION(7, 69, 0)
    case CURLE_QUIC_CONNECT_ERROR:
    case CURLE_PROXY: /* proxy CONNECT handshake failed */
#endif
      return ccol_http_connection_failed;

    /* ---- redirect limit ---------------------------------------------------
     */
    case CURLE_TOO_MANY_REDIRECTS:
      return ccol_http_too_many_redirects;

    /* ---- TLS handshake failure --------------------------------------------
     */
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_ENGINE_NOTFOUND:
    case CURLE_SSL_ENGINE_SETFAILED:
    case CURLE_SSL_ENGINE_INITFAILED:
    case CURLE_USE_SSL_FAILED:
    case CURLE_SSL_SHUTDOWN_FAILED:
#if CURL_AT_LEAST_VERSION(8, 8, 0)
    case CURLE_ECH_REQUIRED: /* Encrypted Client Hello failed */
#endif
      return ccol_http_tls_handshake_failed;

    /* ---- TLS certificate verification failure -----------------------------
     */
    case CURLE_PEER_FAILED_VERIFICATION: /* also covers CURLE_SSL_CACERT alias
                                          */
    case CURLE_SSL_CERTPROBLEM:          /* client certificate problem */
    case CURLE_SSL_CACERT_BADFILE:       /* CA bundle file unreadable */
    case CURLE_SSL_CRL_BADFILE:          /* CRL file unreadable */
    case CURLE_SSL_ISSUER_ERROR:         /* issuer check failed */
#if CURL_AT_LEAST_VERSION(7, 41, 0)
    case CURLE_SSL_INVALIDCERTSTATUS: /* OCSP / cert status invalid */
#endif
#if CURL_AT_LEAST_VERSION(7, 44, 0)
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH: /* public-key pinning mismatch */
#endif
#if CURL_AT_LEAST_VERSION(7, 77, 0)
    case CURLE_SSL_CLIENTCERT: /* server requires a client cert */
#endif
      return ccol_http_tls_cert_verification_failed;

    /* ---- transfer-level failure -------------------------------------------
     */
    case CURLE_HTTP_RETURNED_ERROR: /* server returned 4xx/5xx
                                       (CURLOPT_FAILONERROR) */
    case CURLE_WEIRD_SERVER_REPLY:  /* server sent unparseable response */
    case CURLE_HTTP2:               /* HTTP/2 framing layer error */
    case CURLE_PARTIAL_FILE:        /* connection dropped mid-transfer */
    case CURLE_UPLOAD_FAILED:       /* HTTP PUT/PATCH upload command failed */
    case CURLE_READ_ERROR:          /* upload read callback signalled abort */
    case CURLE_WRITE_ERROR:         /* write callback returned error */
    case CURLE_RANGE_ERROR: /* HTTP range request not satisfied by server */
    case CURLE_SEND_FAIL_REWIND:     /* upload rewind failed */
    case CURLE_BAD_CONTENT_ENCODING: /* unrecognised Transfer-Encoding */
    case CURLE_FILESIZE_EXCEEDED: /* response exceeded CURLOPT_MAXFILESIZE limit
                                   */
    case CURLE_GOT_NOTHING:       /* server sent empty response */
    case CURLE_SEND_ERROR:        /* socket send failed */
    case CURLE_RECV_ERROR:        /* socket recv failed */
    case CURLE_ABORTED_BY_CALLBACK: /* streaming write_fn returned short */
    case CURLE_CHUNK_FAILED: /* chunk-data callback error in chunked transfer */
#if CURL_AT_LEAST_VERSION(7, 49, 0)
    case CURLE_HTTP2_STREAM: /* HTTP/2 stream error */
#endif
#if CURL_AT_LEAST_VERSION(7, 66, 0)
    case CURLE_AUTH_ERROR: /* transport-level auth failure */
#endif
#if CURL_AT_LEAST_VERSION(7, 68, 0)
    case CURLE_HTTP3: /* HTTP/3 layer error */
#endif
#if CURL_AT_LEAST_VERSION(7, 84, 0)
    case CURLE_UNRECOVERABLE_POLL: /* fatal poll/select error mid-transfer */
#endif
#if CURL_AT_LEAST_VERSION(8, 6, 0)
    case CURLE_TOO_LARGE: /* response body or value too large */
#endif
      return ccol_http_transfer_aborted;

    default:
      return ccol_unexpected_failure;
  }
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

  /* Allocate a private copy of the m_procs struct. */
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

  /* Lazily initialise the headers map. */
  if (!req->headers) {
    char *err = NULL;
    chmap hm = chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                                 ccol_string, req->_m_procs, NULL, &err);
    if (!hm) return ccol_not_enough_memory;
    req->headers = hm;
  }

  /* Lowercase the header name. */
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
/*                         CLIENT CONSTRUCTORS                                */
/* ========================================================================== */

chttpcli create_chttpclient_mp(ccol_memmgmt_procs_t *mprocs, char **err_str) {
  if (mprocs && !ccol_verify_memmgmt_procs(mprocs, err_str)) return NULL;
  pthread_once(&_curl_once, _do_curl_global_init);

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
  return cli;
}

/* ========================================================================== */
/*                         CLIENT CONFIGURATION                               */
/* ========================================================================== */

ccol_retval_t chttpclient_set_pool_size(chttpcli cli, size_t n) {
  if (!cli) return ccol_invalid_args;

  mutex_lock(cli->lock);
  cli->configured_pool_size = n;

  if (cli->pool_initialized) {
    size_t new_cap;
    if (n == 0) {
      long np = sysconf(_SC_NPROCESSORS_ONLN);
      new_cap = (np > 0) ? (size_t)np : 1;
    } else {
      new_cap = n;
    }

    if (new_cap > cli->pool_cap) {
      if (new_cap > cli->pool_allocated) {
        chttpcli_slot_t *ns = (chttpcli_slot_t *)_mem_realloc(
            cli->m_procs, cli->slots, new_cap * sizeof(chttpcli_slot_t));
        if (!ns) {
          mutex_unlock(cli->lock);
          return ccol_not_enough_memory;
        }
        memset(ns + cli->pool_allocated, 0,
               (new_cap - cli->pool_allocated) * sizeof(chttpcli_slot_t));
        cli->slots = ns;
        cli->pool_allocated = new_cap;
      }
      cli->pool_cap = new_cap;
      cond_var_broadcast(cli->available);
    } else {
      /* Shrink: free idle CURL handles in [new_cap, pool_cap) before reducing.
       * In-flight handles in that range are exiled and cleaned up by
       * _pool_release_slot when their requests land. */
      for (size_t i = new_cap; i < cli->pool_cap; i++) {
        if (!cli->slots[i].in_use && cli->slots[i].handle) {
          curl_easy_cleanup(cli->slots[i].handle);
          cli->slots[i].handle = NULL;
        }
      }
      cli->pool_cap = new_cap;
    }
  }

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
    mutex_unlock(cli->lock);
    return ccol_success;
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
  mutex_unlock(cli->lock);
  return ccol_success;

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

  for (size_t i = 0; i < cli->pool_allocated; i++) {
    if (cli->slots[i].handle) {
      curl_easy_cleanup(cli->slots[i].handle);
      cli->slots[i].handle = NULL;
    }
  }

  mutex_destroy(cli->lock);
  cond_var_destroy(cli->available);

  ccol_memmgmt_procs_t *mp = cli->m_procs;
  _mem_free(mp, cli->owned_cert_path);
  _mem_free(mp, cli->owned_key_path);
  _mem_free(mp, cli->owned_ca_bundle_path);
  _mem_free(mp, cli->slots);
  _mem_free(mp, cli);
  if (mp) mp->free(mp);
}

/* ========================================================================== */
/*                         REQUEST EXECUTION                                  */
/* ========================================================================== */

ccol_retval_t chttpclient_do(chttpcli cli, const chttp_request_t *req,
                             chttpcli_response **resp_out) {
  if (!cli || !req || !resp_out) return ccol_invalid_args;

  size_t slot_idx;
  CURL *h;
  ccol_retval_t rv = _pool_acquire(cli, &slot_idx, &h);
  if (rv != ccol_success) return rv;

  /* Snapshot m_procs and configuration.  mp is captured here so the
   * post-curl cleanup path can use it after _pool_release_slot (which frees
   * the slot for other callers but does NOT yet decrement in_flight_count).
   * _pool_finish, called at the very end of each return path, decrements
   * in_flight_count; only then may a concurrent __chttpclient_destroy free
   * cli and mp.  Therefore mp is guaranteed live until _pool_finish. */
  ccol_memmgmt_procs_t *mp = cli->m_procs;
  long connect_timeout_ms, request_timeout_ms;
  chttp_tls_config_t tls;
  char *tls_cert = NULL, *tls_key = NULL, *tls_ca = NULL;
  mutex_lock(cli->lock);
  connect_timeout_ms = cli->connect_timeout_ms;
  request_timeout_ms = cli->request_timeout_ms;
  tls = cli->tls;
  /* Deep-copy TLS path strings under the lock so a concurrent
   * chttpclient_set_tls cannot free the originals while we use them. */
  if (tls.cert_path) tls_cert = ccol_strdup(mp, tls.cert_path);
  if (tls.key_path) tls_key = ccol_strdup(mp, tls.key_path);
  if (tls.ca_bundle_path) tls_ca = ccol_strdup(mp, tls.ca_bundle_path);
  mutex_unlock(cli->lock);
  if ((tls.cert_path && !tls_cert) || (tls.key_path && !tls_key) ||
      (tls.ca_bundle_path && !tls_ca)) {
    _mem_free(mp, tls_cert);
    _mem_free(mp, tls_key);
    _mem_free(mp, tls_ca);
    _pool_release(cli, slot_idx);
    return ccol_not_enough_memory;
  }
  tls.cert_path = tls_cert;
  tls.key_path = tls_key;
  tls.ca_bundle_path = tls_ca;

  /* Allocate response. */
  chttpcli_response *resp =
      (chttpcli_response *)_mem_calloc(mp, 1, sizeof(chttpcli_response));
  if (!resp) {
    _mem_free(mp, tls_cert);
    _mem_free(mp, tls_key);
    _mem_free(mp, tls_ca);
    _pool_release(cli, slot_idx);
    return ccol_not_enough_memory;
  }
  resp->_m_procs = mp;

  /* Allocate response headers map. */
  char *map_err = NULL;
  chmap resp_hdrs =
      chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_string, mp, NULL, &map_err);
  if (!resp_hdrs) {
    _mem_free(mp, tls_cert);
    _mem_free(mp, tls_key);
    _mem_free(mp, tls_ca);
    _mem_free(mp, resp);
    _pool_release(cli, slot_idx);
    return ccol_not_enough_memory;
  }
  resp->headers = resp_hdrs;

  body_buf_t bb = {.buf = NULL, .len = 0, .cap = 0, .m_procs = mp};
  header_ctx_t hctx = {.headers = resp_hdrs, .m_procs = mp, .error = false};
  read_buf_t rb = {0};

  curl_easy_reset(h);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_URL, req->url);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, _write_cb);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &bb);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, _header_cb);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &hctx);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, request_timeout_ms);
  curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, tls.verify_peer ? 1L : 0L);
  curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, tls.verify_host ? 2L : 0L);
  if (tls.ca_bundle_path)
    curl_easy_setopt(h, CURLOPT_CAINFO, tls.ca_bundle_path);
  if (tls.cert_path) curl_easy_setopt(h, CURLOPT_SSLCERT, tls.cert_path);
  if (tls.key_path) curl_easy_setopt(h, CURLOPT_SSLKEY, tls.key_path);

  _set_method(h, req, &rb);

  bool hdrs_oom = false;
  struct curl_slist *req_hdrs =
      _build_req_headers(req, req->_m_procs, &hdrs_oom);
  if (hdrs_oom) {
    _mem_free(mp, tls_cert);
    _mem_free(mp, tls_key);
    _mem_free(mp, tls_ca);
    _pool_release_slot(cli, slot_idx);
    __chmap_destroy(resp_hdrs);
    _mem_free(mp, resp);
    _pool_finish(cli);
    return ccol_not_enough_memory;
  }
  if (req_hdrs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, req_hdrs);

  CURLcode rc = curl_easy_perform(h);
  _mem_free(mp, tls_cert);
  _mem_free(mp, tls_key);
  _mem_free(mp, tls_ca);

  long status = 0;
  if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);

  if (req_hdrs) curl_slist_free_all(req_hdrs);
  /* Release the slot so other callers can acquire it immediately.
   * in_flight_count is NOT decremented yet, so a concurrent destroy cannot
   * free cli or mp before the cleanup below completes. */
  _pool_release_slot(cli, slot_idx);

  if (rc != CURLE_OK) {
    _mem_free(mp, bb.buf);
    __chmap_destroy(resp_hdrs);
    _mem_free(mp, resp);
    /* _pool_finish must be called last; cli/mp may be freed after it. */
    _pool_finish(cli);
    /* hctx.error means _header_cb returned 0 -> CURLE_WRITE_ERROR, so this
     * branch covers both body-buffer OOM (bb.oom) and header-map OOM. */
    if (bb.oom || hctx.error) return ccol_not_enough_memory;
    return _map_curl_error(rc);
  }

  resp->status_code = (int)status;
  resp->body = bb.buf;
  resp->body_len = bb.len;
  *resp_out = resp;
  _pool_finish(cli);
  return ccol_success;
}

ccol_retval_t chttpclient_do_streaming(chttpcli cli, const chttp_request_t *req,
                                       chttpcli_write_fn write_fn,
                                       void *write_ctx, int *status_code_out) {
  if (!cli || !req || !write_fn) return ccol_invalid_args;

  size_t slot_idx;
  CURL *h;
  ccol_retval_t rv = _pool_acquire(cli, &slot_idx, &h);
  if (rv != ccol_success) return rv;

  ccol_memmgmt_procs_t *mp = cli->m_procs;
  long connect_timeout_ms, request_timeout_ms;
  chttp_tls_config_t tls;
  char *tls_cert = NULL, *tls_key = NULL, *tls_ca = NULL;
  mutex_lock(cli->lock);
  connect_timeout_ms = cli->connect_timeout_ms;
  request_timeout_ms = cli->request_timeout_ms;
  tls = cli->tls;
  if (tls.cert_path) tls_cert = ccol_strdup(mp, tls.cert_path);
  if (tls.key_path) tls_key = ccol_strdup(mp, tls.key_path);
  if (tls.ca_bundle_path) tls_ca = ccol_strdup(mp, tls.ca_bundle_path);
  mutex_unlock(cli->lock);
  if ((tls.cert_path && !tls_cert) || (tls.key_path && !tls_key) ||
      (tls.ca_bundle_path && !tls_ca)) {
    _mem_free(mp, tls_cert);
    _mem_free(mp, tls_key);
    _mem_free(mp, tls_ca);
    _pool_release(cli, slot_idx);
    return ccol_not_enough_memory;
  }
  tls.cert_path = tls_cert;
  tls.key_path = tls_key;
  tls.ca_bundle_path = tls_ca;

  stream_ctx_t sc = {.fn = write_fn, .ctx = write_ctx};
  read_buf_t rb = {0};

  curl_easy_reset(h);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_URL, req->url);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, _stream_write_cb);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &sc);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, request_timeout_ms);
  curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, tls.verify_peer ? 1L : 0L);
  curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, tls.verify_host ? 2L : 0L);
  if (tls.ca_bundle_path)
    curl_easy_setopt(h, CURLOPT_CAINFO, tls.ca_bundle_path);
  if (tls.cert_path) curl_easy_setopt(h, CURLOPT_SSLCERT, tls.cert_path);
  if (tls.key_path) curl_easy_setopt(h, CURLOPT_SSLKEY, tls.key_path);

  _set_method(h, req, &rb);

  bool hdrs_oom = false;
  struct curl_slist *req_hdrs =
      _build_req_headers(req, req->_m_procs, &hdrs_oom);
  if (hdrs_oom) {
    _mem_free(mp, tls_cert);
    _mem_free(mp, tls_key);
    _mem_free(mp, tls_ca);
    _pool_release(cli, slot_idx);
    return ccol_not_enough_memory;
  }
  if (req_hdrs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, req_hdrs);

  CURLcode rc = curl_easy_perform(h);
  _mem_free(mp, tls_cert);
  _mem_free(mp, tls_key);
  _mem_free(mp, tls_ca);

  long status = 0;
  if (rc == CURLE_OK && status_code_out)
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);

  if (req_hdrs) curl_slist_free_all(req_hdrs);
  _pool_release(cli, slot_idx);

  if (rc == CURLE_OK && status_code_out) *status_code_out = (int)status;
  return _map_curl_error(rc);
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
