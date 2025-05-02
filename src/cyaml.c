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
#include <cvector.h>
#include <cyaml.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>

/* ========================================================================== */
/*                         INTERNAL DOM NODE                                  */
/* ========================================================================== */

/*
 * Full definition of the DOM node.  Unlike cjson's tagged_node_t, this struct
 * is also the public handle type (cyaml == cyaml_node_t *) so it is not
 * truly opaque -- but callers must never access fields directly; they must
 * use the public API.
 *
 * Layout:
 *  type     : one of the seven cyaml_node_type_t values.
 *  m_procs  : allocator for this node and its owned strings; NULL = default.
 *  value    : union of scalar and composite payloads.
 *    list : cvec of cyaml_node_t *   (CYAML_LIST)
 *    dictionary  : chmap char*->cyaml_node_t* (CYAML_DICTIONARY)
 */
typedef struct cyaml_node_t {
  cyaml_node_type_t type;
  ccol_memmgmt_procs_t *m_procs;
  union {
    bool boolean;
    long long integer;
    double number;
    char *string;
    cvec list;
    chmap dictionary;
  } value;
} cyaml_node_t;

/* ========================================================================== */
/*                         SERIALIZATION BUFFER                               */
/* ========================================================================== */

/* Dynamic string buffer for YAML serialization, mirroring cjson's sbuf_t.
 * Once oom is set all yb_* operations become safe no-ops. */
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  bool oom;
  ccol_memmgmt_procs_t *m_procs;
} ybuf_t;

/* Allocate a 256-byte backing store and zero the length counter.
 * Sets oom on allocation failure. */
static void yb_init(ybuf_t *b, ccol_memmgmt_procs_t *mp) {
  b->m_procs = mp;
  b->buf = _mem_alloc(mp, 256);
  b->len = 0;
  b->cap = b->buf ? 256 : 0;
  b->oom = b->buf ? false : true;
  if (b->buf) b->buf[0] = '\0';
}

/* Double the buffer capacity until it holds 'needed' bytes.
 * Sets oom on reallocation failure or size_t overflow. */
static void yb_grow(ybuf_t *b, size_t needed) {
  if (b->oom) return;
  size_t nc = b->cap ? b->cap * 2 : 256;
  while (nc < needed) {
    if (nc > SIZE_MAX / 2) {
      b->oom = true;
      return;
    }
    nc *= 2;
  }
  char *p = _mem_realloc(b->m_procs, b->buf, nc);
  if (!p) {
    b->oom = true;
    return;
  }
  b->buf = p;
  b->cap = nc;
}

/* Append n raw bytes, growing the buffer as needed.  No-op when oom is set. */
static inline void yb_append(ybuf_t *b, const char *data, size_t n) {
  if (b->oom) return;
  if (b->len + n + 1 > b->cap) yb_grow(b, b->len + n + 1);
  if (b->oom) return;
  memcpy(b->buf + b->len, data, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

static inline void yb_append_c(ybuf_t *b, char c) { yb_append(b, &c, 1); }
static inline void yb_append_cstr(ybuf_t *b, const char *s) {
  yb_append(b, s, strlen(s));
}

/* ========================================================================== */
/*                         INTERNAL HELPERS                                   */
/* ========================================================================== */

/*
 * chmap_entry is __attribute__((packed)), so reading stored child pointers
 * via a direct cast is UB at -O3.  Use memcpy like cjson does.
 */
static inline cyaml_node_t *_cyaml_read_child(const void *src) {
  cyaml_node_t *p;
  memcpy(&p, src, sizeof(p));
  return p;
}

/* ========================================================================== */
/*                         THREAD-LOCAL NODE POOL                             */
/* ========================================================================== */

/*
 * Thread-local free-list pool for cyaml_node_t, mirroring the pool in
 * cjson.c.  Nodes allocated with the default allocator (m_procs == NULL) are
 * returned to this pool on node_free() instead of being freed immediately,
 * amortising calloc/free overhead in workloads that repeatedly parse and
 * destroy documents.
 *
 * The pool is capped at _CYAML_POOL_CAP entries; excess nodes are freed
 * immediately.  Custom-allocator nodes always bypass the pool.
 *
 * A pthread destructor registered via _pool_key drains the pool when a thread
 * exits, preventing permanent leaks from worker threads.
 */
#define _CYAML_POOL_CAP 512U
static __thread cyaml_node_t *_pool_head = NULL;
static __thread unsigned _pool_sz = 0;

/* Guards pthread_setspecific calls: set to true after the key is created,
 * false before it is deleted.  Prevents calling into a deleted key if a
 * background thread is still running during dlclose. */
static atomic_bool _pool_key_live = false;
static pthread_key_t _pool_key;

static void _pool_drain(void *);

/* DSO constructor: create the pthread key (registering _pool_drain as the
 * per-thread destructor) and mark it live. */
__attribute__((constructor)) static void _pool_key_init(void) {
  pthread_key_create(&_pool_key, _pool_drain);
  atomic_store(&_pool_key_live, true);
}

/* DSO destructor: drain the calling thread's own pool, mark the key dead so
 * concurrent threads skip the pthread_setspecific call, then delete the key
 * to avoid PTHREAD_KEYS_MAX exhaustion on repeated dlopen/dlclose cycles. */
__attribute__((destructor)) static void _pool_key_fini(void) {
  _pool_drain(NULL);
  atomic_store(&_pool_key_live, false);
  pthread_key_delete(_pool_key);
}

/* Allocate a new node, preferring a recycled entry from the thread-local pool
 * when mp == NULL.  The returned node has the given type tag and a zeroed
 * value union.  Returns NULL on allocation failure. */
static cyaml_node_t *node_alloc(cyaml_node_type_t type,
                                ccol_memmgmt_procs_t *mp) {
  cyaml_node_t *n;
  if (mp == NULL && _pool_head) {
    n = _pool_head;
    cyaml_node_t *next;
    memcpy(&next, (cyaml_node_t **)n, sizeof(next));
    _pool_head = next;
    _pool_sz--;
    memset(n, 0, sizeof(*n));
  } else {
    n = _mem_calloc(mp, 1, sizeof(*n));
    if (!n) return NULL;
  }
  n->type = type;
  n->m_procs = mp;
  return n;
}

/* Return a node to the thread-local pool (when m_procs == NULL and the pool
 * is not full) or release it directly through its own allocator. */
static void node_free(cyaml_node_t *n) {
  if (n->m_procs != NULL) {
    _mem_free(n->m_procs, n);
    return;
  }
  if (_pool_sz >= _CYAML_POOL_CAP) {
    free(n);
    return;
  }
  if (_pool_sz == 0 && atomic_load(&_pool_key_live))
    pthread_setspecific(_pool_key, (void *)1);
  memcpy((cyaml_node_t **)n, &_pool_head, sizeof(_pool_head));
  _pool_head = n;
  _pool_sz++;
}

/* Walk the pool free-list and call free() on every node.
 * Invoked on thread exit via the pthread destructor and on DSO unload. */
static void _pool_drain(void *arg) {
  (void)arg;
  cyaml_node_t *n = _pool_head;
  while (n) {
    cyaml_node_t *next;
    memcpy(&next, (cyaml_node_t **)n, sizeof(next));
    free(n);
    n = next;
  }
  _pool_head = NULL;
  _pool_sz = 0;
}

/* Deep-free the value resources without freeing the node struct itself. */
static void node_clear(cyaml_node_t *n) {
  switch (n->type) {
    case CYAML_STRING:
      _mem_free(n->m_procs, n->value.string);
      n->value.string = NULL;
      break;
    case CYAML_LIST: {
      size_t cnt = cvector_elem_count(n->value.list);
      for (size_t i = 0; i < cnt; i++) {
        cyaml_node_t *child = *(cyaml_node_t **)cvector_at(n->value.list, i);
        __cyaml_destroy((cyaml)child);
      }
      __cvector_destroy(n->value.list);
      n->value.list = NULL;
      break;
    }
    case CYAML_DICTIONARY: {
      cmap_iterator *it = chashmap_begin_iter(n->value.dictionary, NULL);
      while (it) {
        cyaml_node_t *child = _cyaml_read_child(it->val_pair->ptr);
        __cyaml_destroy((cyaml)child);
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
 * Overwrite an existing scalar node with a new typed value, deep-freeing
 * any resources the old content owned.
 */
static bool node_reinit_scalar(cyaml_node_t *n, cyaml_node_type_t type,
                               void *raw, size_t raw_size, bool is_signed) {
  switch (type) {
    case CYAML_NULL:
    case CYAML_BOOL:
    case CYAML_INTEGER:
    case CYAML_FLOAT:
    case CYAML_STRING:
      break;
    default:
      return false;
  }

  char *new_str = NULL;
  if (type == CYAML_STRING) {
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
    case CYAML_NULL:
      break;
    case CYAML_BOOL:
      n->value.boolean = *(bool *)raw;
      break;
    case CYAML_INTEGER: {
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
            _mem_free(n->m_procs, new_str);
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
            _mem_free(n->m_procs, new_str);
            return false;
        }
      }
      n->value.integer = v;
      break;
    }
    case CYAML_FLOAT: {
      double d = 0.0;
      if (raw_size == sizeof(float))
        d = (double)*(float *)raw;
      else if (raw_size == sizeof(double))
        d = *(double *)raw;
      else {
        _mem_free(n->m_procs, new_str);
        return false;
      }
      n->value.number = d;
      break;
    }
    case CYAML_STRING:
      if (!new_str)
        n->type = CYAML_NULL;
      else
        n->value.string = new_str;
      break;
    default:
      _mem_free(n->m_procs, new_str);
      return false;
  }
  return true;
}

/* Allocate a new node and immediately initialise it with a scalar value.
 * Returns NULL on OOM or if type / raw_size is invalid. */
static cyaml_node_t *node_make_scalar(cyaml_node_type_t type, void *raw,
                                      size_t raw_size, bool is_signed,
                                      ccol_memmgmt_procs_t *mp) {
  cyaml_node_t *n = node_alloc(CYAML_NULL, mp);
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

/*
 * Factory functions for individual node types.  mp may be NULL for the
 * default allocator.  All return NULL on allocation failure.
 *
 * cyaml_create_string_mp with val == NULL produces a CYAML_NULL node.
 * cyaml_create_list_mp and cyaml_create_dictionary_mp produce empty
 * containers backed by a cvec and a chmap respectively.
 */
cyaml cyaml_create_null_mp(ccol_memmgmt_procs_t *mp) {
  return (cyaml)node_alloc(CYAML_NULL, mp);
}

cyaml cyaml_create_bool_mp(bool val, ccol_memmgmt_procs_t *mp) {
  cyaml_node_t *n = node_alloc(CYAML_BOOL, mp);
  if (n) n->value.boolean = val;
  return (cyaml)n;
}

cyaml cyaml_create_int_mp(long long val, ccol_memmgmt_procs_t *mp) {
  cyaml_node_t *n = node_alloc(CYAML_INTEGER, mp);
  if (n) n->value.integer = val;
  return (cyaml)n;
}

cyaml cyaml_create_double_mp(double val, ccol_memmgmt_procs_t *mp) {
  cyaml_node_t *n = node_alloc(CYAML_FLOAT, mp);
  if (n) n->value.number = val;
  return (cyaml)n;
}

cyaml cyaml_create_string_mp(const char *val, ccol_memmgmt_procs_t *mp) {
  if (!val) return cyaml_create_null_mp(mp);
  cyaml_node_t *n = node_alloc(CYAML_STRING, mp);
  if (!n) return NULL;
  n->value.string = ccol_strdup(mp, val);
  if (!n->value.string) {
    node_free(n);
    return NULL;
  }
  return (cyaml)n;
}

cyaml cyaml_create_list_mp(ccol_memmgmt_procs_t *mp) {
  cyaml_node_t *n = node_alloc(CYAML_LIST, mp);
  if (!n) return NULL;
  n->value.list = cvector_create_full(sizeof(cyaml_node_t *), mp, NULL);
  if (!n->value.list) {
    node_free(n);
    return NULL;
  }
  return (cyaml)n;
}

cyaml cyaml_create_dictionary_mp(ccol_memmgmt_procs_t *mp) {
  cyaml_node_t *n = node_alloc(CYAML_DICTIONARY, mp);
  if (!n) return NULL;
  char *err = NULL;
  n->value.dictionary = chmap_create_mp(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                        ccol_string, ccol_pointer, mp, &err);
  if (!n->value.dictionary) {
    node_free(n);
    return NULL;
  }
  return (cyaml)n;
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/* Recursively free a node and all its descendants.
 * Safe to call on NULL.  Does NOT null the caller's pointer -- use the
 * cyaml_destroy() macro wrapper for that. */
void __cyaml_destroy(cyaml node) {
  if (!node) return;
  cyaml_node_t *n = (cyaml_node_t *)node;
  node_clear(n);
  node_free(n);
}

/* ========================================================================== */
/*                         LEAF VALUE ACCESS                                  */
/* ========================================================================== */

/* Return the node's type tag.  Returns CYAML_NULL for a NULL handle. */
cyaml_node_type_t cyaml_type(cyaml node) {
  if (!node) return CYAML_NULL;
  return ((cyaml_node_t *)node)->type;
}

/*
 * Typed value accessors.  Each calls fatal_err() on type mismatch or NULL.
 * Guard with cyaml_type() when the type is not statically guaranteed.
 */
bool cyaml_bool_val(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_BOOL)
    fatal_err("cyaml_bool_val: node is %s, expected CYAML_BOOL",
              cyaml_type_str(node));
  return n->value.boolean;
}

long long cyaml_int_val(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_INTEGER)
    fatal_err("cyaml_int_val: node is %s, expected CYAML_INTEGER",
              cyaml_type_str(node));
  return n->value.integer;
}

double cyaml_double_val(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_FLOAT)
    fatal_err("cyaml_double_val: node is %s, expected CYAML_FLOAT",
              cyaml_type_str(node));
  return n->value.number;
}

const char *cyaml_str_val(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_STRING)
    fatal_err("cyaml_str_val: node is %s, expected CYAML_STRING",
              cyaml_type_str(node));
  return n->value.string;
}

/* Return the number of elements in a list or the number of key-value
 * pairs in a dictionary.  Both call fatal_err() if the node is the wrong type.
 */
size_t cyaml_list_len(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_LIST)
    fatal_err("cyaml_list_len: node is %s, expected CYAML_LIST",
              cyaml_type_str(node));
  return cvector_elem_count(n->value.list);
}

size_t cyaml_dictionary_size(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_DICTIONARY)
    fatal_err("cyaml_dictionary_size: node is %s, expected CYAML_DICTIONARY",
              cyaml_type_str(node));
  return chmap_elem_count(n->value.dictionary);
}

