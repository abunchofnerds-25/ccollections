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
#include <cjson.h>
#include <cvector.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>

/* ========================================================================== */
/*                         INTERNAL DOM NODE                                  */
/* ========================================================================== */

/*
 * Full definition of the opaque struct forward-declared in cjson.h.
 *
 * m_procs: per-node allocator pointer, set at node_alloc() time and never
 * changed.  NULL means the default malloc/free/calloc/realloc.  Every node
 * in a tree is expected to carry the same m_procs value; the pool fast-path
 * is available only when m_procs == NULL.
 *
 * Layout:
 *  - type    : one of the seven cjson_node_type_t values
 *  - m_procs : allocator used for this node and its owned strings
 *  - value   : a union sized to the largest member (8 bytes on 64-bit)
 *     boolean : bool        — CJSON_BOOL
 *     integer : long long   — CJSON_INTEGER
 *     number  : double      — CJSON_FLOAT
 *     string  : char *      — CJSON_STRING  (heap-allocated, owned)
 *     array   : cvec        — CJSON_ARRAY   (cvec of tagged_node_t *)
 *     object  : chmap       — CJSON_OBJECT  (chmap char*→tagged_node_t *)
 */
typedef struct tagged_node_t {
  cjson_node_type_t type;
  ccol_memmgmt_procs_t *m_procs;
  union {
    bool boolean;
    long long integer;
    double number;
    char *string;
    cvec array;
    chmap object;
  } value;
} tagged_node_t;

/* ========================================================================== */
/*                         SERIALIZATION BUFFER                               */
/* ========================================================================== */

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  bool oom;
  ccol_memmgmt_procs_t *m_procs;
} sbuf_t;

static void sb_init(sbuf_t *sb, ccol_memmgmt_procs_t *mp) {
  sb->m_procs = mp;
  sb->buf = _mem_alloc(mp, 256);
  sb->len = 0;
  sb->cap = sb->buf ? 256 : 0;
  sb->oom = sb->buf ? false : true;
  if (sb->buf) sb->buf[0] = '\0';
}

static void sb_grow(sbuf_t *sb, size_t needed) {
  if (sb->oom) return;
  size_t new_cap = sb->cap ? sb->cap * 2 : 256;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2) {
      sb->oom = true;
      return;
    }
    new_cap *= 2;
  }
  char *p = _mem_realloc(sb->m_procs, sb->buf, new_cap);
  if (!p) {
    sb->oom = true;
    return;
  }
  sb->buf = p;
  sb->cap = new_cap;
}

static inline void sb_append(sbuf_t *sb, const char *data, size_t n) {
  if (sb->oom) return;
  if (sb->len + n + 1 > sb->cap) sb_grow(sb, sb->len + n + 1);
  if (sb->oom) return;
  memcpy(sb->buf + sb->len, data, n);
  sb->len += n;
  sb->buf[sb->len] = '\0';
}

static inline void sb_append_c(sbuf_t *sb, char c) { sb_append(sb, &c, 1); }

static inline void sb_append_cstr(sbuf_t *sb, const char *s) {
  sb_append(sb, s, strlen(s));
}

/* Initialise with a pre-sized capacity (at least 64 bytes). */
static void sb_init_hint(sbuf_t *sb, ccol_memmgmt_procs_t *mp, size_t hint) {
  size_t cap = hint + 1 > 64 ? hint + 1 : 64;
  sb->m_procs = mp;
  sb->buf = _mem_alloc(mp, cap);
  sb->len = 0;
  sb->cap = sb->buf ? cap : 0;
  sb->oom = sb->buf ? false : true;
  if (sb->buf) sb->buf[0] = '\0';
}

/* ========================================================================== */
/*                         INTERNAL HELPERS                                   */
/* ========================================================================== */

/*
 * chmap_entry is __attribute__((packed)), so val_storage.inline_data can sit
 * at an unaligned address inside the struct.  Dereferencing the void * stored
 * in val_pair->ptr directly as tagged_node_t ** is UB (and crashes at -O3
 * when the compiler emits an aligned load).  Use memcpy to read/write the
 * 8-byte pointer value safely regardless of alignment.
 */
static inline tagged_node_t *_cjson_read_child(const void *src) {
  tagged_node_t *p;
  memcpy(&p, src, sizeof(p));
  return p;
}

/*
 * Thread-local free-list pool for tagged_node_t.
 *
 * The pool is exclusively for nodes whose m_procs == NULL (default allocator).
 * Custom-allocator nodes bypass the pool entirely — they are allocated and
 * freed directly via their own m_procs.  This invariant eliminates the need
 * for any allocator-snapshot bookkeeping: _node_pool_drain always calls plain
 * free() because every node in the pool was originally allocated with calloc().
 *
 * Rationale: every DOM node is normally a calloc+free pair.  In workloads that
 * repeatedly parse and destroy documents the allocator roundtrip dominates.
 * A capped free-list lets us reuse nodes without any per-node overhead: when a
 * node is returned to the pool we store the old list head inside the node's
 * own memory (safe because sizeof(tagged_node_t) >= sizeof(void *)), and
 * recover it with memcpy on the next allocation to avoid strict-aliasing UB.
 */
#define _NODE_POOL_CAP 512U
static __thread tagged_node_t *_node_pool_head = NULL;
static __thread unsigned _node_pool_sz = 0;

static tagged_node_t *node_alloc(cjson_node_type_t type,
                                 ccol_memmgmt_procs_t *mp) {
  tagged_node_t *n;
  if (mp == NULL && _node_pool_head) {
    /* Default allocator + pool available: reuse a pooled node. */
    n = _node_pool_head;
    tagged_node_t *next;
    memcpy(&next, n, sizeof(next));
    _node_pool_head = next;
    _node_pool_sz--;
    memset(n, 0, sizeof(*n));
  } else {
    n = _mem_calloc(mp, 1, sizeof(*n));
    if (!n) return NULL;
  }
  n->type = type;
  n->m_procs = mp;
  return n;
}

/* pthread key used solely to trigger _node_pool_drain on any thread's exit. */
static pthread_key_t _pool_pthread_key;
/* True between _pool_key_init and _pool_key_fini.  node_free guards its
 * pthread_setspecific call with this flag to avoid calling into a deleted key
 * when a background thread is still active during a concurrent dlclose. */
static atomic_bool _pool_key_live = false;

static void _node_pool_drain(void *);

__attribute__((constructor)) static void _pool_key_init(void) {
  pthread_key_create(&_pool_pthread_key, _node_pool_drain);
  atomic_store(&_pool_key_live, true);
}

/* Called when the DSO is unloaded (dlclose or process exit).
 * - Drains the calling thread's own pool first so it is freed before the
 *   DSO's code mapping disappears.
 * - Clears the liveness flag so concurrent threads that have not yet
 *   registered a drain destructor skip the pthread_setspecific call (their
 *   pool is leaked, but that is safe: calling pthread_setspecific with a
 *   deleted key is POSIX UB, whereas leaking a few node structs is not).
 * - Deletes the key to prevent PTHREAD_KEYS_MAX exhaustion on repeated
 *   dlopen/dlclose cycles. */
