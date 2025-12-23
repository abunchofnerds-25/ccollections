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
#include <chttpserver.h>
#include <cthreadpool.h>
#include <fio.h>
#include <fio_tls.h>
#include <fiobj.h>
#include <http.h>
#include <http1.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ========================================================================== */
/*                         INTERNAL TYPES                                     */
/* ========================================================================== */

/* Forward declaration required for the self-referential next pointer below. */
typedef struct chttpsvr_mw_node chttpsvr_mw_node_t;

/** Linked list node for middleware entries.
 *
 * next is a plain pointer.  All reads traverse the list while holding the
 * routes_lock read-lock; all writes append under the write-lock.  The lock
 * provides the ordering guarantee, so no atomic operations are needed. */
struct chttpsvr_mw_node {
  chttpsvr_middleware_fn fn;
  void *ctx;
  chttpsvr_mw_node_t *next;
};

/** Compiled route entry. */
typedef struct chttpsvr_route {
  chttp_method_t method;
  char **segs; /* split pattern segments */
  int seg_count;
  char **param_names; /* extracted from {name} segments */
  int param_count;
  chttpsvr_handler_fn fn;
  void *ctx;
  bool is_streaming;
} chttpsvr_route_t;

/** Sub-router (also used as the root router). */
struct chttpsvr_router {
  char *prefix; /* e.g. "/api/v1"; "" for root */
  size_t prefix_len;
  chttpsvr_mw_node_t *mw_head;
  chttpsvr_mw_node_t *mw_tail;
  int mw_count; /* number of registered middleware; capped at _CHTTPSVR_MAX_MW
                 */
  chttpsvr_route_t **routes; /* pointer array; each entry is a stable alloc */
  size_t route_count;
  size_t route_cap;
  struct chttpserver *srv; /* back-pointer */
  ccol_memmgmt_procs_t *m_procs;
};

/** One response header entry stored in the flat headers array. */
typedef struct {
  char *name;
  char *value;
} resp_header_t;

/** Per-request response accumulator.
 *
 * Response headers are kept in a flat dynamic array (not a linked list) so
 * that the duplicate-detection scan in chttpsvr_resp_set_header is
 * cache-friendly and no per-header node allocation is required. */
struct chttpsvr_resp {
  int status_code;
  resp_header_t *headers; /* flat array; grows 2x+8 on overflow */
  size_t header_count;
  size_t header_cap;
  char *body;
  size_t body_len;
  size_t body_cap;
  ccol_memmgmt_procs_t *m_procs;
};

/** Lazy-parsed query parameters. */
typedef struct chttpsvr_qparams {
  char **keys;   /* URL-decoded, owned */
  char **values; /* URL-decoded, owned */
  size_t count;
  size_t cap;
  ccol_memmgmt_procs_t *m_procs;
} chttpsvr_qparams_t;

/* Maximum number of middleware steps (global + router combined) per request.
 * Exceeding this limit causes the server to respond with 500. */
#define _CHTTPSVR_MAX_MW 32

/* Snapshot entry for one middleware step. */
typedef struct {
  chttpsvr_middleware_fn fn;
  void *ctx;
} _mw_entry_t;

/** Middleware dispatch state (stack-allocated per request). */
typedef struct dispatch_ctx {
  struct chttpserver *srv;
  chttpsvr_router *router;
  chttpsvr_route_t *route;
  _mw_entry_t
      mw_snap[_CHTTPSVR_MAX_MW]; /* fn/ctx pairs, snapshotted under lock */
  int mw_count;                  /* total snapshotted entries */
  int mw_idx;                    /* next entry to dispatch */
} dispatch_ctx_t;

/** Per-request object. */
struct chttpsvr_req {
  chttp_method_t method;
  const char *path;      /* NUL-terminated decoded path */
  const char *raw_query; /* NUL-terminated raw query, or NULL */
  const void *body_data;
  size_t body_len;
  bool is_streaming;
  void *_h;          /* opaque http_s*, valid for the life of the worker's
                         dispatch; used by chttpsvr_req_read/_stream_error */
  void *_body_owned; /* buffered-route ingestion buffer; freed by
                         _task_worker after the handler chain returns */

  /* Pre-extracted headers (always populated; all requests go through ctpool).
   */
  char **_hdr_names;
  char **_hdr_values;
  size_t _hdr_count;

  const char **param_names; /* borrowed from route (const aliases) */
  char **param_values;      /* owned */
  int param_count;

  chttpsvr_qparams_t *_qparams; /* NULL until first query access */
  bool _qparams_attempted;      /* true after first parse attempt */
  bool _qparams_parse_oom;      /* true when _parse_qparams returned OOM */
  const char **_qresult;        /* scratch for multi-value return */
  size_t _qresult_cap;
  bool _qresult_oom; /* true when _qresult realloc failed in chttpsvr_req_query
                      */

  dispatch_ctx_t *_dispatch;
  ccol_memmgmt_procs_t *m_procs;

  /* max_body_read_duration_ms bookkeeping; see _check_read_deadline. */
  struct timespec _read_deadline;
  bool _read_deadline_set;
  bool _deadline_exceeded;
};

/** Streaming-dispatch context (heap-allocated before http_pause).
 *
 * Built at headers-complete time, before the body has arrived; there is no
 * body/body_len here. Buffered routes accumulate the body in _task_worker
 * (via http1_stream_read) directly into the stack-local chttpsvr_req; only
 * streaming routes pull it lazily through chttpsvr_req_read. */
typedef struct streaming_ctx {
  http_pause_handle_s *ph;
  struct chttpserver *srv;
  chttpsvr_router *router;
  chttpsvr_route_t *route;
  chttp_method_t method;
  void *h;          /* opaque http_s*; still valid (paused, not destroyed) until
                       http1_stream_release is called in _task_worker */
  char *path;       /* owned */
  char *raw_query;  /* owned, or NULL */
  char **hdr_names; /* owned arrays */
  char **hdr_values;
  size_t hdr_count;
  char **param_values; /* owned */
  int param_count;
  chttpsvr_resp resp;
  dispatch_ctx_t dispatch;
  ccol_memmgmt_procs_t *m_procs;
} streaming_ctx_t;

/** Main server struct. */
struct chttpserver {
  chttpsvr_router **routers; /* [0] = root, [1..n] = sub-routers */
  size_t router_count;
  size_t router_cap;
  ctpool worker_pool; /* owned; created at chttpsvr_start, replaced on a
                         restart, and destroyed at __chttpsvr_destroy. Every
                         read and write goes through mutex (see
                         _task_pause_cb and _wait_and_detach_worker_pool):
                         an already-accepted keep-alive connection's
                         _on_headers_complete/_task_pause_cb does not check
                         `started` and can still fire while a restart or
                         teardown is swapping this pointer out from under
                         it. */
  fio_tls_s
      *tls; /* non-NULL when TLS was configured; freed in __chttpsvr_destroy */
  mutex_t mutex;
  cond_var_t
      requests_done_cv;   /* signalled when in_flight_requests drops to 0 */
  int in_flight_requests; /* # requests dispatched (from http_pause onward)
                             through completion; guarded by mutex */
  pthread_rwlock_t
      routes_lock; /* guards routers[], route_count, routes[], mw lists */
  clog cl; /* server-owned logger; a derived logger (component=http-server)
              when cl was passed in, or an internal stderr/FATAL-only logger
              when NULL was passed; closed in __chttpsvr_destroy */
  intptr_t listen_uuid;       /* facio listener uuid; -1 when not started */
  bool started;               /* true after a successful chttpsvr_start call */
  bool contributed_to_engine; /* true once this server has acquired its one
                                  shared cfio_engine reference (see
                                  chttpsvr_start/__chttpsvr_destroy) */
  _Atomic unsigned
      stream_read_timeout_ms; /* set at chttpsvr_start; bounds each
                                  http1_stream_read wait for more body bytes,
                                  for both buffered and streaming routes.
                                  _Atomic (plain store/load, no mutex) because
                                  a restart writes it while an
                                  already-accepted keep-alive connection's
                                  worker thread may concurrently be reading it
                                  once per body chunk on the hot ingestion
                                  path (see _ingest_buffered_body /
                                  chttpsvr_req_read); a mutex there would add
                                  lock/unlock overhead to every chunk read. */
  _Atomic unsigned
      max_body_read_duration_ms; /* set at chttpsvr_start; 0 = no limit.
                                     Bounds the *total* time spent reading one
                                     request's body, closing the
                                     trickle-forever loophole that
                                     stream_read_timeout_ms alone leaves open
                                     (see _check_read_deadline). _Atomic for
                                     the same reason as stream_read_timeout_ms
                                     above. */
  ccol_memmgmt_procs_t *m_procs;
};

/* ========================================================================== */
/*                         GLOBAL STATE                                       */
/* ========================================================================== */

/*
 * The raw facil.io reactor lifecycle (start/stop/one-time global init) is
 * owned by the shared cfio_engine module (src/cfio_engine.c), not by this
 * file; see cfio_engine.h for why: chttpclient.c's async engine (Tier 2/3)
 * acquires/releases references to the exact same underlying reactor, so both
 * modules can run simultaneously in the same process. What remains here is
 * purely chttpserver-local bookkeeping: each chttpsvr instance acquires
 * exactly one shared-engine reference for its whole lifetime (across any
 * number of start/stop/restart cycles), tracked by
 * chttpserver::contributed_to_engine and released exactly once, in
 * __chttpsvr_destroy; guarded by the instance's own srv->mutex, so no
 * dedicated global mutex is needed for it any more.
 */
/* Installs a minimal default engine logger if the caller has not already set
 * one via chttpsvr_set_engine_logger(). Deliberately NOT a pthread_once: a
 * fully-stopped-then-restarted engine (chttpsvr_engine_wait() having already
 * nulled the logger via fio_set_logger(NULL), followed by a fresh
 * chttpsvr_start() well after that; an explicitly supported restart
 * pattern) must still get a fresh default logger, exactly like the
 * pre-unification code's per-engine-start check did; a one-shot guard would
 * silently leave the engine loggerless forever after the first stop. Instead
 * this is called once per *server's own* first start (guarded by the
 * need_acquire check at its call site in chttpsvr_start), and is idempotent
 * in effect regardless of how many times or from how many concurrent callers
 * it runs, since it only ever installs a default when fio_has_logger() is
 * still false at the moment it runs; also deliberately decoupled from "is
 * this the very first chttpsvr_start() call to bring the shared engine up":
 * with a shared engine, chttpclient's async engine may already have started
 * it before chttpserver ever calls chttpsvr_start(), so this cannot be tied
 * to that transition either. */
static void _install_default_engine_logger(void) {
  if (!fio_has_logger()) {
    clog base = clog_open_fd(2, CLOG_FATAL);
    if (base) {
      clog derived = clog_derive(base);
      clog_close(base);
      if (derived) {
        clog_set_field(derived, "component", "http-engine");
        fio_set_logger(derived);
      }
    }
  }
}

/* Private sentinel returned by _parse_method for unrecognised method strings.
 * Not part of the public chttp_method_t enum; using it as a route method is
 * impossible, so it can never match any registered route. */
#define _CHTTP_METHOD_UNKNOWN ((chttp_method_t)(CHTTP_ANY + 1))

/* ========================================================================== */
/*                         FORWARD DECLARATIONS                               */
/* ========================================================================== */

static void _chttpsvr_next(chttpsvr_req *req, chttpsvr_resp *resp);
static int _on_headers_complete(http_s *h);
static void _task_pause_cb(http_pause_handle_s *ph);
static void _task_worker(void *arg);
static void _task_send_response(http_s *h);
static void _task_cleanup(void *udata);
static void _free_task_ctx(streaming_ctx_t *sctx);
static void _destroy_resp(chttpsvr_resp *resp, ccol_memmgmt_procs_t *mp);
static void _finalize_response(http_s *h, chttpsvr_resp *resp);

/* ========================================================================== */
/*                         PATTERN COMPILATION                                */
/* ========================================================================== */