/* ========================================================================== */
/*                          LIST / DICTIONARY MANIPULATION                    */
/* ========================================================================== */

/*
 * Append child to the list.  Ownership of child transfers unconditionally:
 * if the push fails (OOM or wrong type), child is destroyed before returning.
 * Mirrors the behaviour of cjson_list_push.
 */
ccol_retval_t cyaml_list_push(cyaml seq, cyaml child) {
  if (!child) return ccol_invalid_args;
  if (!seq) {
    __cyaml_destroy(child);
    return ccol_invalid_args;
  }
  cyaml_node_t *n = (cyaml_node_t *)seq;
  if (n->type != CYAML_LIST) {
    __cyaml_destroy(child);
    return ccol_invalid_args;
  }
  cyaml_node_t *c = (cyaml_node_t *)child;
  ccol_retval_t r = cvector_push_back(n->value.list, &c);
  if (r != ccol_success) __cyaml_destroy(child);
  return r;
}

/* Return the element at position index (borrowed), or NULL if out of bounds
 * or if seq is not a CYAML_LIST node. */
cyaml cyaml_list_get(cyaml seq, size_t index) {
  if (!seq) return NULL;
  cyaml_node_t *n = (cyaml_node_t *)seq;
  if (n->type != CYAML_LIST) return NULL;
  void *slot = cvector_at(n->value.list, index);
  if (!slot) return NULL;
  return *(cyaml *)slot;
}

/* Insert or replace the value for key in map.  Ownership of child transfers
 * unconditionally.  Any previous value for the key is destroyed after the
 * new pointer is safely written so the slot is never dangling. */
ccol_retval_t cyaml_dictionary_set(cyaml map, const char *key, cyaml child) {
  if (!child) return ccol_invalid_args;
  if (!map || !key) {
    __cyaml_destroy(child);
    return ccol_invalid_args;
  }
  cyaml_node_t *n = (cyaml_node_t *)map;
  if (n->type != CYAML_DICTIONARY) {
    __cyaml_destroy(child);
    return ccol_invalid_args;
  }

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};

  cmap_pair *old_vp = NULL;
  cyaml_node_t *old_child = NULL;
  if (chmap_get_elem_ref(n->value.dictionary, &kp, &old_vp) == ccol_success)
    old_child = _cyaml_read_child(old_vp->ptr);

  cyaml_node_t *c = (cyaml_node_t *)child;
  cmap_pair vp = {.ptr = &c, .size = sizeof(c)};
  ccol_retval_t r = chmap_insert_elem(n->value.dictionary, &kp, &vp);
  if (r == ccol_success || r == ccol_key_already_present) {
    if (old_child) __cyaml_destroy((cyaml)old_child);
    return ccol_success;
  }
  __cyaml_destroy((cyaml)child);
  return r;
}

/* Look up key in map and return the associated child (borrowed), or NULL if
 * not found, if map is NULL, or if map is not a CYAML_DICTIONARY node. */
cyaml cyaml_dictionary_get(cyaml map, const char *key) {
  if (!map || !key) return NULL;
  cyaml_node_t *n = (cyaml_node_t *)map;
  if (n->type != CYAML_DICTIONARY) return NULL;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair *vp = NULL;
  if (chmap_get_elem_ref(n->value.dictionary, &kp, &vp) != ccol_success)
    return NULL;
  return _cyaml_read_child(vp->ptr);
}

/*
 * Remove and deep-free the element at position index from a list.
 *
 * The element at index is destroyed and all subsequent elements are shifted
 * left by one position (O(n) in the number of elements after index).  The
 * vector is then shrunk by one via cvector_pop_back.
 */
ccol_retval_t cyaml_list_remove(cyaml seq, size_t index) {
  if (!seq) return ccol_invalid_args;
  cyaml_node_t *n = (cyaml_node_t *)seq;
  if (n->type != CYAML_LIST) return ccol_invalid_args;
  size_t cnt = cvector_elem_count(n->value.list);
  if (index >= cnt) return ccol_invalid_args;

  cyaml_node_t *child = *(cyaml_node_t **)cvector_at(n->value.list, index);
  __cyaml_destroy((cyaml)child);

  for (size_t i = index; i + 1 < cnt; i++) {
    cyaml_node_t **dst = (cyaml_node_t **)cvector_at(n->value.list, i);
    cyaml_node_t **src = (cyaml_node_t **)cvector_at(n->value.list, i + 1);
    *dst = *src;
  }
  cyaml_node_t *tmp = NULL;
  cvector_pop_back(n->value.list, &tmp);
  return ccol_success;
}

/*
 * Remove and deep-free the entry with the given key from a dictionary.
 *
 * The child pointer is read out before the map entry is deleted so the
 * subtree is freed after the hash table no longer references it.
 */
ccol_retval_t cyaml_dictionary_remove(cyaml map, const char *key) {
  if (!map || !key) return ccol_invalid_args;
  cyaml_node_t *n = (cyaml_node_t *)map;
  if (n->type != CYAML_DICTIONARY) return ccol_invalid_args;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair *vp = NULL;
  if (chmap_get_elem_ref(n->value.dictionary, &kp, &vp) != ccol_success)
    return ccol_key_not_found;
  cyaml_node_t *child = _cyaml_read_child(vp->ptr);
  ccol_retval_t r = chmap_delete_elem(n->value.dictionary, &kp);
  if (r == ccol_success) __cyaml_destroy((cyaml)child);
  return r;
}

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

/* Produce an independent deep copy of the subtree rooted at node.
 * The clone inherits the source node's allocator.  On OOM any partially-built
 * clone is destroyed before NULL is returned. */