__attribute__((destructor)) static void _pool_key_fini(void) {
  _node_pool_drain(NULL);
  atomic_store(&_pool_key_live, false);
  pthread_key_delete(_pool_pthread_key);
}

static void node_free(tagged_node_t *n) {
  if (n->m_procs != NULL) {
    /* Custom allocator: free directly, never touch the default pool. */
    _mem_free(n->m_procs, n);
    return;
  }
  /* Default allocator (m_procs == NULL): try to return to the pool. */
  if (_node_pool_sz >= _NODE_POOL_CAP) {
    free(n);
    return;
  }
  if (_node_pool_sz == 0 && atomic_load(&_pool_key_live))
    pthread_setspecific(_pool_pthread_key, (void *)1);
  memcpy(n, &_node_pool_head, sizeof(_node_pool_head));
  _node_pool_head = n;
  _node_pool_sz++;
}

/* Drain the calling thread's node pool. All pool nodes have m_procs == NULL
 * by invariant, so plain free() is always correct here. */
static void _node_pool_drain(void *arg) {
  (void)arg;
  tagged_node_t *n = _node_pool_head;
  while (n) {
    tagged_node_t *next;
    memcpy(&next, n, sizeof(next));
    free(n);
    n = next;
  }
  _node_pool_head = NULL;
  _node_pool_sz = 0;
}

/* Deep-free the value resources of a node without freeing the node itself. */
static void node_clear(tagged_node_t *n) {
  switch (n->type) {
    case CJSON_STRING:
      _mem_free(n->m_procs, n->value.string);
      n->value.string = NULL;
      break;
    case CJSON_ARRAY: {
      size_t cnt = cvector_elem_count(n->value.array);
      for (size_t i = 0; i < cnt; i++) {
        tagged_node_t *child = *(tagged_node_t **)cvector_at(n->value.array, i);
        __cjson_destroy((cjson)child);
      }
      __cvector_destroy(n->value.array);
      n->value.array = NULL;
      break;
    }
    case CJSON_OBJECT: {
      cmap_iterator *it = chashmap_begin_iter(n->value.object, NULL);
      while (it) {
        tagged_node_t *child = _cjson_read_child(it->val_pair->ptr);
        __cjson_destroy((cjson)child);
        it = it->_next_fn(it);
      }
      __chmap_destroy(n->value.object);
      n->value.object = NULL;
      break;
    }
    default:
      break;
  }
}

/*
 * Overwrite an existing node's content with a new scalar value, deep-freeing
 * any resources the old content owned.  Uses n->m_procs for string allocation.
 */
static bool node_reinit_scalar(tagged_node_t *n, cjson_node_type_t type,
                               void *raw, size_t raw_size, bool is_signed) {
  /* Reject composite types before touching the node.  Hitting the default
   * branch of the switch below after node_clear() would leave n with an
   * invalid type tag and a zeroed value — an inconsistent, undetectable
   * corruption.  Early return here keeps the existing node intact. */
  switch (type) {
    case CJSON_NULL:
    case CJSON_BOOL:
    case CJSON_INTEGER:
    case CJSON_FLOAT:
    case CJSON_STRING:
      break;
    default:
      return false;
  }

  /* Pre-validate floating-point values before mutating the node, so a
   * non-finite value leaves the original node untouched. */
  if (type == CJSON_FLOAT) {
    double d;
    if (raw_size == sizeof(float))
      d = (double)*(float *)raw;
    else if (raw_size == sizeof(double))
      d = *(double *)raw;
    else
      return false;
    if (!isfinite(d)) return false;
  }

  /* For strings, allocate the new value before mutating the node.  An OOM
   * here returns false without touching the existing node content. */
  char *new_str = NULL;
  if (type == CJSON_STRING) {
    const char *s = *(const char **)raw;
    if (s) {
      new_str = ccol_strdup(n->m_procs, s);
      if (!new_str) return false;
    }
  }

  node_clear(n);
  memset(&n->value, 0, sizeof(n->value));
  n->type = type;

  switch (type) {
    case CJSON_NULL:
      break;
    case CJSON_BOOL:
      n->value.boolean = *(bool *)raw;
      break;
    case CJSON_INTEGER: {
      long long v = 0;
      if (is_signed) {
        switch (raw_size) {
          case 1:
            v = *(signed char *)raw;
            break;
          case 2:
            v = *(short *)raw;
            break;
          case 4:
            v = *(int *)raw;
            break;
          case 8:
            v = *(long long *)raw;
            break;
          default:
            return false;
        }
      } else {
        switch (raw_size) {
          case 1:
            v = (long long)*(unsigned char *)raw;
            break;
          case 2:
            v = (long long)*(unsigned short *)raw;
            break;
          case 4:
            v = (long long)*(unsigned int *)raw;
            break;
          case 8:
            v = (long long)*(unsigned long long *)raw;
            break;
          default:
            return false;
        }
      }
      n->value.integer = v;
      break;
    }
    case CJSON_FLOAT: {
      double d = 0.0;
      if (raw_size == sizeof(float))
        d = (double)*(float *)raw;
      else if (raw_size == sizeof(double))
        d = *(double *)raw;
      else
        return false;
      n->value.number = d;
      break;
    }
    case CJSON_STRING: {
      if (!new_str) {
        /* NULL C string → JSON null; value union is already zeroed above. */
        n->type = CJSON_NULL;
      } else {
        n->value.string = new_str;
      }
      break;
    }
    default:
      _mem_free(n->m_procs, new_str);
      return false;
  }
  return true;
}

/* Allocate a fresh scalar node from raw data using the given allocator. */
static tagged_node_t *node_make_scalar(cjson_node_type_t type, void *raw,
                                       size_t raw_size, bool is_signed,
                                       ccol_memmgmt_procs_t *mp) {
  tagged_node_t *n = node_alloc(CJSON_NULL, mp);
  if (!n) return NULL;
  if (!node_reinit_scalar(n, type, raw, raw_size, is_signed)) {
    node_free(n);
    return NULL;
  }
  return n;
}

/* ========================================================================== */
/*                         PUBLIC CONSTRUCTION                                */
/* ========================================================================== */

cjson cjson_create_null_mp(ccol_memmgmt_procs_t *mp) {
  return (cjson)node_alloc(CJSON_NULL, mp);
}

cjson cjson_create_bool_mp(bool val, ccol_memmgmt_procs_t *mp) {
  tagged_node_t *n = node_alloc(CJSON_BOOL, mp);
  if (n) n->value.boolean = val;
  return (cjson)n;
}