static ccol_retval_t _compile_pattern(const char *pattern, char ***segs_out,
                                      int *seg_count_out,
                                      char ***param_names_out,
                                      int *param_count_out,
                                      ccol_memmgmt_procs_t *mp) {
  /* All valid HTTP paths begin with '/'.  Reject patterns that don't so that
   * callers receive an immediate error rather than a silently-registered route
   * that happens to work (both the pattern and request path have the leading
   * '/' stripped before comparison, so "health" and "/health" would match the
   * same requests; but the API contract is unambiguous about the leading
   * slash being required).  Empty string is also rejected by this check. */
  if (pattern[0] != '/') return ccol_invalid_args;

  const char *p = pattern;
  if (*p == '/') p++;

  /* Reject a trailing slash.  _match_segments always rejects trailing slashes
   * in request paths, so a pattern ending with '/' would match nothing with a
   * trailing slash and silently behave like the no-trailing-slash pattern for
   * everything else; the opposite of what the registration implies. */
  {
    size_t plen = strlen(p);
    if (plen > 0 && p[plen - 1] == '/') return ccol_invalid_args;
  }

  /* Count segments; reject patterns that contain consecutive slashes (empty
   * segments), which would produce ambiguous or silently-normalised routes. */
  int seg_count = 0;
  {
    const char *s = p;
    while (*s) {
      const char *e = strchr(s, '/');
      size_t len = e ? (size_t)(e - s) : strlen(s);
      if (len == 0 && e != NULL) {
        /* Two adjacent slashes: /foo//bar or a leading // after stripping one
         */
        return ccol_invalid_args;
      }
      if (len > 0) seg_count++;
      if (!e) break;
      s = e + 1;
    }
  }

  if (seg_count == 0) {
    *segs_out = NULL;
    *seg_count_out = 0;
    *param_names_out = NULL;
    *param_count_out = 0;
    return ccol_success;
  }

  char **segs = (char **)_mem_calloc(mp, (size_t)seg_count, sizeof(char *));
  if (!segs) return ccol_not_enough_memory;

  int si = 0;
  int param_count = 0;
  const char *s = p;
  while (*s && si < seg_count) {
    const char *e = strchr(s, '/');
    size_t len = e ? (size_t)(e - s) : strlen(s);
    if (len == 0) {
      if (!e) break;
      s = e + 1;
      continue;
    }
    segs[si] = (char *)_mem_alloc(mp, len + 1);
    if (!segs[si]) {
      for (int j = 0; j < si; j++) _mem_free(mp, segs[j]);
      _mem_free(mp, segs);
      return ccol_not_enough_memory;
    }
    memcpy(segs[si], s, len);
    segs[si][len] = '\0';
    if (segs[si][0] == '{') {
      /* Validate that the parameter segment is well-formed: {name} */
      if (len < 3 || segs[si][len - 1] != '}') {
        for (int j = 0; j <= si; j++) _mem_free(mp, segs[j]);
        _mem_free(mp, segs);
        return ccol_invalid_args;
      }
      /* Validate param name: only [A-Za-z0-9_] are accepted.  Names with
       * other characters (spaces, hyphens, etc.) can never be looked up via
       * chttpsvr_req_param and would create silently unreachable parameters. */
      for (size_t ci = 1; ci < len - 1; ci++) {
        char c = segs[si][ci];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) {
          for (int j = 0; j <= si; j++) _mem_free(mp, segs[j]);
          _mem_free(mp, segs);
          return ccol_invalid_args;
        }
      }
      param_count++;
    }
    si++;
    if (!e) break;
    s = e + 1;
  }

  char **param_names = NULL;
  if (param_count > 0) {
    param_names = (char **)_mem_calloc(mp, (size_t)param_count, sizeof(char *));
    if (!param_names) {
      for (int j = 0; j < seg_count; j++) _mem_free(mp, segs[j]);
      _mem_free(mp, segs);
      return ccol_not_enough_memory;
    }
    int pi = 0;
    for (int i = 0; i < seg_count; i++) {
      if (segs[i][0] == '{') {
        size_t len = strlen(segs[i]);
        /* Strip leading '{' and trailing '}'. */
        param_names[pi] =
            (char *)_mem_alloc(mp, len - 1); /* len-2+1 for NULL */
        if (!param_names[pi]) {
          for (int j = 0; j < pi; j++) _mem_free(mp, param_names[j]);
          _mem_free(mp, param_names);
          for (int j = 0; j < seg_count; j++) _mem_free(mp, segs[j]);
          _mem_free(mp, segs);
          return ccol_not_enough_memory;
        }
        memcpy(param_names[pi], segs[i] + 1, len - 2);
        param_names[pi][len - 2] = '\0';
        pi++;
      }
    }
  }

  *segs_out = segs;
  *seg_count_out = seg_count;
  *param_names_out = param_names;
  *param_count_out = param_count;
  return ccol_success;
}

static void _free_route_data(chttpsvr_route_t *r, ccol_memmgmt_procs_t *mp) {
  for (int i = 0; i < r->seg_count; i++) _mem_free(mp, r->segs[i]);
  _mem_free(mp, r->segs);
  for (int i = 0; i < r->param_count; i++) _mem_free(mp, r->param_names[i]);
  _mem_free(mp, r->param_names);
}

/* ========================================================================== */
/*                         URL DECODING HELPERS                               */
/* ========================================================================== */

/* Decode a path segment (not NUL-terminated) into a heap-allocated string.
 *
 * Returns the decoded string on success.  Returns NULL on failure; when NULL
 * is returned *oom_out (if non-NULL) distinguishes the two failure modes:
 *   true; allocation failure; caller should propagate as OOM (500)
 *   false; bad percent-encoding; caller may treat as no-match (404) */
static char *_decode_seg_alloc(const char *seg, size_t len,
                               ccol_memmgmt_procs_t *mp, bool *oom_out) {
  if (oom_out) *oom_out = false;

  char src_buf[512];
  char *src = src_buf;
  bool src_heap = false;
  if (len + 1 > sizeof(src_buf)) {
    src = (char *)_mem_alloc(mp, len + 1);
    if (!src) {
      if (oom_out) *oom_out = true;
      return NULL;
    }
    src_heap = true;
  }
  memcpy(src, seg, len);
  src[len] = '\0';

  char *dest = (char *)_mem_alloc(mp, len + 1);
  if (!dest) {
    if (src_heap) _mem_free(mp, src);
    if (oom_out) *oom_out = true;
    return NULL;
  }
  /* Use path semantics: '+' is a literal plus character in a path segment,
   * not a space.  http_decode_url_unsafe (query-string semantics) would
   * silently turn '+' into ' ', producing inconsistent param values vs the
   * full decoded path returned by chttpsvr_req_path. */
  ssize_t dlen = http_decode_path_unsafe(dest, src);
  if (src_heap) _mem_free(mp, src);
  if (dlen < 0) {
    _mem_free(mp, dest);
    return NULL; /* bad encoding; *oom_out stays false */
  }
  dest[(size_t)dlen] = '\0';
  return dest;
}

/* Decode a path segment and compare with a literal pattern segment.
 *
 * Returns  1 on match,
 *          0 on no-match (including bad percent-encoding in the path),
 *         -1 on OOM (caller should propagate as a 500, not a 404). */
static int _seg_matches_literal(const char *path_seg, size_t seg_len,
                                const char *pattern_seg,
                                ccol_memmgmt_procs_t *mp) {
  char src_buf[512];
  char decoded_buf[512];
  char *src = src_buf;
  char *decoded = decoded_buf;
  bool src_heap = false;
  bool dec_heap = false;

  if (seg_len + 1 > sizeof(src_buf)) {
    src = (char *)_mem_alloc(mp, seg_len + 1);
    if (!src) return -1; /* OOM */
    src_heap = true;
  }
  if (seg_len + 1 > sizeof(decoded_buf)) {
    decoded = (char *)_mem_alloc(mp, seg_len + 1);
    if (!decoded) {
      if (src_heap) _mem_free(mp, src);
      return -1; /* OOM */
    }
    dec_heap = true;
  }
  memcpy(src, path_seg, seg_len);
  src[seg_len] = '\0';
  ssize_t dlen = http_decode_path_unsafe(decoded, src);
  if (dlen >= 0) decoded[(size_t)dlen] = '\0';
  int result = (dlen >= 0 && strcmp(decoded, pattern_seg) == 0) ? 1 : 0;
  if (src_heap) _mem_free(mp, src);
  if (dec_heap) _mem_free(mp, decoded);
  return result;
}

/* ========================================================================== */
/*                         ROUTE MATCHING                                     */
/* ========================================================================== */

typedef enum {
  ROUTE_MATCH_OK,
  ROUTE_MATCH_METHOD,
  ROUTE_MATCH_OOM,
  ROUTE_MATCH_NONE
} route_match_t;

typedef struct {
  chttpsvr_router *router;
  chttpsvr_route_t *route;
  char **param_values; /* must be freed by caller on OK */
  route_match_t result;
} match_result_t;

/* Returns 0 (no path match), 1 (path matched), or -1 (OOM during capture).
 *
 * pv_out: when non-NULL the function allocates the param_values array lazily
 *         (on first captured parameter) and writes the pointer to *pv_out on
 *         success.  On no-match any partially-allocated entries are freed and
 *         *pv_out is set to NULL.  Pass NULL to perform a structure-only check
 *         with zero heap allocation (dry-run mode).
 *
 * OOM is returned when _mem_calloc fails (capture mode) or when a {param}
 * token is longer than 511 bytes and heap allocation for encoding validation
 * fails (dry-run mode).  _decode_seg_alloc failures (bad encoding) are treated
 * as no-match in both modes, ensuring consistent 404 behaviour regardless of
 * whether the method matched. */
static int _match_segments(const char *sub_path, chttpsvr_route_t *route,
                           char ***pv_out, ccol_memmgmt_procs_t *mp) {
  const char *p = sub_path;
  if (*p == '/') p++;

  if (route->seg_count == 0) {
    /* Matches "/" or "" only. */
    return (*p == '\0') ? 1 : 0;
  }

  char **pv = NULL;
  int param_idx = 0;
  for (int i = 0; i < route->seg_count; i++) {
    const char *next_sep = strchr(p, '/');
    size_t tok_len = next_sep ? (size_t)(next_sep - p) : strlen(p);

    /* Empty tokens are not valid (trailing slash or double slash). */
    if (tok_len == 0) goto no_match;

    if (route->segs[i][0] == '{') {
      if (pv_out) {
        /* Capture mode: allocate the array lazily on the first parameter so
         * that routes with params that do not fully match avoid any heap
         * allocation at all. */
        if (!pv) {
          pv = (char **)_mem_calloc(mp, (size_t)route->param_count,
                                    sizeof(char *));
          if (!pv) return -1; /* OOM; propagate to caller */
        }
        bool seg_oom = false;
        char *val = _decode_seg_alloc(p, tok_len, mp, &seg_oom);
        if (!val) {
          if (seg_oom) {
            /* OOM: free partial captures and propagate so the server returns
             * 500 rather than a misleading 404. */
            for (int j = 0; j < param_idx; j++) _mem_free(mp, pv[j]);
            _mem_free(mp, pv);
            if (pv_out) *pv_out = NULL;
            return -1;
          }
          goto no_match; /* bad percent-encoding: treat as no-match (404) */
        }
        pv[param_idx++] = val;
      } else {
        /* Dry-run: validate percent-encoding without capturing.  Accepting a
         * malformed token here while capture mode rejects it would make
         * _match_segments return different results for the same path depending
         * on whether the method matched, producing incorrect 405 responses
         * instead of the correct 404.  In-place decoding is safe because
         * output is always <= input length.  Stack buffer handles tokens up to
         * 511 bytes; heap fallback for oversized tokens only. */
        char _vbuf[512];
        char *_vp = _vbuf;
        bool _vheap = false;
        if (tok_len + 1 > sizeof(_vbuf)) {
          _vp = (char *)_mem_alloc(mp, tok_len + 1);
          if (!_vp) return -1; /* OOM */
          _vheap = true;
        }
        memcpy(_vp, p, tok_len);
        _vp[tok_len] = '\0';
        ssize_t _vl = http_decode_path_unsafe(_vp, _vp);
        if (_vheap) _mem_free(mp, _vp);
        if (_vl < 0) goto no_match; /* bad encoding; mirrors capture mode */
      }
    } else {
      /* Literal: compare decoded path token against the pattern literal.
       * -1 means OOM (propagate as 500), 0 means no-match (404), 1 matches. */
      int lit = _seg_matches_literal(p, tok_len, route->segs[i], mp);
      if (lit < 0) {
        if (pv) {
          for (int j = 0; j < param_idx; j++) _mem_free(mp, pv[j]);
          _mem_free(mp, pv);
          if (pv_out) *pv_out = NULL;
        }
        return -1;
      }
      if (!lit) goto no_match;
    }

    if (i == route->seg_count - 1) {
      /* Last segment: a trailing '/' means the path has one too many
       * components; /items/1/ must NOT match /items/{id}. */
      if (next_sep != NULL) goto no_match;
      p += tok_len;
    } else {
      p = next_sep ? next_sep + 1 : p + tok_len;
    }
  }
  if (*p != '\0') goto no_match;
  if (pv_out) *pv_out = pv;
  return 1;

no_match:
  if (pv) {
    for (int j = 0; j < param_idx; j++) _mem_free(mp, pv[j]);
    _mem_free(mp, pv);
    if (pv_out) *pv_out = NULL;
  }
  return 0;
}

