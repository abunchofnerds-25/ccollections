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
 *     list   : cvec         — CJSON_LIST  (cvec of cjson_node_t *)
 *     dictionary  : chmap   — CJSON_DICTIONARY  (chmap char* --> cjson_node_t)
 * *)
 */
typedef struct cjson_node_t {
  cjson_node_type_t type;
  ccol_memmgmt_procs_t *m_procs;
  union {
    bool boolean;
    long long integer;
    double number;
    char *string;
    cvec list;
    chmap dictionary;
  } value;
} cjson_node_t;

/* ========================================================================== */
/*                         SERIALIZATION BUFFER                               */
/* ========================================================================== */

/*
 * Dynamic string buffer used exclusively for JSON serialization output.
 * All sb_* operations are no-ops once oom is set, allowing callers to defer
 * error checking to the end of a serialization pass rather than testing after
 * every append.
 */
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  bool oom;
  ccol_memmgmt_procs_t *m_procs;
} sbuf_t;

/* Allocate a 256-byte backing store and zero the length counter.
 * Sets oom on allocation failure; subsequent sb_* calls are then safe no-ops.
 */
static void sb_init(sbuf_t *sb, ccol_memmgmt_procs_t *mp) {
  sb->m_procs = mp;
  sb->buf = _mem_alloc(mp, 256);
  sb->len = 0;
  sb->cap = sb->buf ? 256 : 0;
  sb->oom = sb->buf ? false : true;
  if (sb->buf) sb->buf[0] = '\0';
}

/* Double the buffer capacity until it holds 'needed' bytes.
 * Sets oom on reallocation failure or size_t overflow. */
static void sb_grow(sbuf_t *sb, size_t needed) {
  if (sb->oom) return;
  size_t new_cap =
      sb->cap ? (sb->cap > SIZE_MAX / 2 ? SIZE_MAX : sb->cap * 2) : 256;
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

/* Append n raw bytes, growing the buffer as needed.  No-op when oom is set. */
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
 * in val_pair->ptr directly as cjson_node_t ** is UB (and crashes at -O3
 * when the compiler emits an aligned load).  Use memcpy to read/write the
 * 8-byte pointer value safely regardless of alignment.
 */
static inline cjson_node_t *_cjson_read_child(const void *src) {
  cjson_node_t *p;
  memcpy(&p, src, sizeof(p));
  return p;
}

/*
 * Thread-local free-list pool for cjson_node_t.
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
 * own memory (safe because sizeof(cjson_node_t) >= sizeof(void *)), and
 * recover it with memcpy on the next allocation to avoid strict-aliasing UB.
 */
#define _NODE_POOL_CAP 512U
static __thread cjson_node_t *_node_pool_head = NULL;
static __thread unsigned _node_pool_sz = 0;

static cjson_node_t *node_alloc(cjson_node_type_t type,
                                ccol_memmgmt_procs_t *mp) {
  cjson_node_t *n;
  if (mp == NULL && _node_pool_head) {
    /* Default allocator + pool available: reuse a pooled node. */
    n = _node_pool_head;
    cjson_node_t *next;
    memcpy(&next, (cjson_node_t **)n, sizeof(next));
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

/* Return a node to the thread-local pool (when m_procs == NULL and the pool
 * is not full) or release it directly through its own allocator.
 * The first call from a thread that actually uses the pool registers a
 * pthread destructor so the pool is drained when the thread exits. */
static void node_free(cjson_node_t *n) {
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
  memcpy((cjson_node_t **)n, &_node_pool_head, sizeof(_node_pool_head));
  _node_pool_head = n;
  _node_pool_sz++;
}

/* Drain the calling thread's node pool. All pool nodes have m_procs == NULL
 * by invariant, so plain free() is always correct here. */
static void _node_pool_drain(void *arg) {
  (void)arg;
  cjson_node_t *n = _node_pool_head;
  while (n) {
    cjson_node_t *next;
    memcpy(&next, (cjson_node_t **)n, sizeof(next));
    free(n);
    n = next;
  }
  _node_pool_head = NULL;
  _node_pool_sz = 0;
}

/* Deep-free the value resources of a node without freeing the node itself. */
static void node_clear(cjson_node_t *n) {
  switch (n->type) {
    case CJSON_STRING:
      _mem_free(n->m_procs, n->value.string);
      n->value.string = NULL;
      break;
    case CJSON_LIST: {
      size_t cnt = cvector_elem_count(n->value.list);
      for (size_t i = 0; i < cnt; i++) {
        cjson_node_t *child = *(cjson_node_t **)cvector_at(n->value.list, i);
        __cjson_destroy((cjson)child);
      }
      __cvector_destroy(n->value.list);
      n->value.list = NULL;
      break;
    }
    case CJSON_DICTIONARY: {
      cmap_iterator *it = chashmap_begin_iter(n->value.dictionary, NULL);
      while (it) {
        cjson_node_t *child = _cjson_read_child(it->val_pair->ptr);
        __cjson_destroy((cjson)child);
        it = it->_next_fn(it);
      }
      __chmap_destroy(n->value.dictionary);
      n->value.dictionary = NULL;
      break;
    }
    default:
      break;
  }
}

/*
 * Overwrite an existing node's content with a new scalar value, deep-freeing
 * any resources the old content owned.  Uses n->m_procs for string allocation.
 *
 * Returns ccol_success, ccol_invalid_args (bad type/size/non-finite float), or
 * ccol_not_enough_memory (string strdup failed).  All validation is done before
 * node_clear() so any failure leaves the original node completely intact.
 */
static ccol_retval_t node_reinit_scalar(cjson_node_t *n, cjson_node_type_t type,
                                        void *raw, size_t raw_size,
                                        bool is_signed) {
  /* === Pre-validation (nothing touches the node yet) === */

  /* Reject composite types: the post-clear assignment switch covers only
   * scalars, and reaching its default branch after node_clear would leave
   * the node in an inconsistent state. */
  switch (type) {
    case CJSON_NULL:
    case CJSON_BOOL:
    case CJSON_INTEGER:
    case CJSON_FLOAT:
    case CJSON_STRING:
      break;
    default:
      return ccol_invalid_args;
  }

  /* Validate float size and finiteness; stash the converted value so the
   * post-clear assignment needs no second size-dispatch. */
  double pre_d = 0.0;
  if (type == CJSON_FLOAT) {
    if (raw_size == sizeof(float))
      pre_d = (double)*(float *)raw;
    else if (raw_size == sizeof(double))
      pre_d = *(double *)raw;
    else
      return ccol_invalid_args;
    if (!isfinite(pre_d)) return ccol_invalid_args;
  }

  /* Validate integer raw_size before touching the node. */
  if (type == CJSON_INTEGER) {
    switch (raw_size) {
      case 1:
      case 2:
      case 4:
      case 8:
        break;
      default:
        return ccol_invalid_args;
    }
  }

  /* Pre-allocate the new string so an OOM leaves the existing content
   * untouched. */
  char *new_str = NULL;
  if (type == CJSON_STRING) {
    const char *s = *(const char **)raw;
    if (s) {
      new_str = ccol_strdup(n->m_procs, s);
      if (!new_str) return ccol_not_enough_memory;
    }
  }

  /* === Mutation — all pre-validation passed, cannot fail from here === */
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
        }
      }
      n->value.integer = v;
      break;
    }
    case CJSON_FLOAT:
      n->value.number = pre_d;
      break;
    case CJSON_STRING:
      if (!new_str)
        n->type = CJSON_NULL; /* NULL C string -> JSON null */
      else
        n->value.string = new_str;
      break;
    default:
      break; /* Unreachable: all scalar types handled above. */
  }
  return ccol_success;
}