cjson cjson_create_int_mp(long long val, ccol_memmgmt_procs_t *mp) {
  tagged_node_t *n = node_alloc(CJSON_INTEGER, mp);
  if (n) n->value.integer = val;
  return (cjson)n;
}

cjson cjson_create_double_mp(double val, ccol_memmgmt_procs_t *mp) {
  if (!isfinite(val)) return NULL;
  tagged_node_t *n = node_alloc(CJSON_FLOAT, mp);
  if (n) n->value.number = val;
  return (cjson)n;
}

cjson cjson_create_string_mp(const char *val, ccol_memmgmt_procs_t *mp) {
  if (!val) return cjson_create_null_mp(mp);
  tagged_node_t *n = node_alloc(CJSON_STRING, mp);
  if (!n) return NULL;
  n->value.string = ccol_strdup(mp, val);
  if (!n->value.string) {
    node_free(n);
    return NULL;
  }
  return (cjson)n;
}

cjson cjson_create_array_mp(ccol_memmgmt_procs_t *mp) {
  tagged_node_t *n = node_alloc(CJSON_ARRAY, mp);
  if (!n) return NULL;
  n->value.array = cvector_create_full(sizeof(tagged_node_t *), mp, NULL);
  if (!n->value.array) {
    node_free(n);
    return NULL;
  }
  return (cjson)n;
}

cjson cjson_create_object_mp(ccol_memmgmt_procs_t *mp) {
  tagged_node_t *n = node_alloc(CJSON_OBJECT, mp);
  if (!n) return NULL;
  char *err = NULL;
  n->value.object = chmap_create_mp(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                    ccol_string, ccol_pointer, mp, &err);
  if (!n->value.object) {
    node_free(n);
    return NULL;
  }
  return (cjson)n;
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

void __cjson_destroy(cjson node) {
  if (!node) return;
  tagged_node_t *n = (tagged_node_t *)node;
  node_clear(n);
  node_free(n);
}

/* ========================================================================== */
/*                         LEAF VALUE ACCESS                                  */
/* ========================================================================== */

cjson_node_type_t cjson_type(cjson node) {
  if (!node) return CJSON_NULL;
  return ((tagged_node_t *)node)->type;
}

bool cjson_bool_val(cjson node) {
  tagged_node_t *n = (tagged_node_t *)node;
  if (!n || n->type != CJSON_BOOL)
    fatal_err("cjson_bool_val: node is %s, expected CJSON_BOOL",
              n ? "(non-bool)" : "NULL");
  return n->value.boolean;
}

long long cjson_int_val(cjson node) {
  tagged_node_t *n = (tagged_node_t *)node;
  if (!n || n->type != CJSON_INTEGER)
    fatal_err("cjson_int_val: node is %s, expected CJSON_INTEGER",
              n ? "(non-integer)" : "NULL");
  return n->value.integer;
}

double cjson_double_val(cjson node) {
  tagged_node_t *n = (tagged_node_t *)node;
  if (!n || n->type != CJSON_FLOAT)
    fatal_err("cjson_double_val: node is %s, expected CJSON_FLOAT",
              n ? "(non-number)" : "NULL");
  return n->value.number;
}

const char *cjson_str_val(cjson node) {
  tagged_node_t *n = (tagged_node_t *)node;
  if (!n || n->type != CJSON_STRING)
    fatal_err("cjson_str_val: node is %s, expected CJSON_STRING",
              n ? "(non-string)" : "NULL");
  return n->value.string;
}

size_t cjson_array_len(cjson node) {
  tagged_node_t *n = (tagged_node_t *)node;
  if (!n || n->type != CJSON_ARRAY)
    fatal_err("cjson_array_len: node is %s, expected CJSON_ARRAY",
              n ? "(non-array)" : "NULL");
  return cvector_elem_count(n->value.array);
}

size_t cjson_object_size(cjson node) {
  tagged_node_t *n = (tagged_node_t *)node;
  if (!n || n->type != CJSON_OBJECT)
    fatal_err("cjson_object_size: node is %s, expected CJSON_OBJECT",
              n ? "(non-object)" : "NULL");
  return chmap_elem_count(n->value.object);
}

/* ========================================================================== */
/*                         ARRAY / OBJECT MANIPULATION                        */
/* ========================================================================== */

ccol_retval_t cjson_array_push(cjson arr, cjson child) {
  if (!child) return ccol_invalid_args;
  if (!arr) {
    __cjson_destroy(child);
    return ccol_invalid_args;
  }
  tagged_node_t *n = (tagged_node_t *)arr;
  if (n->type != CJSON_ARRAY) {
    __cjson_destroy(child);
    return ccol_invalid_args;
  }
  tagged_node_t *c = (tagged_node_t *)child;
  ccol_retval_t r = cvector_push_back(n->value.array, &c);
  /* Ownership of child transfers unconditionally; free it on failure so the
   * caller does not have to track ownership across error paths. */
  if (r != ccol_success) __cjson_destroy(child);
  return r;
}

cjson cjson_array_get(cjson arr, size_t index) {
  if (!arr) return NULL;
  tagged_node_t *n = (tagged_node_t *)arr;
  if (n->type != CJSON_ARRAY) return NULL;
  void *slot = cvector_at(n->value.array, index);
  if (!slot) return NULL;
  return *(cjson *)slot;
}

ccol_retval_t cjson_object_set(cjson obj, const char *key, cjson child) {
  if (!child) return ccol_invalid_args;
  if (!obj || !key) {
    __cjson_destroy(child);
    return ccol_invalid_args;
  }
  tagged_node_t *n = (tagged_node_t *)obj;
  if (n->type != CJSON_OBJECT) {
    __cjson_destroy(child);
    return ccol_invalid_args;
  }

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};

  /* Snapshot the old child pointer before the insert overwrites the slot. */
  cmap_pair *old_vp = NULL;
  tagged_node_t *old_child = NULL;
  if (chmap_get_elem_ref(n->value.object, &kp, &old_vp) == ccol_success)
    old_child = _cjson_read_child(old_vp->ptr);

  tagged_node_t *c = (tagged_node_t *)child;
  cmap_pair vp = {.ptr = &c, .size = sizeof(c)};
  ccol_retval_t r = chmap_insert_elem(n->value.object, &kp, &vp);
  if (r == ccol_success || r == ccol_key_already_present) {
    /* Insert succeeded: it is now safe to release the displaced old child. */
    if (old_child) __cjson_destroy((cjson)old_child);
    return ccol_success;
  }
  /* Insert failed: ownership still transfers unconditionally (matches
   * cjson_array_push semantics and the documented API contract). */
  __cjson_destroy((cjson)child);
  return r;
}

cjson cjson_object_get(cjson obj, const char *key) {
  if (!obj || !key) return NULL;
  tagged_node_t *n = (tagged_node_t *)obj;
  if (n->type != CJSON_OBJECT) return NULL;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair *vp = NULL;
  if (chmap_get_elem_ref(n->value.object, &kp, &vp) != ccol_success)
    return NULL;
  return _cjson_read_child(vp->ptr);
}

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