/* Matches `path` (raw, not yet percent-decoded) against `router`'s prefix,
 * segment by segment, decoding each raw path segment before comparing it to
 * the corresponding literal prefix segment; exactly what _seg_matches_literal
 * already does for route-pattern segments. A byte-for-byte strncmp against
 * the raw path (the previous approach) would fail to match a request whose
 * prefix portion happens to be percent-encoded (e.g. "/%61pi/v1/x" for a
 * prefix of "/api/v1"), even though an equivalent root-level route
 * registration would match it via the per-segment decoding _match_segments
 * already performs.
 *
 * Returns 1 on match (sets *sub_path_out to the remainder, "/" if the path
 * was exactly the prefix), 0 on no-match, -1 on OOM.
 *
 * router->prefix is always non-NULL, starts with '/', and contains no "//"
 * (validated in chttpsvr_subrouter at registration time); this function is
 * only ever called for a router with prefix_len > 0 (the caller handles the
 * prefix_len == 0 root-router case directly). */
static int _prefix_matches(const char *path, chttpsvr_router *router,
                           ccol_memmgmt_procs_t *mp,
                           const char **sub_path_out) {
  const char *pp = router->prefix + 1; /* skip leading '/' */
  const char *rp = path;
  if (*rp == '/') rp++;

  if (*pp == '\0') {
    /* Degenerate "/" prefix: matches only the exact root path "/"; see the
     * "/" prefix edge case documented on chttpsvr_subrouter in
     * include/chttpserver.h. */
    if (*rp != '\0') return 0;
    *sub_path_out = "/";
    return 1;
  }

  const char *rp_after_last_seg = path;
  while (*pp) {
    const char *pe = strchr(pp, '/');
    size_t plen = pe ? (size_t)(pe - pp) : strlen(pp);

    const char *re = strchr(rp, '/');
    size_t rlen = re ? (size_t)(re - rp) : strlen(rp);
    if (rlen == 0) return 0; /* path ran out of segments */

    char pseg_buf[256];
    char *pseg = pseg_buf;
    bool pseg_heap = false;
    if (plen + 1 > sizeof(pseg_buf)) {
      pseg = (char *)_mem_alloc(mp, plen + 1);
      if (!pseg) return -1;
      pseg_heap = true;
    }
    memcpy(pseg, pp, plen);
    pseg[plen] = '\0';

    int lit = _seg_matches_literal(rp, rlen, pseg, mp);
    if (pseg_heap) _mem_free(mp, pseg);
    if (lit < 0) return -1;
    if (!lit) return 0;

    rp_after_last_seg = rp + rlen;
    rp = re ? re + 1 : rp + rlen;
    pp = pe ? pe + 1 : pp + plen;
  }

  *sub_path_out = (*rp_after_last_seg == '\0') ? "/" : rp_after_last_seg;
  return 1;
}

static match_result_t _find_route(struct chttpserver *srv, const char *path,
                                  chttp_method_t method) {
  /* Track whether any route matched the path with the wrong method.  We must
   * exhaust all routers and routes before concluding 405, because a later
   * entry may match BOTH path and method (e.g. GET and POST registered on
   * the same path, or a sub-router shadowing a root-router route). */
  bool method_mismatch_seen = false;

  for (size_t ri = 0; ri < srv->router_count; ri++) {
    chttpsvr_router *router = srv->routers[ri];

    /* Determine the sub-path relative to this router's prefix. */
    const char *sub_path;
    if (router->prefix_len == 0) {
      sub_path = path;
    } else {
      int pm = _prefix_matches(path, router, srv->m_procs, &sub_path);
      if (pm < 0) return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM};
      if (pm == 0) continue;
    }

    for (size_t i = 0; i < router->route_count; i++) {
      chttpsvr_route_t *route = router->routes[i];

      /* Single-pass strategy: pass &pv to capture param values only when the
       * method also matches; pass NULL otherwise.  For non-matching paths the
       * return is 0 with zero allocation.  For path-match / method-mismatch
       * the dry-run (NULL pv_out) also allocates nothing.  Only a full
       * (path + method) hit pays for the param capture. */
      char **pv = NULL;
      bool method_ok = (route->method == CHTTP_ANY || route->method == method);
      int ms = _match_segments(sub_path, route, method_ok ? &pv : NULL,
                               srv->m_procs);
      if (ms == -1) return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM};
      if (ms == 0) continue; /* path structure did not match */

      /* Path matched. */
      if (!method_ok) {
        /* Record the mismatch and keep searching; a later route or router
         * may still produce a full match. */
        method_mismatch_seen = true;
        continue;
      }
      return (match_result_t){router, route, pv, ROUTE_MATCH_OK};
    }
  }
  if (method_mismatch_seen)
    return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_METHOD};
  return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_NONE};
}

static void _free_param_values(char **pv, int count, ccol_memmgmt_procs_t *mp) {
  if (!pv) return;
  for (int i = 0; i < count; i++) _mem_free(mp, pv[i]);
  _mem_free(mp, pv);
}

/* ========================================================================== */
/*                         METHOD PARSING                                     */
/* ========================================================================== */

static chttp_method_t _parse_method(const char *s, size_t len) {
  if (len == 3 && memcmp(s, "GET", 3) == 0) return CHTTP_GET;
  if (len == 4 && memcmp(s, "POST", 4) == 0) return CHTTP_POST;
  if (len == 3 && memcmp(s, "PUT", 3) == 0) return CHTTP_PUT;
  if (len == 6 && memcmp(s, "DELETE", 6) == 0) return CHTTP_DELETE;
  if (len == 5 && memcmp(s, "PATCH", 5) == 0) return CHTTP_PATCH;
  if (len == 4 && memcmp(s, "HEAD", 4) == 0) return CHTTP_HEAD;
  if (len == 7 && memcmp(s, "OPTIONS", 7) == 0) return CHTTP_OPTIONS;
  return _CHTTP_METHOD_UNKNOWN;
}

/* ========================================================================== */
/*                         RESPONSE HELPERS                                   */
/* ========================================================================== */

static void _destroy_resp(chttpsvr_resp *resp, ccol_memmgmt_procs_t *mp) {
  for (size_t i = 0; i < resp->header_count; i++) {
    _mem_free(mp, resp->headers[i].name);
    _mem_free(mp, resp->headers[i].value);
  }
  _mem_free(mp, resp->headers);
  resp->headers = NULL;
  resp->header_count = 0;
  resp->header_cap = 0;
  _mem_free(mp, resp->body);
  resp->body = NULL;
  resp->body_len = 0;
  resp->body_cap = 0;
}

static void _destroy_req_qparams(chttpsvr_req *req) {
  ccol_memmgmt_procs_t *mp = req->m_procs;
  if (req->_qparams) {
    chttpsvr_qparams_t *qp = req->_qparams;
    for (size_t i = 0; i < qp->count; i++) {
      _mem_free(mp, qp->keys[i]);
      _mem_free(mp, qp->values[i]);
    }
    _mem_free(mp, qp->keys);
    _mem_free(mp, qp->values);
    _mem_free(mp, qp);
  }
  if (req->_qresult) _mem_free(mp, req->_qresult);
}

static void _finalize_response(http_s *h, chttpsvr_resp *resp) {
  h->status = (resp->status_code >= 100 && resp->status_code <= 999)
                  ? (uintptr_t)resp->status_code
                  : (uintptr_t)500;

  /* Set response headers.
   * http_set_header2 copies the name and value strings into internal FIOBJ
   * string objects synchronously before returning, so resp->headers may be
   * freed immediately after this loop (same contract as http_send_body). */
  for (size_t _hi = 0; _hi < resp->header_count; _hi++) {
    fio_str_info_s name_info = {.capa = 0,
                                .len = strlen(resp->headers[_hi].name),
                                .data = resp->headers[_hi].name};
    fio_str_info_s val_info = {.capa = 0,
                               .len = strlen(resp->headers[_hi].value),
                               .data = resp->headers[_hi].value};
    http_set_header2(h, name_info, val_info);
  }

  /* Send body or finish headers-only.
   * http_send_body copies resp->body into an internal facil.io buffer before
   * returning, so it is safe to free resp->body immediately afterward. */
  if (resp->body && resp->body_len > 0) {
    http_send_body(h, resp->body, (uintptr_t)resp->body_len);
  } else {
    http_finish(h);
  }
}

/* ========================================================================== */
/*                         MIDDLEWARE DISPATCH                                */
/* ========================================================================== */

static void _chttpsvr_next(chttpsvr_req *req, chttpsvr_resp *resp) {
  dispatch_ctx_t *ctx = req->_dispatch;
  if (ctx->mw_idx < ctx->mw_count) {
    _mw_entry_t entry = ctx->mw_snap[ctx->mw_idx++];
    entry.fn(req, resp, entry.ctx, _chttpsvr_next);
    return;
  }
  ctx->route->fn(req, resp, ctx->route->ctx);
}

/* ========================================================================== */
/*                         STREAMING HELPERS                                  */
/* ========================================================================== */

/* Header extraction callback for streaming mode. */
typedef struct {
  char **names;
  char **values;
  size_t count;
  size_t cap;
  ccol_memmgmt_procs_t *mp;
  bool oom;
} hdr_extract_t;

static int _extract_hdr_cb(FIOBJ val, void *arg) {
  hdr_extract_t *he = (hdr_extract_t *)arg;
  FIOBJ key = fiobj_hash_key_in_loop();
  if (!key) return 0; /* skip null FIOBJ (internal sentinel entries) */
  fio_str_info_s key_info = fiobj_obj2cstr(key);
  if (!key_info.data || key_info.len == 0) return 0;

  fio_str_info_s val_info;
  if (FIOBJ_TYPE_IS(val, FIOBJ_T_ARRAY)) {
    FIOBJ last = fiobj_ary_index(val, -1);
    if (!last)
      return 0; /* empty multi-value array; skip, matching _find_hdr_cb */
    val_info = fiobj_obj2cstr(last);
  } else {
    val_info = fiobj_obj2cstr(val);
  }
  /* FIOBJ types that are not strings return NULL data from obj2cstr. */
  if (!val_info.data) {
    val_info.data = (char *)"";
    val_info.len = 0;
  }

  if (he->count >= he->cap) {
    size_t new_cap = he->cap * 2 + 8;
    char **nn =
        (char **)_mem_realloc(he->mp, he->names, new_cap * sizeof(char *));
    if (!nn) {
      he->oom = true;
      return -1;
    }
    char **nv =
        (char **)_mem_realloc(he->mp, he->values, new_cap * sizeof(char *));
    if (!nv) {
      /* nn is the new (larger) allocation for he->names.  Update he->names
       * now so the cleanup loop in _on_headers_complete frees the live
       * allocation.
       * he->cap stays at the old value; cleanup iterates he->count (not
       * he->cap), so the temporarily mismatched capacities are harmless. */
      he->names = nn;
      he->oom = true;
      return -1;
    }
    he->names = nn;
    he->values = nv;
    he->cap = new_cap;
  }

  char *hdr_name = ccol_strdup(he->mp, key_info.data);
  char *hdr_val = ccol_strdup(he->mp, val_info.data);
  if (!hdr_name || !hdr_val) {
    _mem_free(he->mp, hdr_name);
    _mem_free(he->mp, hdr_val);
    he->oom = true;
    return -1;
  }
  he->names[he->count] = hdr_name;
  he->values[he->count] = hdr_val;
  he->count++;
  return 0;
}

static void _free_task_ctx(streaming_ctx_t *sctx) {
  ccol_memmgmt_procs_t *mp = sctx->m_procs;
  _mem_free(mp, sctx->path);
  _mem_free(mp, sctx->raw_query);
  for (size_t i = 0; i < sctx->hdr_count; i++) {
    _mem_free(mp, sctx->hdr_names[i]);
    _mem_free(mp, sctx->hdr_values[i]);
  }
  _mem_free(mp, sctx->hdr_names);
  _mem_free(mp, sctx->hdr_values);
  _free_param_values(sctx->param_values, sctx->param_count, mp);
  _destroy_resp(&sctx->resp, mp);
  _mem_free(mp, sctx);
}

static void _task_pause_cb(http_pause_handle_s *ph) {
  streaming_ctx_t *sctx = (streaming_ctx_t *)http_paused_udata_get(ph);
  sctx->ph = ph;

  /* in_flight_requests was already incremented in _on_headers_complete,
   * before http_pause was called; see the comment there. Both the success
   * and failure paths below end with exactly one http_resume call,
   * guaranteeing that either _task_send_response or _task_cleanup fires;
   * both decrement in_flight_requests under the mutex. */
  mutex_lock(sctx->srv->mutex);
  ctpool pool = sctx->srv->worker_pool;
  mutex_unlock(sctx->srv->mutex);

  if (ctpool_try_submit(pool, _task_worker, sctx, NULL) != ccol_success) {
    /* No worker will ever call http1_stream_read/http1_stream_release for
     * this message now; release it here so http1_destroy doesn't defer
     * forever waiting for a release that will never come. */
    http1_stream_release((http_s *)sctx->h);
    sctx->resp.status_code = CHTTP_STATUS_SERVICE_UNAVAILABLE;
    http_resume(ph, _task_send_response, _task_cleanup);
    return;
  }
  /* On successful submit the worker holds the reference; the decrement
   * happens in _task_send_response or _task_cleanup. */
}