/* Allocate a fresh scalar node from raw data using the given allocator.
 * Returns ccol_not_enough_memory on node allocation failure, or the exact
 * ccol_retval_t from node_reinit_scalar on validation failure. */
static ccol_retval_t node_make_scalar(cjson_node_type_t type, void *raw,
                                      size_t raw_size, bool is_signed,
                                      ccol_memmgmt_procs_t *mp,
                                      cjson_node_t **out) {
  *out = NULL;
  cjson_node_t *n = node_alloc(CJSON_NULL, mp);
  if (!n) return ccol_not_enough_memory;
  ccol_retval_t r = node_reinit_scalar(n, type, raw, raw_size, is_signed);
  if (r != ccol_success) {
    node_free(n);
    return r;
  }
  *out = n;
  return ccol_success;
}

/* ========================================================================== */
/*                         PUBLIC CONSTRUCTION                                */
/* ========================================================================== */

/*
 * Factory functions for individual node types.  mp may be NULL to use the
 * default malloc/calloc/free.  All return NULL on allocation failure.
 *
 * cjson_create_double_mp additionally rejects non-finite values (Inf, NaN)
 * because the JSON specification has no representation for them.
 *
 * cjson_create_string_mp with val == NULL produces a CJSON_NULL node,
 * consistent with the cjson_set behaviour when a char * variable is NULL.
 *
 * cjson_create_list_mp and cjson_create_dictionary_mp produce empty containers
 * ready to be populated with cjson_list_push / cjson_dictionary_set.
 */
cjson cjson_create_null_mp(ccol_memmgmt_procs_t *mp) {
  return (cjson)node_alloc(CJSON_NULL, mp);
}

cjson cjson_create_bool_mp(bool val, ccol_memmgmt_procs_t *mp) {
  cjson_node_t *n = node_alloc(CJSON_BOOL, mp);
  if (n) n->value.boolean = val;
  return (cjson)n;
}

cjson cjson_create_int_mp(long long val, ccol_memmgmt_procs_t *mp) {
  cjson_node_t *n = node_alloc(CJSON_INTEGER, mp);
  if (n) n->value.integer = val;
  return (cjson)n;
}

cjson cjson_create_double_mp(double val, ccol_memmgmt_procs_t *mp) {
  if (!isfinite(val)) return NULL;
  cjson_node_t *n = node_alloc(CJSON_FLOAT, mp);
  if (n) n->value.number = val;
  return (cjson)n;
}

cjson cjson_create_string_mp(const char *val, ccol_memmgmt_procs_t *mp) {
  if (!val) return cjson_create_null_mp(mp);
  cjson_node_t *n = node_alloc(CJSON_STRING, mp);
  if (!n) return NULL;
  n->value.string = ccol_strdup(mp, val);
  if (!n->value.string) {
    node_free(n);
    return NULL;
  }
  return (cjson)n;
}

cjson cjson_create_list_mp(ccol_memmgmt_procs_t *mp) {
  cjson_node_t *n = node_alloc(CJSON_LIST, mp);
  if (!n) return NULL;
  n->value.list = cvector_create_full(sizeof(cjson_node_t *), mp, NULL);
  if (!n->value.list) {
    node_free(n);
    return NULL;
  }
  return (cjson)n;
}