cyaml cyaml_clone(cyaml node) {
  if (!node) return NULL;
  cyaml_node_t *src = (cyaml_node_t *)node;
  ccol_memmgmt_procs_t *mp = src->m_procs;

  switch (src->type) {
    case CYAML_NULL:
      return cyaml_create_null_mp(mp);
    case CYAML_BOOL:
      return cyaml_create_bool_mp(src->value.boolean, mp);
    case CYAML_INTEGER:
      return cyaml_create_int_mp(src->value.integer, mp);
    case CYAML_FLOAT:
      return cyaml_create_double_mp(src->value.number, mp);
    case CYAML_STRING:
      return cyaml_create_string_mp(src->value.string, mp);
    case CYAML_LIST: {
      cyaml dst = cyaml_create_list_mp(mp);
      if (!dst) return NULL;
      size_t cnt = cvector_elem_count(src->value.list);
      for (size_t i = 0; i < cnt; i++) {
        cyaml_node_t *child = *(cyaml_node_t **)cvector_at(src->value.list, i);
        cyaml cc = cyaml_clone((cyaml)child);
        if (!cc) {
          __cyaml_destroy(dst);
          return NULL;
        }
        if (cyaml_list_push(dst, cc) != ccol_success) {
          __cyaml_destroy(dst);
          return NULL;
        }
      }
      return dst;
    }
    case CYAML_DICTIONARY: {
      cyaml dst = cyaml_create_dictionary_mp(mp);
      if (!dst) return NULL;
      cmap_iterator *it = chashmap_begin_iter(src->value.dictionary, NULL);
      while (it) {
        const char *key = (const char *)it->key_pair->ptr;
        cyaml_node_t *child = _cyaml_read_child(it->val_pair->ptr);
        cyaml cc = cyaml_clone((cyaml)child);
        if (!cc) {
          ccol_iter_destroy(it);
          __cyaml_destroy(dst);
          return NULL;
        }
        if (cyaml_dictionary_set(dst, key, cc) != ccol_success) {
          ccol_iter_destroy(it);
          __cyaml_destroy(dst);
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
/*                         PARSER INTERNALS                                   */
/* ========================================================================== */

/*
 * Parse context.
 *
 * anchors: lazily-created chmap (char* -> cyaml_node_t*) storing deep clones
 * of anchored nodes.  NULL until the first & is encountered.  All entries are
 * freed (nodes destroyed, keys freed) at the end of parse_common.
 *
 * indent_stack: not needed -- indentation is passed as an integer argument
 * through the recursive descent.
 */
typedef struct {
  const char *src;
  size_t pos;
  size_t len;
  char error[512];
  ccol_memmgmt_procs_t *mp;
  chmap anchors;
} parse_ctx_t;

/* Format a parse error into ctx->error.  Only the last call is kept;
 * earlier messages are silently overwritten. */
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

/* ----- Position helpers -------------------------------------------------- */

/* at_end: true when all input has been consumed.
 * cur:    return the current character without advancing; '\0' at end. */
static inline bool at_end(parse_ctx_t *ctx) { return ctx->pos >= ctx->len; }

static inline char cur(parse_ctx_t *ctx) {
  return at_end(ctx) ? '\0' : ctx->src[ctx->pos];
}

/* Return the 0-based column of ctx->pos on its line (distance from the
 * last '\n').  Used to detect indentation levels for block collections. */
static int current_col(parse_ctx_t *ctx) {
  if (ctx->pos == 0) return 0;
  size_t p = ctx->pos;
  while (p > 0 && ctx->src[p - 1] != '\n') p--;
  return (int)(ctx->pos - p);
}

/* Skip only horizontal whitespace (spaces and tabs) without crossing a
 * newline.  Essential for indentation-sensitive block parsing where the
 * column position of the next token is meaningful. */
static void skip_inline_ws(parse_ctx_t *ctx) {
  while (!at_end(ctx)) {
    char c = cur(ctx);
    if (c == ' ' || c == '\t')
      ctx->pos++;
    else
      break;
  }
}

/* Discard the rest of the current line, including the terminating newline. */
static void skip_to_eol(parse_ctx_t *ctx) {
  while (!at_end(ctx) && cur(ctx) != '\n') ctx->pos++;
  if (!at_end(ctx)) ctx->pos++; /* consume '\n' */
}

static inline bool at_eol(parse_ctx_t *ctx) {
  return at_end(ctx) || cur(ctx) == '\n' || cur(ctx) == '\r';
}

/* Skip any combination of whitespace (including newlines) and '#' comments.
 * Used between top-level structural tokens where indentation is not
 * significant (e.g., between flow collection elements). */
static void skip_ws_comments(parse_ctx_t *ctx) {
  while (!at_end(ctx)) {
    char c = cur(ctx);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ctx->pos++;
    } else if (c == '#') {
      skip_to_eol(ctx);
    } else {
      break;
    }
  }
}

/* Consume exactly one newline list: CR, LF, or CRLF. */
static void skip_newline(parse_ctx_t *ctx) {
  if (!at_end(ctx) && cur(ctx) == '\r') ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '\n') ctx->pos++;
}

/* Returns true when ctx->pos is at a document-start ('---') or document-end
 * ('...') marker: the three characters at column 0, followed by whitespace, a
 * comment character, or EOF.  Used to stop block collection parsers at document
 * boundaries and to detect the start of the next document in a multi-document
 * stream. */
static bool at_doc_marker(parse_ctx_t *ctx) {
  if (current_col(ctx) != 0) return false;
  if (ctx->pos + 3 > ctx->len) return false;
  const char *p = ctx->src + ctx->pos;
  if (memcmp(p, "---", 3) != 0 && memcmp(p, "...", 3) != 0) return false;
  if (ctx->pos + 3 == ctx->len) return true;
  char nx = ctx->src[ctx->pos + 3];
  return nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '#';
}

/* ----- Anchor / alias helpers -------------------------------------------- */

/*
 * Anchor table management.
 *
 * Anchors (&name) are stored in a lazily-created chmap (char* ->
 * cyaml_node_t*). When an anchor is encountered the node is deep-cloned and
 * stored here. An alias (*name) resolves to yet another deep clone so every
 * expansion produces an independent subtree (no shared ownership).
 *
 * anchors_ensure: lazily allocate the map on first use.
 * anchors_store:  save a clone under name, destroying any prior clone.
 * anchors_lookup: return the stored clone (borrowed) or NULL.
 * anchors_destroy: free all stored clones and the map itself.
 *
 * The table is always destroyed at the end of parse_common regardless of
 * success or failure.
 */
static bool anchors_ensure(parse_ctx_t *ctx) {
  if (ctx->anchors) return true;
  char *err = NULL;
  ctx->anchors = chmap_create_mp(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                                 ccol_pointer, ctx->mp, &err);
  return ctx->anchors != NULL;
}

static void anchors_store(parse_ctx_t *ctx, const char *name,
                          cyaml_node_t *clone) {
  if (!anchors_ensure(ctx)) {
    __cyaml_destroy((cyaml)clone);
    return;
  }
  cmap_pair kp = {.ptr = (void *)name, .size = strlen(name) + 1};
  /* Read the old clone pointer BEFORE the insert so the slot pointer stays
   * valid even if chmap_insert_elem triggers a bucket-array rehash. */
  cmap_pair *old_vp = NULL;
  cyaml_node_t *old_clone = NULL;
  if (chmap_get_elem_ref(ctx->anchors, &kp, &old_vp) == ccol_success)
    old_clone = _cyaml_read_child(old_vp->ptr);

  cmap_pair vp = {.ptr = &clone, .size = sizeof(clone)};
  ccol_retval_t ins = chmap_insert_elem(ctx->anchors, &kp, &vp);
  if (ins == ccol_success || ins == ccol_key_already_present) {
    /* Insert/update succeeded: the map now owns the new clone. */
    if (old_clone) __cyaml_destroy((cyaml)old_clone);
  } else {
    /* Insert failed: preserve the old clone in the map, discard the new one. */
    __cyaml_destroy((cyaml)clone);
  }
}

static cyaml_node_t *anchors_lookup(parse_ctx_t *ctx, const char *name) {
  if (!ctx->anchors) return NULL;
  cmap_pair kp = {.ptr = (void *)name, .size = strlen(name) + 1};
  cmap_pair *vp = NULL;
  if (chmap_get_elem_ref(ctx->anchors, &kp, &vp) != ccol_success) return NULL;
  return _cyaml_read_child(vp->ptr);
}

static void anchors_destroy(parse_ctx_t *ctx) {
  if (!ctx->anchors) return;
  cmap_iterator *it = chashmap_begin_iter(ctx->anchors, NULL);
  while (it) {
    cyaml_node_t *n = _cyaml_read_child(it->val_pair->ptr);
    __cyaml_destroy((cyaml)n);
    it = it->_next_fn(it);
  }
  __chmap_destroy(ctx->anchors);
  ctx->anchors = NULL;
}

/* ----- Anchor / alias name parsing --------------------------------------- */

/* Read the anchor or alias name that follows '&' or '*'.  A name is any
 * non-empty list of characters that are not whitespace, flow indicators,
 * or '#'.  Returns false (with a parse error) if the name is empty. */
static bool parse_anchor_name(parse_ctx_t *ctx, char **name_out) {
  size_t start = ctx->pos;
  while (!at_end(ctx)) {
    char c = cur(ctx);
    /* Anchor name ends at whitespace, flow indicators, or comment. */
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' ||
        c == '[' || c == ']' || c == '{' || c == '}' || c == '#')
      break;
    ctx->pos++;
  }
  size_t len = ctx->pos - start;
  if (len == 0) {
    parse_err(ctx, "empty anchor/alias name at position %zu", start);
    return false;
  }
  char *name = _mem_alloc(ctx->mp, len + 1);
  if (!name) return false;
  memcpy(name, ctx->src + start, len);
  name[len] = '\0';
  *name_out = name;
  return true;
}

/* ----- Implicit type resolution ------------------------------------------ */

/*
 * Attempt to parse s as a YAML 1.2 core-schema typed scalar.
 * Returns a newly allocated node; the caller owns it.
 */
static cyaml_node_t *make_typed_scalar(parse_ctx_t *ctx, const char *s) {
  ccol_memmgmt_procs_t *mp = ctx->mp;

  /* null */
  if (s[0] == '\0' || strcmp(s, "~") == 0 || strcmp(s, "null") == 0 ||
      strcmp(s, "Null") == 0 || strcmp(s, "NULL") == 0)
    return node_alloc(CYAML_NULL, mp);

  /* bool */
  if (strcmp(s, "true") == 0 || strcmp(s, "True") == 0 ||
      strcmp(s, "TRUE") == 0) {
    cyaml_node_t *n = node_alloc(CYAML_BOOL, mp);
    if (n) n->value.boolean = true;
    return n;
  }
  if (strcmp(s, "false") == 0 || strcmp(s, "False") == 0 ||
      strcmp(s, "FALSE") == 0) {
    cyaml_node_t *n = node_alloc(CYAML_BOOL, mp);
    if (n) n->value.boolean = false;
    return n;
  }

  /* float specials */
  if (strcmp(s, ".inf") == 0 || strcmp(s, ".Inf") == 0 ||
      strcmp(s, ".INF") == 0 || strcmp(s, "+.inf") == 0 ||
      strcmp(s, "+.Inf") == 0 || strcmp(s, "+.INF") == 0) {
    cyaml_node_t *n = node_alloc(CYAML_FLOAT, mp);
    if (n) n->value.number = __builtin_inf();
    return n;
  }
  if (strcmp(s, "-.inf") == 0 || strcmp(s, "-.Inf") == 0 ||
      strcmp(s, "-.INF") == 0) {
    cyaml_node_t *n = node_alloc(CYAML_FLOAT, mp);
    if (n) n->value.number = -__builtin_inf();
    return n;
  }
  if (strcmp(s, ".nan") == 0 || strcmp(s, ".NaN") == 0 ||
      strcmp(s, ".NAN") == 0) {
    cyaml_node_t *n = node_alloc(CYAML_FLOAT, mp);
    if (n) n->value.number = __builtin_nan("");
    return n;
  }

  /* integer: optional sign, then digits (decimal / 0o octal / 0x hex) */
  {
    const char *p = s;
    bool neg = false;
    if (*p == '+')
      p++;
    else if (*p == '-') {
      neg = true;
      p++;
    }

    if (*p != '\0') {
      char *endp = NULL;
      long long ival = 0;
      bool is_int = false;
      errno = 0;

      if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        /* hex */
        unsigned long long uval = strtoull(p + 2, &endp, 16);
        if (endp != p + 2 && *endp == '\0' && errno != ERANGE) {
          ival = neg ? -(long long)uval : (long long)uval;
          is_int = true;
        }
      } else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) {
        /* octal */
        unsigned long long uval = strtoull(p + 2, &endp, 8);
        if (endp != p + 2 && *endp == '\0' && errno != ERANGE) {
          ival = neg ? -(long long)uval : (long long)uval;
          is_int = true;
        }
      } else {
        /* decimal: pass the full original string (including the optional sign)
         * so that strtoll handles LLONG_MIN (-9223372036854775808) correctly.
         * strtoll on the sign-stripped substring p would overflow for 2^63. */
        ival = strtoll(s, &endp, 10);
        if (endp != s && *endp == '\0' && errno != ERANGE) is_int = true;
      }

      if (is_int) {
        cyaml_node_t *n = node_alloc(CYAML_INTEGER, mp);
        if (n) n->value.integer = ival;
        return n;
      }
    }
  }

  /* float: optional sign, decimal digits with optional dot/exponent.
   * Pre-filter: YAML 1.2 core schema only recognises dot-prefixed float
   * specials (.nan, .inf, etc.).  strtod() on C99 platforms also accepts bare
   * "nan", "inf", and "infinity" (case-insensitive, with optional +/-), which
   * are not YAML floats and must fall through to the string fallback. */
  {
    const char *q = s;
    if (*q == '+' || *q == '-') q++;
    bool _is_c99_special =
        (strcasecmp(q, "nan") == 0 || strcasecmp(q, "inf") == 0 ||
         strcasecmp(q, "infinity") == 0);
    if (!_is_c99_special) {
      char *endp = NULL;
      errno = 0;
      double dval = strtod(s, &endp);
      if (endp != s && *endp == '\0' && errno != ERANGE) {
        cyaml_node_t *n = node_alloc(CYAML_FLOAT, mp);
        if (n) n->value.number = dval;
        return n;
      }
    }
  }

  /* string fallback */
  cyaml_node_t *n = node_alloc(CYAML_STRING, mp);
  if (!n) return NULL;
  n->value.string = ccol_strdup(mp, s);
  if (!n->value.string) {
    node_free(n);
    return NULL;
  }
  return n;
}

/* ----- Double-quoted scalar ---------------------------------------------- */

/* UTF-8 encode a codepoint into yb. */
static void encode_utf8(ybuf_t *b, uint32_t cp) {
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
  yb_append(b, buf, (size_t)n);
}

/* Read exactly 4 hex digits and decode them to a Unicode code point.
 * Advances ctx->pos by 4 on success.  Returns false on incomplete input
 * or invalid hex digits. */