/* Translate the reason http1_stream_read most recently failed for `h` into
 * this library's error code space. */
static ccol_retval_t _stream_err_to_retval(http_s *h) {
  switch (http1_stream_last_error(h)) {
    case HTTP1_STREAM_ERR_TIMEOUT:
      return ccol_timed_out;
    case HTTP1_STREAM_ERR_TOO_LARGE:
      return ccol_msg_too_large;
    case HTTP1_STREAM_ERR_CLOSED:
    case HTTP1_STREAM_ERR_PROTOCOL:
    case HTTP1_STREAM_ERR_NONE:
    default:
      return ccol_http_transfer_aborted;
  }
}

/* Caps *timeout_ms_inout so a single http1_stream_read call cannot block
 * past req's overall max_body_read_duration_ms deadline (lazily computed on
 * the first call), and returns false once that deadline has already passed
 * ; in which case req->_deadline_exceeded is set and the caller must not
 * call http1_stream_read again (chttpsvr_req_stream_error /
 * _ingest_buffered_body's caller report ccol_timed_out from that flag
 * directly, without ever consulting http1_stream_last_error).
 *
 * stream_read_timeout_ms alone only bounds each individual gap between
 * batches of bytes, so a client that trickles a byte or two just before
 * every such gap expires can otherwise pin a worker thread indefinitely;
 * this closes that loophole. A max_body_read_duration_ms of 0 disables the
 * check entirely (the default), leaving existing behavior unchanged. */
static bool _check_read_deadline(chttpsvr_req *req,
                                 unsigned *timeout_ms_inout) {
  unsigned max_dur =
      req->_dispatch ? req->_dispatch->srv->max_body_read_duration_ms : 0;
  if (!max_dur) return true;

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (!req->_read_deadline_set) {
    req->_read_deadline = now;
    req->_read_deadline.tv_sec += (time_t)(max_dur / 1000);
    req->_read_deadline.tv_nsec += (long)(max_dur % 1000) * 1000000L;
    if (req->_read_deadline.tv_nsec >= 1000000000L) {
      req->_read_deadline.tv_nsec -= 1000000000L;
      req->_read_deadline.tv_sec += 1;
    }
    req->_read_deadline_set = true;
  }

  long remaining_ms = (long)(req->_read_deadline.tv_sec - now.tv_sec) * 1000 +
                      (req->_read_deadline.tv_nsec - now.tv_nsec) / 1000000L;
  if (remaining_ms <= 0) {
    req->_deadline_exceeded = true;
    return false;
  }
  if (!*timeout_ms_inout || (unsigned)remaining_ms < *timeout_ms_inout)
    *timeout_ms_inout = (unsigned)remaining_ms;
  return true;
}

/* Reads the entire request body into one growable heap buffer before a
 * buffered handler is invoked. max_body_size is already enforced by the
 * same parser logic that would enforce it for a streaming route (see
 * http1_on_body_chunk); http1_stream_read simply reports the failure. */
static ccol_retval_t _ingest_buffered_body(streaming_ctx_t *sctx,
                                           chttpsvr_req *req) {
  http_s *h = (http_s *)sctx->h;
  ccol_memmgmt_procs_t *mp = sctx->m_procs;
  unsigned configured_timeout_ms = sctx->srv->stream_read_timeout_ms;
  size_t cap = 0, len = 0;
  char *buf = NULL;

  for (;;) {
    if (len == cap) {
      /* Overflow-safe doubling, mirroring chttpsvr_resp_write's growth
       * guard: max_body_size already bounds how large this buffer can
       * legitimately grow (enforced by http1_on_body_chunk), so this is
       * defense-in-depth against a caller-supplied max_body_size near
       * SIZE_MAX rather than a normally reachable path. */
      if (cap > (SIZE_MAX - 16384) / 2) {
        _mem_free(mp, buf);
        return ccol_not_enough_memory;
      }
      size_t new_cap = cap ? cap * 2 : 16384;
      char *nb = (char *)_mem_realloc(mp, buf, new_cap);
      if (!nb) {
        _mem_free(mp, buf);
        return ccol_not_enough_memory;
      }
      buf = nb;
      cap = new_cap;
    }
    unsigned timeout_ms = configured_timeout_ms;
    if (!_check_read_deadline(req, &timeout_ms)) {
      _mem_free(mp, buf);
      return ccol_timed_out;
    }
    ssize_t n = http1_stream_read(h, buf + len, cap - len, timeout_ms);
    if (n > 0) {
      len += (size_t)n;
      continue;
    }
    if (n == 0) break;
    ccol_retval_t err = _stream_err_to_retval(h);
    _mem_free(mp, buf);
    return err;
  }

  req->body_data = buf;
  req->body_len = len;
  req->_body_owned = buf;
  return ccol_success;
}

static void _task_worker(void *arg) {
  streaming_ctx_t *sctx = (streaming_ctx_t *)arg;

  chttpsvr_req req;
  memset(&req, 0, sizeof(req));
  req.method = sctx->method;
  req.path = sctx->path;
  req.raw_query = sctx->raw_query;
  req.is_streaming = sctx->route->is_streaming;
  req._h = sctx->h;
  req._hdr_names = sctx->hdr_names;
  req._hdr_values = sctx->hdr_values;
  req._hdr_count = sctx->hdr_count;
  req.param_names = (const char **)sctx->route->param_names;
  req.param_values = sctx->param_values;
  req.param_count = sctx->param_count;
  req._dispatch = &sctx->dispatch;
  req.m_procs = sctx->m_procs;

  if (!sctx->route->is_streaming) {
    ccol_retval_t ing = _ingest_buffered_body(sctx, &req);
    if (ing != ccol_success) {
      http1_stream_release((http_s *)sctx->h);
      sctx->resp.status_code =
          (ing == ccol_msg_too_large) ? CHTTP_STATUS_PAYLOAD_TOO_LARGE
          : (ing == ccol_timed_out)   ? CHTTP_STATUS_REQUEST_TIMEOUT
                                      : CHTTP_STATUS_INTERNAL_ERROR;
      _destroy_req_qparams(&req);
      http_resume(sctx->ph, _task_send_response, _task_cleanup);
      return;
    }
  }

  _chttpsvr_next(&req, &sctx->resp);

  /* Streaming handlers may stop reading before EOF; buffered ingestion
   * above always runs to completion or an error. Either way, release
   * exactly once, before resuming, whether or not http1_stream_read ever
   * naturally reached end-of-body. */
  http1_stream_release((http_s *)sctx->h);

  _mem_free(sctx->m_procs, req._body_owned);
  _destroy_req_qparams(&req);

  http_resume(sctx->ph, _task_send_response, _task_cleanup);
}

static void _task_send_response(http_s *h) {
  streaming_ctx_t *sctx = (streaming_ctx_t *)h->udata;
  struct chttpserver *srv = sctx->srv; /* save before sctx is freed */
  _finalize_response(h, &sctx->resp);
  /* _finalize_response transfers all header strings and body to facil.io
   * synchronously; sctx and the embedded resp may be freed now. */
  _free_task_ctx(sctx);
  mutex_lock(srv->mutex);
  if (--srv->in_flight_requests == 0) cond_var_broadcast(srv->requests_done_cv);
  mutex_unlock(srv->mutex);
}

static void _task_cleanup(void *udata) {
  /* Connection was closed before we could send; just free resources. */
  streaming_ctx_t *sctx = (streaming_ctx_t *)udata;
  struct chttpserver *srv = sctx->srv; /* save before sctx is freed */
  _free_task_ctx(sctx);
  mutex_lock(srv->mutex);
  if (--srv->in_flight_requests == 0) cond_var_broadcast(srv->requests_done_cv);
  mutex_unlock(srv->mutex);
}

/* ========================================================================== */
/*                         MAIN REQUEST HANDLER                               */
/* ========================================================================== */

/**
 * http_listen() hard-requires .on_request to be set (it calls exit() at
 * startup otherwise); but _on_headers_complete below always returns 1
 * (handled) for every request that reaches it, which stops http1.c from
 * ever calling on_request. This is therefore unreachable in practice and
 * exists only to satisfy that startup check.
 */
static void _on_request_unreachable(http_s *h) { http_send_error(h, 500); }

/**
 * Called by http1.c right after headers are parsed, before any body byte is
 * read (see http_settings_s.on_headers_complete). Routing happens here now
 * (not after the body arrives) so an unmatched route is rejected without
 * ever reading a body it's about to discard, and a matched route is hand
 * off to a worker immediately regardless of body size, freeing the reactor
 * thread. The worker (see _task_worker) reads the body itself via
 * http1_stream_read, batch by batch, whether the route is buffered or
 * streaming.
 *
 * Returns 1 if the request was paused and handed to the worker pool (the
 * only outcome for a matched route), or 0 if an error response was already
 * sent synchronously (no match, wrong method, or OOM); in the 0 case
 * http1.c forces the connection closed afterward, since the client's
 * still-arriving body would otherwise be misread as the start of a new
 * pipelined request.
 */