cjson cjson_clone(cjson node) {
  if (!node) return NULL;
  tagged_node_t *src = (tagged_node_t *)node;
  ccol_memmgmt_procs_t *mp = src->m_procs;

  switch (src->type) {
    case CJSON_NULL:
      return cjson_create_null_mp(mp);
    case CJSON_BOOL:
      return cjson_create_bool_mp(src->value.boolean, mp);
    case CJSON_INTEGER:
      return cjson_create_int_mp(src->value.integer, mp);
    case CJSON_FLOAT:
      return cjson_create_double_mp(src->value.number, mp);
    case CJSON_STRING:
      return cjson_create_string_mp(src->value.string, mp);
    case CJSON_ARRAY: {
      cjson dst = cjson_create_array_mp(mp);
      if (!dst) return NULL;
      size_t cnt = cvector_elem_count(src->value.array);
      for (size_t i = 0; i < cnt; i++) {
        tagged_node_t *child =
            *(tagged_node_t **)cvector_at(src->value.array, i);
        cjson child_copy = cjson_clone((cjson)child);
        if (!child_copy) {
          __cjson_destroy(dst);
          return NULL;
        }
        if (cjson_array_push(dst, child_copy) != ccol_success) {
          /* child_copy already freed by cjson_array_push (unconditional
           * ownership). */
          __cjson_destroy(dst);
          return NULL;
        }
      }
      return dst;
    }
    case CJSON_OBJECT: {
      cjson dst = cjson_create_object_mp(mp);
      if (!dst) return NULL;
      cmap_iterator *it = chashmap_begin_iter(src->value.object, NULL);
      while (it) {
        const char *key = (const char *)it->key_pair->ptr;
        tagged_node_t *child = _cjson_read_child(it->val_pair->ptr);
        cjson child_copy = cjson_clone((cjson)child);
        if (!child_copy) {
          ccol_iter_destroy(it);
          __cjson_destroy(dst);
          return NULL;
        }
        if (cjson_object_set(dst, key, child_copy) != ccol_success) {
          /* child_copy already freed by cjson_object_set (unconditional
           * ownership). */
          ccol_iter_destroy(it);
          __cjson_destroy(dst);
          return NULL;
        }
        it = it->_next_fn(it);
      }
      return dst;
    }
  }
  return NULL;
}

/* ========================================================================== */
/*                         PARSER                                             */
/* ========================================================================== */

typedef struct {
  const char *src;
  size_t pos;
  size_t len;
  char error[512];
  ccol_memmgmt_procs_t *mp;
} parse_ctx_t;

static void parse_err(parse_ctx_t *ctx, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
  va_end(ap);
}