cjson cjson_create_dictionary_mp(ccol_memmgmt_procs_t *mp) {
  cjson_node_t *n = node_alloc(CJSON_DICTIONARY, mp);
  if (!n) return NULL;
  char *err = NULL;
  n->value.dictionary = chmap_create_mp(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                        ccol_string, ccol_pointer, mp, &err);
  if (!n->value.dictionary) {
    node_free(n);
    return NULL;
  }
  return (cjson)n;
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/* Recursively free a node and all its descendants. Safe to call on NULL. Does
 * NOT null the caller's pointer -- use the cjson_destroy() macro wrapper for
 * that. */
void __cjson_destroy(cjson node) {
  if (!node) return;
  cjson_node_t *n = (cjson_node_t *)node;
  node_clear(n);
  node_free(n);
}

/* ========================================================================== */
/*                         LEAF VALUE ACCESS                                  */
/* ========================================================================== */

/* Return the node's type tag.  Returns CJSON_NULL for a NULL handle. */
cjson_node_type_t cjson_type(cjson node) {
  if (!node) return CJSON_NULL;
  return ((cjson_node_t *)node)->type;
}

/*
 * Typed value accessors.  Each calls fatal_err() on type mismatch or a NULL
 * handle.  Use cjson_type() to guard these calls when the type is not
 * statically guaranteed at the call site.
 */
bool cjson_bool_val(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_BOOL)
    fatal_err("cjson_bool_val: node is %s, expected CJSON_BOOL",
              n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.boolean;
}

long long cjson_int_val(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_INTEGER)
    fatal_err("cjson_int_val: node is %s, expected CJSON_INTEGER",
              n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.integer;
}

double cjson_double_val(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_FLOAT)
    fatal_err("cjson_double_val: node is %s, expected CJSON_FLOAT",
              n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.number;
}

const char *cjson_str_val(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_STRING)
    fatal_err("cjson_str_val: node is %s, expected CJSON_STRING",
              n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.string;
}

/* Return the number of elements in an list or the number of keys in a
 * dictionary.  Both call fatal_err() if the node is not the expected type. */
size_t cjson_list_len(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_LIST)
    fatal_err("cjson_list_len: node is %s, expected CJSON_LIST",
              n ? cjson_type_str((cjson)n) : "NULL");
  return cvector_elem_count(n->value.list);
}

size_t cjson_dictionary_size(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_DICTIONARY)
    fatal_err("cjson_dictionary_size: node is %s, expected CJSON_DICTIONARY",
              n ? cjson_type_str((cjson)n) : "NULL");
  return chmap_elem_count(n->value.dictionary);
}

/* ========================================================================== */
/*                       LIST / DICTIONARY MANIPULATION                       */
/* ========================================================================== */

/*
 * Append child to arr's element list.  Ownership of child transfers to arr
 * unconditionally: if the push fails (OOM or wrong node type), child is
 * destroyed before returning the error code so the caller never has to
 * track ownership across error paths.
 */
ccol_retval_t cjson_list_push(cjson arr, cjson child) {
  if (!child) return ccol_invalid_args;
  if (!arr) {
    __cjson_destroy(child);
    return ccol_invalid_args;
  }
  cjson_node_t *n = (cjson_node_t *)arr;
  if (n->type != CJSON_LIST) {
    __cjson_destroy(child);
    return ccol_invalid_args;
  }
  cjson_node_t *c = (cjson_node_t *)child;
  ccol_retval_t r = cvector_push_back(n->value.list, &c);
  /* Ownership of child transfers unconditionally; free it on failure so the
   * caller does not have to track ownership across error paths. */
  if (r != ccol_success) __cjson_destroy(child);
  return r;
}

/* Return the element at position index (borrowed -- do not destroy it
 * independently of the parent list), or NULL if out of bounds or if arr is
 * NULL / not a CJSON_LIST node. */
cjson cjson_list_get(cjson arr, size_t index) {
  if (!arr) return NULL;
  cjson_node_t *n = (cjson_node_t *)arr;
  if (n->type != CJSON_LIST) return NULL;
  void *slot = cvector_at(n->value.list, index);
  if (!slot) return NULL;
  return *(cjson *)slot;
}

/*
 * Insert or replace the value for key in obj.  Ownership of child transfers
 * to obj unconditionally, mirroring cjson_list_push.  When key already
 * exists the old child is snapshot-ed before the slot is overwritten, then
 * destroyed after the new pointer is safely in place -- the slot is never
 * left dangling.
 */
ccol_retval_t cjson_dictionary_set(cjson obj, const char *key, cjson child) {
  if (!child) return ccol_invalid_args;
  if (!obj || !key) {
    __cjson_destroy(child);
    return ccol_invalid_args;
  }
  cjson_node_t *n = (cjson_node_t *)obj;
  if (n->type != CJSON_DICTIONARY) {
    __cjson_destroy(child);
    return ccol_invalid_args;
  }

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};

  /* Snapshot the old child pointer before the insert overwrites the slot. */
  cmap_pair *old_vp = NULL;
  cjson_node_t *old_child = NULL;
  if (chmap_get_elem_ref(n->value.dictionary, &kp, &old_vp) == ccol_success)
    old_child = _cjson_read_child(old_vp->ptr);

  cjson_node_t *c = (cjson_node_t *)child;
  cmap_pair vp = {.ptr = &c, .size = sizeof(c)};
  ccol_retval_t r = chmap_insert_elem(n->value.dictionary, &kp, &vp);
  if (r == ccol_success || r == ccol_key_already_present) {
    /* Insert succeeded: it is now safe to release the displaced old child. */
    if (old_child) __cjson_destroy((cjson)old_child);
    return ccol_success;
  }
  /* Insert failed: ownership still transfers unconditionally (matches
   * cjson_list_push semantics and the documented API contract). */
  __cjson_destroy((cjson)child);
  return r;
}