static int _on_headers_complete(http_s *h) {
  struct chttpserver *srv = (struct chttpserver *)http_settings(h)->udata;

  /* Extract path (server-side: h->path is the path only, not query). */
  fio_str_info_s path_fi = fiobj_obj2cstr(h->path);
  if (!path_fi.data) {
    http_send_error(h, 400);
    return 0;
  }

  /* Copy to NUL-terminated buffer; use stack for short paths. */
  char path_stk[512];
  char *path = path_stk;
  bool path_heap = false;
  if (path_fi.len + 1 > sizeof(path_stk)) {
    path = (char *)_mem_alloc(srv->m_procs, path_fi.len + 1);
    if (!path) {
      http_send_error(h, 500);
      return 0;
    }
    path_heap = true;
  }
  memcpy(path, path_fi.data, path_fi.len);
  path[path_fi.len] = '\0';

  /* Extract method. */
  fio_str_info_s method_fi = fiobj_obj2cstr(h->method);
  if (!method_fi.data) {
    if (path_heap) _mem_free(srv->m_procs, path);
    http_send_error(h, 400);
    return 0;
  }
  chttp_method_t method = _parse_method(method_fi.data, method_fi.len);

  if (method == _CHTTP_METHOD_UNKNOWN) {
    if (path_heap) _mem_free(srv->m_procs, path);
    http_send_error(h, 501);
    return 0;
  }

  /* Route lookup and middleware snapshot under the routes read lock.
   * The read lock prevents _router_add_route / _router_add_mw /
   * chttpsvr_subrouter from mutating the routing tables while we read them. The
   * lock is held only long enough to find the route and snapshot the middleware
   * head pointers; handler dispatch runs without it.
   *
   * Note: _find_route calls _mem_calloc while this read lock is held.  For the
   * default allocator (malloc) this is safe.  Custom allocators passed via
   * create_chttpsvr_mp must not acquire any lock that _router_add_route,
   * _router_add_mw, or chttpsvr_subrouter also hold; otherwise deadlock is
   * possible. */
  pthread_rwlock_rdlock(&srv->routes_lock);
  match_result_t mr = _find_route(srv, path, method);
  if (mr.result == ROUTE_MATCH_NONE) {
    pthread_rwlock_unlock(&srv->routes_lock);
    if (path_heap) _mem_free(srv->m_procs, path);
    http_send_error(h, 404);
    return 0;
  }
  if (mr.result == ROUTE_MATCH_METHOD) {
    pthread_rwlock_unlock(&srv->routes_lock);
    if (path_heap) _mem_free(srv->m_procs, path);
    http_send_error(h, 405);
    return 0;
  }
  if (mr.result == ROUTE_MATCH_OOM) {
    pthread_rwlock_unlock(&srv->routes_lock);
    if (path_heap) _mem_free(srv->m_procs, path);
    http_send_error(h, 500);
    return 0;
  }
  /* Build a snapshot of the middleware chain while the read lock is still
   * held.  Walking the list here (rather than after releasing the lock)
   * guarantees the snapshot is consistent with the route found by _find_route
   * and cannot include middleware appended by a concurrent writer after the
   * lock is released.  Global middleware is snapshotted first, then any
   * router-specific middleware. */
  dispatch_ctx_t dispatch;
  memset(&dispatch, 0, sizeof(dispatch));
  dispatch.srv = srv;
  dispatch.router = mr.router;
  dispatch.route = mr.route;
  dispatch.mw_idx = 0;
  {
    int mc = 0;
    chttpsvr_mw_node_t *n;
    for (n = srv->routers[0]->mw_head; n && mc < _CHTTPSVR_MAX_MW; n = n->next)
      dispatch.mw_snap[mc++] = (_mw_entry_t){n->fn, n->ctx};
    bool overflow =
        (n != NULL); /* stopped because cap was reached, not list end */
    if (!overflow && mr.router != srv->routers[0]) {
      for (n = mr.router->mw_head; n && mc < _CHTTPSVR_MAX_MW; n = n->next)
        dispatch.mw_snap[mc++] = (_mw_entry_t){n->fn, n->ctx};
      overflow = (n != NULL);
    }
    dispatch.mw_count = mc;
    if (overflow) {
      pthread_rwlock_unlock(&srv->routes_lock);
      _free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
      if (path_heap) _mem_free(srv->m_procs, path);
      http_send_error(h, 500);
      return 0;
    }
  }
  pthread_rwlock_unlock(&srv->routes_lock);

  /* URL-decode the path in-place now that routing is done.
   * The raw (encoded) path was needed for segment comparison; the user-facing
   * chttpsvr_req_path() API is documented to return the decoded path.
   * http_decode_path_unsafe output is always <= input length, so in-place is
   * safe: the write pointer never overtakes the read pointer.
   * Failure (-1) means malformed percent-encoding; return 400.  This is a
   * defensive check: current segment matching also decodes with hex2byte and
   * would reject any bad encoding before reaching this point, but future
   * routing extensions (wildcards, catch-alls) might not. */
  {
    ssize_t _dlen = http_decode_path_unsafe(path, path);
    if (_dlen < 0) {
      _free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
      if (path_heap) _mem_free(srv->m_procs, path);
      http_send_error(h, 400);
      return 0;
    }
    path[(size_t)_dlen] = '\0';
  }

  /* Extract raw query string. Populated from the request line, which is
   * parsed well before headers complete, so this is available here exactly
   * as it was when this extraction ran after the full body previously. */
  const char *raw_query = NULL;
  fio_str_info_s query_fi;
  memset(&query_fi, 0, sizeof(query_fi));
  if (h->query) {
    query_fi = fiobj_obj2cstr(h->query);
    if (query_fi.len > 0) raw_query = query_fi.data;
  }

  /* --- ALL REQUESTS dispatched through server ctpool via http_pause --- */
  streaming_ctx_t *sctx =
      (streaming_ctx_t *)_mem_calloc(srv->m_procs, 1, sizeof(streaming_ctx_t));
  if (!sctx) {
    _free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
    if (path_heap) _mem_free(srv->m_procs, path);
    http_send_error(h, 500);
    return 0;
  }
  sctx->m_procs = srv->m_procs;
  sctx->srv = srv;
  sctx->router = mr.router;
  sctx->route = mr.route;
  sctx->method = method;
  sctx->h = h;
  sctx->param_values = mr.param_values;
  sctx->param_count = mr.route->param_count;
  sctx->resp.status_code = CHTTP_STATUS_OK;
  sctx->resp.m_procs = srv->m_procs;
  sctx->dispatch = dispatch;

  /* Copy path. */
  sctx->path = ccol_strdup(srv->m_procs, path);
  if (!sctx->path) goto task_oom;

  /* Copy raw query. */
  if (raw_query) {
    sctx->raw_query = ccol_strdup(srv->m_procs, raw_query);
    if (!sctx->raw_query) goto task_oom;
  }

  /* Extract all request headers before http_pause so the reactor thread
   * does not need to touch the FIOBJ after the pause. */
  if (h->headers) {
    hdr_extract_t he;
    memset(&he, 0, sizeof(he));
    he.mp = srv->m_procs;
    fiobj_each2(h->headers, _extract_hdr_cb, &he);
    if (he.oom) {
      for (size_t i = 0; i < he.count; i++) {
        _mem_free(srv->m_procs, he.names[i]);
        _mem_free(srv->m_procs, he.values[i]);
      }
      _mem_free(srv->m_procs, he.names);
      _mem_free(srv->m_procs, he.values);
      goto task_oom;
    }
    sctx->hdr_names = he.names;
    sctx->hdr_values = he.values;
    sctx->hdr_count = he.count;
  }

  if (path_heap) _mem_free(srv->m_procs, path);
  h->udata = sctx;

  /* Increment in_flight_requests BEFORE calling http_pause, not inside the
   * deferred _task_pause_cb. http_pause() only queues _task_pause_cb via
   * fio_defer (it does not run it synchronously) so incrementing inside
   * _task_pause_cb would leave a window, between this call returning and the
   * deferred callback actually running, where __chttpsvr_destroy could
   * observe in_flight_requests == 0 and free the server while this request
   * is still (invisibly) in flight. Incrementing here, before the request
   * can be un-tracked by any code path, closes that window entirely. */
  mutex_lock(srv->mutex);
  srv->in_flight_requests++;
  mutex_unlock(srv->mutex);

  /* Must run before http_pause: http_pause defers the actual handoff via
   * fio_defer, and a worker thread may start running http1_stream_read on
   * another core the moment that deferred task is queued; possibly before
   * this function even returns. http1_stream_prepare establishes every
   * piece of state that worker depends on (diversion flags, the zero-body
   * short-circuit) synchronously, right here, so none of it is still being
   * set up after the handoff has already begun. */
  http1_stream_prepare(h);
  http_pause(h, _task_pause_cb);
  return 1;

task_oom:
  _free_task_ctx(sctx);
  if (path_heap) _mem_free(srv->m_procs, path);
  http_send_error(h, 500);
  return 0;
}

/* ========================================================================== */
/*                         ROUTER INTERNAL HELPERS                            */
/* ========================================================================== */

static chttpsvr_router *_create_router(struct chttpserver *srv,
                                       const char *prefix,
                                       ccol_memmgmt_procs_t *mp) {
  chttpsvr_router *r =
      (chttpsvr_router *)_mem_calloc(mp, 1, sizeof(chttpsvr_router));
  if (!r) return NULL;
  r->prefix = ccol_strdup(mp, prefix ? prefix : "");
  if (!r->prefix) {
    _mem_free(mp, r);
    return NULL;
  }
  /* Strip trailing slashes so "/api/v1/" and "/api/v1" route identically.
   * The root router's empty prefix and the degenerate single-char "/" are
   * left unchanged (the loop condition plen > 1 guards both). */
  size_t plen = strlen(r->prefix);
  while (plen > 1 && r->prefix[plen - 1] == '/') r->prefix[--plen] = '\0';
  r->prefix_len = plen;
  r->srv = srv;
  r->m_procs = mp;
  return r;
}

static void _destroy_router(chttpsvr_router *r, ccol_memmgmt_procs_t *mp) {
  /* Free middleware nodes.  Destroy is called only after all in-flight
   * streams have drained (__chttpsvr_destroy guarantees this), so plain
   * pointer access is safe here. */
  chttpsvr_mw_node_t *mw = r->mw_head;
  while (mw) {
    chttpsvr_mw_node_t *next = mw->next;
    _mem_free(mp, mw);
    mw = next;
  }
  /* Free routes (each entry is an individual allocation). */
  for (size_t i = 0; i < r->route_count; i++) {
    _free_route_data(r->routes[i], mp);
    _mem_free(mp, r->routes[i]);
  }
  _mem_free(mp, r->routes);
  _mem_free(mp, r->prefix);
  _mem_free(mp, r);
}

static ccol_retval_t _router_add_route(chttpsvr_router *router,
                                       chttp_method_t method,
                                       const char *pattern,
                                       chttpsvr_handler_fn fn, void *ctx,
                                       bool is_streaming) {
  if (!router || !pattern || !fn) return ccol_invalid_args;

  ccol_memmgmt_procs_t *mp = router->m_procs;

  /* Allocate and fully initialise the route BEFORE taking the write lock so
   * that readers never observe a partially-constructed entry. */
  chttpsvr_route_t *rt =
      (chttpsvr_route_t *)_mem_alloc(mp, sizeof(chttpsvr_route_t));
  if (!rt) return ccol_not_enough_memory;
  memset(rt, 0, sizeof(*rt));
  rt->method = method;
  rt->fn = fn;
  rt->ctx = ctx;
  rt->is_streaming = is_streaming;

  ccol_retval_t rv = _compile_pattern(pattern, &rt->segs, &rt->seg_count,
                                      &rt->param_names, &rt->param_count, mp);
  if (rv != ccol_success) {
    _mem_free(mp, rt);
    return rv;
  }

  /* Publish the fully-initialised route pointer under the write lock. */
  pthread_rwlock_wrlock(&router->srv->routes_lock);
  if (router->route_count >= router->route_cap) {
    size_t new_cap = router->route_cap * 2 + 4;
    chttpsvr_route_t **nr = (chttpsvr_route_t **)_mem_realloc(
        mp, router->routes, new_cap * sizeof(chttpsvr_route_t *));
    if (!nr) {
      pthread_rwlock_unlock(&router->srv->routes_lock);
      _free_route_data(rt, mp);
      _mem_free(mp, rt);
      return ccol_not_enough_memory;
    }
    router->routes = nr;
    router->route_cap = new_cap;
  }
  router->routes[router->route_count++] = rt;
  pthread_rwlock_unlock(&router->srv->routes_lock);
  return ccol_success;
}

static ccol_retval_t _router_add_mw(chttpsvr_router *router,
                                    chttpsvr_middleware_fn fn, void *ctx) {
  if (!router || !fn) return ccol_invalid_args;
  ccol_memmgmt_procs_t *mp = router->m_procs;
  /* Allocate the node before taking the write lock. */
  chttpsvr_mw_node_t *node =
      (chttpsvr_mw_node_t *)_mem_alloc(mp, sizeof(chttpsvr_mw_node_t));
  if (!node) return ccol_not_enough_memory;
  node->fn = fn;
  node->ctx = ctx;
  node->next = NULL;

  /* Append under the write lock.  The lock provides the ordering needed to
   * make the new node visible to snapshot readers under the read-lock. */
  pthread_rwlock_wrlock(&router->srv->routes_lock);
  if (router->mw_count >= _CHTTPSVR_MAX_MW) {
    pthread_rwlock_unlock(&router->srv->routes_lock);
    _mem_free(mp, node);
    return ccol_not_permitted;
  }
  if (!router->mw_tail) {
    router->mw_head = router->mw_tail = node;
  } else {
    router->mw_tail->next = node;
    router->mw_tail = node;
  }
  router->mw_count++;
  pthread_rwlock_unlock(&router->srv->routes_lock);
  return ccol_success;
}

/* ========================================================================== */
/*                         QUERY PARAMETER PARSING                            */
/* ========================================================================== */