static void skip_ws(parse_ctx_t *ctx) {
  while (ctx->pos < ctx->len) {
    char c = ctx->src[ctx->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      ctx->pos++;
    else
      break;
  }
}

static bool peek(parse_ctx_t *ctx, char *out) {
  skip_ws(ctx);
  if (ctx->pos >= ctx->len) return false;
  *out = ctx->src[ctx->pos];
  return true;
}

/* Forward declarations. */
static tagged_node_t *parse_value(parse_ctx_t *ctx);

/* Consume and return true if the next non-whitespace char matches expected. */
static bool expect_char(parse_ctx_t *ctx, char expected) {
  skip_ws(ctx);
  if (ctx->pos >= ctx->len || ctx->src[ctx->pos] != expected) {
    parse_err(ctx, "expected '%c' at position %zu", expected, ctx->pos);
    return false;
  }
  ctx->pos++;
  return true;
}

/* ------------------------------------------------------------------ null -- */
static tagged_node_t *parse_null(parse_ctx_t *ctx) {
  if (ctx->pos + 4 > ctx->len || memcmp(ctx->src + ctx->pos, "null", 4) != 0) {
    parse_err(ctx, "expected 'null' at position %zu", ctx->pos);
    return NULL;
  }
  ctx->pos += 4;
  return node_alloc(CJSON_NULL, ctx->mp);
}

/* ------------------------------------------------------------------ bool -- */
static tagged_node_t *parse_bool(parse_ctx_t *ctx) {
  if (ctx->pos + 4 <= ctx->len && memcmp(ctx->src + ctx->pos, "true", 4) == 0) {
    ctx->pos += 4;
    tagged_node_t *n = node_alloc(CJSON_BOOL, ctx->mp);
    if (n) n->value.boolean = true;
    return n;
  }
  if (ctx->pos + 5 <= ctx->len &&
      memcmp(ctx->src + ctx->pos, "false", 5) == 0) {
    ctx->pos += 5;
    tagged_node_t *n = node_alloc(CJSON_BOOL, ctx->mp);
    if (n) n->value.boolean = false;
    return n;
  }
  parse_err(ctx, "expected 'true' or 'false' at position %zu", ctx->pos);
  return NULL;
}

/* ---------------------------------------------------------------- number -- */
static tagged_node_t *parse_number(parse_ctx_t *ctx) {
  size_t start = ctx->pos;
  bool is_float = false;

  if (ctx->pos < ctx->len && ctx->src[ctx->pos] == '-') ctx->pos++;

  if (ctx->pos >= ctx->len) {
    parse_err(ctx, "unexpected end of number at position %zu", ctx->pos);
    return NULL;
  }

  if (ctx->src[ctx->pos] == '0') {
    ctx->pos++;
    /* RFC 8259 §6: a leading zero may not be followed by more digits. */
    if (ctx->pos < ctx->len && ctx->src[ctx->pos] >= '0' &&
        ctx->src[ctx->pos] <= '9') {
      parse_err(ctx, "invalid leading zero in number at position %zu", start);
      return NULL;
    }
  } else if (ctx->src[ctx->pos] >= '1' && ctx->src[ctx->pos] <= '9') {
    while (ctx->pos < ctx->len && ctx->src[ctx->pos] >= '0' &&
           ctx->src[ctx->pos] <= '9')
      ctx->pos++;
  } else {
    parse_err(ctx, "invalid number at position %zu", ctx->pos);
    return NULL;
  }

  if (ctx->pos < ctx->len && ctx->src[ctx->pos] == '.') {
    is_float = true;
    ctx->pos++;
    if (ctx->pos >= ctx->len || ctx->src[ctx->pos] < '0' ||
        ctx->src[ctx->pos] > '9') {
      parse_err(ctx, "expected digit after '.' at position %zu", ctx->pos);
      return NULL;
    }
    while (ctx->pos < ctx->len && ctx->src[ctx->pos] >= '0' &&
           ctx->src[ctx->pos] <= '9')
      ctx->pos++;
  }

  if (ctx->pos < ctx->len &&
      (ctx->src[ctx->pos] == 'e' || ctx->src[ctx->pos] == 'E')) {
    is_float = true;
    ctx->pos++;
    if (ctx->pos < ctx->len &&
        (ctx->src[ctx->pos] == '+' || ctx->src[ctx->pos] == '-'))
      ctx->pos++;
    if (ctx->pos >= ctx->len || ctx->src[ctx->pos] < '0' ||
        ctx->src[ctx->pos] > '9') {
      parse_err(ctx, "expected digit in exponent at position %zu", ctx->pos);
      return NULL;
    }
    while (ctx->pos < ctx->len && ctx->src[ctx->pos] >= '0' &&
           ctx->src[ctx->pos] <= '9')
      ctx->pos++;
  }

  /* Build a null-terminated token for strtoll / strtod.
   * 360 bytes covers the full decimal form of any IEEE 754 double (~328 chars
   * worst case) and any LLONG_MIN (20 chars).  Longer inputs cannot represent
   * any finite double and are rejected. */
  size_t tok_len = ctx->pos - start;
  char tok[360];
  if (tok_len >= sizeof(tok)) {
    parse_err(ctx, "number literal too long at position %zu", start);
    return NULL;
  }
  memcpy(tok, ctx->src + start, tok_len);
  tok[tok_len] = '\0';

  if (is_float) {
    double dval = strtod(tok, NULL);
    if (isinf(dval)) {
      parse_err(ctx, "number out of range at position %zu", start);
      return NULL;
    }
    tagged_node_t *n = node_alloc(CJSON_FLOAT, ctx->mp);
    if (!n) return NULL;
    n->value.number = dval;
    return n;
  } else {
    /* Try integer first; fall back to double if out of range. */
    char *endp;
    errno = 0;
    long long ival = strtoll(tok, &endp, 10);
    if (*endp == '\0' && errno != ERANGE) {
      tagged_node_t *n = node_alloc(CJSON_INTEGER, ctx->mp);
      if (!n) return NULL;
      n->value.integer = ival;
      return n;
    }
    double dval = strtod(tok, NULL);
    if (isinf(dval)) {
      parse_err(ctx, "number out of range at position %zu", start);
      return NULL;
    }
    tagged_node_t *n = node_alloc(CJSON_FLOAT, ctx->mp);
    if (!n) return NULL;
    n->value.number = dval;
    return n;
  }
}

/* --------------------- UTF-8 encode a Unicode code point into sbuf_t ----- */
static void encode_utf8(sbuf_t *sb, uint32_t cp) {
  char buf[4];
  int n;
  if (cp <= 0x7F) {
    buf[0] = (char)cp;
    n = 1;
  } else if (cp <= 0x7FF) {
    buf[0] = (char)(0xC0 | (cp >> 6));
    buf[1] = (char)(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp <= 0xFFFF) {
    buf[0] = (char)(0xE0 | (cp >> 12));
    buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    n = 4;
  }
  sb_append(sb, buf, (size_t)n);
}

/* Read exactly 4 hex digits and return the code point value. */
static bool parse_hex4(parse_ctx_t *ctx, uint32_t *out) {
  if (ctx->pos + 4 > ctx->len) {
    parse_err(ctx, "incomplete \\uXXXX escape at position %zu", ctx->pos);
    return false;
  }
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    char c = ctx->src[ctx->pos + i];
    uint32_t d;
    if (c >= '0' && c <= '9')
      d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f')
      d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      d = (uint32_t)(c - 'A' + 10);
    else {
      parse_err(ctx, "invalid hex digit '%c' in \\uXXXX at position %zu", c,
                ctx->pos + i);
      return false;
    }
    v = (v << 4) | d;
  }
  ctx->pos += 4;
  *out = v;
  return true;
}

/*
 * Parse a JSON string (from the opening '"' to the closing '"') into a
 * heap-allocated C string stored in *out.  Uses ctx->mp for allocation.
 */
static bool parse_string_raw(parse_ctx_t *ctx, char **out) {
  *out = NULL;
  if (ctx->pos >= ctx->len || ctx->src[ctx->pos] != '"') {
    parse_err(ctx, "expected '\"' at position %zu", ctx->pos);
    return false;
  }
  ctx->pos++; /* skip opening quote */

  /* Pre-scan: find the closing '"' and check whether any escape or control
   * character is present. */
  size_t scan = ctx->pos;
  bool need_slow = false;
  while (scan < ctx->len) {
    unsigned char sc = (unsigned char)ctx->src[scan];
    if (sc == '"') break;
    if (sc == '\\') {
      need_slow = true;
      scan += 2;
      continue;
    }
    if (sc < 0x20) {
      need_slow = true;
    }
    scan++;
  }

  if (scan >= ctx->len) {
    parse_err(ctx, "unterminated string starting before position %zu",
              ctx->pos - 1);
    return false;
  }

  if (!need_slow) {
    /* Fast path: no escapes, no control chars — single alloc+memcpy. */
    size_t slen = scan - ctx->pos;
    char *s = _mem_alloc(ctx->mp, slen + 1);
    if (!s) return false;
    memcpy(s, ctx->src + ctx->pos, slen);
    s[slen] = '\0';
    ctx->pos = scan + 1; /* advance past closing '"' */
    *out = s;
    return true;
  }

  /* Slow path: has escapes or control chars. */
  sbuf_t sb;
  sb_init_hint(&sb, ctx->mp, scan - ctx->pos);

  while (ctx->pos < ctx->len) {
    /* Bulk-copy a run of plain characters before the next special one. */
    size_t chunk_start = ctx->pos;
    while (ctx->pos < ctx->len) {
      unsigned char c2 = (unsigned char)ctx->src[ctx->pos];
      if (c2 == '"' || c2 == '\\' || c2 < 0x20) break;
      ctx->pos++;
    }
    if (ctx->pos > chunk_start)
      sb_append(&sb, ctx->src + chunk_start, ctx->pos - chunk_start);

    if (ctx->pos >= ctx->len) break;
    char c = ctx->src[ctx->pos];

    if (c == '"') {
      ctx->pos++;
      if (sb.oom) {
        _mem_free(sb.m_procs, sb.buf);
        return false;
      }
      *out = sb.buf;
      return true;
    }

    if (c == '\\') {
      ctx->pos++;
      if (ctx->pos >= ctx->len) break;
      char esc = ctx->src[ctx->pos++];
      switch (esc) {
        case '"':
          sb_append_c(&sb, '"');
          break;
        case '\\':
          sb_append_c(&sb, '\\');
          break;
        case '/':
          sb_append_c(&sb, '/');
          break;
        case 'b':
          sb_append_c(&sb, '\b');
          break;
        case 'f':
          sb_append_c(&sb, '\f');
          break;
        case 'n':
          sb_append_c(&sb, '\n');
          break;
        case 'r':
          sb_append_c(&sb, '\r');
          break;
        case 't':
          sb_append_c(&sb, '\t');
          break;
        case 'u': {
          uint32_t cp;
          if (!parse_hex4(ctx, &cp)) {
            _mem_free(sb.m_procs, sb.buf);
            return false;
          }
          /* Handle surrogate pairs. */
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            /* High surrogate — expect \uXXXX low surrogate. */
            if (ctx->pos + 1 < ctx->len && ctx->src[ctx->pos] == '\\' &&
                ctx->src[ctx->pos + 1] == 'u') {
              ctx->pos += 2;
              uint32_t low;
              if (!parse_hex4(ctx, &low)) {
                _mem_free(sb.m_procs, sb.buf);
                return false;
              }
              if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
              } else {
                cp = 0xFFFD; /* invalid low surrogate */
              }
            } else {
              cp = 0xFFFD; /* lone high surrogate */
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD; /* lone low surrogate */
          }
          /*   produces a null byte, which cannot be represented in a
           * null-terminated C string.  Reject it rather than silently
           * truncating the string at the embedded null. */
          if (cp == 0) {
            parse_err(ctx, "\\u0000 not supported at position %zu",
                      ctx->pos - 6);
            _mem_free(sb.m_procs, sb.buf);
            return false;
          }
          encode_utf8(&sb, cp);
          break;
        }
        default:
          parse_err(ctx, "unknown escape '\\%c' at position %zu", esc,
                    ctx->pos - 1);
          _mem_free(sb.m_procs, sb.buf);
          return false;
      }
    } else {
      /* Control character (< 0x20). */
      parse_err(ctx,
                "unescaped control character 0x%02x in string at position %zu",
                (unsigned char)c, ctx->pos);
      _mem_free(sb.m_procs, sb.buf);
      return false;
    }
  }

  parse_err(ctx, "unterminated string starting before position %zu", ctx->pos);
  _mem_free(sb.m_procs, sb.buf);
  return false;
}