/* Look up key in obj and return the associated child (borrowed), or NULL if
 * not found, if obj is NULL, or if obj is not a CJSON_DICTIONARY node. */
cjson cjson_dictionary_get(cjson obj, const char *key) {
  if (!obj || !key) return NULL;
  cjson_node_t *n = (cjson_node_t *)obj;
  if (n->type != CJSON_DICTIONARY) return NULL;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair *vp = NULL;
  if (chmap_get_elem_ref(n->value.dictionary, &kp, &vp) != ccol_success)
    return NULL;
  return _cjson_read_child(vp->ptr);
}

/*
 * Remove and deep-free the element at position index from an list.
 *
 * The element at index is destroyed and all subsequent elements are shifted
 * left by one position (O(n) in the number of elements after index).  The
 * vector is then shrunk by one via cvector_pop_back.
 */
ccol_retval_t cjson_list_remove(cjson arr, size_t index) {
  if (!arr) return ccol_invalid_args;
  cjson_node_t *n = (cjson_node_t *)arr;
  if (n->type != CJSON_LIST) return ccol_invalid_args;
  size_t cnt = cvector_elem_count(n->value.list);
  if (index >= cnt) return ccol_invalid_args;

  cjson_node_t *child = *(cjson_node_t **)cvector_at(n->value.list, index);
  __cjson_destroy((cjson)child);

  for (size_t i = index; i + 1 < cnt; i++) {
    cjson_node_t **dst = (cjson_node_t **)cvector_at(n->value.list, i);
    cjson_node_t **src = (cjson_node_t **)cvector_at(n->value.list, i + 1);
    *dst = *src;
  }
  cjson_node_t *tmp = NULL;
  cvector_pop_back(n->value.list, &tmp);
  return ccol_success;
}

/*
 * Remove and deep-free the entry with the given key from a dictionary.
 *
 * The child pointer is read out before the map entry is deleted so the
 * subtree is freed after the hash table no longer references it.
 */
ccol_retval_t cjson_dictionary_remove(cjson obj, const char *key) {
  if (!obj || !key) return ccol_invalid_args;
  cjson_node_t *n = (cjson_node_t *)obj;
  if (n->type != CJSON_DICTIONARY) return ccol_invalid_args;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair *vp = NULL;
  if (chmap_get_elem_ref(n->value.dictionary, &kp, &vp) != ccol_success)
    return ccol_key_not_found;
  cjson_node_t *child = _cjson_read_child(vp->ptr);
  ccol_retval_t r = chmap_delete_elem(n->value.dictionary, &kp);
  if (r == ccol_success) __cjson_destroy((cjson)child);
  return r;
}

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

/* Produce an independent deep copy of the entire subtree rooted at node.
 * The clone uses the same allocator (m_procs) as the source.  On OOM any
 * partially-built clone is destroyed before NULL is returned. */