static ccol_retval_t _parse_qparams(const char *raw_query,
                                    chttpsvr_qparams_t **qp_out,
                                    ccol_memmgmt_procs_t *mp) {
  chttpsvr_qparams_t *qp =
      (chttpsvr_qparams_t *)_mem_calloc(mp, 1, sizeof(chttpsvr_qparams_t));
  if (!qp) return ccol_not_enough_memory;
  qp->m_procs = mp;

  const char *p = raw_query;
  while (p && *p) {
    /* Find end of this pair. */
    const char *amp = strchr(p, '&');
    size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
    const char *eq = (const char *)memchr(p, '=', pair_len);

    size_t key_raw_len = eq ? (size_t)(eq - p) : pair_len;
    const char *val_raw = eq ? eq + 1 : NULL;
    size_t val_raw_len = eq ? pair_len - key_raw_len - 1 : 0;

    /* Decode key. */
    char key_src[512];
    char *key_sp = key_src;
    bool kh = false;
    if (key_raw_len + 1 > sizeof(key_src)) {
      key_sp = (char *)_mem_alloc(mp, key_raw_len + 1);
      kh = true;
      if (!key_sp) goto oom;
    }
    memcpy(key_sp, p, key_raw_len);
    /* URL query uses + for space. */
    for (size_t i = 0; i < key_raw_len; i++) {
      if (key_sp[i] == '+') key_sp[i] = ' ';
    }
    key_sp[key_raw_len] = '\0';
    char *key_decoded = (char *)_mem_alloc(mp, key_raw_len + 1);
    if (!key_decoded) {
      if (kh) _mem_free(mp, key_sp);
      goto oom;
    }
    ssize_t kl = http_decode_url_unsafe(key_decoded, key_sp);
    if (kh) _mem_free(mp, key_sp);
    if (kl < 0) {
      _mem_free(mp, key_decoded);
      goto next;
    }
    key_decoded[(size_t)kl] = '\0';

    /* Decode value. */
    char *val_decoded = NULL;
    if (val_raw) {
      char val_src[512];
      char *val_sp = val_src;
      bool vh = false;
      if (val_raw_len + 1 > sizeof(val_src)) {
        val_sp = (char *)_mem_alloc(mp, val_raw_len + 1);
        vh = true;
        if (!val_sp) {
          _mem_free(mp, key_decoded);
          goto oom;
        }
      }
      memcpy(val_sp, val_raw, val_raw_len);
      for (size_t i = 0; i < val_raw_len; i++) {
        if (val_sp[i] == '+') val_sp[i] = ' ';
      }
      val_sp[val_raw_len] = '\0';
      val_decoded = (char *)_mem_alloc(mp, val_raw_len + 1);
      if (!val_decoded) {
        if (vh) _mem_free(mp, val_sp);
        _mem_free(mp, key_decoded);
        goto oom;
      }
      ssize_t vl = http_decode_url_unsafe(val_decoded, val_sp);
      if (vh) _mem_free(mp, val_sp);
      if (vl < 0) {
        _mem_free(mp, val_decoded);
        _mem_free(mp, key_decoded);
        goto next;
      }
      val_decoded[(size_t)vl] = '\0';
    } else {
      val_decoded = ccol_strdup(mp, "");
      if (!val_decoded) {
        _mem_free(mp, key_decoded);
        goto oom;
      }
    }

    /* Grow arrays if needed. */
    if (qp->count >= qp->cap) {
      size_t nc = qp->cap * 2 + 8;
      char **nk = (char **)_mem_realloc(mp, qp->keys, nc * sizeof(char *));
      if (!nk) {
        _mem_free(mp, key_decoded);
        _mem_free(mp, val_decoded);
        goto oom;
      }
      qp->keys = nk;
      char **nv = (char **)_mem_realloc(mp, qp->values, nc * sizeof(char *));
      if (!nv) {
        /* qp->keys already points to nk (updated above).  qp->cap is
         * intentionally NOT updated here: it now understates the actual
         * keys-array capacity but remains consistent with the values-array
         * capacity.  The oom cleanup path iterates by qp->count (not
         * qp->cap) for individual string entries, then frees each array
         * pointer once; both arrays are released correctly. */
        _mem_free(mp, key_decoded);
        _mem_free(mp, val_decoded);
        goto oom;
      }
      qp->values = nv;
      qp->cap = nc;
    }
    qp->keys[qp->count] = key_decoded;
    qp->values[qp->count] = val_decoded;
    qp->count++;

  next:
    p = amp ? amp + 1 : NULL;
  }

  *qp_out = qp;
  return ccol_success;

oom:
  for (size_t i = 0; i < qp->count; i++) {
    _mem_free(mp, qp->keys[i]);
    _mem_free(mp, qp->values[i]);
  }
  _mem_free(mp, qp->keys);
  _mem_free(mp, qp->values);
  _mem_free(mp, qp);
  return ccol_not_enough_memory;
}

static chttpsvr_qparams_t *_ensure_qparams(chttpsvr_req *req) {
  if (req->_qparams) return req->_qparams;
  /* _qparams_attempted guards against retrying a failed allocation on every
   * subsequent query call.  Once set, any path that leaves _qparams NULL (both
   * the empty-query sentinel calloc failure and the parse OOM path) returns
   * NULL immediately rather than spinning in a tight OOM retry loop. */
  if (req->_qparams_attempted) return NULL;
  req->_qparams_attempted = true;

  if (!req->raw_query || !*req->raw_query) {
    chttpsvr_qparams_t *qp = (chttpsvr_qparams_t *)_mem_calloc(
        req->m_procs, 1, sizeof(chttpsvr_qparams_t));
    if (!qp) return NULL;
    qp->m_procs = req->m_procs;
    req->_qparams = qp;
    return qp;
  }
  if (_parse_qparams(req->raw_query, &req->_qparams, req->m_procs) !=
      ccol_success) {
    /* OOM during parse.  Record it so that chttpsvr_req_query_one can
     * distinguish a parse failure from a genuine key-absent result (both
     * would otherwise surface as an empty param set). */
    req->_qparams_parse_oom = true;
    /* Install an empty sentinel so that subsequent calls take the fast path
     * (_qparams non-NULL) and _destroy_req_qparams has a valid struct to
     * clean up.  If the sentinel allocation also fails, return NULL; the
     * OOM flag is already set regardless. */
    chttpsvr_qparams_t *qp = (chttpsvr_qparams_t *)_mem_calloc(
        req->m_procs, 1, sizeof(chttpsvr_qparams_t));
    if (qp) {
      qp->m_procs = req->m_procs;
      req->_qparams = qp;
      return qp;
    }
    return NULL;
  }
  return req->_qparams;
}

/* ========================================================================== */
/*                         PUBLIC API IMPLEMENTATION                          */
/* ========================================================================== */

chttpsvr create_chttpsvr_mp(ccol_memmgmt_procs_t *mprocs, clog cl,
                            char **err_str) {
  if (!ccol_verify_memmgmt_procs(mprocs, err_str)) return NULL;

  struct chttpserver *srv =
      (struct chttpserver *)_mem_calloc(mprocs, 1, sizeof(struct chttpserver));
  if (!srv) {
    if (err_str) *err_str = CCOL_ERR_STR("out of memory");
    return NULL;
  }

  if (mprocs) {
    srv->m_procs = (ccol_memmgmt_procs_t *)_mem_alloc(
        mprocs, sizeof(ccol_memmgmt_procs_t));
    if (!srv->m_procs) {
      _mem_free(mprocs, srv);
      if (err_str) *err_str = CCOL_ERR_STR("out of memory");
      return NULL;
    }
    memcpy(srv->m_procs, mprocs, sizeof(ccol_memmgmt_procs_t));
  }

  /* Set up the server-owned logger.  cl == NULL: create a minimal internal
   * logger that writes only FATAL messages to stderr.  cl != NULL: derive a
   * new logger from it (tagged component=http-server) that this server will
   * manage; the caller's handle is never stored directly and is left
   * untouched (and still owned by the caller). */
  clog logger = cl ? clog_derive(cl) : clog_open_fd_mp(2, CLOG_FATAL, mprocs);
  if (logger && cl) clog_set_field(logger, "component", "http-server");
  if (!logger) {
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("failed to create server logger");
    return NULL;
  }

  if (mutex_init(srv->mutex) != 0) {
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("mutex_init failed");
    return NULL;
  }

  {
    pthread_condattr_t _cv_attr;
    int _cv_rc = 0;
    if (pthread_condattr_init(&_cv_attr) == 0) {
      pthread_condattr_setclock(&_cv_attr, CLOCK_MONOTONIC);
      _cv_rc = pthread_cond_init(&srv->requests_done_cv, &_cv_attr);
      pthread_condattr_destroy(&_cv_attr);
    } else {
      _cv_rc = 1;
    }
    if (_cv_rc != 0) {
      mutex_destroy(srv->mutex);
      clog_close(logger);
      _mem_free(mprocs, srv->m_procs);
      _mem_free(mprocs, srv);
      if (err_str) *err_str = CCOL_ERR_STR("cond_var_init failed");
      return NULL;
    }
  }

  if (pthread_rwlock_init(&srv->routes_lock, NULL) != 0) {
    cond_var_destroy(srv->requests_done_cv);
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("pthread_rwlock_init failed");
    return NULL;
  }

  /* Create the root router (prefix = ""). */
  chttpsvr_router *root = _create_router(srv, "", srv->m_procs);
  if (!root) {
    pthread_rwlock_destroy(&srv->routes_lock);
    cond_var_destroy(srv->requests_done_cv);
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("out of memory");
    return NULL;
  }

  srv->routers =
      (chttpsvr_router **)_mem_alloc(srv->m_procs, sizeof(chttpsvr_router *));
  if (!srv->routers) {
    _destroy_router(root, srv->m_procs);
    pthread_rwlock_destroy(&srv->routes_lock);
    cond_var_destroy(srv->requests_done_cv);
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("out of memory");
    return NULL;
  }
  srv->routers[0] = root;
  srv->router_count = 1;
  srv->router_cap = 1;
  srv->cl = logger;
  srv->listen_uuid = -1;
  srv->started = false;
  return srv;
}

/* Waits (bounded by a 30-second safety-net timeout: a permanent hang is
 * worse than a potential use-after-free in that degenerate edge case) for
 * every already-dispatched request to finish, then atomically detaches
 * srv->worker_pool (NULLing it under srv->mutex) and returns the detached
 * pool so the caller can drain and destroy it outside the lock.
 *
 * Waiting first is what makes the detach-then-destroy safe: in_flight_requests
 * spans exactly the window from http_pause (in _on_headers_complete, before
 * _task_pause_cb ever reads worker_pool) through the matching http_resume, so
 * once it reaches zero no _task_pause_cb call can be mid-flight holding a
 * stale local copy of the pool pointer this function is about to hand to
 * ctpool_shutdown_drain/ctpool_destroy. This matters because an
 * already-accepted keep-alive connection's _on_headers_complete does not
 * check srv->started, so it (and the _task_pause_cb it triggers) can still
 * fire while chttpsvr_start is mid-restart or __chttpsvr_destroy is tearing
 * the server down.
 *
 * Used by both __chttpsvr_destroy and chttpsvr_start (leftover-pool cleanup
 * on a restart, and every failure-path teardown after the pool has already
 * been assigned to srv->worker_pool), so a pool is never destroyed while a
 * concurrent request could still be about to submit to it. Caller must hold
 * no lock on entry. */
static ctpool _wait_and_detach_worker_pool(chttpsvr srv) {
  mutex_lock(srv->mutex);
  if (srv->in_flight_requests > 0) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 30;
    while (srv->in_flight_requests > 0) {
      if (cond_var_timedwait(srv->requests_done_cv, srv->mutex, deadline) ==
          ETIMEDOUT)
        break;
    }
  }
  ctpool old_pool = srv->worker_pool;
  srv->worker_pool = NULL;
  mutex_unlock(srv->mutex);
  return old_pool;
}

void __chttpsvr_destroy(chttpsvr srv) {
  if (!srv) return;

  /* Atomically close the listener and claim the stop. */
  mutex_lock(srv->mutex);
  bool was_started = srv->started;
  intptr_t uuid = srv->listen_uuid;
  if (was_started) {
    srv->started = false;
    srv->listen_uuid = -1;
  }
  mutex_unlock(srv->mutex);
  if (was_started) fio_close(uuid);

  /* Release this server's single shared-engine reference (acquired once, in
   * chttpsvr_start, for this server's entire lifetime) exactly once, only if
   * it was ever actually acquired. Guarded by srv->mutex, not a global lock;
   * contributed_to_engine is purely this server's own state now that the
   * shared reference count itself lives in cfio_engine.c.
   *
   * _cfio_engine_release() hands the actual reactor stop-and-join off to a
   * detached reaper thread rather than performing it inline when this is the
   * last reference (see cfio_engine.h); so, unlike the old per-module
   * engine, dropping the last reference here does NOT guarantee the shared
   * reactor has fully stopped by the time this function returns. A caller
   * that needs that guarantee (e.g. before reusing the just-freed port, or
   * before process exit) must call chttpsvr_engine_wait() afterward. */
  bool should_release_engine = false;
  mutex_lock(srv->mutex);
  if (srv->contributed_to_engine) {
    srv->contributed_to_engine = false;
    should_release_engine = true;
  }
  mutex_unlock(srv->mutex);
  if (should_release_engine) _cfio_engine_release();

  /* Block until every in-flight task has called http_resume and its
   * send/cleanup callback has completed, then drain and destroy the
   * server-owned worker pool. All handlers now run through the server's
   * ctpool, so in_flight_requests tracks all dispatched work.
   *
   * For the last reference: _cfio_engine_release() above triggers fio_stop()
   * on its reaper thread momentarily, which signals the reactor to drain;
   * the reactor fires all pending http_resume callbacks as it winds down.
   * For non-last references: the reactor keeps running for other servers (or
   * for chttpclient's async engine), so http_resume callbacks fire
   * naturally regardless. Either way this wait does not depend on the exact
   * timing of the reaper's fio_stop() call; a worker thread calls
   * http_resume when it finishes regardless of engine-stop state.
   *
   * See _wait_and_detach_worker_pool's own comment for why waiting for
   * in_flight_requests to drain before touching worker_pool is required, not
   * just a nice-to-have. */
  ctpool old_pool = _wait_and_detach_worker_pool(srv);
  if (old_pool) {
    ctpool_shutdown_drain(old_pool);
    ctpool_destroy(old_pool);
  }

  if (srv->tls) {
    fio_tls_destroy(srv->tls);
    srv->tls = NULL;
  }

  for (size_t i = 0; i < srv->router_count; i++) {
    _destroy_router(srv->routers[i], srv->m_procs);
  }
  _mem_free(srv->m_procs, srv->routers);
  mutex_destroy(srv->mutex);
  cond_var_destroy(srv->requests_done_cv);
  pthread_rwlock_destroy(&srv->routes_lock);

  clog_close(srv->cl);
  srv->cl = NULL;

  ccol_memmgmt_procs_t *mp = srv->m_procs;
  _mem_free(mp, srv);
  _mem_free(mp, mp);
}