static tagged_node_t *parse_string(parse_ctx_t *ctx) {
  char *s = NULL;
  if (!parse_string_raw(ctx, &s)) return NULL;
  tagged_node_t *n = node_alloc(CJSON_STRING, ctx->mp);
  if (!n) {
    _mem_free(ctx->mp, s);
    return NULL;
  }
  n->value.string = s;
  return n;
}

/* ----------------------------------------------------------------- array -- */
static tagged_node_t *parse_array(parse_ctx_t *ctx) {
  if (!expect_char(ctx, '[')) return NULL;

  tagged_node_t *arr = (tagged_node_t *)cjson_create_array_mp(ctx->mp);
  if (!arr) return NULL;

  char c;
  if (!peek(ctx, &c)) {
    parse_err(ctx, "unterminated array");
    goto fail;
  }
  if (c == ']') {
    ctx->pos++;
    return arr;
  }

  while (1) {
    skip_ws(ctx);
    tagged_node_t *elem = parse_value(ctx);
    if (!elem) goto fail;
    if (cvector_push_back(arr->value.array, &elem) != ccol_success) {
      __cjson_destroy((cjson)elem);
      goto fail;
    }
    if (!peek(ctx, &c)) {
      parse_err(ctx, "unterminated array");
      goto fail;
    }
    if (c == ']') {
      ctx->pos++;
      return arr;
    }
    if (c != ',') {
      parse_err(ctx, "expected ',' or ']' in array at position %zu", ctx->pos);
      goto fail;
    }
    ctx->pos++;
  }

fail:
  __cjson_destroy((cjson)arr);
  return NULL;
}

/* ---------------------------------------------------------------- object -- */
static tagged_node_t *parse_object(parse_ctx_t *ctx) {
  if (!expect_char(ctx, '{')) return NULL;

  tagged_node_t *obj = (tagged_node_t *)cjson_create_object_mp(ctx->mp);
  if (!obj) return NULL;

  char c;
  if (!peek(ctx, &c)) {
    parse_err(ctx, "unterminated object");
    goto fail;
  }
  if (c == '}') {
    ctx->pos++;
    return obj;
  }

  while (1) {
    skip_ws(ctx);
    /* key */
    if (ctx->pos >= ctx->len || ctx->src[ctx->pos] != '"') {
      parse_err(ctx, "expected string key at position %zu", ctx->pos);
      goto fail;
    }
    char *key = NULL;
    if (!parse_string_raw(ctx, &key)) goto fail;

    if (!expect_char(ctx, ':')) {
      _mem_free(ctx->mp, key);
      goto fail;
    }

    skip_ws(ctx);
    tagged_node_t *val = parse_value(ctx);
    if (!val) {
      _mem_free(ctx->mp, key);
      goto fail;
    }

    cmap_pair kp = {.ptr = key, .size = strlen(key) + 1};

    /* Snapshot the old child before the insert overwrites the slot. */
    cmap_pair *existing_vp = NULL;
    tagged_node_t *old_child = NULL;
    if (chmap_get_elem_ref(obj->value.object, &kp, &existing_vp) ==
        ccol_success)
      old_child = _cjson_read_child(existing_vp->ptr);

    cmap_pair vp = {.ptr = &val, .size = sizeof(val)};
    ccol_retval_t r = chmap_insert_elem(obj->value.object, &kp, &vp);
    _mem_free(ctx->mp, key);
    if (r != ccol_success && r != ccol_key_already_present) {
      __cjson_destroy((cjson)val);
      goto fail;
    }
    if (old_child) __cjson_destroy((cjson)old_child);

    if (!peek(ctx, &c)) {
      parse_err(ctx, "unterminated object");
      goto fail;
    }
    if (c == '}') {
      ctx->pos++;
      return obj;
    }
    if (c != ',') {
      parse_err(ctx, "expected ',' or '}' in object at position %zu", ctx->pos);
      goto fail;
    }
    ctx->pos++;
  }

fail:
  __cjson_destroy((cjson)obj);
  return NULL;
}

/* --------------------------------------------------------- value dispatch --
 */
static tagged_node_t *parse_value(parse_ctx_t *ctx) {
  char c;
  if (!peek(ctx, &c)) {
    parse_err(ctx, "unexpected end of input at position %zu", ctx->pos);
    return NULL;
  }
  switch (c) {
    case 'n':
      return parse_null(ctx);
    case 't':
    case 'f':
      return parse_bool(ctx);
    case '"':
      return parse_string(ctx);
    case '[':
      return parse_array(ctx);
    case '{':
      return parse_object(ctx);
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      return parse_number(ctx);
    default:
      parse_err(ctx, "unexpected character '%c' at position %zu", c, ctx->pos);
      return NULL;
  }
}