cjson cjson_clone(cjson node) {
  if (!node) return NULL;
  cjson_node_t *src = (cjson_node_t *)node;
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
    case CJSON_LIST: {
      cjson dst = cjson_create_list_mp(mp);
      if (!dst) return NULL;
      size_t cnt = cvector_elem_count(src->value.list);
      for (size_t i = 0; i < cnt; i++) {
        cjson_node_t *child = *(cjson_node_t **)cvector_at(src->value.list, i);
        cjson child_copy = cjson_clone((cjson)child);
        if (!child_copy) {
          __cjson_destroy(dst);
          return NULL;
        }
        if (cjson_list_push(dst, child_copy) != ccol_success) {
          /* child_copy already freed by cjson_list_push (unconditional
           * ownership). */
          __cjson_destroy(dst);
          return NULL;
        }
      }
      return dst;
    }
    case CJSON_DICTIONARY: {
      cjson dst = cjson_create_dictionary_mp(mp);
      if (!dst) return NULL;
      cmap_iterator *it = chashmap_begin_iter(src->value.dictionary, NULL);
      while (it) {
        const char *key = (const char *)it->key_pair->ptr;
        cjson_node_t *child = _cjson_read_child(it->val_pair->ptr);
        cjson child_copy = cjson_clone((cjson)child);
        if (!child_copy) {
          ccol_iter_destroy(it);
          __cjson_destroy(dst);
          return NULL;
        }
        if (cjson_dictionary_set(dst, key, child_copy) != ccol_success) {
          /* child_copy already freed by cjson_dictionary_set (unconditional
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

/*
 * State threaded through all recursive-descent parse functions.
 *
 * src / pos / len : the source text and the current byte offset.
 * error           : human-readable message filled by parse_err(); only
 *                   meaningful when a parse function has returned NULL/false.
 * mp              : allocator used for all node and string allocations.
 *
 * The parser is a single-pass hand-written recursive descent over the full
 * JSON grammar (RFC 8259).  It does not build a separate token stream; each
 * sub-parser reads directly from src via pos.
 */
typedef struct {
  const char *src;
  size_t pos;
  size_t len;
  char error[512];
  ccol_memmgmt_procs_t *mp;
} parse_ctx_t;

/* Format a human-readable parse error into ctx->error.  Only the last call
 * survives; earlier messages are silently overwritten. */
static void parse_err(parse_ctx_t *ctx, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
  vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
#ifdef __clang__
#pragma clang diagnostic pop
#endif
  va_end(ap);
}

/* Advance ctx->pos past any JSON whitespace (space, tab, CR, LF). */
static void skip_ws(parse_ctx_t *ctx) {
  while (ctx->pos < ctx->len) {
    char c = ctx->src[ctx->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      ctx->pos++;
    else
      break;
  }
}

/* Skip whitespace and store the next character in *out without consuming it.
 * Returns false at end-of-input. */
static bool peek(parse_ctx_t *ctx, char *out) {
  skip_ws(ctx);
  if (ctx->pos >= ctx->len) return false;
  *out = ctx->src[ctx->pos];
  return true;
}

/* Forward declarations. */
static cjson_node_t *parse_value(parse_ctx_t *ctx);

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
/* Consume the literal "null" token and return a CJSON_NULL node. */
static cjson_node_t *parse_null(parse_ctx_t *ctx) {
  if (ctx->pos + 4 > ctx->len || memcmp(ctx->src + ctx->pos, "null", 4) != 0) {
    parse_err(ctx, "expected 'null' at position %zu", ctx->pos);
    return NULL;
  }
  ctx->pos += 4;
  return node_alloc(CJSON_NULL, ctx->mp);
}

/* ------------------------------------------------------------------ bool -- */
/* Consume "true" or "false" and return the corresponding CJSON_BOOL node. */
static cjson_node_t *parse_bool(parse_ctx_t *ctx) {
  if (ctx->pos + 4 <= ctx->len && memcmp(ctx->src + ctx->pos, "true", 4) == 0) {
    ctx->pos += 4;
    cjson_node_t *n = node_alloc(CJSON_BOOL, ctx->mp);
    if (n) n->value.boolean = true;
    return n;
  }
  if (ctx->pos + 5 <= ctx->len &&
      memcmp(ctx->src + ctx->pos, "false", 5) == 0) {
    ctx->pos += 5;
    cjson_node_t *n = node_alloc(CJSON_BOOL, ctx->mp);
    if (n) n->value.boolean = false;
    return n;
  }
  parse_err(ctx, "expected 'true' or 'false' at position %zu", ctx->pos);
  return NULL;
}

/* ---------------------------------------------------------------- number -- */
/*
 * Parse a JSON number according to RFC 8259 section 6.
 *
 * Integers (no decimal point or exponent) that fit in long long are stored
 * as CJSON_INTEGER; everything else becomes CJSON_FLOAT.  A leading zero
 * may not be followed by more digits.  Integer literals that overflow
 * long long fall back to CJSON_FLOAT via strtod; values that would produce
 * an infinite double are rejected.
 */
static cjson_node_t *parse_number(parse_ctx_t *ctx) {
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
    cjson_node_t *n = node_alloc(CJSON_FLOAT, ctx->mp);
    if (!n) return NULL;
    n->value.number = dval;
    return n;
  } else {
    /* Try integer first; fall back to double if out of range. */
    char *endp;
    errno = 0;
    long long ival = strtoll(tok, &endp, 10);
    if (*endp == '\0' && errno != ERANGE) {
      cjson_node_t *n = node_alloc(CJSON_INTEGER, ctx->mp);
      if (!n) return NULL;
      n->value.integer = ival;
      return n;
    }
    double dval = strtod(tok, NULL);
    if (isinf(dval)) {
      parse_err(ctx, "number out of range at position %zu", start);
      return NULL;
    }
    cjson_node_t *n = node_alloc(CJSON_FLOAT, ctx->mp);
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
          /* \u0000 produces a null byte, which cannot be represented in a
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

/* Wrap parse_string_raw(): parse a JSON string literal and return a
 * CJSON_STRING node that owns the resulting heap-allocated C string. */
static cjson_node_t *parse_string(parse_ctx_t *ctx) {
  char *s = NULL;
  if (!parse_string_raw(ctx, &s)) return NULL;
  cjson_node_t *n = node_alloc(CJSON_STRING, ctx->mp);
  if (!n) {
    _mem_free(ctx->mp, s);
    return NULL;
  }
  n->value.string = s;
  return n;
}

/* ----------------------------------------------------------------- list -- */
/* Parse a JSON list ('[' value* ']') and return a CJSON_LIST node whose
 * backing cvec holds cjson_node_t * child pointers. */
static cjson_node_t *parse_list(parse_ctx_t *ctx) {
  if (!expect_char(ctx, '[')) return NULL;

  cjson_node_t *arr = (cjson_node_t *)cjson_create_list_mp(ctx->mp);
  if (!arr) return NULL;

  char c;
  if (!peek(ctx, &c)) {
    parse_err(ctx, "unterminated list");
    goto fail;
  }
  if (c == ']') {
    ctx->pos++;
    return arr;
  }

  while (1) {
    skip_ws(ctx);
    cjson_node_t *elem = parse_value(ctx);
    if (!elem) goto fail;
    if (cvector_push_back(arr->value.list, &elem) != ccol_success) {
      __cjson_destroy((cjson)elem);
      goto fail;
    }
    if (!peek(ctx, &c)) {
      parse_err(ctx, "unterminated list");
      goto fail;
    }
    if (c == ']') {
      ctx->pos++;
      return arr;
    }
    if (c != ',') {
      parse_err(ctx, "expected ',' or ']' in list at position %zu", ctx->pos);
      goto fail;
    }
    ctx->pos++;
  }

fail:
  __cjson_destroy((cjson)arr);
  return NULL;
}

/* ------------------------------------------------------------ dictionary -- */
/*
 * Parse a JSON dictionary ('{' (string ':' value)* '}') and return a
 * CJSON_DICTIONARY node backed by a chmap of char* -> cjson_node_t*.
 * Duplicate keys are silently overwritten (last writer wins), consistent
 * with the permissive guidance in RFC 8259 section 4.
 */
static cjson_node_t *parse_dictionary(parse_ctx_t *ctx) {
  if (!expect_char(ctx, '{')) return NULL;

  cjson_node_t *obj = (cjson_node_t *)cjson_create_dictionary_mp(ctx->mp);
  if (!obj) return NULL;

  char c;
  if (!peek(ctx, &c)) {
    parse_err(ctx, "unterminated dictionary");
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
    cjson_node_t *val = parse_value(ctx);
    if (!val) {
      _mem_free(ctx->mp, key);
      goto fail;
    }

    cmap_pair kp = {.ptr = key, .size = strlen(key) + 1};

    /* Snapshot the old child before the insert overwrites the slot. */
    cmap_pair *existing_vp = NULL;
    cjson_node_t *old_child = NULL;
    if (chmap_get_elem_ref(obj->value.dictionary, &kp, &existing_vp) ==
        ccol_success)
      old_child = _cjson_read_child(existing_vp->ptr);

    cmap_pair vp = {.ptr = &val, .size = sizeof(val)};
    ccol_retval_t r = chmap_insert_elem(obj->value.dictionary, &kp, &vp);
    _mem_free(ctx->mp, key);
    if (r != ccol_success && r != ccol_key_already_present) {
      __cjson_destroy((cjson)val);
      goto fail;
    }
    if (old_child) __cjson_destroy((cjson)old_child);

    if (!peek(ctx, &c)) {
      parse_err(ctx, "unterminated dictionary");
      goto fail;
    }
    if (c == '}') {
      ctx->pos++;
      return obj;
    }
    if (c != ',') {
      parse_err(ctx, "expected ',' or '}' in dictionary at position %zu",
                ctx->pos);
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
/* Skip whitespace then branch on the first character to call the appropriate
 * sub-parser.  Handles all seven JSON value types. */
static cjson_node_t *parse_value(parse_ctx_t *ctx) {
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
      return parse_list(ctx);
    case '{':
      return parse_dictionary(ctx);
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
/*
 * Shared implementation for the public parse entry points.
 *
 * Initialises the parse context, runs the recursive descent starting from
 * parse_value(), then verifies that no significant content follows the root
 * value (trailing garbage is rejected).
 *
 * On failure: *err_str (if non-NULL) receives a heap-allocated human-readable
 * error message, allocated through mp; the caller is responsible for freeing
 * it with cjson_serialize_free_mp() (passing the same mp).
 * On success: *err_str is set to NULL.
 */
static cjson parse_common(const char *src, size_t len, char **err_str,
                          ccol_memmgmt_procs_t *mp) {
  if (!src) {
    if (err_str) *err_str = ccol_strdup(mp, "null input");
    return NULL;
  }
  parse_ctx_t ctx = {.src = src, .pos = 0, .len = len, .error = "", .mp = mp};
  cjson_node_t *root = parse_value(&ctx);
  if (!root) {
    if (err_str) {
      const char *msg = ctx.error[0] ? ctx.error : "unknown parse error";
      *err_str = ccol_strdup(mp, msg);
    }
    return NULL;
  }
  /* Ensure nothing significant follows the root value. */
  skip_ws(&ctx);
  if (ctx.pos != ctx.len) {
    if (err_str) {
      char buf[64];
      snprintf(buf, sizeof(buf), "trailing garbage at position %zu", ctx.pos);
      *err_str = ccol_strdup(mp, buf);
    }
    __cjson_destroy((cjson)root);
    return NULL;
  }
  if (err_str) *err_str = NULL;
  return (cjson)root;
}

/* Parse a null-terminated JSON string.  mp may be NULL for the default
 * allocator.  On failure, *err_str (if non-NULL) receives a heap-allocated
 * error message the caller must free with free(). */
cjson cjson_parse_mp(const char *json_str, char **err_str,
                     ccol_memmgmt_procs_t *mp) {
  if (!json_str) return parse_common(NULL, 0, err_str, mp);
  return parse_common(json_str, strlen(json_str), err_str, mp);
}

/* Like cjson_parse_mp but accepts an explicit byte length so the input need
 * not be null-terminated.  Useful when parsing a JSON value embedded in a
 * larger buffer. */
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
   * decimal point for whole-number values (e.g. 1.0 --> "1"), which would
   * parse back as CJSON_INTEGER.  Appending ".0" fixes this; the longest
   * affected case is ±1e14 (15 digits + ".0\0" = 18 bytes, well within the
   * 32-byte buf passed by the caller). */
  if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
    size_t len = strlen(buf);
    if (len + 2 < cap) {
      buf[len] = '.';
      buf[len + 1] = '0';
      buf[len + 2] = '\0';
    }
  }
}

/* Emit a newline followed by (depth * indent) spaces for pretty-printing.
 * Does nothing when indent == 0 (compact output mode). */
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

/*
 * Recursively emit JSON text for the subtree rooted at n into sb.
 * indent: spaces per indentation level (0 = compact, no whitespace added).
 * depth:  current nesting depth; the top-level caller passes 0.
 * A NULL node pointer is emitted as the literal "null".
 */
static void serialize_node(sbuf_t *sb, cjson_node_t *n, unsigned int indent,
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
    case CJSON_LIST: {
      size_t cnt = cvector_elem_count(n->value.list);
      sb_append_c(sb, '[');
      for (size_t i = 0; i < cnt; i++) {
        if (i > 0) sb_append_c(sb, ',');
        if (indent) sb_append_indent(sb, indent, depth + 1);
        cjson_node_t *child = *(cjson_node_t **)cvector_at(n->value.list, i);
        serialize_node(sb, child, indent, depth + 1);
      }
      if (indent && cnt > 0) sb_append_indent(sb, indent, depth);
      sb_append_c(sb, ']');
      break;
    }
    case CJSON_DICTIONARY: {
      sb_append_c(sb, '{');
      size_t idx = 0;
      cmap_iterator *it = chashmap_begin_iter(n->value.dictionary, NULL);
      while (it) {
        if (idx > 0) sb_append_c(sb, ',');
        if (indent) sb_append_indent(sb, indent, depth + 1);
        sb_append_json_str(sb, (const char *)it->key_pair->ptr);
        sb_append_c(sb, ':');
        if (indent) sb_append_c(sb, ' ');
        cjson_node_t *child = _cjson_read_child(it->val_pair->ptr);
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

/* Serialize node to compact (no added whitespace) JSON.  Returns a
 * heap-allocated string that must be freed with cjson_serialize_free() or
 * cjson_serialize_free_mp().  Returns NULL on OOM. */
char *cjson_serialize(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
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

/* Serialize node to indented JSON.  indent is the number of spaces per
 * nesting level; 0 falls back to 4.  Free the result with
 * cjson_serialize_free() or cjson_serialize_free_mp(). */
char *cjson_serialize_pretty(cjson node, unsigned int indent) {
  cjson_node_t *n = (cjson_node_t *)node;
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

/* Release a string returned by cjson_serialize / cjson_serialize_pretty using
 * the same allocator that produced it (mp == NULL for the default allocator).
 */
void cjson_serialize_free_mp(char *s, ccol_memmgmt_procs_t *mp) {
  _mem_free(mp, s);
}

/* ========================================================================== */
/*                         PATH NAVIGATION                                    */
/* ========================================================================== */

/*
 * Path escape sequences (applies to all navigate/set helpers below):
 *
 *   \.   -> literal '.' in the key (not a path separator)
 *   \\   -> literal '\' in the key
 *
 * A '\' before any other character is left unchanged (passed through as-is).
 * The escape is resolved per-component after splitting on the separator.
 */

/* Returns a pointer to the first unescaped '.' in s, or NULL. */
static char *path_find_unescaped_dot(char *s) {
  char *p = s;
  while (*p) {
    if (*p == '\\' && (*(p + 1) == '.' || *(p + 1) == '\\')) {
      p += 2;
    } else if (*p == '.') {
      return p;
    } else {
      p++;
    }
  }
  return NULL;
}

/* Returns a pointer to the last unescaped '.' in s, or NULL. */
static char *path_find_last_unescaped_dot(char *s) {
  char *last = NULL;
  char *p = s;
  while (*p) {
    if (*p == '\\' && (*(p + 1) == '.' || *(p + 1) == '\\')) {
      p += 2;
    } else if (*p == '.') {
      last = p;
      p++;
    } else {
      p++;
    }
  }
  return last;
}

/* Resolves escape sequences in s in-place.  The string shrinks or stays the
 * same length; it is never lengthened. */
static void path_unescape_component(char *s) {
  char *r = s, *w = s;
  while (*r) {
    if (*r == '\\' && (*(r + 1) == '.' || *(r + 1) == '\\')) {
      *w++ = *(r + 1);
      r += 2;
    } else {
      *w++ = *r++;
    }
  }
  *w = '\0';
}

/*
 * Walk a dot-separated path through a JSON tree, returning the node at the
 * end of the path or NULL if any component is missing.
 *
 * path_copy must be a writable copy of the path string; this function
 * temporarily replaces unescaped '.' with '\0' to carve out each component
 * in-place, then unescapes the component before using it as a key.
 *
 * List elements are addressed with a '#' prefix: e.g. "items.#0.name"
 * navigates to the 'name' key of the first element of 'items'.
 * Consecutive or trailing dots return NULL.
 */
static cjson navigate(cjson root, char *path_copy) {
  cjson cur = root;
  char *p = path_copy;

  while (cur) {
    char *dot = path_find_unescaped_dot(p);
    if (dot) *dot = '\0';

    /* Empty component: consecutive dots ("a..b") or trailing dot ("a."). */
    if (p[0] == '\0') {
      cur = NULL;
      break;
    }

    path_unescape_component(p);

    cjson_node_t *n = (cjson_node_t *)cur;

    if (n->type == CJSON_LIST && p[0] == '#') {
      char *endp;
      errno = 0;
      long idx = strtol(p + 1, &endp, 10);
      if (endp == p + 1 || *endp != '\0' || idx < 0 || errno == ERANGE) {
        cur = NULL;
        break;
      }
      void *slot = cvector_at(n->value.list, (size_t)idx);
      cur = slot ? *(cjson *)slot : NULL;
    } else if (n->type == CJSON_DICTIONARY) {
      cmap_pair kp = {.ptr = p, .size = strlen(p) + 1};
      cmap_pair *vp = NULL;
      if (chmap_get_elem_ref(n->value.dictionary, &kp, &vp) != ccol_success)
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

/* Public implementation of the cjson_get(root, path) macro.
 * Duplicates path into a writable buffer before handing it to navigate()
 * so the caller's string is never modified. */
cjson _cjson_get(cjson root, const char *path) {
  if (!root) return NULL;
  if (!path || path[0] == '\0') return root;

  ccol_memmgmt_procs_t *mp = ((cjson_node_t *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return NULL;
  cjson result = navigate(root, copy);
  _mem_free(mp, copy);
  return result;
}

/*
 * Public implementation of the cjson_set(root, path, value) macro.
 *
 * The path is split on the LAST dot to separate the parent path from the
 * leaf key.  The leaf string is strdup'd before the parent-path copy is
 * freed to prevent use-after-free when the leaf is a direct suffix of the
 * full path.
 *
 * String normalisation: when raw_is_char_array is true the raw pointer
 * points directly at the char bytes (typeof("hi") == char[N] in C).  It is
 * normalised to const char ** so node_reinit_scalar always sees one
 * consistent pointer-to-pointer form.
 *
 * If the leaf key already exists the node is mutated in-place with
 * node_reinit_scalar.  Otherwise a new scalar node is allocated and inserted.
 * Intermediate containers are never created automatically -- the parent must
 * already exist.
 */
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

  ccol_memmgmt_procs_t *mp = ((cjson_node_t *)root)->m_procs;

  char *copy = ccol_strdup(mp, path);
  if (!copy) return ccol_not_enough_memory;

  char *last_dot = path_find_last_unescaped_dot(copy);
  char *leaf_copy;
  cjson parent;

  if (!last_dot) {
    leaf_copy = ccol_strdup(mp, path);
    parent = root;
    _mem_free(mp, copy);
  } else {
    *last_dot = '\0';
    if (copy[0] == '\0') {
      /* Leading dot: parent path is empty, which is a syntax error. */
      _mem_free(mp, copy);
      return ccol_invalid_args;
    }
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    parent = navigate(root, copy);
    _mem_free(mp, copy);
    if (!leaf_copy) return ccol_not_enough_memory;
    if (!parent) {
      _mem_free(mp, leaf_copy);
      return ccol_key_not_found;
    }
  }

  if (!leaf_copy) return ccol_not_enough_memory;
  path_unescape_component(leaf_copy);
  const char *leaf_comp = leaf_copy;

  if (leaf_comp[0] == '\0') {
    _mem_free(mp, leaf_copy);
    return ccol_invalid_args;
  }

  cjson_node_t *pn = (cjson_node_t *)parent;
  ccol_retval_t ret;

  if (pn->type == CJSON_DICTIONARY) {
    cmap_pair kp = {.ptr = (void *)leaf_comp, .size = strlen(leaf_comp) + 1};
    cmap_pair *existing_vp = NULL;

    if (chmap_get_elem_ref(pn->value.dictionary, &kp, &existing_vp) ==
        ccol_success) {
      cjson_node_t *existing = _cjson_read_child(existing_vp->ptr);
      ret = node_reinit_scalar(existing, type, raw, raw_size, is_signed);
    } else {
      cjson_node_t *new_node;
      ccol_retval_t make_r = node_make_scalar(type, raw, raw_size, is_signed,
                                              pn->m_procs, &new_node);
      if (make_r != ccol_success) {
        _mem_free(mp, leaf_copy);
        return make_r;
      }
      cmap_pair vp = {.ptr = &new_node, .size = sizeof(new_node)};
      ccol_retval_t r = chmap_insert_elem(pn->value.dictionary, &kp, &vp);
      if (r != ccol_success && r != ccol_key_already_present) {
        __cjson_destroy((cjson)new_node);
        ret = r;
      } else {
        ret = ccol_success;
      }
    }
  } else if (pn->type == CJSON_LIST) {
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
        void *slot = cvector_at(pn->value.list, (size_t)idx);
        if (!slot) {
          ret = ccol_invalid_args;
        } else {
          cjson_node_t *existing = *(cjson_node_t **)slot;
          ret = node_reinit_scalar(existing, type, raw, raw_size, is_signed);
        }
      }
    }
  } else {
    ret = ccol_invalid_args;
  }

  _mem_free(mp, leaf_copy);
  return ret;
}

/*
 * Public implementation of the cjson_delete(root, path) macro.
 *
 * Splits the path on the LAST unescaped dot to identify the parent node and
 * the leaf key.  When no dot is present, root is the parent.  The leaf key
 * is unescaped before use so '\.' and '\\' work identically to cjson_get and
 * cjson_set.
 *
 * Removes and deep-frees the addressed node.  Returns:
 *   ccol_success          - node removed and freed.
 *   ccol_invalid_args     - NULL root/path, empty path, or index out of range.
 *   ccol_key_not_found    - parent exists but leaf key / index is absent.
 *   ccol_not_enough_memory - strdup failed.
 */
ccol_retval_t _cjson_delete(cjson root, const char *path) {
  if (!root || !path || path[0] == '\0') return ccol_invalid_args;

  ccol_memmgmt_procs_t *mp = ((cjson_node_t *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return ccol_not_enough_memory;

  char *last_dot = path_find_last_unescaped_dot(copy);
  char *leaf_copy;
  cjson parent;

  if (!last_dot) {
    leaf_copy = ccol_strdup(mp, path);
    parent = root;
    _mem_free(mp, copy);
  } else {
    *last_dot = '\0';
    if (copy[0] == '\0') {
      /* Leading dot: parent path is empty, which is a syntax error. */
      _mem_free(mp, copy);
      return ccol_invalid_args;
    }
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    parent = navigate(root, copy);
    _mem_free(mp, copy);
    if (!leaf_copy) return ccol_not_enough_memory;
    if (!parent) {
      _mem_free(mp, leaf_copy);
      return ccol_key_not_found;
    }
  }

  if (!leaf_copy) return ccol_not_enough_memory;
  path_unescape_component(leaf_copy);
  const char *leaf_comp = leaf_copy;

  if (leaf_comp[0] == '\0') {
    _mem_free(mp, leaf_copy);
    return ccol_invalid_args;
  }

  cjson_node_t *pn = (cjson_node_t *)parent;
  ccol_retval_t ret;

  if (pn->type == CJSON_DICTIONARY) {
    ret = cjson_dictionary_remove(parent, leaf_comp);
  } else if (pn->type == CJSON_LIST) {
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
        ret = cjson_list_remove(parent, (size_t)idx);
      }
    }
  } else {
    ret = ccol_invalid_args;
  }

  _mem_free(mp, leaf_copy);
  return ret;
}