ccol_retval_t chttpsvr_start(chttpsvr srv, const chttpsvr_config_t *cfg) {
  if (!srv) return ccol_invalid_args;
  if (cfg && cfg->port == 0) return ccol_invalid_args;

  static const chttpsvr_config_t default_cfg = {
      .host = "0.0.0.0",
      .port = 8080,
      .max_body_size = (4U * 1024U * 1024U),
      .read_timeout_ms = 0,
      .idle_timeout_ms = 0,
      .stream_read_timeout_ms = 30000,
      .tls = NULL,
      .worker_thread_count = 0,
      .worker_queue_capacity = 0,
  };
  if (!cfg) cfg = &default_cfg;

  /* Guard against double-start on the same server instance. */
  mutex_lock(srv->mutex);
  if (srv->started) {
    mutex_unlock(srv->mutex);
    return ccol_not_permitted;
  }
  mutex_unlock(srv->mutex);

  /* Drain and destroy any pool left over from a previous start/stop cycle.
   * chttpsvr_stop only closes the listener; already-accepted keep-alive
   * connections keep running and their _on_headers_complete does not check
   * srv->started, so one can still be dispatching through the leftover pool
   * right up until _wait_and_detach_worker_pool's in_flight_requests wait
   * confirms otherwise. See that helper's comment for the full reasoning. */
  {
    ctpool leftover_pool = _wait_and_detach_worker_pool(srv);
    if (leftover_pool) {
      ctpool_shutdown_drain(leftover_pool);
      ctpool_destroy(leftover_pool);
    }
  }

  /* Determine worker thread count and queue capacity. */
  int nthreads = cfg->worker_thread_count;
  if (nthreads <= 0) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    nthreads = (cpus > 0) ? (int)cpus : 1;
  }
  size_t queue_cap;
  if (cfg->worker_queue_capacity == CHTTPSVR_QUEUE_UNBOUNDED) {
    queue_cap = 0; /* ctpool: 0 means unbounded */
  } else if (cfg->worker_queue_capacity == 0) {
    queue_cap = (size_t)1024 * (size_t)nthreads; /* library default */
  } else {
    queue_cap = cfg->worker_queue_capacity;
  }

  char *pool_err = NULL;
  ctpool new_pool = create_cthread_pool_mp((size_t)nthreads, queue_cap,
                                           srv->m_procs, &pool_err);
  if (!new_pool) return ccol_not_enough_memory;
  mutex_lock(srv->mutex);
  srv->worker_pool = new_pool;
  mutex_unlock(srv->mutex);

  srv->stream_read_timeout_ms = cfg->stream_read_timeout_ms;
  srv->max_body_read_duration_ms = cfg->max_body_read_duration_ms;

  /* Compute timeout in seconds (facil.io uses uint8_t seconds).
   *
   * idle_timeout_ms, when non-zero, overrides read_timeout_ms as the
   * connection timeout.  Both map to the single facil.io timeout parameter;
   * idle_timeout_ms takes precedence because it more directly describes the
   * keep-alive idle semantics that facil.io enforces on quiescent
   * connections. */
  uint8_t timeout_sec = 0;
  {
    long _eff_ms = (cfg->idle_timeout_ms > 0) ? cfg->idle_timeout_ms
                                              : cfg->read_timeout_ms;
    if (_eff_ms > 0) {
      long sec = _eff_ms / 1000;
      timeout_sec = (sec > 255) ? 255 : (sec == 0 ? 1 : (uint8_t)sec);
    }
  }

  /* Build port string. */
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", cfg->port);

  /* Release any TLS context left over from a previous start/stop cycle.
   * fio_tls_s is reference-counted: fio_tls_new starts at ref=1 and
   * http_listen calls fio_tls_dup (ref=2).  After chttpsvr_stop the
   * listener is scheduled for close asynchronously; facil.io still holds
   * its reference.  Our fio_tls_destroy here only decrements our own ref;
   * the memory is freed when facil.io releases the last reference. */
  if (srv->tls) {
    fio_tls_destroy(srv->tls);
    srv->tls = NULL;
  }

  /* Configure TLS if requested. */
  fio_tls_s *tls = NULL;
  if (cfg->tls && cfg->tls->cert_path && cfg->tls->key_path) {
    tls = fio_tls_new(cfg->host, cfg->tls->cert_path, cfg->tls->key_path, NULL);
    if (!tls) {
      ctpool failed_pool = _wait_and_detach_worker_pool(srv);
      if (failed_pool) {
        ctpool_shutdown_drain(failed_pool);
        ctpool_destroy(failed_pool);
      }
      return ccol_unexpected_failure;
    }
    if (cfg->tls->ca_bundle_path) fio_tls_trust(tls, cfg->tls->ca_bundle_path);
  }

  /* Acquire this server's single shared-engine reference, once for its
   * entire lifetime (across any number of start/stop/restart cycles);
   * guarded by srv->mutex, atomically claiming the "first start of this
   * server" slot before actually acquiring, and rolling the claim back if
   * the acquire itself fails. On a restart (stop + start again on the same
   * srv) contributed_to_engine is already true, so this is skipped entirely
   * and the reference from the original start is reused. */
  bool need_acquire;
  mutex_lock(srv->mutex);
  need_acquire = !srv->contributed_to_engine;
  if (need_acquire) srv->contributed_to_engine = true;
  mutex_unlock(srv->mutex);

  if (need_acquire) {
    /* Install a minimal default engine logger if the caller has not already
     * set one via chttpsvr_set_engine_logger(); see
     * _install_default_engine_logger's own comment for why this runs on
     * every server's first start rather than being a one-shot,
     * process-lifetime guard. */
    _install_default_engine_logger();

    ccol_retval_t engine_rc = _cfio_engine_acquire();
    if (engine_rc != ccol_success) {
      mutex_lock(srv->mutex);
      srv->contributed_to_engine = false; /* roll back; nothing was acquired */
      mutex_unlock(srv->mutex);
      if (tls) fio_tls_destroy(tls);
      {
        ctpool failed_pool = _wait_and_detach_worker_pool(srv);
        if (failed_pool) {
          ctpool_shutdown_drain(failed_pool);
          ctpool_destroy(failed_pool);
        }
      }
      return engine_rc;
    }
  }

  /* The shared reactor is now confirmed running (per _cfio_engine_acquire's
   * contract: it always blocks until the reactor has entered its event
   * loop before returning success); http_listen always takes its
   * fio_attach-immediately path (fio_is_running() is true), never the
   * FIO_CALL_ON_START-deferred path. Verified via direct read of
   * fio_listen() in fio.c: the socket bind itself (fio_socket()) is
   * unconditional; only attachment timing depends on fio_is_running(), so
   * there is no first-vs-subsequent-start branching left to do here; this
   * single call covers both cases. */
  intptr_t uuid = http_listen(
      port_str, cfg->host, .on_request = _on_request_unreachable,
      .on_headers_complete = _on_headers_complete, .udata = srv,
      .max_body_size = cfg->max_body_size, .timeout = timeout_sec, .tls = tls);
  if (uuid < 0) {
    /* Deliberately do NOT release the engine reference here (whether just
     * acquired above, or already held from an earlier start of this same
     * srv): contributed_to_engine stays true and __chttpsvr_destroy will
     * release it later, mirroring this module's pre-unification behavior
     * for this exact failure path. */
    if (tls) fio_tls_destroy(tls);
    {
      ctpool failed_pool = _wait_and_detach_worker_pool(srv);
      if (failed_pool) {
        ctpool_shutdown_drain(failed_pool);
        ctpool_destroy(failed_pool);
      }
    }
    return ccol_unexpected_failure;
  }

  /* Store the TLS reference so __chttpsvr_destroy can release it when the
   * server is torn down.  facil.io's reference (from fio_tls_dup inside
   * http_listen) keeps the context alive for ongoing TLS handshakes.
   *
   * Committed under the mutex so a concurrent chttpsvr_stop cannot read a
   * torn/stale view of these three fields: chttpsvr_stop reads `started`
   * and `listen_uuid` under the same lock, and without it a stop racing the
   * tail end of a start could see the pre-start state (started == false)
   * and silently no-op, even though this call is about to (or just did)
   * mark the server started; the caller's stop would then be lost. */
  mutex_lock(srv->mutex);
  srv->tls = tls;
  srv->listen_uuid = uuid;
  srv->started = true;
  mutex_unlock(srv->mutex);

  return ccol_success;
}

void chttpsvr_stop(chttpsvr srv) {
  if (!srv) return;
  /* Atomically claim the stop to prevent concurrent callers from issuing
   * multiple fio_close calls on the same uuid. */
  mutex_lock(srv->mutex);
  bool was_started = srv->started;
  intptr_t uuid = srv->listen_uuid;
  if (was_started) {
    srv->started = false;
    srv->listen_uuid = -1;
  }
  mutex_unlock(srv->mutex);
  /* fio_close schedules the listener socket for close on the reactor; the
   * shared engine and all other registered listeners are unaffected. */
  if (was_started) fio_close(uuid);
}

ccol_retval_t chttpsvr_set_engine_logger(clog cl) {
  if (!cl) return ccol_invalid_args;
  clog derived = clog_derive(cl);
  if (!derived) return ccol_not_enough_memory;
  clog_set_field(derived, "component", "http-engine");
  /* fio_set_logger takes ownership of derived: it closes the previous logger
   * (if any) after swapping the pointer under the write lock. */
  fio_set_logger(derived);
  return ccol_success;
}

ccol_retval_t chttpsvr_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp) {
  if (mp && (!mp->malloc || !mp->free || !mp->calloc || !mp->realloc))
    return ccol_invalid_args;
  if (_cfio_engine_running()) return ccol_not_permitted;
  fio_set_mem_mgmt_procs((const struct ccol_memmgmt_procs_t *)mp);
  return ccol_success;
}

/*
 * Now that the reactor is shared with chttpclient's async engine, forcing it
 * down also tears down any in-flight chttpclient async work in the same
 * process; an inherent, correct consequence of sharing one process-wide
 * reactor for what is meant to be process-shutdown-driven use (this
 * function's documented contract, unchanged: async-signal-safe, non-blocking,
 * safe to call from a SIGINT/SIGTERM handler), not a defect.
 */
void chttpsvr_engine_stop(void) { _cfio_engine_force_stop(); }

void chttpsvr_engine_wait(void) {
  _cfio_engine_wait_until_stopped();
  /* fio_lib_destroy (registered via atexit as part of the shared engine's
   * one-time global init) fires after all other cleanup and calls logging
   * functions.  Clear g_fio_logger now so those calls hit the null-guard in
   * _fio_vlog and do not access a freed handle. */
  fio_set_logger(NULL);
}

ccol_retval_t chttpsvr_register_handler(chttpsvr srv, chttp_method_t method,
                                        const char *pattern,
                                        chttpsvr_handler_fn fn, void *ctx) {
  if (!srv || !pattern || !fn) return ccol_invalid_args;
  return _router_add_route(srv->routers[0], method, pattern, fn, ctx, false);
}

ccol_retval_t chttpsvr_register_streaming_handler(chttpsvr srv,
                                                  chttp_method_t method,
                                                  const char *pattern,
                                                  chttpsvr_handler_fn fn,
                                                  void *ctx) {
  if (!srv || !pattern || !fn) return ccol_invalid_args;
  return _router_add_route(srv->routers[0], method, pattern, fn, ctx, true);
}

ccol_retval_t chttpsvr_use(chttpsvr srv, chttpsvr_middleware_fn fn, void *ctx) {
  if (!srv || !fn) return ccol_invalid_args;
  return _router_add_mw(srv->routers[0], fn, ctx);
}

chttpsvr_router *chttpsvr_subrouter(chttpsvr srv, const char *prefix) {
  if (!srv || !prefix || prefix[0] != '/') return NULL;

  /* Reject prefixes that contain consecutive slashes (e.g. "//api" or
   * "/api//v1").  Such prefixes can never match a valid HTTP path (the router
   * matching code checks path[prefix_len] == '/' || '\0', which requires the
   * prefix itself to be a clean path segment sequence) and would silently
   * create an unreachable router. */
  if (strstr(prefix, "//")) return NULL;

  /* Create the router object outside the write lock (no shared-state access).
   */
  chttpsvr_router *r = _create_router(srv, prefix, srv->m_procs);
  if (!r) return NULL;

  /* Publish under the write lock so _find_route readers never see a
   * partially-updated routers array. */
  pthread_rwlock_wrlock(&srv->routes_lock);
  if (srv->router_count >= srv->router_cap) {
    size_t nc = srv->router_cap * 2 + 2;
    chttpsvr_router **nr = (chttpsvr_router **)_mem_realloc(
        srv->m_procs, srv->routers, nc * sizeof(chttpsvr_router *));
    if (!nr) {
      pthread_rwlock_unlock(&srv->routes_lock);
      _destroy_router(r, srv->m_procs);
      return NULL;
    }
    srv->routers = nr;
    srv->router_cap = nc;
  }
  srv->routers[srv->router_count++] = r;
  pthread_rwlock_unlock(&srv->routes_lock);
  return r;
}