/* ---------------------------------------------------- public parse entry -- */
static cjson parse_common(const char *src, size_t len, char **err_str,
                          ccol_memmgmt_procs_t *mp) {
  if (!src) {
    if (err_str) *err_str = strdup("null input");
    return NULL;
  }
  parse_ctx_t ctx = {.src = src, .pos = 0, .len = len, .error = "", .mp = mp};
  tagged_node_t *root = parse_value(&ctx);
  if (!root) {
    if (err_str) {
      const char *msg = ctx.error[0] ? ctx.error : "unknown parse error";
      *err_str = strdup(msg);
    }
    return NULL;
  }
  /* Ensure nothing significant follows the root value. */
  skip_ws(&ctx);
  if (ctx.pos != ctx.len) {
    if (err_str) {
      char buf[64];
      snprintf(buf, sizeof(buf), "trailing garbage at position %zu", ctx.pos);
      *err_str = strdup(buf);
    }
    __cjson_destroy((cjson)root);
    return NULL;
  }
  if (err_str) *err_str = NULL;
  return (cjson)root;
}

cjson cjson_parse_mp(const char *json_str, char **err_str,
                     ccol_memmgmt_procs_t *mp) {
  if (!json_str) return parse_common(NULL, 0, err_str, mp);
  return parse_common(json_str, strlen(json_str), err_str, mp);
}

cjson cjson_parse_n_mp(const char *json_str, size_t len, char **err_str,
                       ccol_memmgmt_procs_t *mp) {
  return parse_common(json_str, len, err_str, mp);
}

/* ========================================================================== */
/*                         SERIALIZER                                         */
/* ========================================================================== */

/*
 * Format a double into buf using the shortest decimal representation that
 * round-trips correctly.
 */
static void format_double(char *buf, size_t cap, double val) {
  if (!isfinite(val)) {
    snprintf(buf, cap, "null"); /* JSON has no Inf/NaN */
    return;
  }
  snprintf(buf, cap, "%.15g", val);
  if (strtod(buf, NULL) != val) {
    snprintf(buf, cap, "%.17g", val);
  }
  /* Guarantee the output is recognisable as a floating-point literal so that
   * parsing it back yields CJSON_FLOAT, not CJSON_INTEGER.  %.Ng strips the
   * decimal point for whole-number values (e.g. 1.0 → "1"), which would
   * parse back as CJSON_INTEGER.  Appending ".0" fixes this; the longest
   * affected case is ±1e14 (15 digits + ".0\0" = 18 bytes, well within the
   * 32-byte buf passed by the caller). */
  if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
    size_t len = strlen(buf);
    if (len + 2 < cap) {
      buf[len]     = '.';
      buf[len + 1] = '0';
      buf[len + 2] = '\0';
    }
  }
}

static void sb_append_indent(sbuf_t *sb, unsigned int indent,
                             unsigned int depth) {
  if (!indent) return;
  sb_append_c(sb, '\n');
  for (unsigned int i = 0; i < depth * indent; i++) sb_append_c(sb, ' ');
}

/* Append a JSON-escaped string value (with surrounding quotes). */
static void sb_append_json_str(sbuf_t *sb, const char *s) {
  sb_append_c(sb, '"');
  if (!s) {
    sb_append_c(sb, '"');
    return;
  }

  const char *p = s;
  while (*p) {
    const char *start = p;
    while (*p) {
      unsigned char c = (unsigned char)*p;
      if (c == '"' || c == '\\' || c < 0x20) break;
      p++;
    }
    if (p > start) sb_append(sb, start, (size_t)(p - start));

    if (!*p) break;

    unsigned char c = (unsigned char)*p++;
    switch (c) {
      case '"':
        sb_append(sb, "\\\"", 2);
        break;
      case '\\':
        sb_append(sb, "\\\\", 2);
        break;
      case '\b':
        sb_append(sb, "\\b", 2);
        break;
      case '\f':
        sb_append(sb, "\\f", 2);
        break;
      case '\n':
        sb_append(sb, "\\n", 2);
        break;
      case '\r':
        sb_append(sb, "\\r", 2);
        break;
      case '\t':
        sb_append(sb, "\\t", 2);
        break;
      default: {
        char esc[7];
        snprintf(esc, sizeof(esc), "\\u%04x", c);
        sb_append_cstr(sb, esc);
      }
    }
  }
  sb_append_c(sb, '"');
}

/*
 * Convert a long long to a decimal string without snprintf overhead.
 * buf must be at least 21 bytes.
 */
static int _lltoa(char *buf, long long v) {
  char tmp[22];
  unsigned long long u;
  int neg = (v < 0);
  int i = 0;
  u = neg ? (unsigned long long)(-(v + 1)) + 1ULL : (unsigned long long)v;
  do {
    tmp[i++] = '0' + (char)(u % 10);
    u /= 10;
  } while (u);
  if (neg) tmp[i++] = '-';
  int len = i;
  for (int j = 0; j < len; j++) buf[j] = tmp[len - 1 - j];
  buf[len] = '\0';
  return len;
}

static void serialize_node(sbuf_t *sb, tagged_node_t *n, unsigned int indent,
                           unsigned int depth) {
  if (!n) {
    sb_append_cstr(sb, "null");
    return;
  }

  switch (n->type) {
    case CJSON_NULL:
      sb_append_cstr(sb, "null");
      break;
    case CJSON_BOOL:
      sb_append_cstr(sb, n->value.boolean ? "true" : "false");
      break;
    case CJSON_INTEGER: {
      char buf[24];
      int len = _lltoa(buf, n->value.integer);
      sb_append(sb, buf, (size_t)len);
      break;
    }
    case CJSON_FLOAT: {
      char buf[32];
      format_double(buf, sizeof(buf), n->value.number);
      sb_append_cstr(sb, buf);
      break;
    }
    case CJSON_STRING:
      sb_append_json_str(sb, n->value.string);
      break;
    case CJSON_ARRAY: {
      size_t cnt = cvector_elem_count(n->value.array);
      sb_append_c(sb, '[');
      for (size_t i = 0; i < cnt; i++) {
        if (i > 0) sb_append_c(sb, ',');
        if (indent) sb_append_indent(sb, indent, depth + 1);
        tagged_node_t *child = *(tagged_node_t **)cvector_at(n->value.array, i);
        serialize_node(sb, child, indent, depth + 1);
      }
      if (indent && cnt > 0) sb_append_indent(sb, indent, depth);
      sb_append_c(sb, ']');
      break;
    }
    case CJSON_OBJECT: {
      sb_append_c(sb, '{');
      size_t idx = 0;
      cmap_iterator *it = chashmap_begin_iter(n->value.object, NULL);
      while (it) {
        if (idx > 0) sb_append_c(sb, ',');
        if (indent) sb_append_indent(sb, indent, depth + 1);
        sb_append_json_str(sb, (const char *)it->key_pair->ptr);
        sb_append_c(sb, ':');
        if (indent) sb_append_c(sb, ' ');
        tagged_node_t *child = _cjson_read_child(it->val_pair->ptr);
        serialize_node(sb, child, indent, depth + 1);
        idx++;
        it = it->_next_fn(it);
      }
      if (indent && idx > 0) sb_append_indent(sb, indent, depth);
      sb_append_c(sb, '}');
      break;
    }
  }
}