static bool parse_hex4(parse_ctx_t *ctx, uint32_t *out) {
  if (ctx->pos + 4 > ctx->len) {
    parse_err(ctx, "incomplete \\uXXXX at position %zu", ctx->pos);
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

/* Read exactly 8 hex digits for a YAML \UXXXXXXXX escape (Unicode supplementary
 * planes).  Advances ctx->pos by 8 on success. */
static bool parse_hex8(parse_ctx_t *ctx, uint32_t *out) {
  if (ctx->pos + 8 > ctx->len) {
    parse_err(ctx, "incomplete \\UXXXXXXXX at position %zu", ctx->pos);
    return false;
  }
  uint32_t v = 0;
  for (int i = 0; i < 8; i++) {
    char c = ctx->src[ctx->pos + i];
    uint32_t d;
    if (c >= '0' && c <= '9')
      d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f')
      d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      d = (uint32_t)(c - 'A' + 10);
    else {
      parse_err(ctx, "invalid hex digit '%c' in \\UXXXXXXXX at position %zu", c,
                ctx->pos + i);
      return false;
    }
    v = (v << 4) | d;
  }
  ctx->pos += 8;
  *out = v;
  return true;
}

/*
 * Parse a double-quoted YAML scalar starting at the opening '"'.
 * Handles YAML escape sequences (superset of JSON escapes).
 * On success stores a heap-allocated string in *out and returns true.
 */
static bool parse_double_quoted(parse_ctx_t *ctx, char **out) {
  if (at_end(ctx) || cur(ctx) != '"') {
    parse_err(ctx, "expected '\"' at position %zu", ctx->pos);
    return false;
  }
  ctx->pos++;

  ybuf_t b;
  yb_init(&b, ctx->mp);

  while (!at_end(ctx)) {
    char c = cur(ctx);

    if (c == '"') {
      ctx->pos++;
      goto done;
    }

    if (c == '\\') {
      ctx->pos++;
      if (at_end(ctx)) break;
      char esc = cur(ctx);
      ctx->pos++;
      switch (esc) {
        case '0':
          yb_append_c(&b, '\0');
          break;
        case 'a':
          yb_append_c(&b, '\a');
          break;
        case 'b':
          yb_append_c(&b, '\b');
          break;
        case 't':
        case '\t':
          yb_append_c(&b, '\t');
          break;
        case 'n':
          yb_append_c(&b, '\n');
          break;
        case 'v':
          yb_append_c(&b, '\v');
          break;
        case 'f':
          yb_append_c(&b, '\f');
          break;
        case 'r':
          yb_append_c(&b, '\r');
          break;
        case 'e':
          yb_append_c(&b, 0x1B);
          break;
        case ' ':
          yb_append_c(&b, ' ');
          break;
        case '"':
          yb_append_c(&b, '"');
          break;
        case '/':
          yb_append_c(&b, '/');
          break;
        case '\\':
          yb_append_c(&b, '\\');
          break;
        case 'N': /* NEL */
          yb_append(&b, "\xC2\x85", 2);
          break;
        case '_': /* NBSP */
          yb_append(&b, "\xC2\xA0", 2);
          break;
        case 'L': /* LS */
          yb_append(&b, "\xE2\x80\xA8", 3);
          break;
        case 'P': /* PS */
          yb_append(&b, "\xE2\x80\xA9", 3);
          break;
        case 'x': {
          /* \xXX -- 2-digit hex */
          if (ctx->pos + 2 > ctx->len) {
            parse_err(ctx, "incomplete \\xXX at position %zu", ctx->pos);
            goto fail;
          }
          uint32_t v = 0;
          for (int i = 0; i < 2; i++) {
            char hc = ctx->src[ctx->pos + i];
            uint32_t d;
            if (hc >= '0' && hc <= '9')
              d = (uint32_t)(hc - '0');
            else if (hc >= 'a' && hc <= 'f')
              d = (uint32_t)(hc - 'a' + 10);
            else if (hc >= 'A' && hc <= 'F')
              d = (uint32_t)(hc - 'A' + 10);
            else {
              parse_err(ctx, "invalid hex digit in \\xXX at position %zu",
                        ctx->pos + i);
              goto fail;
            }
            v = (v << 4) | d;
          }
          ctx->pos += 2;
          encode_utf8(&b, v);
          break;
        }
        case 'u': {
          uint32_t cp;
          if (!parse_hex4(ctx, &cp)) goto fail;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (ctx->pos + 1 < ctx->len && ctx->src[ctx->pos] == '\\' &&
                ctx->src[ctx->pos + 1] == 'u') {
              ctx->pos += 2;
              uint32_t low;
              if (!parse_hex4(ctx, &low)) goto fail;
              if (low >= 0xDC00 && low <= 0xDFFF)
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
              else
                cp = 0xFFFD;
            } else
              cp = 0xFFFD;
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          encode_utf8(&b, cp);
          break;
        }
        case 'U': {
          uint32_t cp;
          if (!parse_hex8(ctx, &cp)) goto fail;
          encode_utf8(&b, cp);
          break;
        }
        case '\n':
        case '\r': {
          /* Escaped newline: trim trailing spaces, skip leading spaces on next
           * line (YAML line folding in double-quoted scalars). */
          if (esc == '\r' && !at_end(ctx) && cur(ctx) == '\n') ctx->pos++;
          skip_inline_ws(ctx);
          break;
        }
        default:
          parse_err(ctx, "unknown escape '\\%c' at position %zu", esc,
                    ctx->pos - 1);
          goto fail;
      }
      continue;
    }

    /* Newlines within double-quoted scalar: fold to space.
     * YAML 1.2 sec. 8.1.2: trailing white space is stripped from the line
     * where the fold occurs before the fold character is inserted. */
    if (c == '\r' || c == '\n') {
      if (!b.oom) {
        while (b.len > 0 &&
               (b.buf[b.len - 1] == ' ' || b.buf[b.len - 1] == '\t'))
          b.buf[--b.len] = '\0';
      }
      skip_newline(ctx);
      /* Multiple newlines -> keep as newlines; single newline -> space. */
      if (!at_end(ctx) && (cur(ctx) == '\r' || cur(ctx) == '\n')) {
        /* One or more extra blank lines: output newlines. */
        while (!at_end(ctx) && (cur(ctx) == '\r' || cur(ctx) == '\n')) {
          yb_append_c(&b, '\n');
          skip_newline(ctx);
        }
      } else {
        yb_append_c(&b, ' ');
      }
      skip_inline_ws(ctx);
      continue;
    }

    yb_append_c(&b, c);
    ctx->pos++;
  }

  parse_err(ctx, "unterminated double-quoted scalar");
  goto fail;

done:
  if (b.oom) goto fail;
  *out = b.buf;
  return true;

fail:
  _mem_free(b.m_procs, b.buf);
  return false;
}

/* ----- Single-quoted scalar ---------------------------------------------- */

/*
 * Parse a single-quoted YAML scalar.  The only escape list is '' (two
 * consecutive single quotes) representing a literal single quote.
 * Newlines within the scalar are line-folded (same rules as double-quoted).
 */
static bool parse_single_quoted(parse_ctx_t *ctx, char **out) {
  if (at_end(ctx) || cur(ctx) != '\'') {
    parse_err(ctx, "expected \"'\" at position %zu", ctx->pos);
    return false;
  }
  ctx->pos++;

  ybuf_t b;
  yb_init(&b, ctx->mp);

  while (!at_end(ctx)) {
    char c = cur(ctx);

    if (c == '\'') {
      ctx->pos++;
      if (!at_end(ctx) && cur(ctx) == '\'') {
        yb_append_c(&b, '\'');
        ctx->pos++;
        continue;
      }
      goto done;
    }

    /* Same trailing-whitespace-strip rule as double-quoted (YAML 1.2
     * sec. 8.1.2). */
    if (c == '\r' || c == '\n') {
      if (!b.oom) {
        while (b.len > 0 &&
               (b.buf[b.len - 1] == ' ' || b.buf[b.len - 1] == '\t'))
          b.buf[--b.len] = '\0';
      }
      skip_newline(ctx);
      if (!at_end(ctx) && (cur(ctx) == '\r' || cur(ctx) == '\n')) {
        while (!at_end(ctx) && (cur(ctx) == '\r' || cur(ctx) == '\n')) {
          yb_append_c(&b, '\n');
          skip_newline(ctx);
        }
      } else {
        yb_append_c(&b, ' ');
      }
      skip_inline_ws(ctx);
      continue;
    }

    yb_append_c(&b, c);
    ctx->pos++;
  }

  parse_err(ctx, "unterminated single-quoted scalar");
  _mem_free(b.m_procs, b.buf);
  return false;

done:
  if (b.oom) {
    _mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

/* ----- Block scalar (literal | and folded >) ----------------------------- */

typedef enum { CHOMP_CLIP, CHOMP_STRIP, CHOMP_KEEP } chomp_t;

/*
 * Parse the header of a block scalar: the style indicator (| or >) has
 * already been consumed.  The header is on the same line and consists of
 * an optional chomp indicator (- or +) and/or an optional explicit indent
 * indicator (a single digit 1-9).
 *
 * Sets *chomp and *explicit_indent (0 = not specified).
 */
static bool parse_block_scalar_header(parse_ctx_t *ctx, chomp_t *chomp,
                                      int *explicit_indent) {
  *chomp = CHOMP_CLIP;
  *explicit_indent = 0;

  /* The header may have both a chomping and an indentation indicator in
   * either order. */
  for (int pass = 0; pass < 2; pass++) {
    if (at_end(ctx) || at_eol(ctx)) break;
    char c = cur(ctx);
    if (c == '-') {
      *chomp = CHOMP_STRIP;
      ctx->pos++;
    } else if (c == '+') {
      *chomp = CHOMP_KEEP;
      ctx->pos++;
    } else if (c >= '1' && c <= '9') {
      *explicit_indent = c - '0';
      ctx->pos++;
    } else
      break;
  }

  /* Skip optional inline comment and the newline. */
  skip_inline_ws(ctx);
  if (!at_end(ctx) && cur(ctx) == '#')
    skip_to_eol(ctx);
  else if (!at_end(ctx) && at_eol(ctx))
    skip_newline(ctx);
  else if (!at_end(ctx)) {
    parse_err(ctx,
              "unexpected character after block scalar header at position %zu",
              ctx->pos);
    return false;
  }
  return true;
}

/*
 * Read the content of a block scalar.
 *
 * parent_indent: the indentation level of the block container that holds this
 *   scalar (used as base to compute block indent when not explicit).
 *
 * On success, *out receives a heap-allocated string (the caller must free with
 * ctx->mp).  Returns true on success.
 */
static bool parse_block_scalar_content(parse_ctx_t *ctx, int parent_indent,
                                       chomp_t chomp, int explicit_indent,
                                       char **out) {
  /*
   * Determine the block indent.  If an explicit indicator was given, it is
   * relative to the parent; otherwise auto-detect from the first non-empty
   * content line.
   */
  int block_indent = 0;
  bool indent_determined = (explicit_indent > 0);
  if (indent_determined) block_indent = parent_indent + explicit_indent;

  ybuf_t b;
  yb_init(&b, ctx->mp);

  /* Trailing newline tracking for chomping. */
  int trailing_newlines = 0;
  bool have_content = false;

  while (!at_end(ctx)) {
    /* Measure indentation of this line. */
    size_t line_start = ctx->pos;
    int spaces = 0;
    while (!at_end(ctx) && cur(ctx) == ' ') {
      spaces++;
      ctx->pos++;
    }

    /* Empty or comment-only line? */
    if (at_eol(ctx)) {
      /* All blank lines, regardless of indentation, are part of the scalar
       * per YAML 1.2 sec. 8.1. */
      trailing_newlines++;
      ctx->pos = line_start;
      skip_to_eol(ctx);
      continue;
    }

    /* Non-empty line. */
    if (!indent_determined) {
      /* Auto-detect indent from first non-empty content line. */
      if (spaces <= parent_indent) {
        /* First non-empty line is not more indented than parent: scalar is
         * empty. */
        ctx->pos = line_start;
        break;
      }
      block_indent = spaces;
      indent_determined = true;
    }

    if (spaces < block_indent) {
      /* This line is less indented than the block: scalar ends here. */
      ctx->pos = line_start;
      break;
    }

    /* Emit buffered newlines (leading or inter-content) before this line. */
    for (int i = 0; i < trailing_newlines; i++) yb_append_c(&b, '\n');
    trailing_newlines = 0;
    have_content = true;

    /* Skip exactly block_indent spaces (already consumed above, but we
     * may have consumed more than block_indent). */
    int extra = spaces - block_indent;

    /* Append extra leading spaces (for more-indented lines). */
    for (int i = 0; i < extra; i++) yb_append_c(&b, ' ');

    /* Read the content of this line. */
    while (!at_end(ctx) && !at_eol(ctx)) {
      yb_append_c(&b, cur(ctx));
      ctx->pos++;
    }

    trailing_newlines = 1;
    skip_newline(ctx);
  }

  /* Apply chomping. */
  switch (chomp) {
    case CHOMP_STRIP:
      /* No trailing newlines. */
      break;
    case CHOMP_CLIP:
      /* One trailing newline if there was any content. */
      if (have_content) yb_append_c(&b, '\n');
      break;
    case CHOMP_KEEP:
      /* All trailing newlines, including those preceding the first content
       * line.  YAML 1.2 sec. 8.1.1.2: trailing empty lines are part of the
       * scalar's content regardless of whether any non-empty lines appear. */
      for (int i = 0; i < trailing_newlines; i++) yb_append_c(&b, '\n');
      break;
  }

  if (b.oom) {
    _mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

/*
 * Simplified folded block scalar content parser.
 *
 * The folded style folds single newlines between content lines to spaces, but
 * keeps blank lines as literal newlines and preserves newlines before/after
 * more-indented lines.
 *
 * This is implemented by post-processing the literal result: collect lines,
 * then fold according to the YAML 1.2 folded rules.
 */
static bool parse_folded_scalar_content(parse_ctx_t *ctx, int parent_indent,
                                        chomp_t chomp, int explicit_indent,
                                        char **out) {
  int block_indent = 0;
  bool indent_determined = (explicit_indent > 0);
  if (indent_determined) block_indent = parent_indent + explicit_indent;

  /* Collect lines as a dynamic array of {content, extra_indent, is_blank}. */
  typedef struct {
    char *text;
    int extra;
    bool blank;
  } line_t;

  size_t lines_cap = 16, lines_len = 0;
  line_t *lines = _mem_alloc(ctx->mp, lines_cap * sizeof(line_t));
  if (!lines) return false;

  while (!at_end(ctx)) {
    size_t line_start = ctx->pos;
    int spaces = 0;
    while (!at_end(ctx) && cur(ctx) == ' ') {
      spaces++;
      ctx->pos++;
    }

    bool is_blank = at_eol(ctx);

    if (!indent_determined && !is_blank) {
      if (spaces <= parent_indent) {
        ctx->pos = line_start;
        break;
      }
      block_indent = spaces;
      indent_determined = true;
    }

    if (indent_determined && !is_blank && spaces < block_indent) {
      ctx->pos = line_start;
      break;
    }

    /* Grow lines array if needed. */
    if (lines_len == lines_cap) {
      lines_cap *= 2;
      line_t *tmp = _mem_realloc(ctx->mp, lines, lines_cap * sizeof(line_t));
      if (!tmp) {
        for (size_t i = 0; i < lines_len; i++)
          _mem_free(ctx->mp, lines[i].text);
        _mem_free(ctx->mp, lines);
        return false;
      }
      lines = tmp;
    }

    line_t *ln = &lines[lines_len++];
    ln->blank = is_blank;
    ln->extra = is_blank ? 0 : (spaces - block_indent);
    ln->text = NULL;

    if (!is_blank) {
      ybuf_t lb;
      yb_init(&lb, ctx->mp);
      for (int i = 0; i < ln->extra; i++) yb_append_c(&lb, ' ');
      while (!at_end(ctx) && !at_eol(ctx)) {
        yb_append_c(&lb, cur(ctx));
        ctx->pos++;
      }
      ln->text = lb.buf;
      if (lb.oom) {
        for (size_t i = 0; i < lines_len; i++)
          _mem_free(ctx->mp, lines[i].text);
        _mem_free(ctx->mp, lines);
        return false;
      }
    }
    ctx->pos = line_start;
    skip_to_eol(ctx);
  }

  /* Now fold lines into the output buffer. */
  ybuf_t b;
  yb_init(&b, ctx->mp);

  size_t trailing_blanks = 0;
  bool have_content = false;
  bool prev_extra = false; /* was the previous non-blank line more-indented? */

  for (size_t i = 0; i < lines_len; i++) {
    line_t *ln = &lines[i];
    if (ln->blank) {
      trailing_blanks++;
      continue;
    }

    if (trailing_blanks > 0) {
      /* Blank lines before this line (leading or inter-content): emit as
       * literal newlines. */
      for (size_t j = 0; j < trailing_blanks; j++) yb_append_c(&b, '\n');
    } else if (have_content) {
      /* A line break adjacent to a more-indented line (either the current
       * line OR the previous non-blank line) is preserved as a newline per
       * YAML 1.2 spec section 8.1.1.2; otherwise it folds to a space. */
      if (ln->extra > 0 || prev_extra)
        yb_append_c(&b, '\n');
      else
        yb_append_c(&b, ' ');
    }
    trailing_blanks = 0;
    prev_extra = (ln->extra > 0);
    yb_append_cstr(&b, ln->text);
    have_content = true;
  }

  /* Free line data. */
  for (size_t i = 0; i < lines_len; i++) _mem_free(ctx->mp, lines[i].text);
  _mem_free(ctx->mp, lines);

  /* Apply chomping. */
  switch (chomp) {
    case CHOMP_STRIP:
      break;
    case CHOMP_CLIP:
      if (have_content) yb_append_c(&b, '\n');
      break;
    case CHOMP_KEEP:
      /* trailing_blanks are blank lines after the last content line.  Emit
       * them unconditionally; the +1 terminating newline is only applicable
       * when there was at least one content line to terminate. */
      for (size_t j = 0; j < trailing_blanks; j++) yb_append_c(&b, '\n');
      if (have_content) yb_append_c(&b, '\n');
      break;
  }

  if (b.oom) {
    _mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

/* ----- Plain scalar (block context, single line) ------------------------- */

/*
 * Parse a plain scalar in block context.  The scalar ends at end-of-line,
 * an inline comment (' #'), or a value indicator (': ' or ':\n').
 *
 * In flow context (in_flow=true), also ends at ',' / ']' / '}', and at ':'
 * when ':' is followed by whitespace or a flow terminator.  A bare ':'
 * not followed by one of those characters is part of the scalar (e.g. URLs
 * such as "http://example.com") per YAML 1.2 section 7.3.3.
 *
 * Multi-line plain scalars are not supported (see header for rationale).
 */
static bool parse_plain_scalar(parse_ctx_t *ctx, bool in_flow, char **out) {
  ybuf_t b;
  yb_init(&b, ctx->mp);

  size_t start = ctx->pos;

  while (!at_end(ctx)) {
    char c = cur(ctx);

    /* End-of-line terminates the plain scalar. */
    if (c == '\n' || c == '\r') break;

    /* Inline comment terminator: space followed by '#'. */
    if (c == '#' && ctx->pos > start) {
      char prev = ctx->src[ctx->pos - 1];
      if (prev == ' ' || prev == '\t') break;
    }

    /* Colon+space or colon+EOL terminates (dictionary value indicator). */
    if (c == ':') {
      if (ctx->pos + 1 >= ctx->len) break; /* colon at very end = terminator */
      char next = ctx->src[ctx->pos + 1];
      if (next == ' ' || next == '\t' || next == '\n' || next == '\r') break;
      /* In flow context ':' is also a value indicator when followed by a
       * flow terminator.  Whitespace already handled by the check above. */
      if (in_flow) {
        char safe = ctx->src[ctx->pos + 1]; /* pos+1 < len guaranteed above */
        if (safe == ',' || safe == ']' || safe == '}') break;
      }
    }

    /* Flow terminators. */
    if (in_flow && (c == ',' || c == ']' || c == '}')) break;

    yb_append_c(&b, c);
    ctx->pos++;
  }

  /* Trim trailing spaces. */
  while (b.len > 0 && (b.buf[b.len - 1] == ' ' || b.buf[b.len - 1] == '\t')) {
    b.len--;
    b.buf[b.len] = '\0';
  }

  if (b.oom) {
    _mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

/* ========================================================================== */
/*                         FORWARD DECLARATIONS                               */
/* ========================================================================== */

static cyaml_node_t *parse_node(parse_ctx_t *ctx, int indent, bool in_flow);

/* ========================================================================== */
/*                         FLOW COLLECTION PARSERS                            */
/* ========================================================================== */

/* Parse a flow list '[' elem (',' elem)* ']' and return a CYAML_LIST
 * node whose backing cvec holds cyaml_node_t * child pointers. */
static cyaml_node_t *parse_flow_list(parse_ctx_t *ctx) {
  /* consume '[' */
  ctx->pos++;
  skip_ws_comments(ctx);

  cyaml_node_t *seq = (cyaml_node_t *)cyaml_create_list_mp(ctx->mp);
  if (!seq) return NULL;

  if (!at_end(ctx) && cur(ctx) == ']') {
    ctx->pos++;
    return seq;
  }

  while (1) {
    skip_ws_comments(ctx);
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow list");
      goto fail;
    }
    if (cur(ctx) == ']') {
      ctx->pos++;
      return seq;
    }

    cyaml_node_t *elem = parse_node(ctx, -1, true);
    if (!elem) goto fail;

    cyaml_node_t *ep = elem;
    if (cvector_push_back(seq->value.list, &ep) != ccol_success) {
      __cyaml_destroy((cyaml)elem);
      goto fail;
    }

    skip_ws_comments(ctx);
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow list");
      goto fail;
    }
    if (cur(ctx) == ']') {
      ctx->pos++;
      return seq;
    }
    if (cur(ctx) != ',') {
      parse_err(ctx, "expected ',' or ']' in flow list at position %zu",
                ctx->pos);
      goto fail;
    }
    ctx->pos++;
  }

fail:
  __cyaml_destroy((cyaml)seq);
  return NULL;
}

/*
 * Parse a flow dictionary '{' key ':' value (',' key ':' value)* '}'.
 * Keys must be scalars (string, integer, null, or bool); complex keys are
 * rejected.  Integer / null / bool keys are stringified for storage in the
 * chmap so that all dictionary keys are uniformly char *.
 */
static cyaml_node_t *parse_flow_dictionary(parse_ctx_t *ctx) {
  /* consume '{' */
  ctx->pos++;
  skip_ws_comments(ctx);

  cyaml_node_t *map = (cyaml_node_t *)cyaml_create_dictionary_mp(ctx->mp);
  if (!map) return NULL;

  if (!at_end(ctx) && cur(ctx) == '}') {
    ctx->pos++;
    return map;
  }

  while (1) {
    skip_ws_comments(ctx);
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow dictionary");
      goto fail;
    }
    if (cur(ctx) == '}') {
      ctx->pos++;
      return map;
    }

    /* Parse key. */
    cyaml_node_t *key_node = parse_node(ctx, -1, true);
    if (!key_node) goto fail;

    /* Key must be a scalar for our purposes. */
    char *key_str = NULL;
    switch (key_node->type) {
      case CYAML_STRING:
        key_str = ccol_strdup(ctx->mp, key_node->value.string);
        break;
      case CYAML_INTEGER: {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%lld", key_node->value.integer);
        key_str = ccol_strdup(ctx->mp, tmp);
        break;
      }
      case CYAML_NULL:
        key_str = ccol_strdup(ctx->mp, "null");
        break;
      case CYAML_BOOL:
        key_str =
            ccol_strdup(ctx->mp, key_node->value.boolean ? "true" : "false");
        break;
      default:
        parse_err(ctx, "non-scalar key in flow dictionary at position %zu",
                  ctx->pos);
        __cyaml_destroy((cyaml)key_node);
        goto fail;
    }
    __cyaml_destroy((cyaml)key_node);
    if (!key_str) goto fail;

    skip_ws_comments(ctx);
    if (at_end(ctx) || cur(ctx) != ':') {
      parse_err(ctx, "expected ':' after flow dictionary key at position %zu",
                ctx->pos);
      _mem_free(ctx->mp, key_str);
      goto fail;
    }
    ctx->pos++;
    skip_ws_comments(ctx);

    cyaml_node_t *val = parse_node(ctx, -1, true);
    if (!val) {
      _mem_free(ctx->mp, key_str);
      goto fail;
    }

    ccol_retval_t r = cyaml_dictionary_set((cyaml)map, key_str, (cyaml)val);
    _mem_free(ctx->mp, key_str);
    if (r != ccol_success) goto fail;

    skip_ws_comments(ctx);
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow dictionary");
      goto fail;
    }
    if (cur(ctx) == '}') {
      ctx->pos++;
      return map;
    }
    if (cur(ctx) != ',') {
      parse_err(ctx, "expected ',' or '}' in flow dictionary at position %zu",
                ctx->pos);
      goto fail;
    }
    ctx->pos++;
  }

fail:
  __cyaml_destroy((cyaml)map);
  return NULL;
}

/* ========================================================================== */
/*                         BLOCK COLLECTION PARSERS                           */
/* ========================================================================== */

/*
 * Parse a block dictionary starting at the current position.
 *
 * map_indent: the column at which dictionary keys must appear.
 *
 * If first_key is non-NULL, it is the key string of the first entry already
 * parsed by the caller (used when a key was read before realising we have a
 * dictionary rather than a standalone scalar).
 */
static cyaml_node_t *parse_block_dictionary(parse_ctx_t *ctx, int map_indent,
                                            const char *first_key);

/*
 * Parse a block list starting at the current position.
 *
 * seq_indent: the column where '-' entries must appear.  The caller always
 * supplies the column of the first '-' as detected by parse_node().
 */
static cyaml_node_t *parse_block_list(parse_ctx_t *ctx, int seq_indent);

/*
 * Core node parser.  Dispatches to the appropriate sub-parser based on the
 * first significant character(s).
 *
 * indent: block collection indent level for the current container.
 *   -1 means "top level / flow context / not yet established".
 * in_flow: true when inside a flow collection ({} or []).
 */
static cyaml_node_t *parse_node(parse_ctx_t *ctx, int indent, bool in_flow) {
  if (!in_flow)
    skip_ws_comments(ctx);
  else
    skip_inline_ws(ctx);

  if (at_end(ctx)) {
    if (!in_flow) return node_alloc(CYAML_NULL, ctx->mp);
    parse_err(ctx, "unexpected end of input in flow context at position %zu",
              ctx->pos);
    return NULL;
  }

  char c = cur(ctx);

  /* --- Anchor --- */
  if (c == '&') {
    ctx->pos++;
    char *name = NULL;
    if (!parse_anchor_name(ctx, &name)) return NULL;
    skip_inline_ws(ctx);
    cyaml_node_t *n = parse_node(ctx, indent, in_flow);
    if (!n) {
      _mem_free(ctx->mp, name);
      return NULL;
    }
    cyaml_node_t *clone = (cyaml_node_t *)cyaml_clone((cyaml)n);
    if (!clone) {
      __cyaml_destroy((cyaml)n);
      _mem_free(ctx->mp, name);
      return NULL;
    }
    anchors_store(ctx, name, clone);
    _mem_free(ctx->mp, name);
    return n;
  }

  /* --- Alias --- */
  if (c == '*') {
    ctx->pos++;
    char *name = NULL;
    if (!parse_anchor_name(ctx, &name)) return NULL;
    cyaml_node_t *anchored = anchors_lookup(ctx, name);
    if (!anchored) {
      parse_err(ctx, "unknown alias '*%s' at position %zu", name, ctx->pos);
      _mem_free(ctx->mp, name);
      return NULL;
    }
    _mem_free(ctx->mp, name);
    return (cyaml_node_t *)cyaml_clone((cyaml)anchored);
  }

  /* --- Tag (ignored) --- */
  if (c == '!') {
    /* Skip the tag token. */
    ctx->pos++;
    if (!at_end(ctx) && cur(ctx) == '!') ctx->pos++;
    while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
           cur(ctx) != '\n' && cur(ctx) != '\r')
      ctx->pos++;
    skip_inline_ws(ctx);
    return parse_node(ctx, indent, in_flow);
  }

  /* --- Flow list --- */
  if (c == '[') return parse_flow_list(ctx);

  /* --- Flow dictionary --- */
  if (c == '{') return parse_flow_dictionary(ctx);

  /* --- Literal block scalar --- */
  if (c == '|' && !in_flow) {
    ctx->pos++;
    chomp_t chomp;
    int exp_indent;
    if (!parse_block_scalar_header(ctx, &chomp, &exp_indent)) return NULL;
    char *s = NULL;
    if (!parse_block_scalar_content(ctx, indent < 0 ? 0 : indent, chomp,
                                    exp_indent, &s))
      return NULL;
    cyaml_node_t *n = node_alloc(CYAML_STRING, ctx->mp);
    if (!n) {
      _mem_free(ctx->mp, s);
      return NULL;
    }
    n->value.string = s;
    return n;
  }

  /* --- Folded block scalar --- */
  if (c == '>' && !in_flow) {
    ctx->pos++;
    chomp_t chomp;
    int exp_indent;
    if (!parse_block_scalar_header(ctx, &chomp, &exp_indent)) return NULL;
    char *s = NULL;
    if (!parse_folded_scalar_content(ctx, indent < 0 ? 0 : indent, chomp,
                                     exp_indent, &s))
      return NULL;
    cyaml_node_t *n = node_alloc(CYAML_STRING, ctx->mp);
    if (!n) {
      _mem_free(ctx->mp, s);
      return NULL;
    }
    n->value.string = s;
    return n;
  }

  /* --- Block list (- item) --- */
  if (c == '-' && !in_flow) {
    if (ctx->pos + 1 < ctx->len) {
      char next = ctx->src[ctx->pos + 1];
      if (next == ' ' || next == '\t' || next == '\n' || next == '\r') {
        int col = current_col(ctx);
        return parse_block_list(ctx, col);
      }
    } else {
      /* '-' at very end of input = single-element list with null value. */
      int col = current_col(ctx);
      return parse_block_list(ctx, col);
    }
  }

  /* --- Double-quoted scalar --- */
  if (c == '"') {
    int col = current_col(ctx);
    char *s = NULL;
    if (!parse_double_quoted(ctx, &s)) return NULL;
    /* In block context a quoted scalar may be a dictionary key. */
    if (!in_flow) {
      skip_inline_ws(ctx);
      if (!at_end(ctx) && cur(ctx) == ':') {
        char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
        if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
            ctx->pos + 1 >= ctx->len) {
          cyaml_node_t *map = parse_block_dictionary(ctx, col, s);
          _mem_free(ctx->mp, s);
          return map;
        }
      }
    }
    cyaml_node_t *n = node_alloc(CYAML_STRING, ctx->mp);
    if (!n) {
      _mem_free(ctx->mp, s);
      return NULL;
    }
    n->value.string = s;
    /* A quoted scalar is always a string (no implicit typing). */
    return n;
  }

  /* --- Single-quoted scalar --- */
  if (c == '\'') {
    int col = current_col(ctx);
    char *s = NULL;
    if (!parse_single_quoted(ctx, &s)) return NULL;
    /* In block context a quoted scalar may be a dictionary key. */
    if (!in_flow) {
      skip_inline_ws(ctx);
      if (!at_end(ctx) && cur(ctx) == ':') {
        char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
        if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
            ctx->pos + 1 >= ctx->len) {
          cyaml_node_t *map = parse_block_dictionary(ctx, col, s);
          _mem_free(ctx->mp, s);
          return map;
        }
      }
    }
    cyaml_node_t *n = node_alloc(CYAML_STRING, ctx->mp);
    if (!n) {
      _mem_free(ctx->mp, s);
      return NULL;
    }
    n->value.string = s;
    return n;
  }

  /* --- Plain scalar (or block dictionary key) --- */
  {
    int col = current_col(ctx);
    char *s = NULL;
    if (!parse_plain_scalar(ctx, in_flow, &s)) return NULL;

    /* Check if a ':' value indicator follows (making this a dictionary key). */
    if (!in_flow) {
      skip_inline_ws(ctx);
      if (!at_end(ctx) && cur(ctx) == ':') {
        char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
        if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
            ctx->pos + 1 >= ctx->len) {
          /* This is a block dictionary key. */
          cyaml_node_t *map = parse_block_dictionary(ctx, col, s);
          _mem_free(ctx->mp, s);
          return map;
        }
      }
    }

    cyaml_node_t *n = make_typed_scalar(ctx, s);
    _mem_free(ctx->mp, s);
    return n;
  }
}

/* ----- Block list ---------------------------------------------------- */

static cyaml_node_t *parse_block_list(parse_ctx_t *ctx, int seq_indent) {
  cyaml_node_t *seq = (cyaml_node_t *)cyaml_create_list_mp(ctx->mp);
  if (!seq) return NULL;

  while (!at_end(ctx)) {
    /* Check we're at the right indent and the '-' indicator. */
    skip_ws_comments(ctx);
    if (at_end(ctx)) break;

    int col = current_col(ctx);
    if (col != seq_indent) break;
    if (cur(ctx) != '-') break;

    /* Is the '-' a list indicator? */
    bool is_seq_entry = false;
    if (ctx->pos + 1 < ctx->len) {
      char next = ctx->src[ctx->pos + 1];
      if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
        is_seq_entry = true;
    } else {
      is_seq_entry = true; /* '-' at EOF */
    }

    if (!is_seq_entry) break;

    ctx->pos++; /* consume '-' */

    /* Skip optional space after '-'. */
    if (!at_end(ctx) && (cur(ctx) == ' ' || cur(ctx) == '\t')) ctx->pos++;

    /* Determine where the element value is. */
    cyaml_node_t *elem;
    if (at_eol(ctx) || at_end(ctx)) {
      /* Nothing on this line after '-'; peek at the next non-empty line.
       * If it is more indented than seq_indent it is the element value;
       * otherwise (same or less indent, or EOF) the element is null. */
      skip_ws_comments(ctx);
      if (at_end(ctx) || current_col(ctx) <= seq_indent) {
        elem = node_alloc(CYAML_NULL, ctx->mp);
      } else {
        elem = parse_node(ctx, seq_indent, false);
      }
    } else {
      /* The element starts on the same line.  Pass seq_indent so block
       * scalars nested inside the element use the list's indent as
       * their parent_indent, which is what the YAML spec requires. */
      elem = parse_node(ctx, seq_indent, false);
    }

    if (!elem) goto fail;

    cyaml_node_t *ep = elem;
    if (cvector_push_back(seq->value.list, &ep) != ccol_success) {
      __cyaml_destroy((cyaml)elem);
      goto fail;
    }
  }

  return seq;

fail:
  __cyaml_destroy((cyaml)seq);
  return NULL;
}

/* ----- Block dictionary -----------------------------------------------------
 */

/*
 * Parse a block dictionary.  The first key has already been read as a plain
 * scalar and is provided in first_key.  map_indent is the column of the keys.
 *
 * This function consumes the ':' following first_key and continues parsing
 * k-v pairs until the indent changes.
 */
static cyaml_node_t *parse_block_dictionary(parse_ctx_t *ctx, int map_indent,
                                            const char *first_key) {
  cyaml_node_t *map = (cyaml_node_t *)cyaml_create_dictionary_mp(ctx->mp);
  if (!map) return NULL;

  const char *key = first_key;
  char *key_owned = NULL; /* allocated key for subsequent iterations */

  while (1) {
    /* Consume ':' indicator. */
    if (at_end(ctx) || cur(ctx) != ':') {
      parse_err(ctx, "expected ':' after dictionary key at position %zu",
                ctx->pos);
      goto fail;
    }
    ctx->pos++;

    /* Skip optional space after ':'. */
    if (!at_end(ctx) && (cur(ctx) == ' ' || cur(ctx) == '\t')) ctx->pos++;

    /* Parse value. */
    cyaml_node_t *val;
    if (at_eol(ctx) || at_end(ctx)) {
      /* Value is on the next line(s). */
      skip_ws_comments(ctx);
      if (at_end(ctx)) {
        val = node_alloc(CYAML_NULL, ctx->mp);
      } else {
        int val_col = current_col(ctx);
        if (val_col <= map_indent) {
          /* Next line is at or below dictionary indent: value is null. */
          val = node_alloc(CYAML_NULL, ctx->mp);
        } else {
          val = parse_node(ctx, map_indent, false);
        }
      }
    } else {
      /* Value starts on the same line as the key.  Use map_indent as
       * parent_indent so that block scalars (|, >) can detect their
       * content lines, which are always indented relative to the
       * containing dictionary, not relative to the scalar indicator itself. */
      val = parse_node(ctx, map_indent, false);
    }

    if (!val) {
      /* val is NULL either because parse_node already set ctx->error, or
       * because node_alloc returned NULL (OOM).  Set a diagnostic when no
       * other message is present so the caller always gets a useful string. */
      if (!ctx->error[0])
        parse_err(ctx, "out of memory parsing dictionary value at position %zu",
                  ctx->pos);
      goto fail;
    }

    if (cyaml_dictionary_set((cyaml)map, key, (cyaml)val) != ccol_success) {
      goto fail;
    }
    _mem_free(ctx->mp, key_owned);
    key_owned = NULL;

    /* Look for the next key at the same indent. */
    skip_ws_comments(ctx);
    if (at_end(ctx)) break;
    if (at_doc_marker(ctx)) break;

    int next_col = current_col(ctx);
    if (next_col != map_indent) break;

    /* Peek: is this a plain, double-quoted, or single-quoted scalar key? */
    char c = cur(ctx);
    /* Indicator characters that cannot start a scalar dictionary key. */
    /* '-' is only a list indicator when followed by whitespace or EOF;
     * otherwise it may legitimately start a plain scalar key (e.g. -key:). */
    if (c == '-') {
      char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
      if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
        break;
    } else if (c == '[' || c == '{' || c == '|' || c == '>' || c == '&' ||
               c == '*' || c == '!' || c == '#') {
      break;
    }

    /* Try to read the next key (plain, double-quoted, or single-quoted). */
    char *next_key = NULL;
    size_t saved_pos = ctx->pos;
    if (c == '"') {
      if (!parse_double_quoted(ctx, &next_key)) goto fail;
    } else if (c == '\'') {
      if (!parse_single_quoted(ctx, &next_key)) goto fail;
    } else {
      if (!parse_plain_scalar(ctx, false, &next_key)) goto fail;
    }

    /* Check for ':' indicator after the key. */
    skip_inline_ws(ctx);
    if (at_end(ctx) || cur(ctx) != ':') {
      /* Not a dictionary key: put back and stop. */
      ctx->pos = saved_pos;
      _mem_free(ctx->mp, next_key);
      break;
    }
    char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
    if (after != ' ' && after != '\t' && after != '\n' && after != '\r' &&
        after != '\0') {
      ctx->pos = saved_pos;
      _mem_free(ctx->mp, next_key);
      break;
    }

    key_owned = next_key;
    key = key_owned;
  }

  _mem_free(ctx->mp, key_owned);
  return map;

fail:
  _mem_free(ctx->mp, key_owned);
  __cyaml_destroy((cyaml)map);
  return NULL;
}

/* ========================================================================== */
/*                         PUBLIC PARSE ENTRY                                 */
/* ========================================================================== */

/*
 * Parse a single YAML document from the current context position.
 *
 * Consumes an optional leading '---' document-start marker (and any preceding
 * '%YAML' / '%TAG' directives, which are silently ignored).  Parses one root
 * node, resets the per-document anchor table, and consumes an optional trailing
 * '...' document-end marker.
 *
 * On success returns the root node with ctx->pos advanced past the consumed
 * document (including any trailing '...').  The position is left just before
 * any leading '---' of the NEXT document so the caller can loop.
 *
 * Returns NULL on parse error; ctx->error is set in that case.  The anchor
 * table is always destroyed before returning.
 */
static cyaml_node_t *parse_one_document(parse_ctx_t *ctx) {
  /* Accept optional leading document-start marker.  Per YAML 1.2 the '---'
   * token is only a document-start indicator when its three dashes are
   * followed by whitespace, a comment, or EOF; '---X' where X is any other
   * character is a plain scalar, not a marker. */
  if (at_doc_marker(ctx) && memcmp(ctx->src + ctx->pos, "---", 3) == 0) {
    ctx->pos += 3;
    skip_inline_ws(ctx);
    if (!at_end(ctx) && at_eol(ctx)) skip_newline(ctx);
    skip_ws_comments(ctx);
  }

  /* Accept %YAML / %TAG directives silently. */
  while (!at_end(ctx) && cur(ctx) == '%') {
    skip_to_eol(ctx);
    skip_ws_comments(ctx);
    if (at_doc_marker(ctx) && memcmp(ctx->src + ctx->pos, "---", 3) == 0) {
      ctx->pos += 3;
      skip_inline_ws(ctx);
      if (!at_end(ctx) && at_eol(ctx)) skip_newline(ctx);
      skip_ws_comments(ctx);
      break;
    }
  }

  /* Empty document: at EOF or immediately at another document boundary. */
  cyaml_node_t *root;
  if (at_end(ctx) || at_doc_marker(ctx)) {
    root = node_alloc(CYAML_NULL, ctx->mp);
  } else {
    root = parse_node(ctx, 0, false);
  }

  /* Anchor table is scoped to one document; reset it before returning. */
  anchors_destroy(ctx);

  if (!root) return NULL;

  /* Consume optional trailing '...' document-end marker.  at_doc_marker()
   * enforces column 0 so an indented '...' sequence is not silently swallowed
   * as a document boundary (it would then be reported as trailing content by
   * the outer parse_common loop). */
  skip_ws_comments(ctx);
  if (at_doc_marker(ctx) && memcmp(ctx->src + ctx->pos, "...", 3) == 0) {
    ctx->pos += 3;
    skip_inline_ws(ctx);
    if (!at_end(ctx) && at_eol(ctx)) skip_newline(ctx);
  }

  return root;
}

/*
 * Shared YAML parse entry point.
 *
 * Handles an optional UTF-8 BOM (EF BB BF) then loops over all documents in
 * the stream, each delimited by optional '---' / '...' markers.  The anchor
 * table is reset between documents so anchors do not leak across boundaries.
 *
 * Single-document streams: the root node is returned directly (same type as
 * before, fully backwards-compatible).
 *
 * Multi-document streams: all document roots are collected and returned as a
 * single CYAML_LIST node whose elements are the individual document roots, in
 * order.  Subsequent documents must begin with a '---' marker; bare content
 * after a document end is treated as a parse error.
 *
 * On failure *err_str (if non-NULL) is set to a heap-allocated message the
 * caller must free.  Any successfully parsed documents are destroyed before
 * returning NULL.
 */
static cyaml parse_common(const char *src, size_t len, char **err_str,
                          ccol_memmgmt_procs_t *mp) {
  if (!src) {
    if (err_str) *err_str = strdup("null input");
    return NULL;
  }
  parse_ctx_t ctx = {
      .src = src, .pos = 0, .len = len, .error = "", .mp = mp, .anchors = NULL};

  /* Skip optional BOM. */
  if (len >= 3 && (unsigned char)src[0] == 0xEF &&
      (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF)
    ctx.pos = 3;

  skip_ws_comments(&ctx);

  /* Temporary array that accumulates parsed document roots. */
  cyaml_node_t **docs = NULL;
  size_t ndocs = 0;
  size_t dcap = 0;

  while (!at_end(&ctx)) {
    /* After the first document, the next document MUST start with '---'.
     * Bare content here would be ambiguous trailing material. */
    if (ndocs > 0) {
      if (!at_doc_marker(&ctx) || memcmp(ctx.src + ctx.pos, "---", 3) != 0) {
        char buf[80];
        snprintf(buf, sizeof(buf), "trailing content at position %zu", ctx.pos);
        if (err_str) *err_str = strdup(buf);
        goto fail;
      }
    }

    cyaml_node_t *root = parse_one_document(&ctx);
    if (!root) {
      if (err_str) {
        const char *msg = ctx.error[0] ? ctx.error : "unknown parse error";
        *err_str = strdup(msg);
      }
      goto fail;
    }

    /* Grow the docs array if needed. */
    if (ndocs == dcap) {
      size_t new_cap = dcap ? dcap * 2 : 4;
      cyaml_node_t **nd =
          _mem_realloc(mp, docs, new_cap * sizeof(cyaml_node_t *));
      if (!nd) {
        __cyaml_destroy((cyaml)root);
        if (err_str) *err_str = strdup("out of memory");
        goto fail;
      }
      docs = nd;
      dcap = new_cap;
    }
    docs[ndocs++] = root;

    skip_ws_comments(&ctx);
  }

  /* Empty input: return a null node, matching the behaviour of parsing an
   * empty plain scalar (YAML 1.2 core schema: empty value resolves to null). */
  if (ndocs == 0) {
    _mem_free(mp, docs);
    if (err_str) *err_str = NULL;
    return (cyaml)node_alloc(CYAML_NULL, mp);
  }

  /* Single document: return as-is (backwards compatible). */
  if (ndocs == 1) {
    cyaml_node_t *result = docs[0];
    _mem_free(mp, docs);
    if (err_str) *err_str = NULL;
    return (cyaml)result;
  }

  /* Multiple documents: wrap in a CYAML_LIST. */
  cyaml_node_t *list = (cyaml_node_t *)cyaml_create_list_mp(mp);
  if (!list) {
    if (err_str) *err_str = strdup("out of memory");
    goto fail;
  }
  for (size_t i = 0; i < ndocs; i++) {
    cyaml_node_t *elem = docs[i];
    if (cvector_push_back(list->value.list, &elem) != ccol_success) {
      /* Destroy remaining docs not yet adopted by the list. */
      for (size_t j = i; j < ndocs; j++) __cyaml_destroy((cyaml)docs[j]);
      _mem_free(mp, docs);
      __cyaml_destroy((cyaml)list);
      if (err_str) *err_str = strdup("out of memory");
      return NULL;
    }
  }
  _mem_free(mp, docs);
  if (err_str) *err_str = NULL;
  return (cyaml)list;

fail:
  for (size_t i = 0; i < ndocs; i++) __cyaml_destroy((cyaml)docs[i]);
  _mem_free(mp, docs);
  return NULL;
}

/* Parse a null-terminated YAML string.  mp may be NULL for the default
 * allocator.  On failure *err_str (if non-NULL) receives a heap-allocated
 * error message the caller must free with free(). */
cyaml cyaml_parse_mp(const char *yaml_str, char **err_str,
                     ccol_memmgmt_procs_t *mp) {
  if (!yaml_str) return parse_common(NULL, 0, err_str, mp);
  return parse_common(yaml_str, strlen(yaml_str), err_str, mp);
}

/* Like cyaml_parse_mp but accepts an explicit byte length so the input need
 * not be null-terminated. */
cyaml cyaml_parse_n_mp(const char *yaml_str, size_t len, char **err_str,
                       ccol_memmgmt_procs_t *mp) {
  return parse_common(yaml_str, len, err_str, mp);
}

/* ========================================================================== */
/*                         SERIALIZER HELPERS                                 */
/* ========================================================================== */

/*
 * Returns true if the string s can be emitted as a plain YAML scalar without
 * quoting: it does not look like null/bool/number, does not contain
 * indicator characters, and is not empty.
 */
static bool needs_quoting(const char *s) {
  if (!s || s[0] == '\0') return true;

  /* Strings that would be misinterpreted as other types. */
  if (strcmp(s, "~") == 0 || strcmp(s, "null") == 0 || strcmp(s, "Null") == 0 ||
      strcmp(s, "NULL") == 0 || strcmp(s, "true") == 0 ||
      strcmp(s, "True") == 0 || strcmp(s, "TRUE") == 0 ||
      strcmp(s, "false") == 0 || strcmp(s, "False") == 0 ||
      strcmp(s, "FALSE") == 0 || strcmp(s, ".inf") == 0 ||
      strcmp(s, ".Inf") == 0 || strcmp(s, ".INF") == 0 ||
      strcmp(s, "+.inf") == 0 || strcmp(s, "+.Inf") == 0 ||
      strcmp(s, "+.INF") == 0 || strcmp(s, "-.inf") == 0 ||
      strcmp(s, "-.Inf") == 0 || strcmp(s, "-.INF") == 0 ||
      strcmp(s, ".nan") == 0 || strcmp(s, ".NaN") == 0 ||
      strcmp(s, ".NAN") == 0)
    return true;

  /* Check for content that needs quoting. */
  const char *p = s;

  /* First character indicators. */
  char fc = *p;
  if (fc == '-' || fc == '?' || fc == ':' || fc == ',' || fc == '[' ||
      fc == ']' || fc == '{' || fc == '}' || fc == '#' || fc == '&' ||
      fc == '*' || fc == '!' || fc == '|' || fc == '>' || fc == '\'' ||
      fc == '"' || fc == '%' || fc == '@' || fc == '`' || fc == ' ' ||
      fc == '\t')
    return true;

  /* If first char is a digit or numeric sign, might parse as a number.
   * '+' only needs quoting when followed by a digit (e.g. +42, +100).
   * '+.inf', '+.Inf', '+.INF' are already caught by the explicit strcmp block
   * above; other '+.' strings like '+.foo' are valid plain scalars.
   * '-' is already caught by the indicator-character check above. */
  if (fc >= '0' && fc <= '9') return true;
  if (fc == '+' && (s[1] >= '0' && s[1] <= '9')) return true;
  /* "+.N" patterns (e.g. "+.3", "+.5e-10") are parsed as floats by strtod but
   * are not caught by the digit or the "+.inf" checks above. */
  if (fc == '+' && s[1] == '.') {
    char *endp = NULL;
    errno = 0;
    strtod(s, &endp);
    if (endp != s && *endp == '\0' && errno != ERANGE) return true;
  }
  if (fc == '.') return true;

  while (*p) {
    char c = *p;
    if (c == '\n' || c == '\r') return true; /* multiline -> block scalar */
    /* These characters terminate a plain scalar in flow context, so any string
     * containing them must be quoted to survive a flow round-trip. */
    if (c == ',' || c == ']' || c == '}') return true;
    if (c == ':') {
      char nx = *(p + 1);
      if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0' ||
          nx == ',' || nx == ']' || nx == '}')
        return true;
    }
    if ((c == ' ' || c == '\t') && *(p + 1) == '#') return true;
    p++;
  }
  /* Trailing spaces/tabs are stripped by the plain scalar parser. */
  if (p > s && (*(p - 1) == ' ' || *(p - 1) == '\t')) return true;
  return false;
}

/* Emit a double-quoted YAML string with necessary escapes. */
static void yb_append_yaml_dquoted(ybuf_t *b, const char *s) {
  yb_append_c(b, '"');
  if (!s) {
    yb_append_c(b, '"');
    return;
  }
  const char *p = s;
  while (*p) {
    unsigned char c = (unsigned char)*p;
    switch (c) {
      case '"':
        yb_append(b, "\\\"", 2);
        break;
      case '\\':
        yb_append(b, "\\\\", 2);
        break;
      case '\n':
        yb_append(b, "\\n", 2);
        break;
      case '\r':
        yb_append(b, "\\r", 2);
        break;
      case '\t':
        yb_append(b, "\\t", 2);
        break;
      case '\b':
        yb_append(b, "\\b", 2);
        break;
      case '\f':
        yb_append(b, "\\f", 2);
        break;
      default:
        if (c < 0x20) {
          char esc[7];
          snprintf(esc, sizeof(esc), "\\x%02x", c);
          yb_append_cstr(b, esc);
        } else {
          yb_append_c(b, (char)c);
        }
        break;
    }
    p++;
  }
  yb_append_c(b, '"');
}

/* Indent helper: emit N*depth spaces. */
static void yb_indent(ybuf_t *b, int depth) {
  for (int i = 0; i < depth * 2; i++) yb_append_c(b, ' ');
}

/* Emit a double in YAML notation (.nan, .inf, or shortest round-trip decimal).
 */
static void yb_append_double(ybuf_t *b, double v) {
  if (__builtin_isnan(v)) {
    yb_append_cstr(b, ".nan");
    return;
  }
  if (__builtin_isinf(v)) {
    yb_append_cstr(b, v > 0 ? ".inf" : "-.inf");
    return;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%.15g", v);
  if (strtod(buf, NULL) != v) snprintf(buf, sizeof(buf), "%.17g", v);
  /* Ensure it parses as float, not integer. */
  if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
    size_t len = strlen(buf);
    if (len + 2 < sizeof(buf)) {
      buf[len] = '.';
      buf[len + 1] = '0';
      buf[len + 2] = '\0';
    }
  }
  yb_append_cstr(b, buf);
}

/* Integer to decimal without snprintf. */
static int _lltoa_y(char *buf, long long v) {
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

/* ========================================================================== */
/*                         BLOCK SERIALIZER                                   */
/* ========================================================================== */

static void serialize_block(ybuf_t *b, cyaml_node_t *n, int depth);

/* Emit a scalar string in block context.  Strings that would be misread as
 * a different YAML type or that contain indicator characters are double-quoted;
 * all others are emitted as plain scalars. */
static void serialize_block_scalar(ybuf_t *b, cyaml_node_t *n) {
  const char *s = n->value.string;
  if (needs_quoting(s)) {
    yb_append_yaml_dquoted(b, s);
  } else {
    yb_append_cstr(b, s);
  }
}

/*
 * Recursively emit block-style YAML for the subtree rooted at n.
 * depth controls indentation (each level adds 2 spaces).
 * Mapping and list children are put on separate indented lines.
 * A NULL node pointer is emitted as the YAML null literal '~'.
 */
static void serialize_block(ybuf_t *b, cyaml_node_t *n, int depth) {
  if (!n) {
    yb_append_cstr(b, "~");
    return;
  }

  switch (n->type) {
    case CYAML_NULL:
      yb_append_cstr(b, "~");
      break;
    case CYAML_BOOL:
      yb_append_cstr(b, n->value.boolean ? "true" : "false");
      break;
    case CYAML_INTEGER: {
      char buf[24];
      int len = _lltoa_y(buf, n->value.integer);
      yb_append(b, buf, (size_t)len);
      break;
    }
    case CYAML_FLOAT:
      yb_append_double(b, n->value.number);
      break;
    case CYAML_STRING:
      serialize_block_scalar(b, n);
      break;
    case CYAML_LIST: {
      size_t cnt = cvector_elem_count(n->value.list);
      if (cnt == 0) {
        yb_indent(b, depth);
        yb_append_cstr(b, "[]\n");
        break;
      }
      for (size_t i = 0; i < cnt; i++) {
        yb_indent(b, depth);
        cyaml_node_t *child = *(cyaml_node_t **)cvector_at(n->value.list, i);
        /* If child is a dictionary or list, put it on the next line.
         * Emit just "-" (no trailing space) to avoid a whitespace-only line. */
        if (child &&
            (child->type == CYAML_DICTIONARY || child->type == CYAML_LIST)) {
          yb_append_cstr(b, "-\n");
          serialize_block(b, child, depth + 1);
        } else {
          yb_append_cstr(b, "- ");
          serialize_block(b, child, depth);
          yb_append_c(b, '\n');
        }
      }
      break;
    }
    case CYAML_DICTIONARY: {
      size_t cnt = chmap_elem_count(n->value.dictionary);
      if (cnt == 0) {
        yb_indent(b, depth);
        yb_append_cstr(b, "{}\n");
        break;
      }
      cmap_iterator *it = chashmap_begin_iter(n->value.dictionary, NULL);
      while (it) {
        const char *key = (const char *)it->key_pair->ptr;
        cyaml_node_t *child = _cyaml_read_child(it->val_pair->ptr);

        yb_indent(b, depth);
        if (needs_quoting(key))
          yb_append_yaml_dquoted(b, key);
        else
          yb_append_cstr(b, key);
        yb_append_c(b, ':');

        if (child &&
            (child->type == CYAML_DICTIONARY || child->type == CYAML_LIST)) {
          yb_append_c(b, '\n');
          serialize_block(b, child, depth + 1);
        } else {
          yb_append_c(b, ' ');
          serialize_block(b, child, depth);
          yb_append_c(b, '\n');
        }
        it = it->_next_fn(it);
      }
      break;
    }
  }
}

/* Serialize node to human-readable block YAML.  The returned string is
 * heap-allocated and guaranteed to end with a newline.  Must be freed with
 * cyaml_serialize_free() or cyaml_serialize_free_mp().  Returns NULL on OOM. */
char *cyaml_serialize(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  ybuf_t b;
  yb_init(&b, mp);
  serialize_block(&b, n, 0);
  /* Ensure output ends with a newline. */
  if (b.len > 0 && b.buf[b.len - 1] != '\n') yb_append_c(&b, '\n');
  if (b.oom) {
    _mem_free(b.m_procs, b.buf);
    return NULL;
  }
  return b.buf;
}

/* ========================================================================== */
/*                         FLOW SERIALIZER                                    */
/* ========================================================================== */

/*
 * Recursively emit compact single-line flow YAML for the subtree rooted at n.
 * Lists are enclosed in '[ ]', dictionaries in '{ }'.  Null is emitted as
 * '~'. String quoting follows the same needs_quoting() rules as block output so
 * the result is always a valid parseable YAML value.
 */
static void serialize_flow(ybuf_t *b, cyaml_node_t *n) {
  if (!n) {
    yb_append_cstr(b, "~");
    return;
  }
  switch (n->type) {
    case CYAML_NULL:
      yb_append_cstr(b, "~");
      break;
    case CYAML_BOOL:
      yb_append_cstr(b, n->value.boolean ? "true" : "false");
      break;
    case CYAML_INTEGER: {
      char buf[24];
      int len = _lltoa_y(buf, n->value.integer);
      yb_append(b, buf, (size_t)len);
      break;
    }
    case CYAML_FLOAT:
      yb_append_double(b, n->value.number);
      break;
    case CYAML_STRING:
      if (needs_quoting(n->value.string))
        yb_append_yaml_dquoted(b, n->value.string);
      else
        yb_append_cstr(b, n->value.string);
      break;
    case CYAML_LIST: {
      yb_append_c(b, '[');
      size_t cnt = cvector_elem_count(n->value.list);
      for (size_t i = 0; i < cnt; i++) {
        if (i > 0) yb_append_cstr(b, ", ");
        cyaml_node_t *child = *(cyaml_node_t **)cvector_at(n->value.list, i);
        serialize_flow(b, child);
      }
      yb_append_c(b, ']');
      break;
    }
    case CYAML_DICTIONARY: {
      yb_append_c(b, '{');
      size_t idx = 0;
      cmap_iterator *it = chashmap_begin_iter(n->value.dictionary, NULL);
      while (it) {
        if (idx > 0) yb_append_cstr(b, ", ");
        const char *key = (const char *)it->key_pair->ptr;
        if (needs_quoting(key))
          yb_append_yaml_dquoted(b, key);
        else
          yb_append_cstr(b, key);
        yb_append_cstr(b, ": ");
        cyaml_node_t *child = _cyaml_read_child(it->val_pair->ptr);
        serialize_flow(b, child);
        idx++;
        it = it->_next_fn(it);
      }
      yb_append_c(b, '}');
      break;
    }
  }
}

/* Serialize node to compact single-line flow YAML.  Returns a heap-allocated
 * string that must be freed with cyaml_serialize_free() or
 * cyaml_serialize_free_mp().  Returns NULL on OOM. */
char *cyaml_serialize_flow(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  ybuf_t b;
  yb_init(&b, mp);
  serialize_flow(&b, n);
  if (b.oom) {
    _mem_free(b.m_procs, b.buf);
    return NULL;
  }
  return b.buf;
}

/* Release a string returned by cyaml_serialize or cyaml_serialize_flow using
 * the matching allocator (mp == NULL for the default allocator). */
void cyaml_serialize_free_mp(char *s, ccol_memmgmt_procs_t *mp) {
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
 * Walk a dot-separated path through a YAML tree, returning the node at the
 * end of the path or NULL if any component is not found.
 *
 * path_copy must be writable; unescaped dots are temporarily replaced with
 * '\0' to carve out each component in-place, then the component is unescaped
 * before use.
 *
 * List elements are addressed with a '#' prefix: "items.#0.name"
 * navigates to the 'name' key of the first element of 'items'.
 */
static cyaml navigate_y(cyaml root, char *path_copy) {
  cyaml node = root;
  char *p = path_copy;

  while (node) {
    char *dot = path_find_unescaped_dot(p);
    if (dot) *dot = '\0';

    if (p[0] == '\0') {
      node = NULL;
      break;
    }

    path_unescape_component(p);

    cyaml_node_t *n = (cyaml_node_t *)node;

    if (n->type == CYAML_LIST && p[0] == '#') {
      char *endp;
      errno = 0;
      long idx = strtol(p + 1, &endp, 10);
      if (endp == p + 1 || *endp != '\0' || idx < 0 || errno == ERANGE) {
        node = NULL;
        break;
      }
      void *slot = cvector_at(n->value.list, (size_t)idx);
      node = slot ? *(cyaml *)slot : NULL;
    } else if (n->type == CYAML_DICTIONARY) {
      cmap_pair kp = {.ptr = p, .size = strlen(p) + 1};
      cmap_pair *vp = NULL;
      if (chmap_get_elem_ref(n->value.dictionary, &kp, &vp) != ccol_success)
        node = NULL;
      else
        node = _cyaml_read_child(vp->ptr);
    } else {
      node = NULL;
    }

    if (!dot) break;
    p = dot + 1;
  }
  return node;
}

/* Public implementation of the cyaml_get(root, path) macro.
 * Duplicates path before passing it to navigate_y() so the caller's string
 * is never modified. */
cyaml _cyaml_get(cyaml root, const char *path) {
  if (!root) return NULL;
  if (!path || path[0] == '\0') return root;
  ccol_memmgmt_procs_t *mp = ((cyaml_node_t *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return NULL;
  cyaml result = navigate_y(root, copy);
  _mem_free(mp, copy);
  return result;
}

/*
 * Public implementation of the cyaml_set(root, path, value) macro.
 *
 * Splits the path on the LAST dot to separate the parent path from the leaf
 * key.  The leaf is strdup'd before the parent-path copy is freed to prevent
 * use-after-free when the leaf string is a suffix of the full path.
 *
 * String normalisation: raw_is_char_array handles string literals where
 * typeof("hi") == char[N] -- the raw pointer is normalised to const char **
 * so node_reinit_scalar always receives one consistent form.
 *
 * Existing scalar nodes at the leaf are mutated in-place (node_reinit_scalar).
 * New keys are allocated and inserted.  Intermediate containers are never
 * created automatically -- the parent node must already exist.
 */
ccol_retval_t _cyaml_set_typed(cyaml root, const char *path,
                               cyaml_node_type_t type, void *raw,
                               size_t raw_size, bool is_signed,
                               bool raw_is_char_array) {
  if (!root || !path || path[0] == '\0') return ccol_invalid_args;

  const char *_cyaml_str_norm;
  if (raw_is_char_array) {
    _cyaml_str_norm = (const char *)raw;
    raw = (void *)&_cyaml_str_norm;
    raw_size = sizeof(_cyaml_str_norm);
    type = CYAML_STRING;
  }

  ccol_memmgmt_procs_t *mp = ((cyaml_node_t *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return ccol_not_enough_memory;

  char *last_dot = path_find_last_unescaped_dot(copy);
  char *leaf_copy;
  cyaml parent;

  if (!last_dot) {
    leaf_copy = ccol_strdup(mp, path);
    parent = root;
    _mem_free(mp, copy);
  } else {
    *last_dot = '\0';
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    parent = navigate_y(root, copy);
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

  cyaml_node_t *pn = (cyaml_node_t *)parent;
  ccol_retval_t ret;

  if (pn->type == CYAML_DICTIONARY) {
    cmap_pair kp = {.ptr = (void *)leaf_comp, .size = strlen(leaf_comp) + 1};
    cmap_pair *existing_vp = NULL;
    if (chmap_get_elem_ref(pn->value.dictionary, &kp, &existing_vp) ==
        ccol_success) {
      cyaml_node_t *existing = _cyaml_read_child(existing_vp->ptr);
      ret = node_reinit_scalar(existing, type, raw, raw_size, is_signed)
                ? ccol_success
                : ccol_not_enough_memory;
    } else {
      cyaml_node_t *new_node =
          node_make_scalar(type, raw, raw_size, is_signed, pn->m_procs);
      if (!new_node) {
        _mem_free(mp, leaf_copy);
        return ccol_not_enough_memory;
      }
      cmap_pair vp = {.ptr = &new_node, .size = sizeof(new_node)};
      ccol_retval_t r = chmap_insert_elem(pn->value.dictionary, &kp, &vp);
      if (r != ccol_success && r != ccol_key_already_present) {
        __cyaml_destroy((cyaml)new_node);
        ret = r;
      } else {
        ret = ccol_success;
      }
    }
  } else if (pn->type == CYAML_LIST) {
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
          cyaml_node_t *existing = *(cyaml_node_t **)slot;
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

/*
 * Public implementation of the cyaml_delete(root, path) macro.
 *
 * Splits the path on the LAST unescaped dot to identify the parent node and
 * the leaf key.  When no dot is present, root is the parent.  The leaf key
 * is unescaped before use so '\.' and '\\' work identically to cyaml_get and
 * cyaml_set.
 *
 * Removes and deep-frees the addressed node.  Returns:
 *   ccol_success          - node removed and freed.
 *   ccol_invalid_args     - NULL root/path, empty path, or index out of range.
 *   ccol_key_not_found    - parent exists but leaf key / index is absent.
 *   ccol_not_enough_memory - strdup failed.
 */
ccol_retval_t _cyaml_delete(cyaml root, const char *path) {
  if (!root || !path || path[0] == '\0') return ccol_invalid_args;

  ccol_memmgmt_procs_t *mp = ((cyaml_node_t *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return ccol_not_enough_memory;

  char *last_dot = path_find_last_unescaped_dot(copy);
  char *leaf_copy;
  cyaml parent;

  if (!last_dot) {
    leaf_copy = ccol_strdup(mp, path);
    parent = root;
    _mem_free(mp, copy);
  } else {
    *last_dot = '\0';
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    parent = navigate_y(root, copy);
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

  cyaml_node_t *pn = (cyaml_node_t *)parent;
  ccol_retval_t ret;

  if (pn->type == CYAML_DICTIONARY) {
    ret = cyaml_dictionary_remove(parent, leaf_comp);
  } else if (pn->type == CYAML_LIST) {
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
        ret = cyaml_list_remove(parent, (size_t)idx);
      }
    }
  } else {
    ret = ccol_invalid_args;
  }

  _mem_free(mp, leaf_copy);
  return ret;
}