ccol_retval_t chttpsvr_router_on(chttpsvr_router *router, chttp_method_t method,
                                 const char *pattern, chttpsvr_handler_fn fn,
                                 void *ctx) {
  if (!router || !pattern || !fn) return ccol_invalid_args;
  return _router_add_route(router, method, pattern, fn, ctx, false);
}

ccol_retval_t chttpsvr_router_on_stream(chttpsvr_router *router,
                                        chttp_method_t method,
                                        const char *pattern,
                                        chttpsvr_handler_fn fn, void *ctx) {
  if (!router || !pattern || !fn) return ccol_invalid_args;
  return _router_add_route(router, method, pattern, fn, ctx, true);
}

ccol_retval_t chttpsvr_router_use(chttpsvr_router *router,
                                  chttpsvr_middleware_fn fn, void *ctx) {
  if (!router || !fn) return ccol_invalid_args;
  return _router_add_mw(router, fn, ctx);
}

/* ========================================================================== */
/*                         REQUEST API                                        */
/* ========================================================================== */

chttp_method_t chttpsvr_req_method(const chttpsvr_req *req) {
  if (!req) return CHTTP_GET;
  return req->method;
}

const char *chttpsvr_req_path(const chttpsvr_req *req) {
  if (!req) return NULL;
  return req->path;
}

const char *chttpsvr_req_header(const chttpsvr_req *req, const char *name) {
  if (!req || !name) return NULL;
  /* All requests are dispatched through ctpool with pre-extracted headers. */
  for (size_t i = 0; i < req->_hdr_count; i++) {
    if (strcasecmp(req->_hdr_names[i], name) == 0) return req->_hdr_values[i];
  }
  return NULL;
}

const void *chttpsvr_req_body(const chttpsvr_req *req, size_t *len_out) {
  if (!req) {
    if (len_out) *len_out = 0;
    return NULL;
  }
  if (len_out) *len_out = req->body_len;
  return req->body_data;
}

ssize_t chttpsvr_req_read(chttpsvr_req *req, void *buf, size_t buflen) {
  if (!req) return -1;
  if (buflen == 0) return 0; /* read zero bytes: valid no-op, not an error */
  if (!buf) return -1;
  if (!req->is_streaming) return -1;
  if (!req->_h) return -1;
  unsigned timeout_ms =
      req->_dispatch ? req->_dispatch->srv->stream_read_timeout_ms : 0;
  if (!_check_read_deadline(req, &timeout_ms)) return -1;
  return http1_stream_read((http_s *)req->_h, buf, buflen, timeout_ms);
}

ccol_retval_t chttpsvr_req_stream_error(const chttpsvr_req *req) {
  if (!req || !req->_h) return ccol_unexpected_failure;
  if (req->_deadline_exceeded) return ccol_timed_out;
  switch (http1_stream_last_error((http_s *)req->_h)) {
    case HTTP1_STREAM_ERR_TIMEOUT:
      return ccol_timed_out;
    case HTTP1_STREAM_ERR_TOO_LARGE:
      return ccol_msg_too_large;
    case HTTP1_STREAM_ERR_CLOSED:
    case HTTP1_STREAM_ERR_PROTOCOL:
      return ccol_http_transfer_aborted;
    case HTTP1_STREAM_ERR_NONE:
    default:
      return ccol_success;
  }
}

const char *chttpsvr_req_param(const chttpsvr_req *req, const char *name) {
  if (!req || !name) return NULL;
  for (int i = 0; i < req->param_count; i++) {
    if (req->param_names[i] && strcmp(req->param_names[i], name) == 0) {
      return req->param_values[i];
    }
  }
  return NULL;
}

const char **chttpsvr_req_query(chttpsvr_req *req, const char *key,
                                size_t *count_out) {
  if (!req || !key) {
    if (count_out) *count_out = 0;
    return NULL;
  }

  chttpsvr_qparams_t *qp = _ensure_qparams(req);
  if (!qp) {
    if (count_out) *count_out = 0;
    return NULL;
  }

  /* Count matching pairs. */
  size_t n = 0;
  for (size_t i = 0; i < qp->count; i++) {
    if (strcmp(qp->keys[i], key) == 0) n++;
  }
  if (n == 0) {
    if (count_out) *count_out = 0;
    return NULL;
  }

  /* Build result array (NULL-terminated). */
  if (n + 1 > req->_qresult_cap) {
    const char **nr = (const char **)_mem_realloc(req->m_procs, req->_qresult,
                                                  (n + 1) * sizeof(char *));
    if (!nr) {
      req->_qresult_oom = true;
      if (count_out) *count_out = 0;
      return NULL;
    }
    req->_qresult = nr;
    req->_qresult_cap = n + 1;
  }
  size_t idx = 0;
  for (size_t i = 0; i < qp->count; i++) {
    if (strcmp(qp->keys[i], key) == 0) req->_qresult[idx++] = qp->values[i];
  }
  req->_qresult[idx] = NULL;
  if (count_out) *count_out = n;
  return req->_qresult;
}

ccol_retval_t chttpsvr_req_query_one(chttpsvr_req *req, const char *key,
                                     const char **val_out) {
  if (!req || !key) return ccol_invalid_args;

  /* Go directly to the parsed parameter store so OOM is distinguishable from
   * "key absent": chttpsvr_req_query returns (NULL, count=0) for both cases,
   * making them indistinguishable at that level. */
  chttpsvr_qparams_t *qp = _ensure_qparams(req);
  if (!qp) return ccol_not_enough_memory;
  /* Even when a fallback sentinel was installed (qp != NULL), the underlying
   * parse may have failed with OOM.  Propagate the error so the caller is not
   * misled into thinking the key was simply absent from the query string. */
  if (req->_qparams_parse_oom) return ccol_not_enough_memory;

  size_t n = 0;
  const char *found_val = NULL;
  for (size_t i = 0; i < qp->count; i++) {
    if (strcmp(qp->keys[i], key) == 0) {
      n++;
      found_val = qp->values[i];
    }
  }
  if (n == 0) {
    if (val_out) *val_out = NULL;
    return ccol_key_not_found;
  }
  if (n > 1) {
    if (val_out) *val_out = NULL;
    return ccol_not_permitted;
  }
  if (val_out) *val_out = found_val;
  return ccol_success;
}

const char *chttpsvr_req_raw_query(const chttpsvr_req *req) {
  if (!req) return NULL;
  return req->raw_query;
}

bool chttpsvr_req_query_oom(const chttpsvr_req *req) {
  if (!req) return false;
  return req->_qresult_oom || req->_qparams_parse_oom;
}

/* ========================================================================== */
/*                         RESPONSE API                                       */
/* ========================================================================== */

void chttpsvr_resp_set_status(chttpsvr_resp *resp, int status_code) {
  if (resp) resp->status_code = status_code;
}

ccol_retval_t chttpsvr_resp_set_header(chttpsvr_resp *resp, const char *name,
                                       const char *value) {
  if (!resp || !name || !value) return ccol_invalid_args;
  ccol_memmgmt_procs_t *mp = resp->m_procs;

  /* Case-insensitive scan for an existing entry.  When a duplicate is found
   * the stored value is replaced in-place; the stored name retains the casing
   * from the FIRST call (HTTP header names are case-insensitive per RFC 7230,
   * so this does not affect wire-level correctness).  The flat-array layout
   * keeps this O(n) scan cache-friendly for the typical response header count
   * of fewer than ~20 entries. */
  for (size_t i = 0; i < resp->header_count; i++) {
    if (strcasecmp(resp->headers[i].name, name) == 0) {
      char *new_val = ccol_strdup(mp, value);
      if (!new_val) return ccol_not_enough_memory;
      _mem_free(mp, resp->headers[i].value);
      resp->headers[i].value = new_val;
      return ccol_success;
    }
  }

  /* New header; grow the flat array if needed. */
  if (resp->header_count >= resp->header_cap) {
    if (resp->header_cap > (SIZE_MAX - 8) / 2) return ccol_not_enough_memory;
    size_t new_cap = resp->header_cap * 2 + 8;
    if (new_cap > SIZE_MAX / sizeof(resp_header_t))
      return ccol_not_enough_memory;
    resp_header_t *nh = (resp_header_t *)_mem_realloc(
        mp, resp->headers, new_cap * sizeof(resp_header_t));
    if (!nh) return ccol_not_enough_memory;
    resp->headers = nh;
    resp->header_cap = new_cap;
  }

  char *n_name = ccol_strdup(mp, name);
  char *n_val = ccol_strdup(mp, value);
  if (!n_name || !n_val) {
    _mem_free(mp, n_name);
    _mem_free(mp, n_val);
    return ccol_not_enough_memory;
  }
  resp->headers[resp->header_count].name = n_name;
  resp->headers[resp->header_count].value = n_val;
  resp->header_count++;
  return ccol_success;
}

ccol_retval_t chttpsvr_resp_write(chttpsvr_resp *resp, const void *data,
                                  size_t len) {
  if (!resp || (!data && len > 0)) return ccol_invalid_args;
  if (len == 0) return ccol_success;

  ccol_memmgmt_procs_t *mp = resp->m_procs;

  /* Grow buffer if needed.  Use subtraction-based check (body_len <= body_cap
   * is a class invariant) to avoid the addition overflow that would occur with
   * the naive body_len + len > body_cap form. */
  if (len > resp->body_cap - resp->body_len) {
    /* Guard against len + body_len wrapping before we compute min_cap. */
    if (len > SIZE_MAX - resp->body_len) return ccol_not_enough_memory;
    size_t min_cap = resp->body_len + len;
    size_t new_cap = min_cap;
    /* Double the current cap (plus a small constant) as long as the doubling
     * itself doesn't overflow size_t.  The guard uses (SIZE_MAX-256)/2 rather
     * than SIZE_MAX/2 so that the +256 addend never wraps either. */
    if (resp->body_cap <= (SIZE_MAX - 256) / 2) {
      size_t doubled = resp->body_cap * 2 + 256;
      if (doubled > new_cap) new_cap = doubled;
    }
    char *nb = (char *)_mem_realloc(mp, resp->body, new_cap);
    if (!nb) return ccol_not_enough_memory;
    resp->body = nb;
    resp->body_cap = new_cap;
  }
  memcpy(resp->body + resp->body_len, data, len);
  resp->body_len += len;
  return ccol_success;
}

ccol_retval_t chttpsvr_resp_write_str(chttpsvr_resp *resp, const char *str) {
  if (!resp || !str) return ccol_invalid_args;
  return chttpsvr_resp_write(resp, str, strlen(str));
}

ccol_retval_t chttpsvr_resp_printf(chttpsvr_resp *resp, const char *fmt, ...) {
  if (!resp || !fmt) return ccol_invalid_args;

  ccol_memmgmt_procs_t *mp = resp->m_procs;
  char stack_buf[256];
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);

  int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    return ccol_invalid_args;
  }

  if ((size_t)n < sizeof(stack_buf)) {
    va_end(ap2);
    return chttpsvr_resp_write(resp, stack_buf, (size_t)n);
  }

  char *heap_buf = (char *)_mem_alloc(mp, (size_t)n + 1);
  if (!heap_buf) {
    va_end(ap2);
    return ccol_not_enough_memory;
  }
  int n2 = vsnprintf(heap_buf, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  if (n2 < 0) {
    _mem_free(mp, heap_buf);
    return ccol_invalid_args;
  }
  ccol_retval_t rv = chttpsvr_resp_write(resp, heap_buf, (size_t)n2);
  _mem_free(mp, heap_buf);
  return rv;
}

ccol_retval_t chttpsvr_resp_write_json(chttpsvr_resp *resp, const char *json,
                                       size_t len) {
  if (!resp || !json || len == 0) return ccol_invalid_args;
  /* Write the body first so that a body-OOM leaves resp completely unchanged
   * (no header is set, no bytes are appended).  The header string is tiny and
   * far less likely to fail, so if the body succeeds and the header then fails
   * the caller receives ccol_not_enough_memory with the body bytes already
   * buffered; an unlikely but documented partial-state scenario. */
  ccol_retval_t rv = chttpsvr_resp_write(resp, json, len);
  if (rv != ccol_success) return rv;
  return chttpsvr_resp_set_header(resp, "content-type", "application/json");
}