char *cjson_serialize(cjson node) {
  tagged_node_t *n = (tagged_node_t *)node;
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  sbuf_t sb;
  sb_init(&sb, mp);
  serialize_node(&sb, n, 0, 0);
  if (sb.oom) {
    _mem_free(sb.m_procs, sb.buf);
    return NULL;
  }
  return sb.buf;
}

char *cjson_serialize_pretty(cjson node, unsigned int indent) {
  tagged_node_t *n = (tagged_node_t *)node;
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  sbuf_t sb;
  sb_init(&sb, mp);
  serialize_node(&sb, n, indent ? indent : 4, 0);
  if (sb.oom) {
    _mem_free(sb.m_procs, sb.buf);
    return NULL;
  }
  return sb.buf;
}

void cjson_serialize_free_mp(char *s, ccol_memmgmt_procs_t *mp) {
  _mem_free(mp, s);
}

/* ========================================================================== */
/*                         PATH NAVIGATION                                    */
/* ========================================================================== */

static cjson navigate(cjson root, char *path_copy) {
  cjson cur = root;
  char *p = path_copy;

  while (cur) {
    char *dot = strchr(p, '.');
    if (dot) *dot = '\0';

    /* Empty component: consecutive dots ("a..b") or trailing dot ("a."). */
    if (p[0] == '\0') {
      cur = NULL;
      break;
    }

    tagged_node_t *n = (tagged_node_t *)cur;

    if (n->type == CJSON_ARRAY && p[0] == '#') {
      char *endp;
      errno = 0;
      long idx = strtol(p + 1, &endp, 10);
      if (endp == p + 1 || *endp != '\0' || idx < 0 || errno == ERANGE) {
        cur = NULL;
        break;
      }
      void *slot = cvector_at(n->value.array, (size_t)idx);
      cur = slot ? *(cjson *)slot : NULL;
    } else if (n->type == CJSON_OBJECT) {
      cmap_pair kp = {.ptr = p, .size = strlen(p) + 1};
      cmap_pair *vp = NULL;
      if (chmap_get_elem_ref(n->value.object, &kp, &vp) != ccol_success)
        cur = NULL;
      else
        cur = _cjson_read_child(vp->ptr);
    } else {
      cur = NULL;
    }

    if (!dot) break;
    p = dot + 1;
  }

  return cur;
}

cjson _cjson_get(cjson root, const char *path) {
  if (!root) return NULL;
  if (!path || path[0] == '\0') return root;

  ccol_memmgmt_procs_t *mp = ((tagged_node_t *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return NULL;
  cjson result = navigate(root, copy);
  _mem_free(mp, copy);
  return result;
}

ccol_retval_t _cjson_set_typed(cjson root, const char *path,
                               cjson_node_type_t type, void *raw,
                               size_t raw_size, bool is_signed,
                               bool raw_is_char_array) {
  if (!root || !path || path[0] == '\0') return ccol_invalid_args;

  /*
   * String normalisation: for string literals, typeof("hi") = char[N], so
   * &_cjson_sv points to the char bytes directly.  For char * variables,
   * &_cjson_sv points to a const char * variable.  Normalise to const char **
   * so node_reinit_scalar always sees one consistent form.
   */
  const char *_cjson_str_norm;
  if (type == CJSON_STRING && raw_is_char_array) {
    _cjson_str_norm = (const char *)raw;
    raw = (void *)&_cjson_str_norm;
    raw_size = sizeof(_cjson_str_norm);
  }

  ccol_memmgmt_procs_t *mp = ((tagged_node_t *)root)->m_procs;

  char *copy = ccol_strdup(mp, path);
  if (!copy) return ccol_not_enough_memory;

  char *last_dot = strrchr(copy, '.');
  char *leaf_copy;
  cjson parent;

  if (!last_dot) {
    leaf_copy = ccol_strdup(mp, path);
    parent = root;
    _mem_free(mp, copy);
  } else {
    *last_dot = '\0';
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    parent = navigate(root, copy);
    _mem_free(mp, copy);
    if (!parent) {
      _mem_free(mp, leaf_copy);
      return ccol_key_not_found;
    }
  }

  if (!leaf_copy) return ccol_not_enough_memory;
  const char *leaf_comp = leaf_copy;

  if (leaf_comp[0] == '\0') {
    _mem_free(mp, leaf_copy);
    return ccol_invalid_args;
  }

  if (type == CJSON_FLOAT) {
    double d;
    bool size_ok = false;
    if (raw_size == sizeof(float)) {
      d = (double)*(float *)raw;
      size_ok = true;
    } else if (raw_size == sizeof(double)) {
      d = *(double *)raw;
      size_ok = true;
    }
    if (!size_ok || !isfinite(d)) {
      _mem_free(mp, leaf_copy);
      return ccol_invalid_args;
    }
  }

  tagged_node_t *pn = (tagged_node_t *)parent;
  ccol_retval_t ret;

  if (pn->type == CJSON_OBJECT) {
    cmap_pair kp = {.ptr = (void *)leaf_comp, .size = strlen(leaf_comp) + 1};
    cmap_pair *existing_vp = NULL;

    if (chmap_get_elem_ref(pn->value.object, &kp, &existing_vp) ==
        ccol_success) {
      tagged_node_t *existing = _cjson_read_child(existing_vp->ptr);
      ret = node_reinit_scalar(existing, type, raw, raw_size, is_signed)
                ? ccol_success
                : ccol_not_enough_memory;
    } else {
      tagged_node_t *new_node =
          node_make_scalar(type, raw, raw_size, is_signed, pn->m_procs);
      if (!new_node) {
        _mem_free(mp, leaf_copy);
        return ccol_not_enough_memory;
      }
      cmap_pair vp = {.ptr = &new_node, .size = sizeof(new_node)};
      ccol_retval_t r = chmap_insert_elem(pn->value.object, &kp, &vp);
      if (r != ccol_success && r != ccol_key_already_present) {
        __cjson_destroy((cjson)new_node);
        ret = r;
      } else {
        ret = ccol_success;
      }
    }
  } else if (pn->type == CJSON_ARRAY) {
    if (leaf_comp[0] != '#') {
      ret = ccol_invalid_args;
    } else {
      char *endp;
      errno = 0;
      long idx = strtol(leaf_comp + 1, &endp, 10);
      if (endp == leaf_comp + 1 || *endp != '\0' || idx < 0 ||
          errno == ERANGE) {
        ret = ccol_invalid_args;
      } else {
        void *slot = cvector_at(pn->value.array, (size_t)idx);
        if (!slot) {
          ret = ccol_invalid_args;
        } else {
          tagged_node_t *existing = *(tagged_node_t **)slot;
          ret = node_reinit_scalar(existing, type, raw, raw_size, is_signed)
                    ? ccol_success
                    : ccol_not_enough_memory;
        }
      }
    }
  } else {
    ret = ccol_invalid_args;
  }

  _mem_free(mp, leaf_copy);
  return ret;
}
