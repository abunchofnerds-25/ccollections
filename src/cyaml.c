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
#include <stdarg.h>
#include <stdatomic.h>

/* ========================================================================== */
/*                         INTERNAL DOM NODE                                  */
/* ========================================================================== */

/*
 * Full definition of the DOM node.  Unlike cjson's tagged_node_t, this struct
 * is also the public handle type (cyaml == cyaml_node_t *) so it is not
 * truly opaque; but callers must never access fields directly; they must
 * use the public API.
 *
 * Layout:
 *  type     : one of the seven cyaml_node_type_t values.
 *  m_procs  : allocator for this node and its owned strings; NULL = default.
 *  tag      : owned, fully-resolved YAML tag string (e.g.
 *             "tag:yaml.org,2002:str", or a custom verbatim/shorthand-
 *             resolved tag), or NULL if this node carries no tag.  Freed
 *             only by __cyaml_destroy, never by node_clear (see that
 *             function's own doc comment for why the two must not be
 *             conflated).
 *  value    : union of scalar and composite payloads.
 *    list : cvec of cyaml_node_t *   (CYAML_LIST)
 *    dictionary  : chmap char*->cyaml_node_t* (CYAML_DICTIONARY)
 */
typedef struct cyaml_node_t {
  cyaml_node_type_t type;
  ccol_memmgmt_procs_t *m_procs;
  char *tag;
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

/* Dynamic string buffer for YAML serialization, backed by common.h's
 * ccol_growbuf_t (the same growable-byte-buffer type cjson.c's own sbuf_t
 * uses); the yb_* names are kept as thin forwarding wrappers so every
 * existing call site in this file needs no change. Once oom is set all
 * yb_* operations become safe no-ops. */
typedef ccol_growbuf_t ybuf_t;

static inline void yb_init(ybuf_t *b, ccol_memmgmt_procs_t *mp) {
  ccol_growbuf_init(b, mp);
}

static inline void yb_append(ybuf_t *b, const char *data, size_t n) {
  ccol_growbuf_append(b, data, n);
}

static inline void yb_append_c(ybuf_t *b, char c) {
  ccol_growbuf_append_c(b, c);
}

static inline void yb_append_cstr(ybuf_t *b, const char *s) {
  ccol_growbuf_append_cstr(b, s);
}

/* ========================================================================== */
/*                         INTERNAL HELPERS                                   */
/* ========================================================================== */

/*
 * chmap_entry's SSO storage is naturally aligned, so this memcpy is
 * defense-in-depth rather than a live alignment requirement; kept for
 * consistency with cjson's own identical pattern.
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
static thread_ls_key_t _pool_key;

/* Serializes "use the still-live key" (node_free's own read side, taken as a
 * reader) against "flip _pool_key_live false and delete the key" (the DSO
 * destructor below, taken as the sole writer). A bare atomic_load-then-act
 * on _pool_key_live is not sufficient on its own: the destructor can run
 * between node_free's load and its own thread_ls_set call, deleting the key
 * out from under a pthread_setspecific() already in flight (UB per POSIX).
 * Holding this lock across the load-and-act keeps the two mutually
 * exclusive, so thread_ls_set either fully completes before the key is
 * deleted or never runs at all. Lazily initialized inside
 * _do_pool_key_init() (guarded by the same _pool_key_once below that already
 * gates every other part of this subsystem's one-time setup), per this
 * codebase's own standing rule against static/constant lock initializers. */
static rw_lock_t _pool_key_rwlock;

static void _pool_drain(void *);

/*
 * Thread-local remaining node-allocation budget for the top-level
 * parse_common() call currently in progress on this thread. (size_t)-1
 * means "no parse in progress on this thread right now" (the default, and
 * the value node_alloc() treats as unlimited); see CYAML_MAX_PARSE_NODES's
 * own doc comment (further below, near parse_ctx_t) for why this exists.
 * parse_node_budget_arm()/_disarm() (defined alongside that macro) are the
 * only writers; parse_common() calls _arm() once at entry and _disarm() on
 * every one of its own exit paths, so a decremented value never leaks into
 * unrelated node creation (a plain cyaml_create_list_mp() call, or a later,
 * separate parse on the same thread) once the parse that armed it returns.
 */
static __thread size_t _parse_node_budget = (size_t)-1;

/* Set by node_alloc() the moment it refuses an allocation specifically
 * because _parse_node_budget hit zero (as opposed to a real allocator OOM),
 * so the anchor-registration and alias-resolution cloning call sites (the
 * ones this budget primarily exists to bound) can report a specific,
 * actionable diagnostic instead of the generic "out of memory" message
 * node_alloc's own NULL return would otherwise fall back to. */
static __thread bool _parse_node_budget_exhausted = false;

/* Lazy key init: runs exactly once on the first node_alloc call.
 * _pool_key_live gates both pthread_setspecific (in node_free) and the
 * destructor below, so processes that never call any cyaml function pay
 * zero cost. It is also the sole signal of whether this one-time init
 * actually succeeded: it is left false (its static initial value) if either
 * pthread call below fails (e.g. genuine resource exhaustion, such as
 * PTHREAD_KEYS_MAX already reached process-wide), so a caller must never
 * assume the rwlock/key are valid to use without checking it first; see
 * node_free's own cold path for the corresponding read side. */
static once_flag_t _pool_key_once = ONCE_INIT;

static void _do_pool_key_init(void) {
  if (rw_lock_init(_pool_key_rwlock) != 0) return;
  if (thread_ls_key_create(_pool_key, _pool_drain) != 0) return;
  atomic_store(&_pool_key_live, true);
}

/* DSO destructor: drain the calling thread's own pool, mark the key dead so
 * concurrent threads skip the pthread_setspecific call, then delete the key
 * to avoid PTHREAD_KEYS_MAX exhaustion on repeated dlopen/dlclose cycles.
 * The _pool_key_live guard makes this a no-op if no cyaml function was ever
 * called (i.e. _do_pool_key_init never ran, so _pool_key_rwlock was never
 * initialized either). The flag flip and key deletion happen under the
 * write lock so they can never interleave with a concurrent node_free()'s
 * own read-locked use of the key; see _pool_key_rwlock's own doc comment. */
__attribute__((destructor)) static void _pool_key_fini(void) {
  if (!atomic_load(&_pool_key_live)) return;
  _pool_drain(NULL);
  rw_lock_wrlock(_pool_key_rwlock);
  atomic_store(&_pool_key_live, false);
  thread_ls_key_delete(_pool_key);
  rw_lock_unlock(_pool_key_rwlock);
}

/* Allocate a new node, preferring a recycled entry from the thread-local pool
 * when mp == NULL.  The returned node has the given type tag and a zeroed
 * value union.  Returns NULL on allocation failure (including when a parse
 * currently in progress on this thread has exhausted its
 * _parse_node_budget; see that variable's own doc comment). */
static cyaml_node_t *node_alloc(cyaml_node_type_t type,
                                ccol_memmgmt_procs_t *mp) {
  call_once(_pool_key_once, _do_pool_key_init);
  if (_parse_node_budget != (size_t)-1 && _parse_node_budget == 0) {
    _parse_node_budget_exhausted = true;
    return NULL;
  }
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
  /* Only counted against the budget once a node has actually been
   * materialized (pool-recycled or freshly allocated): decrementing this
   * unconditionally before the allocation attempt above would permanently
   * consume one unit of budget for a node that never came into existence
   * whenever the underlying allocator itself transiently fails (e.g. under
   * single-fault-injection OOM testing), understating how many real nodes
   * a subsequent parse attempt on the same thread may still create. */
  if (_parse_node_budget != (size_t)-1) _parse_node_budget--;
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
  /* Per this codebase's own rule for every pthread/once_flag_t primitive:
   * never rely on call-graph reasoning (e.g. "every node reaching this
   * function was already allocated via node_alloc(), which already ran
   * this") to skip the once-guard in a function that directly touches
   * _pool_key_rwlock; that exact reasoning is what has broken silently
   * before elsewhere in this codebase the moment a new call path appeared. */
  call_once(_pool_key_once, _do_pool_key_init);
  if (_pool_sz >= _CYAML_POOL_CAP) {
    free(n);
    return;
  }
  if (_pool_sz == 0) {
    /* Cold path (once per thread until its pool empties out again): see
     * _pool_key_rwlock's own doc comment for why the load-and-act must be
     * lock-protected rather than a bare atomic check. The outer, unlocked
     * check here exists only to skip touching _pool_key_rwlock at all when
     * _do_pool_key_init() never completed successfully (e.g. genuine
     * pthread resource exhaustion), in which case the rwlock was never
     * validly initialized and locking it would be undefined behavior; this
     * does not reopen the very race the inner lock exists to close, since
     * _pool_key_live only ever transitions false->true from inside
     * _do_pool_key_init() itself (already run to completion by the
     * call_once() above, with the usual pthread_once happens-before
     * guarantee), so observing true here means the rwlock is guaranteed
     * already fully initialized and remains valid memory for the rest of
     * the process even if a concurrent DSO unload flips the flag back to
     * false immediately afterward; the inner, lock-protected re-check is
     * what makes that later false transition safe to race against. */
    if (atomic_load(&_pool_key_live)) {
      rw_lock_rdlock(_pool_key_rwlock);
      if (atomic_load(&_pool_key_live)) thread_ls_set(_pool_key, (void *)1);
      rw_lock_unlock(_pool_key_rwlock);
    }
  }
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

#ifdef RUNNING_UNIT_TESTS
/* Exposes the calling thread's own node-pool free-list size for white-box
 * unit tests that verify the pool's cap-eviction behavior (_CYAML_POOL_CAP)
 * and per-thread isolation. Not part of the public API. */
size_t cyaml_debug_pool_size(void) { return (size_t)_pool_sz; }
#endif

/*
 * chashmap_begin_iter() returns NULL both when the map is genuinely empty
 * (nothing to iterate, not an error) and when the small per-iteration
 * bookkeeping struct it allocates internally fails to allocate on a
 * non-empty map (a real OOM); its own doc comment says as much, and by
 * default (a NULL err argument) the two are indistinguishable to the
 * caller. Every call site in this file that walks a dictionary's entries
 * has no other way to enumerate them at all, so silently treating "OOM on
 * a non-empty map" the same as "empty" would leak or corrupt whatever that
 * walk was for. This wrapper checks chmap_elem_count() first (a legitimately
 * empty map never even attempts the allocation) and retries the
 * allocation itself a handful of times before giving up, on the chance a
 * transient allocator failure clears; it still returns NULL if the map is
 * genuinely empty or if the retries are exhausted, but a caller can
 * distinguish the latter case by checking chmap_elem_count() again itself
 * if it needs to.
 */
static cmap_iterator *chmap_begin_iter_safe(chmap m) {
  if (chmap_elem_count(m) == 0) return NULL;
  for (int attempt = 0; attempt < 3; attempt++) {
    char *err = NULL;
    cmap_iterator *it = chashmap_begin_iter(m, &err);
    if (it || !err) return it;
  }
  return NULL;
}

/* Destructor callback for chmap_destroy_with_dtor(), used by node_clear's
 * own CYAML_DICTIONARY case below: destroys one dictionary entry's child
 * node. See chmap_destroy_with_dtor's own doc comment (chashmap.h) for why
 * this is the allocation-free way to reach every child during teardown,
 * unlike enumerating the dictionary via chashmap_begin_iter() first (which
 * needs its own small allocation that can itself fail under sustained
 * OOM, silently leaking every already-inserted child that allocation
 * failure prevents ever being reached). Still used, unmodified, by the
 * per-document anchor table's own teardown (anchors_destroy), where each
 * anchor is an independent __cyaml_destroy() call rather than part of the
 * same worklist-driven walk node_clear()/__cyaml_destroy() use below. */
static void _cyaml_destroy_dict_child(cmap_pair *val_pair, void *dtor_ctx) {
  (void)dtor_ctx;
  cyaml_node_t *child = _cyaml_read_child(val_pair->ptr);
  __cyaml_destroy((cyaml)child);
}

/*
 * Explicit, heap-backed worklist used by node_clear()/__cyaml_destroy()
 * below to tear down an entire subtree without recursing once per nesting
 * level. A tree reaching either of them need not have come from
 * cyaml_parse() at all (which is separately bounded by
 * CYAML_MAX_PARSE_DEPTH): it can be built to arbitrary depth directly via
 * the public cyaml_list_push()/cyaml_dictionary_set() API. Unlike
 * clone_node()/serialize_block()/serialize_flow(), destruction has no
 * "fail cleanly" contract to fall back on (both are void; a caller cannot
 * be handed back a still-owned, half-freed tree to try again), so bounding
 * recursion the way those three do (CYAML_CLONE_MAX_DEPTH /
 * CYAML_MAX_SERIALIZE_DEPTH) is not an option here: it would only trade an
 * immediate crash for a silent, permanent memory leak of everything past
 * the cap. An explicit worklist avoids both failure modes by keeping the
 * native call stack at O(1) depth regardless of how deep or wide the tree
 * actually is, no matter how many levels are drained.
 */
typedef struct destroy_worklist {
  cyaml_node_t **items;
  size_t cap;
  size_t len;
} destroy_worklist_t;

/* Push child onto wl, growing it as needed (default allocator; the
 * worklist is transient scratch state with no ties to any node's own
 * m_procs). A NULL child is silently ignored, matching every other
 * "destroy this child pointer" call site in this file.
 *
 * On a worklist-growth allocation failure, child is torn down immediately
 * via an ordinary recursive __cyaml_destroy() call instead of being
 * deferred. This can only reintroduce depth-proportional stack usage for
 * that one child's own subtree, and only when BOTH conditions hold at
 * once: the tree is deep enough to matter, AND the allocator is
 * simultaneously unable to grow a small scratch array. That compound
 * failure is accepted as a rare, graceful degradation back to the
 * pre-existing recursive behavior rather than engineered around further;
 * it is categorically narrower than the bug this worklist exists to fix,
 * which triggered on depth alone with no memory pressure required at all. */
static void destroy_worklist_push(destroy_worklist_t *wl, cyaml_node_t *child) {
  if (!child) return;
  if (wl->len == wl->cap) {
    size_t new_cap = wl->cap == 0 ? 32 : wl->cap * 2;
    cyaml_node_t **grown = mem_realloc(wl->items, new_cap * sizeof(*wl->items));
    if (!grown) {
      __cyaml_destroy((cyaml)child);
      return;
    }
    wl->items = grown;
    wl->cap = new_cap;
  }
  wl->items[wl->len++] = child;
}

/* Destructor callback for chmap_destroy_with_dtor(), used by
 * node_clear_value()'s own CYAML_DICTIONARY case: enqueues one dictionary
 * entry's child node onto the worklist threaded through dtor_ctx instead of
 * recursing into it directly, so a dictionary value is drained by the same
 * iterative worklist as everything else. */
static void _cyaml_enqueue_dict_child(cmap_pair *val_pair, void *dtor_ctx) {
  cyaml_node_t *child = _cyaml_read_child(val_pair->ptr);
  destroy_worklist_push((destroy_worklist_t *)dtor_ctx, child);
}

/* Deep-free n's own value payload (string bytes, or a list/dictionary
 * container), pushing any direct children onto wl rather than recursing
 * into them. Leaves n->type, n->tag, and n itself untouched. */
static void node_clear_value(cyaml_node_t *n, destroy_worklist_t *wl) {
  switch (n->type) {
    case CYAML_STRING:
      _mem_free(n->m_procs, n->value.string);
      n->value.string = NULL;
      break;
    case CYAML_LIST: {
      size_t cnt = cvector_elem_count(n->value.list);
      for (size_t i = 0; i < cnt; i++) {
        cyaml_node_t *child = *(cyaml_node_t **)cvector_at(n->value.list, i);
        destroy_worklist_push(wl, child);
      }
      __cvector_destroy(n->value.list);
      n->value.list = NULL;
      break;
    }
    case CYAML_DICTIONARY: {
      chmap_destroy_with_dtor(n->value.dictionary, _cyaml_enqueue_dict_child,
                              wl);
      n->value.dictionary = NULL;
      break;
    }
    default:
      break;
  }
}

/* Drain wl until empty, fully destroying (tag and node struct included)
 * every node it contains; each drained node's own children (enqueued by
 * node_clear_value() above) keep the worklist fed until the whole subtree
 * referenced by wl's initial contents is gone. A plain loop, not
 * recursion: this is what keeps __cyaml_destroy()'s own stack usage
 * independent of tree depth. */
static void destroy_worklist_drain(destroy_worklist_t *wl) {
  while (wl->len > 0) {
    cyaml_node_t *n = wl->items[--wl->len];
    node_clear_value(n, wl);
    _mem_free(n->m_procs, n->tag);
    n->tag = NULL;
    node_free(n);
  }
}

/* Deep-free the value resources without freeing the node struct itself.
 * Used by node_reinit_scalar() to discard an existing node's old
 * list/dictionary value before overwriting it in place with a new scalar
 * (e.g. via cyaml_set()); the old value may be an arbitrarily deep tree
 * (see destroy_worklist_t's own doc comment above), so every descendant
 * below n's direct children is torn down through the same iterative
 * worklist __cyaml_destroy() uses, not through recursion. */
static void node_clear(cyaml_node_t *n) {
  destroy_worklist_t wl = {NULL, 0, 0};
  node_clear_value(n, &wl);
  destroy_worklist_drain(&wl);
  mem_free(wl.items);
}

/*
 * Overwrite an existing scalar node with a new typed value, deep-freeing
 * any resources the old content owned.  All validation happens before
 * node_clear() runs, so any rejected call leaves the existing node
 * completely untouched.
 *
 * Returns ccol_success, ccol_invalid_args (an unsupported/composite `type`:
 * this includes _CYAML_TYPE_UNSUPPORTED, the sentinel _cyaml_type_of()
 * (cyaml.h) produces for a C value whose type cyaml_set() does not
 * document support for, or a raw_size that doesn't match any real
 * integer/float width), or ccol_not_enough_memory (string strdup failed).
 * This is the enforcement point for cyaml_set()'s documented accepted-type
 * list: without it, an unsupported C type would silently reach here as
 * whatever _cyaml_type_of()'s default case happens to produce and be
 * written as if it were a genuine value instead of being rejected.
 */
static ccol_retval_t node_reinit_scalar(cyaml_node_t *n, cyaml_node_type_t type,
                                        void *raw, size_t raw_size,
                                        bool is_signed) {
  switch (type) {
    case CYAML_NULL:
    case CYAML_BOOL:
    case CYAML_INTEGER:
    case CYAML_FLOAT:
    case CYAML_STRING:
      break;
    default:
      return ccol_invalid_args;
  }

  if (type == CYAML_INTEGER) {
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

  if (type == CYAML_FLOAT && raw_size != sizeof(float) &&
      raw_size != sizeof(double))
    return ccol_invalid_args;

  char *new_str = NULL;
  if (type == CYAML_STRING) {
    const char *s = *(const char **)raw;
    if (s) {
      new_str = ccol_strdup(n->m_procs, s);
      if (!new_str) return ccol_not_enough_memory;
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
    case CYAML_FLOAT:
      n->value.number =
          (raw_size == sizeof(float)) ? (double)*(float *)raw : *(double *)raw;
      break;
    case CYAML_STRING:
      if (!new_str)
        n->type = CYAML_NULL;
      else
        n->value.string = new_str;
      break;
    default:
      break;
  }
  return ccol_success;
}

/* Allocate a new node and immediately initialise it with a scalar value.
 * *out receives the new node on success, or NULL on any failure. Returns
 * ccol_not_enough_memory on node allocation failure, or the exact
 * ccol_retval_t node_reinit_scalar reports on validation failure (e.g.
 * ccol_invalid_args for an unsupported type), so a caller can distinguish
 * the two rather than reporting every failure as an allocation failure. */
static ccol_retval_t node_make_scalar(cyaml_node_type_t type, void *raw,
                                      size_t raw_size, bool is_signed,
                                      ccol_memmgmt_procs_t *mp,
                                      cyaml_node_t **out) {
  *out = NULL;
  cyaml_node_t *n = node_alloc(CYAML_NULL, mp);
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

/* Free a node and all its descendants, iteratively (see destroy_worklist_t's
 * own doc comment above for why: a tree reaching here need not have come
 * from cyaml_parse() at all, and has no depth bound when built directly
 * through the public mutation API).  Safe to call on NULL.  Does NOT null
 * the caller's pointer; use the cyaml_destroy() macro wrapper for that.
 *
 * n itself is cleared and freed directly, without ever passing through
 * destroy_worklist_push(): that function's own OOM fallback recursively
 * calls __cyaml_destroy() on the item it failed to enqueue, and n is this
 * very call's own argument, so pushing it there would risk this function
 * calling itself on the same node under simultaneous OOM. Every genuine
 * descendant is unaffected by this and is still drained through the
 * worklist exactly as node_clear() itself uses it. */
void __cyaml_destroy(cyaml node) {
  if (!node) return;
  cyaml_node_t *n = (cyaml_node_t *)node;

  destroy_worklist_t wl = {NULL, 0, 0};
  node_clear_value(n, &wl);
  _mem_free(n->m_procs, n->tag);
  n->tag = NULL;
  node_free(n);

  destroy_worklist_drain(&wl);
  mem_free(wl.items);
}

/* ========================================================================== */
/*                         LEAF VALUE ACCESS                                  */
/* ========================================================================== */

/* Return the node's type tag.  Returns CYAML_NULL for a NULL handle. */
cyaml_node_type_t cyaml_type(cyaml node) {
  if (!node) return CYAML_NULL;
  return ((cyaml_node_t *)node)->type;
}

/* ========================================================================== */
/*                         TAGS                                               */
/* ========================================================================== */

const char *cyaml_node_tag(cyaml node) {
  if (!node) return NULL;
  return ((cyaml_node_t *)node)->tag;
}

ccol_retval_t cyaml_node_set_tag(cyaml node, const char *tag) {
  if (!node) return ccol_invalid_args;
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!tag) {
    _mem_free(n->m_procs, n->tag);
    n->tag = NULL;
    return ccol_success;
  }
  char *copy = ccol_strdup(n->m_procs, tag);
  if (!copy) return ccol_not_enough_memory;
  _mem_free(n->m_procs, n->tag);
  n->tag = copy;
  return ccol_success;
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
    /* A caller may fetch this dictionary's own current value at `key`
     * (cyaml_dictionary_get's borrowed reference) and hand that exact
     * pointer back to this function for the same key. In that case
     * old_child == c: the slot was just overwritten with the pointer it
     * already held, and destroying "the old value" would destroy the node
     * the dictionary still (correctly) points to. */
    if (old_child && old_child != c) __cyaml_destroy((cyaml)old_child);
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

#ifdef RUNNING_UNIT_TESTS
/* Enumerates a CYAML_DICTIONARY node's keys by index, for tools that need
 * to walk the DOM generically without knowing its keys ahead of time (e.g.
 * the differential-testing helper in tests/cyaml/differential/). There is
 * no public equivalent: an ordinary caller always knows its own keys and
 * uses cyaml_dictionary_get() directly, so adding permanent public API
 * surface for this would serve no real caller. Order matches the
 * dictionary's own internal chmap iteration order (reverse insertion
 * order), not alphabetical or original-insertion order; a caller wanting
 * a stable order must sort the returned keys itself.
 *
 * Re-walks the chmap from scratch on every call rather than caching a
 * cursor, an O(n) cost per call (O(n^2) to enumerate a whole dictionary)
 * that is a non-issue here: this function is compiled only under
 * RUNNING_UNIT_TESTS and is never part of any production code path.
 *
 * Returns NULL once index is out of range, or if map is NULL/not a
 * CYAML_DICTIONARY node. */
const char *cyaml_debug_dictionary_key_at(cyaml map, size_t index) {
  if (!map) return NULL;
  cyaml_node_t *n = (cyaml_node_t *)map;
  if (n->type != CYAML_DICTIONARY) return NULL;

  cmap_iterator *it = chashmap_begin_iter(n->value.dictionary, NULL);
  size_t i = 0;
  while (it) {
    if (i == index) {
      const char *key = (const char *)it->key_pair->ptr;
      ccol_iter_destroy(it);
      return key;
    }
    i++;
    it = it->_next_fn(it);
  }
  return NULL;
}
#endif

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

/* Attach a strdup'd copy of src_tag onto an already-fully-built dst clone.
 * Returns false (and destroys dst) on OOM or if dst itself is NULL (the
 * base constructor already failed); a NULL src_tag is a no-op success,
 * matching a node with no tag producing an untagged clone.  Used by every
 * branch of cyaml_clone below so tag propagation cannot be accidentally
 * skipped for a subset of node shapes (cyaml_clone has no single shared
 * "finish building this node" tail for this to hook into automatically). */
static bool clone_attach_tag(cyaml dst, const char *src_tag,
                             ccol_memmgmt_procs_t *mp) {
  if (!dst) return false;
  if (!src_tag) return true;
  char *t = ccol_strdup(mp, src_tag);
  if (!t) {
    __cyaml_destroy(dst);
    return false;
  }
  ((cyaml_node_t *)dst)->tag = t;
  return true;
}

/*
 * Maximum recursion depth for clone_node() below.  Mirrors
 * CYAML_MAX_SERIALIZE_DEPTH (defined later in this file, in the PARSER
 * INTERNALS section) for the identical reason: a tree handed to
 * cyaml_clone() need not have come from cyaml_parse() at all, it can be
 * built directly, to an arbitrary depth, via the public
 * cyaml_list_push()/cyaml_dictionary_set() API, so CYAML_MAX_PARSE_DEPTH
 * (which only bounds parse_node's own recursion) gives no protection here.
 * Without an independent cap, a legitimately acyclic but deep tree (e.g.
 * one built by a recursive application-level builder driven by external
 * input) recurses cyaml_clone() without bound and crashes the process via
 * stack overflow instead of returning NULL as documented, confirmed
 * empirically well below any depth that would raise practical suspicion.
 * Kept as its own constant, rather than referencing CYAML_MAX_SERIALIZE_DEPTH
 * directly, only because that constant is defined later in this file, after
 * clone_node(); both are set to the identical value 500 deliberately, so a
 * tree exactly at the parser's own CYAML_MAX_PARSE_DEPTH limit can still be
 * cloned (or serialized) without spuriously failing.
 */
#define CYAML_CLONE_MAX_DEPTH 500

/* Produce an independent deep copy of the subtree rooted at src, depth
 * levels below the original cyaml_clone() call (0 at the root).  See
 * CYAML_CLONE_MAX_DEPTH's own doc comment for why this cap exists
 * independently of the parser's CYAML_MAX_PARSE_DEPTH. */
static cyaml clone_node(cyaml_node_t *src, int depth) {
  if (depth > CYAML_CLONE_MAX_DEPTH) return NULL;
  ccol_memmgmt_procs_t *mp = src->m_procs;

  switch (src->type) {
    case CYAML_NULL: {
      cyaml dst = cyaml_create_null_mp(mp);
      return clone_attach_tag(dst, src->tag, mp) ? dst : NULL;
    }
    case CYAML_BOOL: {
      cyaml dst = cyaml_create_bool_mp(src->value.boolean, mp);
      return clone_attach_tag(dst, src->tag, mp) ? dst : NULL;
    }
    case CYAML_INTEGER: {
      cyaml dst = cyaml_create_int_mp(src->value.integer, mp);
      return clone_attach_tag(dst, src->tag, mp) ? dst : NULL;
    }
    case CYAML_FLOAT: {
      cyaml dst = cyaml_create_double_mp(src->value.number, mp);
      return clone_attach_tag(dst, src->tag, mp) ? dst : NULL;
    }
    case CYAML_STRING: {
      cyaml dst = cyaml_create_string_mp(src->value.string, mp);
      return clone_attach_tag(dst, src->tag, mp) ? dst : NULL;
    }
    case CYAML_LIST: {
      cyaml dst = cyaml_create_list_mp(mp);
      if (!dst) return NULL;
      size_t cnt = cvector_elem_count(src->value.list);
      for (size_t i = 0; i < cnt; i++) {
        cyaml_node_t *child = *(cyaml_node_t **)cvector_at(src->value.list, i);
        cyaml cc = clone_node(child, depth + 1);
        if (!cc) {
          __cyaml_destroy(dst);
          return NULL;
        }
        if (cyaml_list_push(dst, cc) != ccol_success) {
          __cyaml_destroy(dst);
          return NULL;
        }
      }
      return clone_attach_tag(dst, src->tag, mp) ? dst : NULL;
    }
    case CYAML_DICTIONARY: {
      cyaml dst = cyaml_create_dictionary_mp(mp);
      if (!dst) return NULL;
      size_t src_count = chmap_elem_count(src->value.dictionary);
      cmap_iterator *it = chmap_begin_iter_safe(src->value.dictionary);
      if (!it && src_count > 0) {
        /* Non-empty source, but the iterator could not be built even
         * after retrying (see chmap_begin_iter_safe's own doc comment): a
         * real OOM, not "nothing to clone". Returning dst here would
         * silently produce a "successful" clone missing every key,
         * violating this function's own documented "NULL on allocation
         * failure" contract. __cyaml_destroy(dst) below is safe
         * regardless of how far this loop got, since node_clear's own
         * CYAML_DICTIONARY case routes through chmap_destroy_with_dtor
         * (chashmap.h), which never needs to allocate to reach dst's
         * already-inserted entries. */
        __cyaml_destroy(dst);
        return NULL;
      }
      size_t seen = 0;
      while (it) {
        const char *key = (const char *)it->key_pair->ptr;
        cyaml_node_t *child = _cyaml_read_child(it->val_pair->ptr);
        cyaml cc = clone_node(child, depth + 1);
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
        seen++;
        it = it->_next_fn(it);
      }
      if (seen != src_count) {
        /* chmap_begin_iter_safe() succeeded, but a later _next_fn() call
         * failed partway through (a real OOM, not "nothing more to
         * iterate": see chmap_begin_iter_safe's own doc comment for why
         * these are otherwise indistinguishable). Silently returning dst
         * here would produce a "successful" clone missing one or more of
         * this dictionary's entries, the exact same contract violation
         * the src_count==0 case above already guards against; mirrors
         * serialize_block's identical seen != cnt check for this exact
         * situation. */
        __cyaml_destroy(dst);
        return NULL;
      }
      return clone_attach_tag(dst, src->tag, mp) ? dst : NULL;
    }
  }
  return NULL;
}

/* Produce an independent deep copy of the subtree rooted at node.
 * The clone inherits the source node's allocator and tag.  On OOM, on a
 * source nested more than CYAML_CLONE_MAX_DEPTH levels deep, or if a
 * dictionary's entries could not all be enumerated, any partially-built
 * clone is destroyed before NULL is returned. */
cyaml cyaml_clone(cyaml node) {
  if (!node) return NULL;
  return clone_node((cyaml_node_t *)node, 0);
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
 * tag_handles: lazily-created chmap (char* -> char*) mapping a %TAG handle
 * ("!", "!!", or "!name!") to its resolved prefix, mirroring anchors'
 * lifecycle exactly: scoped to one document, destroyed (and reset to NULL)
 * at the end of parse_one_document.  Unlike anchors, the table itself
 * starts genuinely empty even once created; tag_handles_lookup falls back
 * to the two YAML 1.2 default handles itself when no explicit entry is
 * present, so shorthand resolution always has a base case even with no
 * explicit %TAG directive in the document (see tag_handles_ensure's own
 * doc comment for why this matters for duplicate-%TAG-directive detection).
 *
 * indent_stack: not needed; indentation is passed as an integer argument
 * through the recursive descent.
 *
 * depth: current parse_node recursion depth, maintained entirely by
 * parse_node's own thin wrapper (see CYAML_MAX_PARSE_DEPTH below); every
 * other function in this file that recurses back into node parsing does so
 * exclusively through parse_node, so this one counter bounds the whole
 * parser's call-stack usage regardless of which YAML construct (nested
 * mappings, sequences, explicit keys, anchors, tags, ...) drives the
 * recursion.
 *
 * line_starts/line_starts_len/line_starts_cap/line_scan_pos/
 * line_cache_disabled: back line_start_pos()'s amortized line-boundary
 * cache (see that function's own doc comment). line_starts holds every
 * line-start byte offset discovered so far, in increasing order, with
 * line_starts[0] always 0; line_scan_pos is the highest ctx->pos the cache
 * has been extended to. Allocated once, eagerly, by parse_common() itself
 * before any parsing function runs (a failure there is a fatal parse
 * failure, not a degraded fallback); line_cache_disabled latches true
 * (permanently) only if a LATER growth of the cache fails under OOM, at
 * which point line_start_pos() falls back to a direct per-call backward
 * scan for the rest of the parse.
 */
typedef struct {
  const char *src;
  size_t pos;
  size_t len;
  char error[512];
  ccol_memmgmt_procs_t *mp;
  chmap anchors;
  chmap tag_handles;
  size_t depth;
  size_t *line_starts;
  size_t line_starts_len;
  size_t line_starts_cap;
  size_t line_scan_pos;
  bool line_cache_disabled;
} parse_ctx_t;

/*
 * Maximum parse_node recursion depth. Exists to bound stack usage against
 * pathologically deep or self-amplifying nesting (a document with this many
 * levels of block/flow/explicit-key structure is not a realistic real-world
 * document regardless of its exact shape); exceeding it is a hard parse
 * error, not a silent truncation. 500 is comfortably below where a debug
 * (non-optimized) build's per-frame stack usage would risk overflowing an
 * 8 MiB default thread stack, while still permitting any realistic
 * hand-written or generated document.
 */
#define CYAML_MAX_PARSE_DEPTH 500

/*
 * Maximum length (in bytes) of a non-scalar dictionary key's canonical
 * flow-YAML text (see node_to_dict_key_string's own doc comment). Exists
 * for a reason CYAML_MAX_PARSE_DEPTH's own generic bound does not cover:
 * a non-scalar key that is itself a mapping whose own single key is
 * another such mapping, nested N levels deep, forces each enclosing
 * level's canonicalization to re-double-quote the text the level below it
 * already produced, so the canonical string's length grows as O(2^N) in
 * nesting depth, not O(N) (confirmed empirically; roughly 2^N bytes at
 * depth N). CYAML_MAX_PARSE_DEPTH alone cannot bound this: a value
 * generous enough for legitimate deep nesting (hundreds of levels) is
 * still far beyond the ~30 levels at which this specific pattern already
 * produces gigabyte-sized strings, so a document deliberately kept just
 * under the depth cap can still exhaust memory or run for an unbounded
 * time. This length check, applied to the canonicalized text itself
 * rather than to nesting depth, catches exactly that pattern (and no
 * other) at its actual source, regardless of what CYAML_MAX_PARSE_DEPTH
 * is set to; 64 KiB is far beyond any realistic non-scalar key's own
 * canonical text in ordinary use, while stopping the O(2^N) pattern at a
 * shallow, cheap-to-reject depth (already exceeded by depth 20).
 */
#define CYAML_MAX_CANONICAL_KEY_LEN (64 * 1024)

/*
 * Maximum number of cyaml_node_t allocations a single top-level parse_common()
 * call is permitted to perform before node_alloc() refuses any further
 * allocation (returning NULL, exactly like a real allocator OOM). Exists for
 * a reason neither CYAML_MAX_PARSE_DEPTH nor CYAML_MAX_CANONICAL_KEY_LEN
 * covers: cyaml_clone() (used both to snapshot an anchor's subtree at
 * registration time and to materialize an independent copy of it at every
 * subsequent *alias reference, and again by merge-key expansion) performs
 * a genuine, unshared deep copy, so a document that nests aliases of aliases
 * can make the final live node count grow exponentially in the number of
 * anchor levels even though the source text and the parser's own recursion
 * depth both stay small (the classic "billion laughs" entity-expansion
 * shape; CYAML_MAX_PARSE_DEPTH bounds recursion depth, not clone/sibling
 * fan-out). At roughly 40 bytes per cyaml_node_t, this bounds worst-case
 * node-struct memory to under 200 MiB while comfortably permitting any
 * legitimately large hand-written or generated document (which would need
 * millions of distinct scalar values before ever approaching this).
 */
#define CYAML_MAX_PARSE_NODES (4 * 1000 * 1000)

/*
 * Maximum recursion depth for serialize_block()/serialize_flow(). A tree
 * handed to cyaml_serialize()/cyaml_serialize_flow() need not have come
 * from cyaml_parse() at all: it can be built directly, to an arbitrary
 * depth, via the public cyaml_list_push()/cyaml_dictionary_set() API, so
 * CYAML_MAX_PARSE_DEPTH (which only bounds parse_node's own recursion)
 * gives no protection here. Without an independent cap of its own, a
 * legitimately acyclic but deep tree (e.g. one built by a recursive
 * application-level builder driven by external input) recurses the
 * serializer without bound and crashes the process via stack overflow,
 * confirmed empirically well below any depth that would raise practical
 * suspicion. Exceeding this is reported the same way any other
 * serialization failure is: by setting the ybuf_t's own oom flag, which
 * cyaml_serialize()/cyaml_serialize_flow() already translate into a NULL
 * return, since neither function has a separate error-string channel of
 * its own to report a more specific diagnostic through.
 */
#define CYAML_MAX_SERIALIZE_DEPTH 500

/* Arm/disarm _parse_node_budget (declared in the THREAD-LOCAL NODE POOL
 * section above, alongside node_alloc) for the parse_common() call
 * currently in progress on this thread. Disarming resets to "unlimited"
 * rather than merely leaving whatever count remains, so a decremented
 * value can never leak into unrelated node creation on this thread once
 * the parse that armed it returns. */
static inline void parse_node_budget_arm(void) {
  _parse_node_budget = CYAML_MAX_PARSE_NODES;
  _parse_node_budget_exhausted = false;
}
static inline void parse_node_budget_disarm(void) {
  _parse_node_budget = (size_t)-1;
  _parse_node_budget_exhausted = false;
}

/* Format a parse error into ctx->error.  Only the last call is kept;
 * earlier messages are silently overwritten.
 *
 * The __attribute__((format(printf, 2, 3))) below is what lets this avoid
 * -Wformat-nonliteral entirely rather than suppressing it: that warning
 * fires whenever a printf-family function (here, vsnprintf) is handed a
 * format string that is not a literal at the call site, since the compiler
 * can no longer cross-check it against the varargs actually supplied. fmt
 * is exactly such a non-literal (it is this function's own parameter, not
 * a literal), but both GCC and Clang special-case the "pass-through
 * wrapper" pattern: when the enclosing function is itself annotated as
 * printf-like for that same parameter, the internal call is accepted as
 * correct by construction, and the compiler instead moves the real
 * checking to every call site of parse_err() itself, where fmt IS a
 * literal. This is strictly better than suppressing the warning: it adds
 * genuine, compile-time format-string/argument-type checking (with
 * -Werror) to every parse_err() call in this file, catching a real class
 * of bug (e.g. "%d" on a size_t) instead of just silencing the warning. */
static void parse_err(parse_ctx_t *ctx, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void parse_err(parse_ctx_t *ctx, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
  va_end(ap);
}

/* Position helpers */

/* at_end: true when all input has been consumed.
 * cur:    return the current character without advancing; '\0' at end. */
static inline bool at_end(parse_ctx_t *ctx) { return ctx->pos >= ctx->len; }

static inline char cur(parse_ctx_t *ctx) {
  return at_end(ctx) ? '\0' : ctx->src[ctx->pos];
}

/* Fallback/degraded-mode implementation of line_start_pos(): scan backward
 * from ctx->pos one byte at a time. O(column) per call on its own, which is
 * exactly what line_start_pos()'s cache exists to avoid paying on every
 * call; kept as a small standalone helper so the cache-disabled path (used
 * only immediately after a cache-growth OOM, an already-degraded state) and
 * the cache's own first-ever call still share one implementation. */
static size_t line_start_pos_backward_scan(parse_ctx_t *ctx) {
  size_t p = ctx->pos;
  while (p > 0 && ctx->src[p - 1] != '\n' && ctx->src[p - 1] != '\r') p--;
  return p;
}

/* Extend ctx->line_starts so it accounts for every line boundary up to (but
 * not including) byte offset up_to, appending one entry per '\n' or '\r'
 * crossed (matching line_start_pos_backward_scan()'s own line-boundary
 * rule). Only ever called with up_to > ctx->line_scan_pos. On an allocation
 * failure the cache is torn down and ctx->line_cache_disabled latches
 * permanently; the caller (line_start_pos()) falls back to a direct scan
 * for this call and every subsequent one, rather than retrying a doomed
 * allocation on every future call. */
static void line_starts_extend(parse_ctx_t *ctx, size_t up_to) {
  size_t p = ctx->line_scan_pos;
  while (p < up_to) {
    char c = ctx->src[p];
    p++;
    if (c == '\n' || c == '\r') {
      if (ctx->line_starts_len == ctx->line_starts_cap) {
        size_t new_cap = ctx->line_starts_cap ? ctx->line_starts_cap * 2 : 64;
        size_t *grown =
            _mem_realloc(ctx->mp, ctx->line_starts, new_cap * sizeof(size_t));
        if (!grown) {
          _mem_free(ctx->mp, ctx->line_starts);
          ctx->line_starts = NULL;
          ctx->line_starts_len = 0;
          ctx->line_starts_cap = 0;
          ctx->line_cache_disabled = true;
          return;
        }
        ctx->line_starts = grown;
        ctx->line_starts_cap = new_cap;
      }
      ctx->line_starts[ctx->line_starts_len++] = p;
    }
  }
  ctx->line_scan_pos = p;
}

/* Return the byte offset of the start of ctx->pos's own physical line: the
 * position immediately after the nearest preceding '\n' or '\r', or 0 if
 * none precedes it. Shared by current_col() and line_indent_has_tab(),
 * both of which need exactly this "where did the current line begin"
 * computation, and called from parse_node_inner() (among many other sites)
 * once per parsed node. A standalone '\r' is recognized as a line boundary
 * here, not just '\n', matching skip_to_eol()/at_eol()/skip_newline()
 * elsewhere in this file (input is never CR/LF-normalized before parsing);
 * for a "\r\n" ending, the scan always reaches the '\n' first (the later of
 * the two bytes), so the line is correctly taken to start right after the
 * pair, never after just the '\r'.
 *
 * A naive backward byte-by-byte scan here is O(column) per call; since a
 * flow collection (or any other construct that keeps parse_node_inner on
 * one physical line without crossing a newline) drives one such call per
 * sibling with no bound on how many siblings share a line, that made the
 * total parse cost O(n^2) in the line length (measured: tens of seconds on
 * a single-line flow list of a few tens of thousands of elements) with none
 * of CYAML_MAX_PARSE_DEPTH/CYAML_MAX_PARSE_NODES/CYAML_MAX_CANONICAL_KEY_LEN
 * bounding it, since none of them bounds sibling fan-out on one line.
 *
 * Fixed with an amortized cache (ctx->line_starts): a monotonically growing,
 * sorted list of every line-start offset discovered so far. ctx->pos is
 * overwhelmingly monotonic (parsing only rarely backtracks, and only ever by
 * a small, bounded distance to retry the immediately preceding token - see
 * e.g. try_parse_scalar_dict_key()), so the common case only ever needs to
 * extend the cache forward past bytes never scanned before, making the
 * total scanning work O(n) for the whole parse rather than O(n) per call.
 * A query is then answered by binary-searching the recorded line starts for
 * the largest one <= ctx->pos, which is correct regardless of whether
 * ctx->pos is ahead of or behind the highest point reached so far - unlike
 * a scheme that only remembers the single most recent line start, this
 * also stays correct across a backward jump into an already-scanned
 * region, with no re-scanning at all.
 *
 * The cache's own initial allocation happens once, eagerly, in
 * parse_common() itself (before any parsing function runs), and a failure
 * there is a fatal, immediately-reported parse failure, exactly like this
 * parser's other early setup steps: this is a real, load-bearing
 * allocation this function has no way to route around, so its failure
 * must be surfaced honestly, not silently absorbed by a graceful fallback
 * deep inside a hot, per-node call path with no error-reporting channel
 * of its own. Only a LATER growth failure (line_starts_extend(), once the
 * cache's initial small capacity is exhausted by a large document) falls
 * back to a direct per-call scan for the remainder of the parse: that
 * failure mode is reached progressively, deep into an otherwise-successful
 * parse under sustained memory pressure, where discarding all completed
 * work in favor of a slower-but-correct fallback is the better trade,
 * unlike the initial allocation's all-or-nothing, parse-hasn't-even-
 * started-yet position. */
static size_t line_start_pos(parse_ctx_t *ctx) {
  if (ctx->line_cache_disabled) return line_start_pos_backward_scan(ctx);

  if (ctx->pos > ctx->line_scan_pos) {
    line_starts_extend(ctx, ctx->pos);
    if (ctx->line_cache_disabled) return line_start_pos_backward_scan(ctx);
  }

  size_t lo = 0, hi = ctx->line_starts_len;
  while (lo + 1 < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (ctx->line_starts[mid] <= ctx->pos)
      lo = mid;
    else
      hi = mid;
  }
  return ctx->line_starts[lo];
}

/* Return the 0-based column of ctx->pos on its line (distance from the
 * start of the line, per line_start_pos()).  Used to detect indentation
 * levels for block collections. */
static int current_col(parse_ctx_t *ctx) {
  if (ctx->pos == 0) return 0;
  size_t p = line_start_pos(ctx);
  size_t col = ctx->pos - p;
  /* Every caller uses this purely for indentation-level comparisons
   * (against another int, or against a small explicit indent indicator),
   * never as a byte count; saturating here instead of letting a line
   * longer than INT_MAX bytes silently wrap into an implementation-defined
   * (and potentially negative) int is what keeps every one of those
   * comparisons well-defined regardless of how long a single physical line
   * is. */
  return col > (size_t)INT_MAX ? INT_MAX : (int)col;
}

/* True when a tab character appears anywhere between the start of the
 * current line and ctx->pos. YAML 1.2 sec. 6.1 forbids a tab from ever
 * being interpreted as block-structural indentation or separation;
 * empirically verified (against PyYAML, the more reliable of the two
 * reference parsers for this specific rule; see this project's own
 * "Tab-as-indentation full audit" internal history notes for why)
 * that this extends to every position current_col() is used to measure
 * or compare an indentation LEVEL against, not merely the classic
 * "leading spaces before block content" case.
 *
 * Deliberately narrower than "does skip_ws_comments/skip_inline_ws see a
 * tab": those two shared functions are also used for whitespace genuinely
 * INTERNAL to already-open content (e.g. a quoted scalar's own line-
 * folding), where a tab is legitimate data, not indentation; conflating
 * the two caused a real, reproduced infinite loop when tried. This helper
 * is instead called explicitly, only at the specific points that
 * genuinely represent a block-structural indentation decision. */
static bool line_indent_has_tab(parse_ctx_t *ctx) {
  size_t p = line_start_pos(ctx);
  return memchr(ctx->src + p, '\t', ctx->pos - p) != NULL;
}

/* Skip only horizontal whitespace (spaces and tabs) without crossing a
 * newline.  Essential for indentation-sensitive block parsing where the
 * column position of the next token is meaningful.
 *
 * Deliberately still skips a tab unconditionally: this function is also
 * used for whitespace genuinely internal to already-open content (e.g.
 * parse_double_quoted's own blank-line fold logic), where a tab is
 * legitimate data, not block-structural indentation; confirmed by a
 * real infinite loop found when an earlier draft of the tab-indentation
 * fix made this function stop at a tab unconditionally: parse_double_
 * quoted's fold loop calls this function expecting it to always make
 * forward progress, and a tab it left unconsumed broke that invariant.
 * Tab rejection for genuine block-structural positions is instead done
 * at each specific call site that represents one, via current_col()
 * plus line_indent_has_tab() (see that helper's own doc comment), not
 * inside this shared, lower-level function. */
static void skip_inline_ws(parse_ctx_t *ctx) {
  while (!at_end(ctx)) {
    char c = cur(ctx);
    if (c == ' ' || c == '\t')
      ctx->pos++;
    else
      break;
  }
}

/* Discard the rest of the current line, including the terminating newline.
 * A standalone '\r', a standalone '\n', or a "\r\n" pair all count as the
 * line's own terminator (matching at_eol()/skip_newline() elsewhere in this
 * file; input is never CR/LF-normalized before parsing, so a bare-CR line
 * ending must be recognized here too, not just LF). */
static void skip_to_eol(parse_ctx_t *ctx) {
  while (!at_end(ctx) && cur(ctx) != '\n' && cur(ctx) != '\r') ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '\r') ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '\n') ctx->pos++;
}

static inline bool at_eol(parse_ctx_t *ctx) {
  return at_end(ctx) || cur(ctx) == '\n' || cur(ctx) == '\r';
}

/* True when the byte span [start, ctx->pos) contains a line break, whether
 * '\n', a bare '\r', or a "\r\n" pair.  This parser never CR/LF-normalizes
 * its input, so a bare-CR-only line break must be recognized here exactly
 * like it already is by skip_to_eol()/at_eol()/skip_newline() above; a
 * check for '\n' alone would be silently bypassed by a document using
 * nothing but bare-CR line endings within the span being tested.  Used by
 * every "did this token span more than one physical line" decision (an
 * implicit key's single-line restriction, a flow collection's
 * continuation-line indentation/tab check). */
static bool span_crosses_newline(parse_ctx_t *ctx, size_t start) {
  return memchr(ctx->src + start, '\n', ctx->pos - start) != NULL ||
         memchr(ctx->src + start, '\r', ctx->pos - start) != NULL;
}

/*
 * True when nothing but inline whitespace and, optionally, a trailing '#'
 * comment remains before the next newline (or EOF); i.e. this line has no
 * further real content, even when cur(ctx) is not itself '\n'/EOF yet
 * (e.g. cur(ctx) == '#'). Does not mutate ctx->pos. Used wherever a "does
 * this entry's value/anchor/tag content start on THIS line or a later one"
 * decision must not be fooled by a trailing comment sitting between the
 * indicator and the newline (e.g. "a: # comment\n  b: c\n"; the value
 * genuinely starts on the next line, even though the character right after
 * "a: " is '#', not '\n').
 */
static bool rest_of_line_is_blank(parse_ctx_t *ctx) {
  size_t p = ctx->pos;
  while (p < ctx->len && (ctx->src[p] == ' ' || ctx->src[p] == '\t')) p++;
  if (p < ctx->len && ctx->src[p] == '#' &&
      (p == 0 || ctx->src[p - 1] == ' ' || ctx->src[p - 1] == '\t' ||
       ctx->src[p - 1] == '\n' || ctx->src[p - 1] == '\r')) {
    while (p < ctx->len && ctx->src[p] != '\n' && ctx->src[p] != '\r') p++;
  }
  return p >= ctx->len || ctx->src[p] == '\n' || ctx->src[p] == '\r';
}

/* Skip any combination of whitespace (including newlines) and '#' comments.
 * Used between top-level structural tokens where indentation is not
 * significant (e.g., between flow collection elements).
 *
 * Per the YAML spec, a '#' only starts a comment when preceded by
 * whitespace or the start of input; '#' directly abutting the previous
 * token (e.g. "]#comment", "\"value\"#comment") is not a comment at all,
 * and must be left as-is for the caller to reject as unexpected trailing
 * content, not silently swallowed here. */
static void skip_ws_comments(parse_ctx_t *ctx) {
  while (!at_end(ctx)) {
    char c = cur(ctx);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ctx->pos++;
    } else if (c == '#' && (ctx->pos == 0 || ctx->src[ctx->pos - 1] == ' ' ||
                            ctx->src[ctx->pos - 1] == '\t' ||
                            ctx->src[ctx->pos - 1] == '\n' ||
                            ctx->src[ctx->pos - 1] == '\r')) {
      skip_to_eol(ctx);
    } else {
      break;
    }
  }
}

/*
 * Like skip_ws_comments, but for content inside a flow collection: per
 * YAML 1.2's s-separate(n,c) grammar for flow-in/flow-out contexts,
 * s-separate-lines(n) requires every line crossed while skipping
 * whitespace to be indented strictly more than `indent` (the enclosing
 * block value's own indent; the same parameter threaded throughout
 * parse_node/parse_block_scalar_content, with -1 as the document-root
 * sentinel for "no enclosing block context"). Only the final landing
 * line is checked, not every intermediate blank line crossed along the
 * way, which is sufficient for every case this parser needs to
 * distinguish (verified against a reference parser: "k: {\nk\n:\nv\n}"
 * is rejected, since its continuation lines sit at column 0, no more
 * indented than "k:"'s own column 0; the identical content indented by
 * one more space throughout is accepted).
 *
 * Returns false (with ctx->error set) if the check fails; true otherwise
 * (including when no newline was crossed at all, matching plain
 * skip_ws_comments' unconditional success).
 */
static bool flow_skip_ws(parse_ctx_t *ctx, int indent) {
  size_t start = ctx->pos;
  skip_ws_comments(ctx);
  if (!at_end(ctx)) {
    bool crossed_newline = span_crosses_newline(ctx, start);
    if (crossed_newline && line_indent_has_tab(ctx)) {
      /* s-flow-line-prefix(n), the crossed-line continuation production,
       * requires genuine s-indent (spaces only); a same-line separator
       * (crossed_newline false) is still ordinary s-separate-in-line,
       * which does permit tabs, so this check is deliberately scoped to
       * the crossed-newline case only (verified against a reference
       * parser). */
      parse_err(ctx,
                "tab cannot be used as indentation on a flow collection "
                "continuation line at position %zu",
                ctx->pos);
      return false;
    }
    if (crossed_newline && current_col(ctx) <= indent) {
      parse_err(ctx,
                "flow collection continuation line must be indented "
                "more than %d at position %zu",
                indent, ctx->pos);
      return false;
    }
  }
  return true;
}

/* Consume exactly one newline list: CR, LF, or CRLF. */
static void skip_newline(parse_ctx_t *ctx) {
  if (!at_end(ctx) && cur(ctx) == '\r') ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '\n') ctx->pos++;
}

/* Peeks (without consuming) whether the remainder of the current line, up
 * to the next line break or EOF, consists only of inline whitespace
 * (spaces/tabs); i.e. whether this is a genuinely blank line. Used by
 * double- and single-quoted scalar line folding to detect a blank
 * continuation line (which must fold to a newline, not a space) even when
 * that line carries its own trailing whitespace (e.g. a lone tab): a bare
 * cur(ctx) == '\n'/'\r' check taken immediately after skip_newline()
 * cannot see past such whitespace to the line break behind it. */
static bool at_blank_line(parse_ctx_t *ctx) {
  size_t p = ctx->pos;
  while (p < ctx->len && (ctx->src[p] == ' ' || ctx->src[p] == '\t')) p++;
  return p >= ctx->len || ctx->src[p] == '\r' || ctx->src[p] == '\n';
}

/*
 * True for a C0 control byte (0x00-0x1F) other than tab (0x09), or for DEL
 * (0x7F), which YAML 1.2 categorically excludes from literal, unescaped
 * scalar content in every style. c-printable, the production underlying
 * every non-JSON-compatible scalar style's own nb-char-based grammar, is
 * "#x9 | #xA | #xD | [#x20-#x7E] | ..."; note the range stops at 0x7E, one
 * short of DEL. nb-json, which governs double-quoted content specifically
 * and is textually wider ("#x9 | [#x20-#x10FFFF]", with no upper gap around
 * 0x7F at all), would appear to permit a raw DEL byte there on the grammar
 * text alone; confirmed empirically against PyYAML, however (the same
 * verify-before-fixing discipline this file already applies elsewhere),
 * that a raw DEL byte is rejected identically across double-quoted,
 * single-quoted, plain, and literal-block scalars alike ("unacceptable
 * character #x007f: special characters are not allowed"); PyYAML filters
 * it at the character-stream level before any scalar-style-specific
 * grammar is even consulted, taking precedence over nb-json's own wider
 * textual range for this one byte.
 *
 * Every call site is a scalar scanner's own "ordinary content" fallthrough,
 * reached only after that scanner's own newline handling (folding a
 * quoted scalar's line break, or splitting a block scalar's own lines) has
 * already consumed '\n'/'\r' through a separate, earlier branch; this
 * predicate is therefore never consulted for either of those two bytes,
 * despite both also being < 0x20.  It is never consulted inside an escape
 * sequence's own handling either (a double-quoted scalar's explicit "\x01"
 * or "\u0001" escape remains the sole, deliberate way to embed such a
 * byte, exactly like the existing "\0" null-byte escape).
 */
static inline bool is_disallowed_control_byte(char c) {
  unsigned char u = (unsigned char)c;
  return (u < 0x20 && u != '\t') || u == 0x7F;
}

/* Returns true when ctx->pos is at a document-start ('---') or document-end
 * ('...') marker: the three characters at column 0, followed by whitespace or
 * EOF.  A '#' glued directly onto the marker with no separating whitespace
 * does NOT count (matching this file's own skip_ws_comments()/
 * rest_of_line_is_blank() rule elsewhere that a comment must be preceded by
 * real whitespace, and YAML 1.2's own s-l-comments grammar, which requires
 * s-separate-in-line before c-nb-comment-text): "---#x" is ordinary plain-
 * scalar content, not a marker followed by a comment. Used to stop block
 * collection parsers at document boundaries and to detect the start of the
 * next document in a multi-document stream. */
static bool at_doc_marker(parse_ctx_t *ctx) {
  if (current_col(ctx) != 0) return false;
  if (ctx->pos + 3 > ctx->len) return false;
  const char *p = ctx->src + ctx->pos;
  if (memcmp(p, "---", 3) != 0 && memcmp(p, "...", 3) != 0) return false;
  if (ctx->pos + 3 == ctx->len) return true;
  char nx = ctx->src[ctx->pos + 3];
  return nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r';
}

/* Anchor / alias helpers */

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

/* Returns false (with a parse error set via parse_err()) on OOM, either
 * from anchors_ensure()'s lazy table creation or from chmap_insert_elem()
 * itself; true on a genuine store.  clone is always consumed exactly once
 * either way (stored on success, destroyed on failure), so the caller never
 * needs its own cleanup for clone specifically.  Reporting failure here
 * (rather than silently discarding clone and returning nothing, as this
 * function used to) matters beyond a merely-cosmetic error message: a
 * caller that instead unconditionally treated storing an anchor as having
 * succeeded would return a "successful" parse result whose anchor was
 * silently never actually registered, which a later '*name' alias
 * reference to it would then fail against with a confusing "unknown alias"
 * error instead of the real "out of memory" cause; see this function's own
 * callers for how the return value propagates that failure instead. */
static bool anchors_store(parse_ctx_t *ctx, const char *name,
                          cyaml_node_t *clone) {
  if (!anchors_ensure(ctx)) {
    parse_err(ctx, "out of memory registering anchor '%s' at position %zu",
              name, ctx->pos);
    __cyaml_destroy((cyaml)clone);
    return false;
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
    return true;
  }
  /* Insert failed: preserve the old clone in the map, discard the new one. */
  parse_err(ctx, "out of memory registering anchor '%s' at position %zu", name,
            ctx->pos);
  __cyaml_destroy((cyaml)clone);
  return false;
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
  /* chmap_destroy_with_dtor (chashmap.h), not an enumerate-then-destroy
   * sequence: reaches every stored clone via chashmap's own already-
   * allocation-free internal teardown walk, so this can never leak a
   * clone under sustained OOM the way enumerating via
   * chashmap_begin_iter() first (even through chmap_begin_iter_safe's own
   * bounded retries) still could. Mirrors node_clear's own identical fix
   * for a CYAML_DICTIONARY node's children. */
  chmap_destroy_with_dtor(ctx->anchors, _cyaml_destroy_dict_child, NULL);
  ctx->anchors = NULL;
}

/* %TAG handle table */

/*
 * tag_handles_ensure/_set/_lookup/_destroy: a %TAG handle -> prefix table,
 * mirroring anchors_ensure/_store/_lookup/_destroy's lifecycle exactly
 * (lazily created, destroyed at the end of parse_one_document).  Unlike
 * anchors, the table starts genuinely empty rather than pre-populated with
 * the two YAML 1.2 default handles: tag_handles_lookup falls back to the
 * hardcoded defaults itself when the table has no explicit entry, which
 * lets tag_handles_set reuse chmap_insert_elem's own "already present"
 * signal directly to detect a second %TAG directive redefining the same
 * handle within one document, without a separate tracking structure; the
 * first %TAG for any handle (including "!"/"!!") always inserts cleanly
 * since nothing is pre-populated.
 */
static bool tag_handles_ensure(parse_ctx_t *ctx) {
  if (ctx->tag_handles) return true;
  char *err = NULL;
  ctx->tag_handles = chmap_create_mp(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                     ccol_string, ccol_string, ctx->mp, &err);
  return ctx->tag_handles != NULL;
}

/* Returns ccol_success on a clean first-time insert, ccol_key_already_present
 * if this handle was already explicitly set by an earlier %TAG directive in
 * this same document (the caller rejects this as a duplicate redefinition),
 * or another ccol_retval_t on allocation failure. */
static ccol_retval_t tag_handles_set(parse_ctx_t *ctx, const char *handle,
                                     const char *prefix) {
  if (!tag_handles_ensure(ctx)) return ccol_not_enough_memory;
  cmap_pair kp = {.ptr = (void *)handle, .size = strlen(handle) + 1};
  cmap_pair vp = {.ptr = (void *)prefix, .size = strlen(prefix) + 1};
  return chmap_insert_elem(ctx->tag_handles, &kp, &vp);
}

/* Returns a borrowed pointer to the resolved prefix for `handle` (the
 * explicitly %TAG-registered one if present, otherwise the YAML 1.2
 * built-in default for "!"/"!!"), or NULL if `handle` is neither
 * registered nor one of the two built-ins (an undefined named handle). */
static const char *tag_handles_lookup(parse_ctx_t *ctx, const char *handle) {
  if (ctx->tag_handles) {
    cmap_pair kp = {.ptr = (void *)handle, .size = strlen(handle) + 1};
    cmap_pair *vp = NULL;
    if (chmap_get_elem_ref(ctx->tag_handles, &kp, &vp) == ccol_success)
      return (const char *)vp->ptr;
  }
  if (strcmp(handle, "!") == 0) return "!";
  if (strcmp(handle, "!!") == 0) return "tag:yaml.org,2002:";
  return NULL;
}

static void tag_handles_destroy(parse_ctx_t *ctx) {
  if (!ctx->tag_handles) return;
  __chmap_destroy(ctx->tag_handles);
  ctx->tag_handles = NULL;
}

/* Anchor / alias name parsing */

/* Read the anchor or alias name that follows '&' or '*'.  A name is any
 * non-empty list of characters that are not whitespace, flow indicators,
 * or '#'.  Returns false (with a parse error set via parse_err(), on an
 * empty name or an allocation failure) otherwise.  Always setting
 * ctx->error on failure matters beyond this function's own direct callers:
 * try_parse_scalar_dict_key() calls this speculatively and relies on
 * ctx->error[0] alone (not a restored ctx->pos, which this function's own
 * scan loop has already advanced past on this path) to distinguish "a
 * genuine error" from "not a key after all"; leaving ctx->error unset here
 * on OOM would let that caller misclassify the failure and resume parsing
 * from a corrupted position instead of cleanly failing the document. */
static bool parse_anchor_name(parse_ctx_t *ctx, char **name_out) {
  size_t start = ctx->pos;
  while (!at_end(ctx)) {
    char c = cur(ctx);
    /* Anchor name ends at whitespace, flow indicators, or comment. */
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' ||
        c == '[' || c == ']' || c == '{' || c == '}' || c == '#')
      break;
    /* A ':' followed by whitespace or end-of-input is a mapping value
     * indicator, not part of the anchor/alias name: mirrors
     * scan_plain_scalar_line's own identical "colon+space or colon+EOL
     * terminates" rule, so an anchor/alias used directly as a mapping key
     * with no separating space before the colon (e.g. "*x: y", by far the
     * most natural way to write it, since an ordinary key is written
     * "key: value" the same way) is tokenized as just the name "x", not
     * "x:" (verified against a reference parser, which resolves "*x: y"
     * to alias "x" used as a key rather than reporting an unknown alias
     * "x:"). A bare ':' NOT followed by whitespace (e.g. as part of a
     * URL-like anchor name) is left as ordinary name content, exactly like
     * scan_plain_scalar_line's own identical exception. */
    if (c == ':') {
      bool stop = (ctx->pos + 1 >= ctx->len) || ctx->src[ctx->pos + 1] == ' ' ||
                  ctx->src[ctx->pos + 1] == '\t' ||
                  ctx->src[ctx->pos + 1] == '\n' ||
                  ctx->src[ctx->pos + 1] == '\r';
      if (stop) break;
    }
    ctx->pos++;
  }
  size_t len = ctx->pos - start;
  if (len == 0) {
    parse_err(ctx, "empty anchor/alias name at position %zu", start);
    return false;
  }
  char *name = _mem_alloc(ctx->mp, len + 1);
  if (!name) {
    parse_err(ctx, "out of memory parsing anchor/alias name at position %zu",
              start);
    return false;
  }
  memcpy(name, ctx->src + start, len);
  name[len] = '\0';
  *name_out = name;
  return true;
}

/* Implicit type resolution */

/*
 * try_parse_null_scalar/_bool_/_int_/_float_scalar: the individual
 * core-schema type predicates make_typed_scalar tries in sequence for
 * implicit (untagged) resolution.  Factored out as standalone, independently
 * callable functions (rather than inlined once inside make_typed_scalar)
 * specifically so an explicit tag (finalize_scalar_node, below) can force
 * exactly one of them without also trying every other type first the way
 * make_typed_scalar's own sequential fallthrough does.
 */
static bool try_parse_bool_scalar(const char *s, bool *out) {
  if (strcmp(s, "true") == 0 || strcmp(s, "True") == 0 ||
      strcmp(s, "TRUE") == 0) {
    *out = true;
    return true;
  }
  if (strcmp(s, "false") == 0 || strcmp(s, "False") == 0 ||
      strcmp(s, "FALSE") == 0) {
    *out = false;
    return true;
  }
  return false;
}

/*
 * Convert a non-negative magnitude into its final long long value, negating
 * it first when neg is true.  The caller must have already verified
 * uval <= (unsigned long long)LLONG_MAX + (neg ? 1 : 0), i.e. that the
 * result actually fits in a long long; this function's only remaining job
 * is producing that negation without invoking signed-overflow UB for the
 * single boundary magnitude (2^63) whose negation is LLONG_MIN itself,
 * which cannot be reached by first casting to (signed) long long and then
 * negating: -(long long)0x8000000000000000ULL negates the already-signed
 * LLONG_MIN, which C never permits (mirrors the reasoning already spelled
 * out where the decimal branch below passes strtoll the full signed string
 * instead of negating a sign-stripped magnitude, for the identical reason).
 */
static long long magnitude_to_signed(unsigned long long uval, bool neg) {
  if (!neg) return (long long)uval;
  if (uval == (unsigned long long)LLONG_MAX + 1ULL) return LLONG_MIN;
  return -(long long)uval;
}

static bool try_parse_int_scalar(const char *s, long long *out) {
  /* The core schema's own int grammar ([-+]?[0-9]+ for decimal, plus the
   * 0x/0o forms) has no leading-whitespace production at all; strtoll()
   * below is
   * more permissive than that grammar (per the C standard it silently skips
   * leading whitespace before the subject sequence), which would otherwise
   * let e.g. " 42" be silently accepted as 42 instead of failing as it must
   * (trim_trailing_ws_for_numeric_tag, this function's own caller for an
   * explicit !!int tag, deliberately trims only trailing whitespace, never
   * leading, for exactly this reason). Rejected here, before anything else,
   * so every caller (implicit resolution included) gets the same strict
   * behavior. */
  if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') return false;

  const char *p = s;
  bool neg = false;
  if (*p == '+')
    p++;
  else if (*p == '-') {
    neg = true;
    p++;
  }
  if (*p == '\0') return false;

  char *endp = NULL;
  long long ival = 0;
  errno = 0;

  if (p[0] == '0' && p[1] == 'x') {
    /* hex; the core schema's own grammar for this form is exactly
     * "0x" [0-9a-fA-F]+, with no room for a sign (or anything else) between
     * the prefix and the digits, and the 'x' itself is lowercase only (unlike
     * the digit sequence, which is explicitly case-insensitive). An
     * uppercase "0X" prefix has no core-schema int representation at all and
     * must fall through to the decimal branch below, which correctly
     * rejects it (strtoll with an explicit base of 10 never gives "0X..." a
     * hex interpretation, regardless of case). strtoull() itself is more
     * permissive than the grammar above (per the C standard it accepts an
     * optional leading '+'/'-' before the digit sequence it consumes),
     * which would otherwise let a malformed literal like "0x-0" or "0x+5"
     * be silently accepted as a valid integer (magnitude 0 or 5
     * respectively) instead of falling back to CYAML_STRING; every digit
     * after the prefix is therefore validated by hand first, exactly like
     * the float fallback's own octal branch already does below. */
    const char *digits = p + 2;
    if (*digits == '\0') return false;
    for (const char *d = digits; *d; d++) {
      bool is_hex_digit = (*d >= '0' && *d <= '9') ||
                          (*d >= 'a' && *d <= 'f') || (*d >= 'A' && *d <= 'F');
      if (!is_hex_digit) return false;
    }
    unsigned long long uval = strtoull(digits, &endp, 16);
    if (endp == digits || *endp != '\0' || errno == ERANGE) return false;
    /* strtoull only rejects a magnitude that overflows ULLONG_MAX (2^64-1);
     * a value between 2^63 and 2^64-1 still fits in an unsigned 64-bit
     * word but has no representation as a long long of the requested sign,
     * so it must be rejected here too rather than silently reinterpreted
     * (previously: 0xFFFFFFFFFFFFFFFF silently became -1, and
     * 0x8000000000000000 silently became negative). Rejecting it here
     * lets the caller's usual cascade (make_typed_scalar) fall back to a
     * CYAML_FLOAT for a value this large, exactly mirroring how the
     * decimal branch's own errno==ERANGE check already behaves for a
     * plain decimal literal wider than 64 bits. */
    unsigned long long limit =
        (unsigned long long)LLONG_MAX + (neg ? 1ULL : 0ULL);
    if (uval > limit) return false;
    ival = magnitude_to_signed(uval, neg);
  } else if (p[0] == '0' && p[1] == 'o') {
    /* octal; lowercase 'o' only, mirroring the hex branch's identical
     * case-sensitivity requirement above (an uppercase "0O" prefix falls
     * through to the decimal branch below, which correctly rejects it). See
     * the hex branch's own comment above for why every digit must be
     * validated by hand before calling strtoull(), rather than relying on
     * strtoull() to reject a malformed "0o-0"/"0o+7"-style literal on its
     * own. */
    const char *digits = p + 2;
    if (*digits == '\0') return false;
    for (const char *d = digits; *d; d++) {
      if (*d < '0' || *d > '7') return false;
    }
    unsigned long long uval = strtoull(digits, &endp, 8);
    if (endp == digits || *endp != '\0' || errno == ERANGE) return false;
    unsigned long long limit =
        (unsigned long long)LLONG_MAX + (neg ? 1ULL : 0ULL);
    if (uval > limit) return false;
    ival = magnitude_to_signed(uval, neg);
  } else {
    /* decimal: pass the full original string (including the optional sign)
     * so that strtoll handles LLONG_MIN (-9223372036854775808) correctly.
     * strtoll on the sign-stripped substring p would overflow for 2^63. */
    ival = strtoll(s, &endp, 10);
    if (endp == s || *endp != '\0' || errno == ERANGE) return false;
  }
  *out = ival;
  return true;
}

/* Float specials (.inf/-.inf/.nan) plus ordinary strtod parsing, matching
 * YAML 1.2 core schema's own definition (dot-prefixed specials only; strtod's
 * own bare "nan"/"inf"/"infinity" acceptance on C99 platforms is explicitly
 * excluded, since those are not YAML floats). */
static bool try_parse_float_scalar(const char *s, double *out) {
  /* See try_parse_int_scalar's identical check and comment above: the core
   * schema's own float grammar has no leading-whitespace production, but
   * strtod() below silently skips it, which would otherwise let e.g. " 3.5"
   * be silently accepted as 3.5 instead of failing as it must. */
  if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') return false;

  if (strcmp(s, ".inf") == 0 || strcmp(s, ".Inf") == 0 ||
      strcmp(s, ".INF") == 0 || strcmp(s, "+.inf") == 0 ||
      strcmp(s, "+.Inf") == 0 || strcmp(s, "+.INF") == 0) {
    *out = __builtin_inf();
    return true;
  }
  if (strcmp(s, "-.inf") == 0 || strcmp(s, "-.Inf") == 0 ||
      strcmp(s, "-.INF") == 0) {
    *out = -__builtin_inf();
    return true;
  }
  if (strcmp(s, ".nan") == 0 || strcmp(s, ".NaN") == 0 ||
      strcmp(s, ".NAN") == 0) {
    *out = __builtin_nan("");
    return true;
  }

  const char *q = s;
  bool neg = false;
  if (*q == '+') {
    q++;
  } else if (*q == '-') {
    neg = true;
    q++;
  }
  if (strcasecmp(q, "nan") == 0 || strcasecmp(q, "inf") == 0 ||
      strcasecmp(q, "infinity") == 0)
    return false;
  /* glibc's strtod() also accepts C99's nan(n-char-sequence) syntax (e.g.
   * "nan(123)", "nan()"), which is not part of the YAML core schema float
   * grammar at all (only the bare/dot-prefixed forms already handled
   * above are); left unguarded, the generic strtod() call below would
   * silently accept it as a NaN float, discarding the original text and
   * colliding every distinct "nan(...)" spelling onto the same
   * canonicalized dictionary key. Reject the whole q up front so it falls
   * through to CYAML_STRING like any other non-numeric token. */
  if (strncasecmp(q, "nan(", 4) == 0) return false;

  /* Hex/octal integer syntax has no float representation of its own in the
   * YAML core schema at all; the ONLY reason this function ever accepts
   * one is to reproduce make_typed_scalar()'s own implicit-typing cascade
   * (try int first, fall back to float only once int overflows int64) for
   * an oversized literal, mirroring how an oversized decimal literal
   * already falls back to a float of the same magnitude below. That
   * fallback reasoning does NOT hold for a small, in-range hex/octal
   * literal (try_parse_int_scalar would have already accepted it as a
   * genuine CYAML_INTEGER, so it must never reach here as float syntax at
   * all) or for finalize_scalar_node()'s explicit !!float tag path (which
   * calls this function directly, without ever trying int first); an
   * in-range "0x10"/"0o17" tagged !!float has no valid float
   * interpretation and must be rejected, not silently coerced. This
   * function therefore re-derives the "genuinely overflows int64" check
   * itself (mirroring try_parse_int_scalar's own identical limit
   * arithmetic) rather than trusting a caller to have already established
   * it. */
  if (q[0] == '0' && q[1] == 'o') {
    const char *c = q + 2;
    if (*c == '\0') return false;
    for (; *c; c++) {
      if (*c < '0' || *c > '7') return false;
    }
    errno = 0;
    unsigned long long ouval = strtoull(q + 2, NULL, 8);
    if (errno == ERANGE) return false; /* magnitude too large even for u64 */
    unsigned long long limit =
        (unsigned long long)LLONG_MAX + (neg ? 1ULL : 0ULL);
    if (ouval <= limit) return false; /* fits in int64: not a float fallback */
    *out = neg ? -(double)ouval : (double)ouval;
    return true;
  }

  /* Verify the remainder is purely hex digits before treating this as a
   * hex integer literal at all: strtod() itself accepts real C99
   * hex-float syntax (an explicit p/P exponent, optionally with a '.') as
   * a GNU/C99 extension that is not a YAML float and must not be silently
   * misinterpreted as one (e.g. "0x1p3" -> 8.0, "0x1.8p10" -> 1536,
   * neither of which is valid YAML of any kind and both of which must
   * fall through to CYAML_STRING instead); this loop already rejects both
   * by construction, since '.'/'p'/'P' are not hex digits. Lowercase 'x'
   * only, mirroring try_parse_int_scalar's identical case-sensitivity
   * requirement for the core schema's own "0x" grammar. */
  if (q[0] == '0' && q[1] == 'x') {
    const char *c = q + 2;
    if (*c == '\0') return false;
    for (; *c; c++) {
      bool is_hex_digit = (*c >= '0' && *c <= '9') ||
                          (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F');
      if (!is_hex_digit) return false;
    }
    /* See the octal branch's own doc comment just above: only a magnitude
     * that genuinely overflows int64 is a legitimate float fallback; an
     * in-range hex literal (e.g. "0x10") has no float representation and
     * must be rejected here rather than handed to strtod(), which would
     * otherwise silently accept it via its own hex-integer extension. */
    errno = 0;
    unsigned long long hxuval = strtoull(q + 2, NULL, 16);
    if (errno == ERANGE) return false; /* magnitude too large even for u64 */
    unsigned long long limit =
        (unsigned long long)LLONG_MAX + (neg ? 1ULL : 0ULL);
    if (hxuval <= limit) return false; /* fits in int64: not a float fallback */
    *out = neg ? -(double)hxuval : (double)hxuval;
    return true;
  }

  /* An uppercase "0X"/"0O" prefix (in any position, including after the
   * '+'/'-' sign already stripped into q) has no core-schema numeric
   * representation at all (only the lowercase forms handled above are
   * defined), and must be rejected outright here rather than falling
   * through to the generic strtod() call below. Unlike the octal case
   * (strtod has no "0o" syntax of any case, so "0O17" already fails
   * strtod's own parse naturally), strtod() itself still recognizes an
   * uppercase "0X" hex prefix per the C standard's own case-insensitive
   * "0x or 0X" wording for its accepted subject sequence, and would
   * otherwise silently accept e.g. "0X10" as 16.0 via that extension. */
  if (q[0] == '0' && (q[1] == 'X' || q[1] == 'O')) return false;

  char *endp = NULL;
  errno = 0;
  double dval = strtod(s, &endp);
  if (endp == s || *endp != '\0') return false;
  /* strtod() sets errno=ERANGE both on true overflow (the result is
   * clamped to +-infinity) and on underflow to a valid subnormal or to
   * 0.0 (a completely legitimate, correctly-computed result); only the
   * former is a real failure; rejecting on ERANGE alone would silently
   * misclassify a valid tiny float (e.g. "5e-324") as CYAML_STRING. */
  if (errno == ERANGE && (dval == __builtin_inf() || dval == -__builtin_inf()))
    return false;
  *out = dval;
  return true;
}

static bool is_null_scalar_text(const char *s) {
  return s[0] == '\0' || strcmp(s, "~") == 0 || strcmp(s, "null") == 0 ||
         strcmp(s, "Null") == 0 || strcmp(s, "NULL") == 0;
}

/*
 * Attempt to parse s as a YAML 1.2 core-schema typed scalar.
 * Returns a newly allocated node; the caller owns it.
 */
static cyaml_node_t *make_typed_scalar(parse_ctx_t *ctx, const char *s) {
  ccol_memmgmt_procs_t *mp = ctx->mp;

  if (is_null_scalar_text(s)) return node_alloc(CYAML_NULL, mp);

  bool bval;
  if (try_parse_bool_scalar(s, &bval)) {
    cyaml_node_t *n = node_alloc(CYAML_BOOL, mp);
    if (n) n->value.boolean = bval;
    return n;
  }

  long long ival;
  if (try_parse_int_scalar(s, &ival)) {
    cyaml_node_t *n = node_alloc(CYAML_INTEGER, mp);
    if (n) n->value.integer = ival;
    return n;
  }

  double dval;
  if (try_parse_float_scalar(s, &dval)) {
    cyaml_node_t *n = node_alloc(CYAML_FLOAT, mp);
    if (n) n->value.number = dval;
    return n;
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

/* Tag-driven scalar type resolution */

/*
 * Strip all trailing whitespace/newlines from text in place, before a
 * forced !!int/!!float tag attempts to parse it as a number (a literal
 * or folded block scalar's chomped content, under any chomp mode including
 * KEEP's own multiple preserved trailing newlines, would otherwise never
 * match). Confirmed empirically against PyYAML's own construct_yaml_int/
 * _float (both accept a KEEP-chomped "42\n\n\n" as 42; Python's underlying
 * int()/float() are themselves whitespace-tolerant); an earlier, narrower
 * "trim exactly one trailing newline" design was
 * verified against the same reference and found too strict, rejecting
 * exactly the KEEP-chomped multi-newline case PyYAML accepts. Deliberately
 * NOT applied to !!bool: PyYAML's own construct_yaml_bool does a strict,
 * untrimmed dict lookup and rejects the identical shape (also confirmed
 * empirically), so trimming there would only diverge from the reference in
 * the other direction. */
static void trim_trailing_ws_for_numeric_tag(char *text) {
  size_t len = strlen(text);
  while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
                     text[len - 1] == ' ' || text[len - 1] == '\t'))
    len--;
  text[len] = '\0';
}

/*
 * Explicit !!bool tagging accepts a wider, case-insensitive vocabulary
 * than this module's own narrower implicit-typing default (true/True/
 * TRUE/false/False/FALSE only, tried via try_parse_bool_scalar);
 * confirmed empirically against PyYAML's own construct_yaml_bool, which
 * matches case-insensitively against {yes, no, true, false, on, off}
 * (but not single-letter y/n) regardless of loader; a more deliberate,
 * explicit tag gets PyYAML's own historically wider vocabulary, not the
 * narrower one implicit resolution uses.
 */
static bool try_parse_bool_scalar_explicit(const char *s, bool *out) {
  if (strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0 ||
      strcasecmp(s, "on") == 0) {
    *out = true;
    return true;
  }
  if (strcasecmp(s, "false") == 0 || strcasecmp(s, "no") == 0 ||
      strcasecmp(s, "off") == 0) {
    *out = false;
    return true;
  }
  return false;
}

/*
 * Finalize a scalar node from its raw text, honoring an explicit tag's
 * override where one applies. Takes ownership of `text` (frees it or
 * transfers it into the returned node); the caller must not touch it
 * again after this call. `resolved_tag` is a borrowed pointer (not owned
 * by this function); NULL means untagged. `implicit_ok` is true only for a
 * plain scalar's own natural default (make_typed_scalar's implicit
 * core-schema resolution); false for the four quoted/block styles, whose
 * own natural default is always CYAML_STRING with no implicit typing.
 *
 * Returns NULL on OOM (ctx->error left untouched, matching every other
 * allocation-failure return in this file) or on a genuine tag/content
 * mismatch (ctx->error is set; a hard parse failure for the whole
 * document, matching this parser's fail-fast convention).
 */
static cyaml_node_t *finalize_scalar_node(parse_ctx_t *ctx, char *text,
                                          const char *resolved_tag,
                                          bool implicit_ok) {
  cyaml_node_t *n = NULL;

  if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_NULL) == 0) {
    /* Empty tag value defaults to null per spec; force null regardless of
     * the actual text content. */
    n = node_alloc(CYAML_NULL, ctx->mp);
  } else if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_BOOL) == 0) {
    bool bval;
    if (!try_parse_bool_scalar_explicit(text, &bval)) {
      parse_err(ctx, "'%s' is not a valid !!bool value", text);
      _mem_free(ctx->mp, text);
      return NULL;
    }
    n = node_alloc(CYAML_BOOL, ctx->mp);
    if (n) n->value.boolean = bval;
  } else if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_INT) == 0) {
    trim_trailing_ws_for_numeric_tag(text);
    long long ival;
    if (!try_parse_int_scalar(text, &ival)) {
      parse_err(ctx, "'%s' is not a valid !!int value", text);
      _mem_free(ctx->mp, text);
      return NULL;
    }
    n = node_alloc(CYAML_INTEGER, ctx->mp);
    if (n) n->value.integer = ival;
  } else if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_FLOAT) == 0) {
    trim_trailing_ws_for_numeric_tag(text);
    double dval;
    if (!try_parse_float_scalar(text, &dval)) {
      parse_err(ctx, "'%s' is not a valid !!float value", text);
      _mem_free(ctx->mp, text);
      return NULL;
    }
    n = node_alloc(CYAML_FLOAT, ctx->mp);
    if (n) n->value.number = dval;
  } else if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_STR) == 0) {
    n = node_alloc(CYAML_STRING, ctx->mp);
    if (n) {
      n->value.string = text;
      text = NULL; /* ownership transferred */
    }
  } else if (resolved_tag && (strcmp(resolved_tag, CYAML_TAG_SEQ) == 0 ||
                              strcmp(resolved_tag, CYAML_TAG_MAP) == 0)) {
    /* !!seq/!!map decorating a scalar (the reverse direction of the
     * structural-kind check finalize_collection_node performs for a
     * scalar-only core-schema tag on actual collection syntax): a
     * scalar can never satisfy either, since a collection's own type is
     * fully determined by its own block/flow syntax, never inferred. */
    parse_err(ctx, "tag '%s' does not match the node it decorates",
              resolved_tag);
    _mem_free(ctx->mp, text);
    return NULL;
  } else if (implicit_ok) {
    /* No forcing tag (untagged, non-specific "!", custom, or !!binary):
     * fall through to ordinary implicit resolution. */
    n = make_typed_scalar(ctx, text);
  } else {
    /* A quoted/block scalar with no forcing tag is always a string; no
     * implicit typing. */
    n = node_alloc(CYAML_STRING, ctx->mp);
    if (n) {
      n->value.string = text;
      text = NULL;
    }
  }

  _mem_free(ctx->mp, text); /* no-op if ownership was already transferred */

  if (!n) return NULL;

  if (resolved_tag) {
    char *tag_copy = ccol_strdup(ctx->mp, resolved_tag);
    if (!tag_copy) {
      __cyaml_destroy((cyaml)n);
      return NULL;
    }
    n->tag = tag_copy;
  }

  return n;
}

/*
 * Attach an explicit tag to an already-fully-parsed collection node (a
 * list or dictionary), validating it structurally first. Unlike a scalar
 * (whose type can be forced by a tag, via finalize_scalar_node), a
 * collection's type is already fully determined by its own block/flow
 * syntax; a tag here is either a matching structural tag (!!seq/!!map, or
 * a custom tag with no structural expectation) or a genuine mismatch (any
 * of the 5 scalar core-schema tags, or !!seq/!!map applied to the wrong
 * collection kind), which is a hard parse failure.
 *
 * Called at the exact point each collection-constructing dispatch branch
 * in parse_node builds its result (the same "as early as possible"
 * discipline finalize_scalar_node already follows for scalars), rather
 * than deferred to after the enclosing tag's own recursive call returns:
 * a tag threaded through one or more intervening '&' anchor layers (e.g.
 * "!!seq &x\n  - 1\n  - 2\n") must already be attached by the time the
 * anchor branch clones the result for its own anchor table, or that clone
 * (and therefore every later alias of it) would silently end up untagged.
 *
 * `resolved_tag` is a borrowed pointer, exactly like finalize_scalar_node's
 * own parameter (never transferred, always strdup'd here); it may still
 * need to reach further collection-construction sites deeper in the same
 * recursion (through more `&` layers) after this call returns, so this
 * function must not take ownership of it.
 *
 * Returns NULL (destroying `coll`) on a structural mismatch or OOM;
 * returns `coll` unchanged (with its ->tag strdup'd in) on success. A NULL
 * `resolved_tag` is always a no-op success.
 */
static cyaml_node_t *finalize_collection_node(parse_ctx_t *ctx,
                                              cyaml_node_t *coll,
                                              const char *resolved_tag) {
  if (!resolved_tag || !coll) return coll;

  bool seq_ok = strcmp(resolved_tag, CYAML_TAG_SEQ) == 0;
  bool map_ok = strcmp(resolved_tag, CYAML_TAG_MAP) == 0;
  bool scalar_core_tag = strcmp(resolved_tag, CYAML_TAG_NULL) == 0 ||
                         strcmp(resolved_tag, CYAML_TAG_BOOL) == 0 ||
                         strcmp(resolved_tag, CYAML_TAG_INT) == 0 ||
                         strcmp(resolved_tag, CYAML_TAG_FLOAT) == 0 ||
                         strcmp(resolved_tag, CYAML_TAG_STR) == 0;
  if (scalar_core_tag || (seq_ok && coll->type != CYAML_LIST) ||
      (map_ok && coll->type != CYAML_DICTIONARY)) {
    parse_err(ctx,
              "tag '%s' does not match the node it decorates at "
              "position %zu",
              resolved_tag, ctx->pos);
    __cyaml_destroy((cyaml)coll);
    return NULL;
  }

  char *tag_copy = ccol_strdup(ctx->mp, resolved_tag);
  if (!tag_copy) {
    __cyaml_destroy((cyaml)coll);
    return NULL;
  }
  coll->tag = tag_copy;
  return coll;
}

/* Double-quoted scalar */

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
 * Shared newline-folding step for both quoted scalar styles (YAML 1.2 sec.
 * 8.1.2), called with cur(ctx) already known to be '\r' or '\n'. Strips
 * trailing inline whitespace already written to b, down to but never past
 * strip_floor bytes (double-quoted passes its own escape_boundary, since an
 * escape sequence's own output is real content, never incidental source
 * whitespace; single-quoted, which has no escape mechanism at all, always
 * passes 0), then folds the line break to a literal newline (one or more
 * blank continuation lines follow, via at_blank_line()) or a single space
 * (an ordinary line break), identically for both styles.
 *
 * On success, b has been updated and ctx->pos left just past the folded
 * whitespace, with true returned. On a document marker found mid-fold (an
 * unclosed quote reaching a '---'/'...' boundary, which is never literal
 * content to fold across), ctx->error is set naming style_name and false is
 * returned; the caller is responsible for its own cleanup (freeing b) on
 * that path, since the two callers do so differently (goto fail vs. an
 * inline free-and-return).
 */
static bool fold_quoted_newline(parse_ctx_t *ctx, ybuf_t *b, size_t strip_floor,
                                const char *style_name) {
  if (!b->oom) {
    while (b->len > strip_floor &&
           (b->buf[b->len - 1] == ' ' || b->buf[b->len - 1] == '\t'))
      b->buf[--b->len] = '\0';
  }
  skip_newline(ctx);
  if (at_doc_marker(ctx)) {
    parse_err(ctx,
              "document marker inside unterminated %s scalar at position %zu",
              style_name, ctx->pos);
    return false;
  }
  /* at_blank_line() (not a bare cur(ctx) check) is what makes a blank line
   * consisting only of inline whitespace (e.g. a lone tab) fold to a
   * newline rather than a space, matching YAML 1.2 sec. 6.5. */
  if (at_blank_line(ctx)) {
    while (at_blank_line(ctx)) {
      skip_inline_ws(ctx);
      if (at_end(ctx)) break;
      yb_append_c(b, '\n');
      skip_newline(ctx);
    }
    if (at_doc_marker(ctx)) {
      parse_err(ctx,
                "document marker inside unterminated %s scalar at position "
                "%zu",
                style_name, ctx->pos);
      return false;
    }
  } else {
    yb_append_c(b, ' ');
  }
  skip_inline_ws(ctx);
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

  /* Tracks how many bytes at the START of the buffer are "protected" from
   * the trailing-whitespace-strip-before-fold logic below: b.len at the
   * end of the most recent escape sequence. An escaped space/tab (e.g.
   * "\t" or a backslash immediately before a literal tab byte) is real
   * content the author explicitly asked for, not incidental source
   * whitespace sitting before a line break; YAML 1.2 sec. 8.1.2's own
   * "trailing white space is stripped" rule applies only to literal,
   * unescaped whitespace copied straight from the source, never to a byte
   * an escape sequence produced. Without this, an escaped trailing tab
   * was silently eaten by the very next fold. */
  size_t escape_boundary = 0;

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
          /* A "\0" escape resolves to codepoint U+0000; see the shared
           * null-byte rejection comment on the \\x/\\u/\\U cases below for
           * why this cannot be appended as a real byte. */
          parse_err(ctx,
                    "double-quoted scalar contains a null byte escape "
                    "'\\0' at position %zu",
                    ctx->pos - 2);
          goto fail;
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
          /* \xXX; 2-digit hex */
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
          /* A node's scalar value is a plain NUL-terminated char* with no
           * separate length field (see cyaml_str_val's own contract), so
           * decoding an escape into a real embedded NUL byte would silently
           * truncate the value on any later strlen()/printf()/serialize
           * call; exactly the silent-data-corruption shape this
           * library's own no-embedded-null-bytes policy exists to
           * prevent (the same reasoning percent_decode_tag_inplace already
           * applies to tag text). Reject rather than silently truncate. */
          if (v == 0) {
            parse_err(ctx,
                      "double-quoted scalar contains a null byte "
                      "escape '\\x00' at position %zu",
                      ctx->pos - 4);
            goto fail;
          }
          encode_utf8(&b, v);
          break;
        }
        case 'u': {
          uint32_t cp;
          size_t esc_pos = ctx->pos - 2;
          if (!parse_hex4(ctx, &cp)) goto fail;
          /* A high surrogate (0xD800-0xDBFF) only has meaning when
           * immediately followed by a matching low surrogate
           * (0xDC00-0xDFFF) in a second \uXXXX escape, combining into one
           * supplementary-plane codepoint. When it is not followed by
           * one, it is a lone/invalid surrogate on its own (substituted
           * with U+FFFD, matching the standalone-lone-surrogate case
           * below); but the escape that follows it still names its own,
           * independent character (or is itself a lone surrogate) and
           * must still be processed, never silently discarded along with
           * the failed pairing attempt. Loop rather than stopping after
           * one lookahead, since a run of several consecutive unpaired
           * high surrogates (each its own U+FFFD) can still end in a real
           * pair with whatever escape finally follows them. */
          for (;;) {
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              if (ctx->pos + 1 < ctx->len && ctx->src[ctx->pos] == '\\' &&
                  ctx->src[ctx->pos + 1] == 'u') {
                size_t next_esc_pos = ctx->pos;
                ctx->pos += 2;
                uint32_t next;
                if (!parse_hex4(ctx, &next)) goto fail;
                if (next >= 0xDC00 && next <= 0xDFFF) {
                  cp = 0x10000u + ((cp - 0xD800u) << 10) + (next - 0xDC00u);
                  break;
                }
                encode_utf8(&b, 0xFFFD);
                esc_pos = next_esc_pos;
                cp = next;
                continue;
              }
              cp = 0xFFFD;
              break;
            }
            if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD;
            break;
          }
          /* See the \\x case above for why a null-byte result must be
           * rejected rather than silently appended. */
          if (cp == 0) {
            parse_err(ctx,
                      "double-quoted scalar contains a null byte "
                      "escape '\\u0000' at position %zu",
                      esc_pos);
            goto fail;
          }
          encode_utf8(&b, cp);
          break;
        }
        case 'U': {
          uint32_t cp;
          size_t esc_pos = ctx->pos - 2;
          if (!parse_hex8(ctx, &cp)) goto fail;
          /* A \U escape names an arbitrary 32-bit hex value; only
           * 0..0x10FFFF outside the UTF-16 surrogate range (0xD800-0xDFFF)
           * is a valid Unicode scalar value. Substitute U+FFFD for
           * anything outside that range, mirroring the \u escape's own
           * lone-surrogate handling above, rather than letting
           * encode_utf8() truncate an out-of-range value into a
           * structurally invalid UTF-8 byte sequence. */
          if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFD;
          /* See the \\x case above for why a null-byte result must be
           * rejected rather than silently appended. */
          if (cp == 0) {
            parse_err(ctx,
                      "double-quoted scalar contains a null byte "
                      "escape '\\U00000000' at position %zu",
                      esc_pos);
            goto fail;
          }
          encode_utf8(&b, cp);
          break;
        }
        case '\n':
        case '\r': {
          /* Escaped newline (YAML 1.2 sec. 8.1.2's s-double-escaped
           * production, "\" b-non-content l-empty(n,flow-in)*
           * s-flow-line-prefix(n)): only the escaped break ITSELF is
           * non-content and contributes nothing, unlike an ordinary,
           * unescaped break (s-flow-folded, handled by
           * fold_quoted_newline() below), which always folds to a space
           * or to literal newlines. Each l-empty blank line that follows
           * the escaped break still ends in a real b-as-line-feed,
           * exactly like fold_quoted_newline()'s own at_blank_line()
           * loop, so it must still append a literal '\n' per blank line;
           * only the very first break (the escaped one) is skipped with
           * nothing appended. */
          if (esc == '\r' && !at_end(ctx) && cur(ctx) == '\n') ctx->pos++;
          while (at_blank_line(ctx)) {
            skip_inline_ws(ctx);
            if (at_end(ctx)) break;
            yb_append_c(&b, '\n');
            skip_newline(ctx);
          }
          if (at_doc_marker(ctx)) {
            parse_err(ctx,
                      "document marker inside unterminated double-quoted "
                      "scalar at position %zu",
                      ctx->pos);
            goto fail;
          }
          skip_inline_ws(ctx);
          break;
        }
        default:
          parse_err(ctx, "unknown escape '\\%c' at position %zu", esc,
                    ctx->pos - 1);
          goto fail;
      }
      /* Whatever this escape just appended (if anything) is protected
       * from the trailing-whitespace strip below; see escape_boundary's
       * own doc comment above. */
      escape_boundary = b.len;
      continue;
    }

    /* Newlines within double-quoted scalar: fold to space (or, for one or
     * more blank continuation lines, to newlines); see escape_boundary's
     * own doc comment above for why the trailing-whitespace strip this
     * involves stops there rather than at 0. */
    if (c == '\r' || c == '\n') {
      if (!fold_quoted_newline(ctx, &b, escape_boundary, "double-quoted"))
        goto fail;
      continue;
    }

    if (is_disallowed_control_byte(c)) {
      parse_err(ctx,
                "raw control character 0x%02x is not allowed in a "
                "double-quoted scalar at position %zu; use an escape "
                "sequence instead",
                (unsigned char)c, ctx->pos);
      goto fail;
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

/* Single-quoted scalar */

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

    /* Same fold_quoted_newline() step as parse_double_quoted, with
     * strip_floor = 0: single-quoted has no escape mechanism at all, so
     * there is no escape_boundary-style protected span to stop short of. */
    if (c == '\r' || c == '\n') {
      if (!fold_quoted_newline(ctx, &b, 0, "single-quoted")) {
        _mem_free(b.m_procs, b.buf);
        return false;
      }
      continue;
    }

    if (is_disallowed_control_byte(c)) {
      parse_err(ctx,
                "raw control character 0x%02x is not allowed in a "
                "single-quoted scalar at position %zu",
                (unsigned char)c, ctx->pos);
      _mem_free(b.m_procs, b.buf);
      return false;
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

/* Block scalar (literal | and folded >) */

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
   * either order, but c-b-block-header permits at most ONE of each kind;
   * had_chomp/had_indent reject a duplicate/contradictory second
   * occurrence of either (e.g. "|--", "|+-", "|24") instead of silently
   * letting the later one overwrite the earlier. */
  bool had_chomp = false, had_indent = false;
  for (int pass = 0; pass < 2; pass++) {
    if (at_end(ctx) || at_eol(ctx)) break;
    char c = cur(ctx);
    if (c == '-' || c == '+') {
      if (had_chomp) {
        parse_err(ctx,
                  "duplicate chomping indicator in block scalar header "
                  "at position %zu",
                  ctx->pos);
        return false;
      }
      *chomp = (c == '-') ? CHOMP_STRIP : CHOMP_KEEP;
      had_chomp = true;
      ctx->pos++;
    } else if (c >= '1' && c <= '9') {
      if (had_indent) {
        parse_err(ctx,
                  "duplicate indentation indicator in block scalar header "
                  "at position %zu",
                  ctx->pos);
        return false;
      }
      *explicit_indent = c - '0';
      had_indent = true;
      ctx->pos++;
    } else
      break;
  }

  /* A trailing comment requires whitespace before the '#' (YAML 1.2's
   * s-b-comment: the comment text is only reachable through the same
   * optional s-separate-in-line group that gates it, matching the
   * general "comment must follow whitespace" rule already enforced by
   * skip_ws_comments elsewhere); '>#comment' with nothing between the
   * header and '#' is not a comment, just an invalid trailing character. */
  bool had_ws = false;
  while (!at_end(ctx) && (cur(ctx) == ' ' || cur(ctx) == '\t')) {
    ctx->pos++;
    had_ws = true;
  }
  if (!at_end(ctx) && cur(ctx) == '#' && had_ws)
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

/* Outcome of scan_block_scalar_line, shared by parse_block_scalar_content
 * (literal |) and parse_folded_scalar_content (folded >): what the caller
 * should do with the line just classified. */
typedef enum {
  SCAN_BLOCK_LINE_ERROR,  /* ctx->error already set; caller cleans up and
                              returns false. */
  SCAN_BLOCK_LINE_END,    /* Scalar ends before this line (a first non-
                              blank line not indented past parent_indent,
                              or a later line less indented than
                              block_indent); ctx->pos rewound to this
                              line's own start so the caller's own caller
                              sees it as unconsumed input. */
  SCAN_BLOCK_LINE_BLANK,  /* An empty (or, before indent_determined, not-
                              yet-disambiguated) line; *spaces_out is
                              meaningless here (both callers already know
                              a blank line's own excess indentation is
                              zero, or don't need it). ctx->pos sits
                              exactly at the line's own end (EOF, '\r', or
                              '\n'); the caller still owns advancing past
                              it, matching each caller's own existing
                              mechanism for doing so. */
  SCAN_BLOCK_LINE_CONTENT /* A genuine content line; *spaces_out receives
                              its own already-consumed leading-space
                              count (>= block_indent) and ctx->pos sits
                              just past those spaces, ready for the
                              caller to read the line's own text. */
} scan_block_line_t;

/*
 * Measure one block-scalar line's leading spaces and classify it, mutating
 * indent_determined, block_indent, and max_leading_blank_spaces exactly as
 * parse_block_scalar_content's and parse_folded_scalar_content's own,
 * previously separately hand-duplicated, per-line scanning logic did:
 * YAML 1.2 sec. 6.1's tab-as-indentation-is-ambiguous-while-undetermined
 * rule, sec. 8.1.1's "no leading empty line may be more indented than the
 * first non-empty line" rule, and the auto-detect-from-first-content-line
 * rule are all enforced here, once, rather than kept in sync by hand across
 * both callers.
 *
 * Deliberately stops short of unifying what happens AFTER classification
 * (committing buffered blank lines, reading a content line's own text, and
 * advancing past the line's own trailing newline): the two callers do this
 * differently enough (a single streaming ybuf_t vs. a collect-then-fold
 * line_t array) that unifying it too would trade this file's own "narrow,
 * independently verified" refactoring discipline for a much larger, riskier
 * change to code with a documented history of subtle tab/indentation bugs,
 * for little further benefit.
 */
static scan_block_line_t scan_block_scalar_line(
    parse_ctx_t *ctx, int parent_indent, bool *indent_determined,
    int *block_indent, int *max_leading_blank_spaces, int *spaces_out) {
  size_t line_start = ctx->pos;
  size_t space_count = 0;
  while (!at_end(ctx) && cur(ctx) == ' ') {
    space_count++;
    ctx->pos++;
  }
  /* Saturate rather than let a line with more than INT_MAX leading spaces
   * silently overflow a signed int; mirrors current_col()'s own identical
   * saturation and its doc comment's rationale, which this later-added,
   * independent counter did not originally share. */
  int spaces = space_count > (size_t)INT_MAX ? INT_MAX : (int)space_count;

  /* See parse_block_scalar_content's own, more detailed doc comment
   * (historically carried on this exact check) for the full YAML 1.2 sec.
   * 6.1 rationale: a tab as this line's very first character while
   * block_indent is still undetermined is genuinely ambiguous and a hard
   * error; the moment even one real space has been consumed, or once
   * block_indent is already established, a tab is ordinary content
   * instead, left for the caller's own content-reading step. */
  if (!*indent_determined && spaces == 0 && !at_end(ctx) && cur(ctx) == '\t') {
    parse_err(ctx,
              "tab cannot be used as block scalar indentation at "
              "position %zu",
              ctx->pos);
    return SCAN_BLOCK_LINE_ERROR;
  }

  /* A document-start ('---') or document-end ('...') marker at column 0
   * always terminates the block scalar (YAML 1.2 sec. 6.9: a document
   * marker is c-forbidden, never legal as scalar content). This must be
   * checked before the indent-determination/content-vs-end logic below:
   * when the block's own indentation is auto-detected as exactly 0 (a
   * document-root scalar with content flush at column 0), a marker line
   * also has spaces == 0, so it would otherwise satisfy that logic's own
   * "this is a(nother) content line" criteria and be silently absorbed as
   * scalar text instead of ending the scalar (and the document). Every
   * other block-content parser in this file (parse_block_dictionary,
   * parse_one_document, parse_plain_scalar_multiline, both quoted-scalar
   * folders) already guards against this via at_doc_marker(); this
   * function previously had no equivalent check at all. at_doc_marker()
   * itself already requires column 0, so this is a no-op whenever
   * spaces > 0 (a more-indented line can never be a marker). */
  if (spaces == 0 && at_doc_marker(ctx)) {
    ctx->pos = line_start;
    return SCAN_BLOCK_LINE_END;
  }

  /* Once block_indent is known, a line with MORE spaces than block_indent
   * is never "blank" for chomping/folding purposes, even with no other
   * content: YAML 1.2's own l-strip-empty grammar production only ever
   * matches a chompable/foldable trailing blank line whose indentation is
   * AT MOST block_indent; a more-indented line, even an all-whitespace
   * one, is ordinary literal content instead. A leading blank line
   * (indent not yet determined) is always still a candidate: the error
   * check below, once indent IS determined, guarantees every leading
   * blank line's own indentation was already <= the eventual
   * block_indent. */
  bool is_blank =
      at_eol(ctx) && (!*indent_determined || spaces <= *block_indent);

  if (is_blank) {
    if (!*indent_determined && spaces > *max_leading_blank_spaces)
      *max_leading_blank_spaces = spaces;
    return SCAN_BLOCK_LINE_BLANK;
  }

  if (!*indent_determined) {
    if (spaces <= parent_indent) {
      /* First non-blank line is not more indented than parent: the scalar
       * is empty. */
      ctx->pos = line_start;
      return SCAN_BLOCK_LINE_END;
    }
    if (*max_leading_blank_spaces > spaces) {
      parse_err(ctx,
                "a leading empty line in a block scalar is more "
                "indented than the first non-empty line at position %zu",
                line_start);
      return SCAN_BLOCK_LINE_ERROR;
    }
    *block_indent = spaces;
    *indent_determined = true;
    *spaces_out = spaces;
    return SCAN_BLOCK_LINE_CONTENT;
  }

  if (spaces < *block_indent) {
    /* This line is less indented than the block: the scalar ends here. */
    ctx->pos = line_start;
    return SCAN_BLOCK_LINE_END;
  }
  *spaces_out = spaces;
  return SCAN_BLOCK_LINE_CONTENT;
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
  if (indent_determined) {
    /* The explicit indentation indicator is always relative to an
     * effective parent indentation of at least 0: -1 is only a sentinel
     * for "no enclosing block context" (document root), used below to let
     * auto-detected indentation be 0; an indicator's own arithmetic has no
     * such negative-base case (verified against a reference parser). */
    int effective_parent = parent_indent < 0 ? 0 : parent_indent;
    block_indent = effective_parent + explicit_indent;
  }

  ybuf_t b;
  yb_init(&b, ctx->mp);

  /* Buffers every not-yet-committed trailing line (each blank line's own
   * excess indentation beyond block_indent, plus a '\n', in order; and the
   * single '\n' that ends whatever the most recent content line was) until
   * either more content arrives (flushed into b verbatim, preserving each
   * line's own excess spaces) or the scalar ends (handled by chomping
   * below). Deferring is what lets chomping decide, only once the whole
   * scalar has been seen, whether these trailing lines are kept in full
   * (CHOMP_KEEP), or discarded down to at most one newline (CHOMP_CLIP) or
   * none at all (CHOMP_STRIP). */
  ybuf_t pending;
  yb_init(&pending, ctx->mp);
  bool have_content = false;

  /* Whether the most recently processed line (blank or content) had a
   * REAL line-break character following it in the source, as opposed to
   * the input simply ending right there. YAML 1.2's own b-chomped-last
   * grammar production (governing the scalar's very last line under
   * every chomp mode, CLIP and KEEP included) is "b-as-line-feed | <end
   * of file>"; when the source has no line break at all after the
   * scalar's last line, nothing is added on that line's account, not
   * even a chomp-mandated one, so CLIP/KEEP must not synthesize a
   * newline the input never actually had. */
  bool had_trailing_newline = false;

  /* Like had_trailing_newline, but updated only when a genuine CONTENT
   * line is processed (never by a trailing blank line). YAML 1.2's
   * b-chomped-last governs the break after the scalar's very last TEXT
   * line specifically, not whatever trailing blank line happens to be
   * processed last; had_trailing_newline alone cannot distinguish "the
   * last content line had a real break, but is followed by an
   * EOF-terminated blank line with none of its own" from "the last
   * content line itself had no break", since it is unconditionally
   * overwritten by every line, blank or not. CLIP (the only mode that
   * needs this distinction) reads this variable instead. */
  bool content_had_trailing_newline = false;

  /* Tracks the most-indented LEADING empty line seen so far, while the
   * block's own indentation is still auto-detected (explicit_indent <= 0)
   *; YAML 1.2 sec. 8.1.1: "It is an error for any of the leading empty
   * lines to contain more spaces than the first non-empty line." Only
   * meaningful before indent_determined; a blank line's indentation is
   * unconstrained once real content (or an explicit indicator) has
   * already fixed block_indent. */
  int max_leading_blank_spaces = -1;

  while (!at_end(ctx)) {
    size_t line_start = ctx->pos;
    int spaces = 0;
    scan_block_line_t kind = scan_block_scalar_line(
        ctx, parent_indent, &indent_determined, &block_indent,
        &max_leading_blank_spaces, &spaces);

    if (kind == SCAN_BLOCK_LINE_ERROR) {
      _mem_free(b.m_procs, b.buf);
      _mem_free(pending.m_procs, pending.buf);
      return false;
    }
    if (kind == SCAN_BLOCK_LINE_END) break;

    if (kind == SCAN_BLOCK_LINE_BLANK) {
      had_trailing_newline = !at_end(ctx);
      if (had_trailing_newline) yb_append_c(&pending, '\n');
      ctx->pos = line_start;
      skip_to_eol(ctx);
      continue;
    }

    /* SCAN_BLOCK_LINE_CONTENT. Commit every buffered blank/newline line
     * before this one, each with its own excess indentation intact. */
    if (pending.len > 0) yb_append(&b, pending.buf, pending.len);
    pending.len = 0;
    have_content = true;

    /* Skip exactly block_indent spaces (already consumed above, but we
     * may have consumed more than block_indent). */
    int extra = spaces - block_indent;

    /* Append extra leading spaces (for more-indented lines). */
    for (int i = 0; i < extra; i++) yb_append_c(&b, ' ');

    /* Read the content of this line. */
    while (!at_end(ctx) && !at_eol(ctx)) {
      if (is_disallowed_control_byte(cur(ctx))) {
        parse_err(ctx,
                  "raw control character 0x%02x is not allowed in a "
                  "block scalar at position %zu",
                  (unsigned char)cur(ctx), ctx->pos);
        _mem_free(b.m_procs, b.buf);
        _mem_free(pending.m_procs, pending.buf);
        return false;
      }
      yb_append_c(&b, cur(ctx));
      ctx->pos++;
    }

    /* This line's own trailing newline is deferred too, exactly like a
     * blank line's: it may yet turn out to be the scalar's very last
     * line, whose fate chomping alone decides. See had_trailing_newline's
     * own doc comment above: if the source has no real line break here
     * at all (input simply ends), nothing is deferred on this line's
     * account. */
    had_trailing_newline = !at_end(ctx);
    content_had_trailing_newline = had_trailing_newline;
    if (had_trailing_newline) yb_append_c(&pending, '\n');
    skip_newline(ctx);
  }

  /* Apply chomping. */
  switch (chomp) {
    case CHOMP_STRIP:
      /* No trailing newlines: pending is discarded in full. */
      break;
    case CHOMP_CLIP:
      /* One trailing newline if there was any content AND the LAST
       * CONTENT line actually had a real line break there to preserve
       * (see content_had_trailing_newline's own doc comment): content
       * whose very last text line was also the input's very last byte,
       * with no line break at all, gets none, even under CLIP. A
       * trailing blank line with no break of its own must not suppress
       * this, since the content line before it did have one. pending's
       * own contents (which may hold several blank lines' worth of
       * excess indentation) are discarded either way. */
      if (have_content && content_had_trailing_newline) yb_append_c(&b, '\n');
      break;
    case CHOMP_KEEP:
      /* All trailing lines, including those preceding the first content
       * line and each one's own excess indentation.  YAML 1.2 sec.
       * 8.1.1.2: trailing empty lines are part of the scalar's content
       * regardless of whether any non-empty lines appear. pending
       * already omits a final '\n' with no real line break behind it
       * (had_trailing_newline's own doc comment), so no separate check
       * is needed here. */
      if (pending.len > 0) yb_append(&b, pending.buf, pending.len);
      break;
  }

  bool pending_oom = pending.oom;
  _mem_free(pending.m_procs, pending.buf);

  if (b.oom || pending_oom) {
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
  if (indent_determined) {
    /* See the identical comment in parse_block_scalar_content: -1 is only
     * a sentinel for auto-detection at the document root, not a real base
     * for the indicator's own arithmetic. */
    int effective_parent = parent_indent < 0 ? 0 : parent_indent;
    block_indent = effective_parent + explicit_indent;
  }

  /* Collect lines as a dynamic array of {content, extra_indent, is_blank}.
   * text/text_len are a borrowed span directly into ctx->src, not an owned
   * copy: by the time a content line is scanned below, ctx->pos has
   * already advanced past this line's own leading spaces (both the
   * mandatory block_indent portion and any "extra" more-indented portion,
   * which is still real scalar content per YAML 1.2 sec. 8.1.3), so that
   * whole span - extra spaces included - sits contiguously, unmodified,
   * in ctx->src already; ctx->src is never mutated or freed before this
   * function returns, so borrowing directly avoids a per-line allocate/
   * copy/free cycle for what could be a many-thousand-line wrapped
   * scalar, mirroring fold_quoted_newline's identical avoid-a-copy-when-
   * the-source-is-already-stable reasoning for quoted-scalar folding. */
  typedef struct {
    const char *text;
    size_t text_len;
    int extra;
    bool blank;
    bool spaced;
  } line_t;

  size_t lines_cap = 16, lines_len = 0;
  line_t *lines = _mem_alloc(ctx->mp, lines_cap * sizeof(line_t));
  if (!lines) return false;

  /* See the identical tracker in parse_block_scalar_content: YAML 1.2
   * sec. 8.1.1 forbids a leading empty line from being more indented than
   * the first non-empty line, while the block's own indentation is still
   * auto-detected. */
  int max_leading_blank_spaces = -1;

  /* See had_trailing_newline's own doc comment in
   * parse_block_scalar_content: reflects whichever line was most
   * recently collected, so it holds the scalar's true final line's own
   * status once the loop below exits. */
  bool had_trailing_newline = false;

  while (!at_end(ctx)) {
    size_t line_start = ctx->pos;
    int spaces = 0;
    scan_block_line_t kind = scan_block_scalar_line(
        ctx, parent_indent, &indent_determined, &block_indent,
        &max_leading_blank_spaces, &spaces);

    if (kind == SCAN_BLOCK_LINE_ERROR) {
      _mem_free(ctx->mp, lines);
      return false;
    }
    if (kind == SCAN_BLOCK_LINE_END) break;

    bool is_blank = (kind == SCAN_BLOCK_LINE_BLANK);

    /* Grow lines array if needed. */
    if (lines_len == lines_cap) {
      lines_cap *= 2;
      line_t *tmp = _mem_realloc(ctx->mp, lines, lines_cap * sizeof(line_t));
      if (!tmp) {
        _mem_free(ctx->mp, lines);
        return false;
      }
      lines = tmp;
    }

    line_t *ln = &lines[lines_len++];
    ln->blank = is_blank;
    /* A genuinely blank line (is_blank's own updated definition above
     * already excludes any line more indented than block_indent, which
     * is classified as ordinary content instead) never has an excess of
     * its own: it is either a leading blank line (indent not yet
     * determined, and the error check above guarantees its indentation
     * was already <= the eventual block_indent) or a trailing/inter-
     * content blank line whose own indentation is, by is_blank's own
     * definition, already <= block_indent. */
    ln->extra = is_blank ? 0 : spaces - block_indent;
    /* YAML 1.2 sec. 8.1.3's "more-indented" (s-nb-spaced-text) line is
     * s-indent(n) s-white nb-char*, and s-white is space OR tab - so a
     * line whose first byte past the mandatory block_indent spaces is a
     * tab is also "spaced" for fold-vs-newline purposes, even though it
     * adds no extra literal SPACE of its own (ln->extra stays 0, since
     * the leading-space counter above only ever counts ' '). The tab
     * itself still ends up as ordinary line content below, unaffected by
     * this flag; only the fold decision further down consults it. */
    ln->spaced =
        !is_blank && (ln->extra > 0 || (!at_end(ctx) && cur(ctx) == '\t'));
    ln->text = NULL;
    ln->text_len = 0;

    if (!is_blank) {
      /* ctx->pos already sits ln->extra bytes past where this line's own
       * "extra" (more-than-block_indent) leading spaces begin, per
       * scan_block_scalar_line's own contract; those spaces, and the real
       * content that immediately follows them, are therefore already one
       * contiguous, unmodified span of ctx->src (see line_t's own doc
       * comment above), so span_start is simply ln->extra bytes behind
       * ctx->pos, with no copy required. */
      const char *span_start = ctx->src + (ctx->pos - (size_t)ln->extra);
      bool bad_byte = false;
      while (!at_end(ctx) && !at_eol(ctx)) {
        if (is_disallowed_control_byte(cur(ctx))) {
          parse_err(ctx,
                    "raw control character 0x%02x is not allowed in a "
                    "block scalar at position %zu",
                    (unsigned char)cur(ctx), ctx->pos);
          bad_byte = true;
          break;
        }
        ctx->pos++;
      }
      if (bad_byte) {
        _mem_free(ctx->mp, lines);
        return false;
      }
      ln->text = span_start;
      ln->text_len = (size_t)((ctx->src + ctx->pos) - span_start);
    }
    /* See had_trailing_newline's own doc comment in
     * parse_block_scalar_content: whether THIS line had a real line
     * break following it in the source, as opposed to the input simply
     * ending right there, matters for whichever line turns out to be the
     * scalar's very last one. */
    had_trailing_newline = !at_end(ctx);
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
      /* Blank lines before this line (leading or inter-content): emit
       * each one's own excess indentation (if any), preserved as literal
       * spaces, immediately before its own newline. */
      for (size_t j = i - trailing_blanks; j < i; j++) {
        for (int k = 0; k < lines[j].extra; k++) yb_append_c(&b, ' ');
        yb_append_c(&b, '\n');
      }
      /* Per YAML 1.2 sec. 8.1.3, a transition into or out of a
       * more-indented ("spaced") run of lines costs one unconditional
       * literal newline ON TOP OF however many blank lines separate the
       * two chunks: additive with the blank-line newlines just emitted
       * above, not an alternative to them (verified against a reference
       * parser). Only applicable once there is a preceding content line
       * for this break to be adjacent to; leading blank lines before the
       * scalar's own very first content line have no such preceding line,
       * so have_content guards this the same way it already gates the
       * identical check in the sibling branch below. */
      if (have_content && (ln->spaced || prev_extra)) yb_append_c(&b, '\n');
    } else if (have_content) {
      /* A line break adjacent to a more-indented line (either the current
       * line OR the previous non-blank line) is preserved as a newline per
       * YAML 1.2 spec section 8.1.1.2; otherwise it folds to a space. */
      if (ln->spaced || prev_extra)
        yb_append_c(&b, '\n');
      else
        yb_append_c(&b, ' ');
    }
    trailing_blanks = 0;
    prev_extra = ln->spaced;
    yb_append(&b, ln->text, ln->text_len);
    have_content = true;
  }

  /* Apply chomping. lines[] (and each line's own excess indentation) must
   * stay alive through CHOMP_KEEP's own use of it below, so freeing it is
   * deferred past this switch rather than done immediately after folding. */
  switch (chomp) {
    case CHOMP_STRIP:
      break;
    case CHOMP_CLIP:
      /* trailing_blanks > 0 means at least one blank line follows the
       * last real content line, which can only be true if a genuine line
       * break already separated them in the source - so the content
       * line's own break must be preserved even if that LAST trailing
       * blank line itself has none (had_trailing_newline, unconditionally
       * overwritten by every line including a trailing blank one, cannot
       * tell the two apart on its own). Mirrors KEEP's own identical,
       * already-correct check just below. See had_trailing_newline's own
       * doc comment in parse_block_scalar_content for the "no newline at
       * all when the source had none" case this still preserves. */
      if (have_content && (trailing_blanks > 0 || had_trailing_newline))
        yb_append_c(&b, '\n');
      break;
    case CHOMP_KEEP:
      /* First, the newline ending the last real content line itself: it
       * is unconditional whenever any trailing blank line follows it
       * (only the scalar's true final line can possibly lack a real
       * source line break, and a trailing blank line, if any, is always
       * that final line instead of this one). */
      if (have_content && (trailing_blanks > 0 || had_trailing_newline))
        yb_append_c(&b, '\n');
      /* Then each trailing blank line (lines[] entries after the last
       * content line, never flushed by the main fold loop above since no
       * further content line ever arrived to trigger that flush): its
       * own excess indentation, then its own newline; conditioned on
       * had_trailing_newline only for the very last one, for the
       * identical reason. */
      for (size_t j = lines_len - trailing_blanks; j < lines_len; j++) {
        for (int k = 0; k < lines[j].extra; k++) yb_append_c(&b, ' ');
        if (j + 1 < lines_len || had_trailing_newline) yb_append_c(&b, '\n');
      }
      break;
  }

  /* Free the lines[] array itself; each entry's own text/text_len is a
   * borrowed span into ctx->src (see line_t's own doc comment above), not
   * an owned allocation, so there is nothing per-entry to free here. */
  _mem_free(ctx->mp, lines);

  if (b.oom) {
    _mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

/* Plain scalar (block and flow context) */

/*
 * Scan one physical line's worth of plain-scalar content from the current
 * position, appending characters to *b (already-initialized).  Shared by
 * parse_plain_scalar (the first line) and parse_plain_scalar_multiline
 * (every continuation line), since a continuation line is subject to the
 * identical per-character termination rules as the first.
 *
 * line_start is the position this line's content scan began at (needed so
 * a ':' at the very first character scanned isn't mistaken for "preceded
 * by content"; only relevant to the '#' comment check below, which does
 * not apply to ':').
 *
 * Returns true if a real terminator (an inline comment, or a ':'/flow
 * character value/collection indicator) was hit, with ctx->pos left
 * exactly at that terminator, not consumed.  Returns false if the line
 * ended at EOL or end-of-input instead, with ctx->pos left there.
 */
static bool scan_plain_scalar_line(parse_ctx_t *ctx, bool in_flow,
                                   size_t line_start, ybuf_t *b) {
  while (!at_end(ctx)) {
    char c = cur(ctx);

    /* End-of-line terminates this line, not the whole scalar. */
    if (c == '\n' || c == '\r') return false;

    /* Inline comment terminator: space followed by '#'. */
    if (c == '#' && ctx->pos > line_start) {
      char prev = ctx->src[ctx->pos - 1];
      if (prev == ' ' || prev == '\t') return true;
    }

    /* Colon+space or colon+EOL terminates (dictionary value indicator). */
    if (c == ':') {
      if (ctx->pos + 1 >= ctx->len) return true; /* colon at very end */
      char next = ctx->src[ctx->pos + 1];
      if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
        return true;
      /* In flow context ':' is also a value indicator when followed by a
       * flow terminator.  Whitespace already handled by the check above. */
      if (in_flow) {
        char safe = ctx->src[ctx->pos + 1]; /* pos+1 < len guaranteed above */
        if (safe == ',' || safe == ']' || safe == '}') return true;
      }
    }

    /* Flow terminators. */
    if (in_flow && (c == ',' || c == ']' || c == '}')) return true;

    if (is_disallowed_control_byte(c)) {
      /* A genuine, unrecoverable error, unlike every other return in this
       * function: those all just report where an ordinary terminator was
       * (or wasn't) found, for a caller that may still go on to interpret
       * this line as something other than a plain scalar (e.g. its own
       * key-detection logic). ctx->error is what callers check to tell
       * the two apart (see parse_plain_scalar's own doc comment). */
      parse_err(ctx,
                "raw control character 0x%02x is not allowed in a "
                "plain scalar at position %zu",
                (unsigned char)c, ctx->pos);
      return false;
    }

    yb_append_c(b, c);
    ctx->pos++;
  }
  return false; /* reached end of input, not a real terminator */
}

/*
 * Parse a single-line plain scalar.  The scalar ends at end-of-line, an
 * inline comment (' #'), or a value indicator (': ' or ':\n').
 *
 * In flow context (in_flow=true), also ends at ',' / ']' / '}', and at ':'
 * when ':' is followed by whitespace or a flow terminator.  A bare ':'
 * not followed by one of those characters is part of the scalar (e.g. URLs
 * such as "http://example.com") per YAML 1.2 section 7.3.3.
 *
 * hit_real_terminator, if non-NULL, reports whether the scalar stopped at
 * a real terminator (true) as opposed to EOL/end-of-input (false);
 * needed by callers that may want to attempt multi-line continuation
 * (see parse_plain_scalar_multiline) only in the latter case, since an
 * implicit dictionary key is always restricted to a single line and must
 * never attempt continuation regardless of what follows.
 */
static bool parse_plain_scalar(parse_ctx_t *ctx, bool in_flow, char **out,
                               bool *hit_real_terminator) {
  ybuf_t b;
  yb_init(&b, ctx->mp);

  size_t start = ctx->pos;
  bool terminated = scan_plain_scalar_line(ctx, in_flow, start, &b);
  if (ctx->error[0]) {
    /* A genuine error (e.g. a raw control byte), not merely "this line
     * ended without a real terminator"; see scan_plain_scalar_line's own
     * doc comment for why this is the one case its return value alone
     * cannot signal. */
    _mem_free(b.m_procs, b.buf);
    return false;
  }
  if (hit_real_terminator) *hit_real_terminator = terminated;

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

/*
 * Extend an already-parsed plain scalar's first line with further
 * continuation lines (YAML 1.2 sec. 7.3.3, ns-plain-multi-line); used
 * only once a value/standalone-node position has confirmed the first line
 * does NOT introduce a dictionary key (an implicit key is always
 * single-line; see parse_plain_scalar's own doc comment), since the first
 * line must be inspected for a following ':' before it is safe to decide
 * whether continuation should even be attempted.
 *
 * Takes ownership of first_owned (the first line's already-parsed text,
 * from parse_plain_scalar) and first_terminated (whether that call hit a
 * real terminator, as opposed to EOL/end-of-input; continuation is only
 * ever attempted in the latter case).  On success, *out receives the
 * final, possibly-extended text (still first_owned itself, unmodified, if
 * no continuation applied).
 *
 * Only if the first line ended at EOL/end-of-input rather than a real
 * terminator, greedily consumes further lines indented more than `indent`
 * (the enclosing block's own indent; the same parameter already
 * threaded through parse_node/parse_block_map_node, so a value
 * continuation is never confused with an unrelated sibling entry at or
 * below it) or blank.
 *
 * Folding: unlike a '>' folded block scalar, a plain scalar's line-folding
 * grammar (s-flow-folded) has no "an extra-indented line preserves its own
 * newline" exception; every line transition folds to a single space, or
 * to as many newlines as there were consecutive blank lines, regardless of
 * how much further indented a continuation line happens to be. Any
 * continuation line's own leading whitespace beyond `indent` is therefore
 * simply not preserved.
 *
 * Continuation stops (without consuming) at a document marker ('---'/'...'
 * at column 0), at a line indented at or below `indent`, or the moment a
 * continuation line itself hits a real terminator (mirroring the first
 * line's own termination rules).
 */
static bool parse_plain_scalar_multiline(parse_ctx_t *ctx, bool in_flow,
                                         int indent, char *first_owned,
                                         bool first_terminated, char **out) {
  if (first_terminated || at_end(ctx)) {
    *out = first_owned;
    return true;
  }

  ybuf_t b;
  yb_init(&b, ctx->mp);
  yb_append_cstr(&b, first_owned);
  _mem_free(ctx->mp, first_owned);

  size_t blank_count = 0;
  while (!at_end(ctx)) {
    size_t save = ctx->pos;
    skip_newline(ctx);

    size_t line_start = ctx->pos;
    size_t line_space_count = 0;
    while (!at_end(ctx) && cur(ctx) == ' ') {
      line_space_count++;
      ctx->pos++;
    }
    /* Saturate rather than let a continuation line with more than INT_MAX
     * leading spaces silently overflow a signed int; mirrors current_col()'s
     * own identical saturation (and scan_block_scalar_line's matching fix
     * for its own independent leading-space counter). */
    int spaces =
        line_space_count > (size_t)INT_MAX ? INT_MAX : (int)line_space_count;

    if (at_end(ctx)) {
      ctx->pos = save;
      break;
    }
    if (at_blank_line(ctx)) {
      /* Blank line: counts toward folding: keep scanning. at_blank_line()
       * (not a bare at_eol() check on the byte right after the leading
       * spaces) is what recognizes a line as blank even when its own
       * remaining whitespace is a tab rather than more spaces; a raw
       * at_eol() check here cannot see past such a tab to the real line
       * break behind it. Mirrors the identical fix already applied to
       * double-/single-quoted scalar folding (see at_blank_line's own doc
       * comment). Advance past the remaining whitespace up to (but not
       * including) the line terminator itself, so the loop's own
       * skip_newline() call at the top of the next iteration consumes
       * exactly one line break, not this line's terminator plus the
       * next line's leading bytes. */
      while (!at_end(ctx) && cur(ctx) != '\n' && cur(ctx) != '\r') ctx->pos++;
      blank_count++;
      continue;
    }
    if (at_doc_marker(ctx)) {
      ctx->pos = save;
      break;
    }
    if (spaces <= indent) {
      ctx->pos = save;
      break;
    }
    /* Beyond the block's own required (space-only) indentation, further
     * separator whitespace (spaces and/or tabs alike; YAML 1.2's
     * s-separate-in-line ::= s-white+, where s-white is either) is not
     * itself scalar content; skip it here the same way the spaces that
     * established the indentation were already fully consumed above
     * regardless of how far past indent they went, so a tab in this
     * position is not fed into scan_plain_scalar_line() below as if it
     * were a literal content byte. */
    while (!at_end(ctx) && (cur(ctx) == ' ' || cur(ctx) == '\t')) ctx->pos++;
    if (in_flow && (cur(ctx) == ']' || cur(ctx) == '}' || cur(ctx) == ',')) {
      /* A flow terminator can never be plain-scalar content; a line whose
       * first non-whitespace character is one is the end of the
       * enclosing flow collection or entry, not a continuation of this
       * scalar (e.g. "baz\n]" must yield the value "baz", not "baz "
       * with a trailing fold-space swallowed from a ']' that was never
       * really content). */
      ctx->pos = save;
      break;
    }
    if (cur(ctx) == ':') {
      char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
      bool is_value_indicator =
          after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
          ctx->pos + 1 >= ctx->len ||
          (in_flow && (after == ',' || after == ']' || after == '}'));
      if (is_value_indicator) {
        /* A ':' value indicator reached only by crossing onto a new line,
         * with nothing else scanned from that line first, is not a
         * continuation of this scalar; an implicit key's own ':' must
         * stay on the SAME line as the key's last real content (verified
         * against two independent reference parsers: "{k\n: v}" is
         * rejected, while "{k: \nv}", where the colon immediately
         * follows the key on its own line and only the VALUE folds onto
         * the next line, is accepted). Leave it for the caller's own
         * key-detection logic to see exactly as if this multi-line
         * attempt had never peeked past the previous line at all. */
        ctx->pos = save;
        break;
      }
    }
    if (cur(ctx) == '#') {
      /* A comment line (a line whose first non-whitespace character is
       * '#', which; like anywhere else; can never legitimately be
       * plain-scalar content of its own) is not a continuation of this
       * scalar; leave it for the caller's own ordinary skip_ws_comments
       * to consume normally, exactly as if this multi-line attempt had
       * never peeked past the previous line at all. */
      ctx->pos = save;
      break;
    }

    /* Genuine continuation line. */
    if (blank_count > 0) {
      for (size_t i = 0; i < blank_count; i++) yb_append_c(&b, '\n');
    } else {
      yb_append_c(&b, ' ');
    }
    blank_count = 0;

    /* Scan this line's own content directly into the shared buffer b
     * instead of a throwaway per-line ybuf_t: mirrors fold_quoted_newline's
     * own strip_floor technique (used for double-/single-quoted scalar
     * folding) rather than allocating, copying, and freeing a separate
     * buffer for every continuation line of a long wrapped scalar.
     * line_floor marks where this line's own content starts within b
     * (right after the fold separator(s) just appended above), so the
     * trailing-whitespace trim below can never reach back into an earlier
     * line's already-folded content. */
    size_t line_floor = b.len;
    bool cont_terminated = scan_plain_scalar_line(ctx, in_flow, line_start, &b);
    if (ctx->error[0]) {
      /* See parse_plain_scalar's own identical check for why this is
       * distinguished from an ordinary "no real terminator on this line"
       * outcome. */
      _mem_free(b.m_procs, b.buf);
      return false;
    }
    if (!b.oom) {
      while (b.len > line_floor &&
             (b.buf[b.len - 1] == ' ' || b.buf[b.len - 1] == '\t'))
        b.buf[--b.len] = '\0';
    }

    if (cont_terminated) break;
  }

  if (b.oom) {
    _mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

/* Forward declaration; defined below alongside parse_flow_dictionary,
 * which was its first user, but also needed here by try_parse_scalar_
 * dict_key's alias-as-key and flow-collection-as-key handling. */
static char *node_to_dict_key_string(parse_ctx_t *ctx, cyaml_node_t *key_node,
                                     const char *context_label);

/* Forward declaration; defined below alongside the flow serializer, which
 * it is a canonical-mode variant of, but needed here since
 * node_to_dict_key_string (this file's only caller) is defined well before
 * that section. */
static char *serialize_flow_canonical(cyaml_node_t *n);

/* Forward declarations; needed by try_parse_scalar_dict_key's own
 * flow-collection-as-key handling (a flow list/dictionary is unambiguous
 * on its own; no "was this actually a key" backtracking is needed the
 * way a bare plain scalar requires; so calling these directly here,
 * rather than through the generic parse_node dispatch, is deliberate: it
 * sidesteps parse_node's own key-vs-value ambiguity handling entirely,
 * which does not apply to a flow collection in the first place. */
static cyaml_node_t *parse_flow_list(parse_ctx_t *ctx, int indent);
static cyaml_node_t *parse_flow_dictionary(parse_ctx_t *ctx, int indent);

/*
 * Check whether the current position (assumed already skipped past
 * leading whitespace, at the start of a non-blank line) represents a
 * genuine value continuation for a block construct governed by `indent`,
 * as opposed to belonging to a sibling entry (in which case this
 * construct's own value/content is absent, i.e. null).  True whenever
 * more indented than `indent`.
 *
 * seq_ok_at_indent additionally permits exactly `indent` itself, but only
 * when it starts a block sequence entry ('- '); YAML 1.2 sec. 8.2.2's
 * one exception to "a value must be more indented than its key", and
 * specifically only an exception for a MAPPING key's value: the caller
 * must pass true only when `indent` is a mapping's own indent
 * (parse_block_map_node's map_indent, or that same value forwarded
 * through an anchor/tag decorating the value), never a sequence's own
 * indent (parse_block_list's seq_indent); at exactly a sequence's own
 * indent, a further '-' is unambiguously a sibling element of that same
 * sequence, not nested content, regardless of any anchor/tag in between.
 */
static bool at_block_value_col(parse_ctx_t *ctx, int indent,
                               bool seq_ok_at_indent) {
  if (line_indent_has_tab(ctx)) {
    /* A tab is never valid here regardless of what the (meaningless, in
     * this case) column comparison below would conclude; sets ctx->error
     * so callers can distinguish this from an ordinary "not eligible,
     * treat as null" false. */
    parse_err(ctx, "tab cannot be used as block indentation at position %zu",
              ctx->pos);
    return false;
  }
  int col = current_col(ctx);
  if (col > indent) return true;
  if (col < indent) return false;
  if (seq_ok_at_indent && cur(ctx) == '-') {
    char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
    if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
      return true;
  }
  return false;
}

/*
 * True when the current position holds a bare '-' block sequence
 * indicator (immediately followed by whitespace or end of input, as
 * opposed to a '-' that is merely the first character of ordinary plain
 * scalar content, e.g. "-1" or "-foo").
 */
static bool at_bare_seq_indicator(parse_ctx_t *ctx) {
  if (at_end(ctx) || cur(ctx) != '-') return false;
  char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
  return nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0';
}

/*
 * True when the current character is a valid first character for a plain
 * scalar in flow context (YAML 1.2 sec. 6.6, ns-plain-first(c)). Every
 * character is valid except '-' and '?', which additionally require an
 * ns-plain-safe(c) character to immediately follow; i.e. one that could
 * itself legally appear WITHIN a flow plain scalar. Whitespace, end-of-
 * line/input, and any flow indicator (',' '[' ']' '{' '}') all fail that
 * requirement, so a bare "-"/"?" immediately followed by one of those has
 * no valid interpretation as a plain scalar at all in flow context (e.g.
 * the lone "-" in "[-, -]" or "[-]"; verified against a reference
 * parser). Block context has no equivalent gap: a '-'/'?' immediately
 * followed by whitespace/EOL is already claimed earlier, by the block
 * sequence/explicit-key dispatch, before this function would ever be
 * consulted. ':' is deliberately NOT included here even though YAML 1.2
 * gives it the identical grammar restriction: scan_plain_scalar_line
 * already has its own, earlier special case for a ':' reached with
 * nothing yet scanned, returning an empty scalar with the ':' left
 * unconsumed; the exact mechanism parse_flow_list's own bare-colon-key
 * shorthand ("[: value]", an empty implicit key) depends on; rejecting
 * ':' here would pre-empt that mechanism before it ever runs.
 */
static bool at_valid_flow_plain_scalar_start(parse_ctx_t *ctx) {
  char c = cur(ctx);
  if (c != '-' && c != '?') return true;
  if (ctx->pos + 1 >= ctx->len) return false;
  char next = ctx->src[ctx->pos + 1];
  return next != ' ' && next != '\t' && next != '\n' && next != '\r' &&
         next != ',' && next != '[' && next != ']' && next != '{' &&
         next != '}';
}

/*
 * Skip a tag token starting at '!' (already confirmed present at the
 * current position by the caller).  Handles both the shorthand forms
 * ("!", "!foo", "!!foo") and the verbatim form ("!<...>"), whose content
 * runs through the closing '>' and may legitimately contain characters
 * (including a literal ',') that a shorthand tag's own character set
 * excludes; verbatim syntax exists specifically to allow such
 * characters, so the two forms cannot share one termination rule.
 */
static void skip_tag_token(parse_ctx_t *ctx) {
  ctx->pos++; /* consume the leading '!' */
  if (!at_end(ctx) && cur(ctx) == '<') {
    ctx->pos++;
    while (!at_end(ctx) && cur(ctx) != '>' && cur(ctx) != '\n' &&
           cur(ctx) != '\r')
      ctx->pos++;
    if (!at_end(ctx) && cur(ctx) == '>') ctx->pos++;
    return;
  }
  if (!at_end(ctx) && cur(ctx) == '!') ctx->pos++;
  /* A shorthand tag token never contains a flow indicator (YAML 1.2 sec.
   * 5.5, ns-tag-char excludes c-flow-indicator); without this check, a tag
   * directly followed by ',' / '[' / ']' / '{' / '}' in flow context
   * (e.g. "!!str,") would swallow that indicator as if it were part of
   * the tag name, desynchronizing the caller's own flow-terminator scan. */
  while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
         cur(ctx) != '\n' && cur(ctx) != '\r' && cur(ctx) != ',' &&
         cur(ctx) != '[' && cur(ctx) != ']' && cur(ctx) != '{' &&
         cur(ctx) != '}')
    ctx->pos++;
}

/*
 * Percent-decode a verbatim tag's content in place (YAML 1.2 sec. 5.6/5.7's
 * ns-uri-char permits a "%" followed by two hex digits as an escape for
 * characters the URI grammar otherwise excludes). Rejects a %00 escape
 * outright: a node's tag is a plain NUL-terminated char* with no separate
 * length field, so decoding it into a real embedded NUL byte would
 * silently truncate the tag rather than erroring (exactly the silent-
 * data-corruption shape this library's own no-embedded-null-bytes policy
 * exists to prevent). Returns false (with a parse error already set) on a
 * malformed escape or an embedded NUL; true otherwise, with text
 * shortened in place as needed. */
static bool percent_decode_tag_inplace(parse_ctx_t *ctx, char *text,
                                       size_t pos_for_err) {
  char *w = text;
  for (const char *r = text; *r; r++) {
    if (*r != '%') {
      *w++ = *r;
      continue;
    }
    char h1 = r[1];
    char h2 = h1 ? r[2] : '\0';
    int hi = -1, lo = -1;
    if (h1 >= '0' && h1 <= '9')
      hi = h1 - '0';
    else if (h1 >= 'a' && h1 <= 'f')
      hi = h1 - 'a' + 10;
    else if (h1 >= 'A' && h1 <= 'F')
      hi = h1 - 'A' + 10;
    if (h2 >= '0' && h2 <= '9')
      lo = h2 - '0';
    else if (h2 >= 'a' && h2 <= 'f')
      lo = h2 - 'a' + 10;
    else if (h2 >= 'A' && h2 <= 'F')
      lo = h2 - 'A' + 10;
    if (hi < 0 || lo < 0) {
      parse_err(ctx, "malformed percent-escape in tag at position %zu",
                pos_for_err);
      return false;
    }
    int byte = (hi << 4) | lo;
    if (byte == 0) {
      parse_err(ctx, "tag contains a percent-escaped null byte at position %zu",
                pos_for_err);
      return false;
    }
    *w++ = (char)byte;
    r += 2;
  }
  *w = '\0';
  return true;
}

/*
 * Parse a tag token starting at '!' (already confirmed present at the
 * current position by the caller) and resolve it to a single, fully-
 * normalized tag string. Handles both shorthand forms ("!", "!foo",
 * "!!foo", "!name!foo") and the verbatim form ("!<...>"), mirroring
 * skip_tag_token's own tokenization exactly but capturing and resolving
 * the content instead of only skipping it.
 *
 * On success, *resolved_out receives an owned, fully-resolved tag string
 * (the caller must free it), or NULL for a bare "!" non-specific tag
 * (resolves to no forced type, treated identically to no tag at all).
 * Returns false (with a parse error already set via parse_err(), on every
 * failure path including an allocation failure) on a malformed token, a
 * shorthand handle with no suffix, an undefined named handle, an
 * allocation failure, or (for a verbatim tag) a malformed/null-producing
 * percent-escape.  Always setting ctx->error on failure matters beyond
 * this function's own direct callers: try_parse_scalar_dict_key() calls
 * this speculatively and relies on ctx->error[0] alone to distinguish "a
 * genuine error" from "not a key after all", since ctx->pos is not
 * restored on this path.
 */
static bool parse_tag_token(parse_ctx_t *ctx, char **resolved_out) {
  size_t tag_start = ctx->pos;
  ctx->pos++; /* consume the leading '!' */

  if (!at_end(ctx) && cur(ctx) == '<') {
    /* Verbatim form: !<content> */
    ctx->pos++;
    size_t content_start = ctx->pos;
    while (!at_end(ctx) && cur(ctx) != '>' && cur(ctx) != '\n' &&
           cur(ctx) != '\r')
      ctx->pos++;
    size_t content_len = ctx->pos - content_start;
    if (at_end(ctx) || cur(ctx) != '>') {
      parse_err(ctx, "unterminated verbatim tag at position %zu", tag_start);
      return false;
    }
    ctx->pos++; /* consume '>' */
    if (content_len == 0) {
      parse_err(ctx, "empty verbatim tag at position %zu", tag_start);
      return false;
    }
    char *content = _mem_alloc(ctx->mp, content_len + 1);
    if (!content) {
      parse_err(ctx, "out of memory parsing verbatim tag at position %zu",
                tag_start);
      return false;
    }
    memcpy(content, ctx->src + content_start, content_len);
    content[content_len] = '\0';
    if (!percent_decode_tag_inplace(ctx, content, content_start)) {
      _mem_free(ctx->mp, content);
      return false;
    }
    *resolved_out = content;
    return true;
  }

  /* Shorthand form: determine which of primary "!", secondary "!!", or a
   * named "!name!" introduces the token. Secondary is checked first since
   * it is unambiguous (a bare second '!' can never begin a named handle's
   * own word-char name, which by definition cannot be empty); otherwise a
   * speculative lookahead scans ns-word-char+ (alnum/'-') looking for a
   * closing '!' to confirm a named handle, falling back to primary
   * (leaving ctx->pos untouched by the lookahead) when none is found. */
  size_t handle_len = 1;
  if (!at_end(ctx) && cur(ctx) == '!') {
    handle_len = 2;
    ctx->pos++;
  } else {
    size_t scan = ctx->pos;
    while (scan < ctx->len) {
      char sc = ctx->src[scan];
      bool is_word = (sc >= 'a' && sc <= 'z') || (sc >= 'A' && sc <= 'Z') ||
                     (sc >= '0' && sc <= '9') || sc == '-';
      if (!is_word) break;
      scan++;
    }
    if (scan > ctx->pos && scan < ctx->len && ctx->src[scan] == '!') {
      handle_len = (scan - tag_start) + 1;
      ctx->pos = scan + 1;
    }
  }

  char *handle = _mem_alloc(ctx->mp, handle_len + 1);
  if (!handle) {
    parse_err(ctx, "out of memory parsing tag handle at position %zu",
              tag_start);
    return false;
  }
  memcpy(handle, ctx->src + tag_start, handle_len);
  handle[handle_len] = '\0';

  /* A shorthand tag token never contains a flow indicator (YAML 1.2 sec.
   * 5.5, ns-tag-char excludes c-flow-indicator); see skip_tag_token's own
   * identical comment for why. */
  size_t suffix_start = ctx->pos;
  while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
         cur(ctx) != '\n' && cur(ctx) != '\r' && cur(ctx) != ',' &&
         cur(ctx) != '[' && cur(ctx) != ']' && cur(ctx) != '{' &&
         cur(ctx) != '}')
    ctx->pos++;
  size_t suffix_len = ctx->pos - suffix_start;

  if (suffix_len == 0 && handle_len == 1) {
    /* A bare "!" with nothing after it at all: the non-specific tag
     * (c-non-specific-tag), a separate grammar production from a
     * shorthand tag (which requires ns-tag-char+ after its handle). */
    _mem_free(ctx->mp, handle);
    *resolved_out = NULL;
    return true;
  }
  if (suffix_len == 0) {
    /* "!!" or "!name!" with nothing after: not a valid shorthand tag
     * (c-ns-shorthand-tag requires at least one ns-tag-char). */
    parse_err(ctx, "tag handle '%s' with no suffix at position %zu", handle,
              tag_start);
    _mem_free(ctx->mp, handle);
    return false;
  }

  const char *prefix = tag_handles_lookup(ctx, handle);
  if (!prefix) {
    parse_err(ctx, "undefined tag handle '%s' at position %zu", handle,
              tag_start);
    _mem_free(ctx->mp, handle);
    return false;
  }

  /* ns-tag-char (the shorthand suffix's own grammar, YAML 1.2 sec. 5.5)
   * derives from ns-uri-char exactly like the verbatim form's content does,
   * so a suffix may contain the identical "%" ns-hex-digit ns-hex-digit
   * escape syntax; decode it into its own buffer first (percent_decode_
   * tag_inplace shortens in place, so decoded_len can only be <= suffix_len)
   * before concatenating with prefix, mirroring the verbatim branch's own
   * decode-before-use treatment rather than copying the raw source bytes
   * verbatim the way this branch previously did. */
  char *suffix = _mem_alloc(ctx->mp, suffix_len + 1);
  if (!suffix) {
    parse_err(ctx, "out of memory parsing tag suffix at position %zu",
              tag_start);
    _mem_free(ctx->mp, handle);
    return false;
  }
  memcpy(suffix, ctx->src + suffix_start, suffix_len);
  suffix[suffix_len] = '\0';
  if (!percent_decode_tag_inplace(ctx, suffix, suffix_start)) {
    _mem_free(ctx->mp, handle);
    _mem_free(ctx->mp, suffix);
    return false;
  }
  size_t decoded_suffix_len = strlen(suffix);

  size_t prefix_len = strlen(prefix);
  char *resolved = _mem_alloc(ctx->mp, prefix_len + decoded_suffix_len + 1);
  if (!resolved) {
    parse_err(ctx, "out of memory resolving tag at position %zu", tag_start);
    _mem_free(ctx->mp, handle);
    _mem_free(ctx->mp, suffix);
    return false;
  }
  memcpy(resolved, prefix, prefix_len);
  memcpy(resolved + prefix_len, suffix, decoded_suffix_len);
  resolved[prefix_len + decoded_suffix_len] = '\0';
  _mem_free(ctx->mp, handle);
  _mem_free(ctx->mp, suffix);
  *resolved_out = resolved;
  return true;
}

/*
 * Register anchor_name (if non-NULL; a no-op returning true otherwise) as
 * pointing to anchor_value if non-NULL (taking ownership of it), or
 * otherwise to a fresh CYAML_STRING clone of key_str; the shared "this
 * key text is what a later '*name' alias resolves to" step needed
 * wherever a dictionary key may itself carry an anchor: the '&' and '!'
 * dispatch branches in parse_node, and parse_one_dict_entry_key's own
 * later-entry equivalent, all of which obtain (anchor_name, key_str,
 * anchor_value) from try_parse_scalar_dict_key.
 *
 * anchor_value, when non-NULL, is the key's own real, fully-typed/
 * structured node (a flow-collection key's actual list/mapping, or a
 * plain-scalar key's implicitly-typed scalar) built by the caller
 * specifically so that an alias to this anchor resolves to the same kind
 * of value ordinary value-position anchoring would produce, rather than
 * always collapsing to the plain string used for the dictionary's own
 * key storage. A quoted-scalar key has no such richer representation (a
 * quoted scalar is always a string, matching key_str exactly), so its
 * caller passes NULL and this function falls back to building the
 * CYAML_STRING wrapper directly from key_str, as before. If anchor_name
 * is NULL, any anchor_value passed in is destroyed rather than silently
 * leaked (defensive; no current caller does this).
 *
 * Always consumes (frees) anchor_name, and always consumes (stores or
 * destroys) anchor_value when anchor_name is non-NULL. Returns false
 * (with a parse error already set via parse_err(), either by this
 * function directly or by anchors_store()) only on OOM.
 */
static bool register_key_anchor(parse_ctx_t *ctx, char *anchor_name,
                                const char *key_str,
                                cyaml_node_t *anchor_value) {
  if (!anchor_name) {
    if (anchor_value) __cyaml_destroy((cyaml)anchor_value);
    return true;
  }
  cyaml_node_t *anchor_val = anchor_value;
  if (!anchor_val) {
    anchor_val = node_alloc(CYAML_STRING, ctx->mp);
    if (anchor_val) {
      anchor_val->value.string = ccol_strdup(ctx->mp, key_str);
      if (!anchor_val->value.string) {
        node_free(anchor_val);
        anchor_val = NULL;
      }
    }
  }
  if (!anchor_val) {
    parse_err(ctx, "out of memory registering key anchor '%s' at position %zu",
              anchor_name, ctx->pos);
    _mem_free(ctx->mp, anchor_name);
    return false;
  }
  bool stored = anchors_store(ctx, anchor_name, anchor_val);
  _mem_free(ctx->mp, anchor_name);
  return stored;
}

/*
 * Attempt to parse an (optionally anchored and/or tagged) plain,
 * double-quoted, or single-quoted scalar, or a resolved alias, acting as a
 * block dictionary key; i.e. everything through its own ':' indicator.
 * Shared by parse_node's '&' dispatch (a dictionary's very first entry,
 * when the anchored node turns out to introduce a key rather than being a
 * standalone value) and parse_one_dict_entry_key (every later entry).
 *
 * On a genuine match, returns true: *anchor_name_out receives an owned
 * anchor name if one was present (NULL for an alias key or an
 * undecorated one); the caller must still call anchors_store() once it
 * knows the key's final node; *key_out receives the owned key text;
 * *is_merge_candidate_out (if non-NULL) receives whether this key is a
 * genuine merge-key trigger (see below); *anchor_value_out (if non-NULL)
 * receives the key's own real, fully-typed/structured node (owned by the
 * caller) when anchor_name_out came back non-NULL AND a richer
 * representation than the plain key string exists (a flow-collection
 * key's actual list/mapping, or a plain-scalar key's implicitly-typed
 * scalar); otherwise it receives NULL, meaning the caller should register
 * the anchor as a plain string of the key text instead (see
 * register_key_anchor). ctx->pos is left just past the consumed ':'.
 *
 * On no match, returns false with ctx->pos restored to exactly where it
 * was on entry and all out-params untouched.  ctx->error[0] distinguishes
 * a genuine parse error (a malformed quoted scalar, or an alias to an
 * unknown or non-scalar anchor) from an ordinary "this wasn't a key after
 * all" outcome.
 *
 * *is_merge_candidate_out is true only when the key came from the plain-
 * scalar branch specifically (never the quoted, alias-as-key, or flow-
 * collection-as-key branches), its text is exactly "<<", and no tag
 * sigil was seen decorating it; mirroring implicit core-schema type
 * resolution's own "only a plain, untagged scalar" rule (confirmed
 * against two independent reference parsers: a quoted "<<" or an
 * explicitly tagged "<<" is always treated as an ordinary literal key,
 * never expanded). An anchor decorating "<<" does NOT disqualify it: an
 * anchor is an orthogonal c-ns-properties concern, unrelated to implicit
 * typing, so "&x <<: *y" is still a genuine merge-key trigger.
 */
static bool try_parse_scalar_dict_key(parse_ctx_t *ctx, char **anchor_name_out,
                                      char **key_out,
                                      bool *is_merge_candidate_out,
                                      cyaml_node_t **anchor_value_out) {
  size_t saved_pos = ctx->pos;
  char *anchor_name = NULL;

  /* An alias may itself be used directly as a key (e.g. "*b : *a"):
   * resolve it now and canonicalize its value to a string key exactly
   * like any other typed scalar, with no anchor name to register. */
  if (!at_end(ctx) && cur(ctx) == '*') {
    ctx->pos++;
    char *alias_name = NULL;
    if (!parse_anchor_name(ctx, &alias_name)) return false;
    cyaml_node_t *aliased = anchors_lookup(ctx, alias_name);
    if (!aliased) {
      parse_err(ctx, "unknown alias '*%s' at position %zu", alias_name,
                ctx->pos);
      _mem_free(ctx->mp, alias_name);
      return false;
    }
    _mem_free(ctx->mp, alias_name);
    cyaml_node_t *clone = (cyaml_node_t *)cyaml_clone((cyaml)aliased);
    if (!clone) {
      /* cyaml_clone() never calls parse_err() on OOM. Without an explicit
       * error here, every caller's own "ctx->error[0] ? genuine error :
       * not a key after all" check (see this function's own doc comment)
       * would misclassify this as "not a key" and resume parsing from
       * ctx->pos, which (unlike every other "not a key" return in this
       * function) is NOT restored to saved_pos on this path, desyncing
       * the parser instead of cleanly failing the whole document. */
      parse_err(ctx,
                "out of memory resolving alias as dictionary key at "
                "position %zu",
                saved_pos);
      return false;
    }

    skip_inline_ws(ctx);
    bool alias_ok = false;
    if (!at_end(ctx) && cur(ctx) == ':') {
      char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
      if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
          after == '\0')
        alias_ok = true;
    }
    if (!alias_ok) {
      __cyaml_destroy((cyaml)clone);
      ctx->pos = saved_pos;
      return false;
    }
    ctx->pos++; /* consume ':' */

    char *key_str = node_to_dict_key_string(ctx, clone, "block dictionary");
    if (!key_str) return false;

    *anchor_name_out = NULL;
    *key_out = key_str;
    if (is_merge_candidate_out) *is_merge_candidate_out = false;
    if (anchor_value_out) *anchor_value_out = NULL;
    return true;
  }

  /* c-ns-properties(n,c) permits an anchor and a tag in either order
   * (c-tag-property s-separate c-ns-anchor-property | the reverse); each
   * appears at most once, so two iterations suffice regardless of order.
   * The tag branch is guarded by !had_tag, mirroring the anchor branch's
   * own !anchor_name guard, so a second tag is never silently re-consumed
   * here as if it were part of the property list. */
  bool had_tag = false;
  for (int prop_pass = 0; prop_pass < 2; prop_pass++) {
    if (!at_end(ctx) && cur(ctx) == '&' && !anchor_name) {
      ctx->pos++;
      if (!parse_anchor_name(ctx, &anchor_name)) return false;
      skip_inline_ws(ctx);
    } else if (!at_end(ctx) && cur(ctx) == '!' && !had_tag) {
      /* A dictionary key can never carry a tag (see cyaml_node_tag()'s own
       * doc comment: this DOM's keys are plain strings, not nodes), so the
       * resolved tag has nowhere to be stored and is discarded immediately
       * below. It must still be resolved through parse_tag_token(), not
       * merely skipped over lexically: an undefined %TAG handle, a
       * malformed verbatim tag, or a percent-escaped null byte is a
       * genuine parse error here exactly like it is anywhere else a tag
       * appears (documented as "parsed for validity"), and only
       * parse_tag_token() actually performs that validation. */
      char *key_tag = NULL;
      if (!parse_tag_token(ctx, &key_tag)) {
        _mem_free(ctx->mp, anchor_name);
        return false;
      }
      _mem_free(ctx->mp, key_tag);
      had_tag = true;
      skip_inline_ws(ctx);
    } else {
      break;
    }
  }
  if (!at_end(ctx) && cur(ctx) == '!' && had_tag) {
    /* A second tag stacked on the same key: c-ns-properties permits at
     * most one tag per node. Reported explicitly here (mirroring the
     * identical value-position check in parse_node_inner) as a genuine,
     * unrecoverable error rather than left to fall through to the
     * plain-scalar dispatch below, which has no protection of its own
     * against a leading '!' at all (unlike '&'/'*', excluded just below)
     * and would otherwise silently absorb the unconsumed second tag's
     * text as part of the key string itself. */
    parse_err(ctx, "a node cannot carry two tags at position %zu", ctx->pos);
    _mem_free(ctx->mp, anchor_name);
    return false;
  }
  if (at_end(ctx) || cur(ctx) == '\n' || cur(ctx) == '\r') {
    ctx->pos = saved_pos;
    _mem_free(ctx->mp, anchor_name);
    return false;
  }
  char c = cur(ctx);
  if (c == '|' || c == '>' || c == '&' || c == '*' || c == '#' || c == '%' ||
      c == '@' || c == '`') {
    /* '%'/'@'/'`' (like '#' just above) are c-indicator characters
     * (YAML 1.2 sec. 6.6, ns-plain-first) with no valid plain-scalar-key
     * interpretation at all; unlike '|'/'>'/'&'/'*', which are genuine
     * indicators for a DIFFERENT node kind a caller may still want to try
     * reinterpreting this position as (a block scalar, an anchor, an
     * alias), '%'/'@'/'`' have no such alternative reading either. Falling
     * through here without a direct parse_err() call still produces a
     * clear, specific error: the caller reinterprets this position as an
     * ordinary value, which routes back through parse_node_inner's own
     * identical check for these three characters. */
    ctx->pos = saved_pos;
    _mem_free(ctx->mp, anchor_name);
    return false;
  }

  char *key_str = NULL;
  cyaml_node_t *anchor_value = NULL;
  bool parsed;
  if (c == '"' || c == '\'') {
    size_t quote_start = ctx->pos;
    parsed = (c == '"') ? parse_double_quoted(ctx, &key_str)
                        : parse_single_quoted(ctx, &key_str);
    if (parsed && span_crosses_newline(ctx, quote_start)) {
      /* See the identical single-line restriction for a flow-collection
       * key just below: an implicit key is always single-line
       * (ns-s-implicit-yaml-key), and a quoted scalar is no exception
       * (verified against two independent reference parsers). */
      _mem_free(ctx->mp, key_str);
      ctx->pos = saved_pos;
      _mem_free(ctx->mp, anchor_name);
      return false;
    }
  } else if (c == '[' || c == '{') {
    /* A flow collection is itself a valid (non-scalar) implicit block
     * mapping key (YAML 1.2 sec. 8.2.2 permits ns-flow-node in block-key
     * context, not just a scalar); canonicalize it the same way a flow
     * dictionary's own non-scalar key already is. Unambiguous on its own
     * (a flow collection's '['/'{' ... ']'/'}' boundaries need no
     * lookahead-and-restore the way a bare plain scalar does), so parsed
     * directly rather than through parse_node. */
    size_t coll_start = ctx->pos;
    /* This function is only ever used for a block-context implicit key,
     * always constrained to a single physical line by the memchr check
     * below regardless of what indent value is used here; any newline
     * crossed inside the collection fails that check unconditionally, so
     * flow_skip_ws's own indent-based validation would be redundant; -1
     * (the most permissive value) is passed simply because there is no
     * more meaningful enclosing indent available at this call site. */
    cyaml_node_t *coll =
        (c == '[') ? parse_flow_list(ctx, -1) : parse_flow_dictionary(ctx, -1);
    if (!coll) {
      /* A genuine syntax error inside the flow collection already set
       * ctx->error via parse_err(); but an OOM at the very top of
       * parse_flow_list()/parse_flow_dictionary() (e.g. cyaml_create_
       * list_mp() itself failing) returns NULL without ever calling
       * parse_err(). See the alias-as-key branch's own identical comment
       * above for why leaving ctx->error unset here would desync the
       * parser rather than cleanly fail. */
      if (!ctx->error[0])
        parse_err(ctx,
                  "out of memory parsing flow collection dictionary key "
                  "at position %zu",
                  coll_start);
      _mem_free(ctx->mp, anchor_name);
      return false;
    }
    /* Every implicit key, non-scalar or not, must fit on a single line
     * (ns-s-implicit-yaml-key); unlike an explicit '? key', this function
     * is only ever used for implicit-style keys. */
    if (span_crosses_newline(ctx, coll_start)) {
      __cyaml_destroy((cyaml)coll);
      ctx->pos = saved_pos;
      _mem_free(ctx->mp, anchor_name);
      return false;
    }
    if (anchor_name) {
      /* Preserve the collection's real structure for anchor-registration
       * purposes before node_to_dict_key_string() below destroys coll and
       * replaces it with a canonicalized string: an alias to this key's
       * anchor must resolve to the real list/mapping, matching ordinary
       * value-position anchoring, not to the string used only for the
       * dictionary's own key storage. */
      anchor_value = (cyaml_node_t *)cyaml_clone((cyaml)coll);
      if (!anchor_value) {
        parse_err(ctx,
                  "out of memory preserving anchored key structure at "
                  "position %zu",
                  coll_start);
        __cyaml_destroy((cyaml)coll);
        _mem_free(ctx->mp, anchor_name);
        return false;
      }
    }
    key_str = node_to_dict_key_string(ctx, coll, "block dictionary");
    parsed = (key_str != NULL);
    if (!parsed && anchor_value) {
      __cyaml_destroy((cyaml)anchor_value);
      anchor_value = NULL;
    }
  } else
    /* An implicit key is always single-line (ns-plain-one-line), so no
     * hit_real_terminator out-param is needed here: continuation is never
     * attempted for a key regardless of what follows. */
    parsed = parse_plain_scalar(ctx, false, &key_str, NULL);
  bool from_plain_branch = (c != '"' && c != '\'' && c != '[' && c != '{');
  if (!parsed) {
    /* Usually a genuine parse error (ctx->error already set by
     * parse_double_quoted()/parse_single_quoted()/parse_plain_scalar()/
     * node_to_dict_key_string()). But each of those can also fail purely
     * on allocator OOM without ever calling parse_err(); without this
     * fallback such a failure would be silently misclassified by every
     * caller as "not a key after all" (see this function's own doc
     * comment) rather than the genuine failure it actually is. */
    if (!ctx->error[0])
      parse_err(ctx, "out of memory parsing dictionary key at position %zu",
                saved_pos);
    _mem_free(ctx->mp, anchor_name);
    return false;
  }

  skip_inline_ws(ctx);
  bool ok = false;
  if (!at_end(ctx) && cur(ctx) == ':') {
    char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
    if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
        after == '\0')
      ok = true;
  }
  if (!ok) {
    ctx->pos = saved_pos;
    _mem_free(ctx->mp, key_str);
    _mem_free(ctx->mp, anchor_name);
    if (anchor_value) __cyaml_destroy((cyaml)anchor_value);
    return false;
  }
  ctx->pos++; /* consume ':' */

  if (from_plain_branch) {
    /* An implicit plain-scalar key must canonicalize through the exact
     * same core-schema typing node_to_dict_key_string() already applies
     * to every OTHER key-capture site (the flow-collection/alias-as-key
     * branches above, a flow dictionary/sequence key, and an explicit
     * "? key" block key); without this, "~: v" stored the literal text
     * "~" while "? ~\n: v" stored "null" for the identical value, even
     * though YAML 1.2 treats the two notations as exactly equivalent
     * spellings of the same entry. Also resolves the key's own implicit
     * type (exactly as an ordinary, un-anchored value-position plain
     * scalar would be) for anchor_value below, so a later alias to this
     * key's anchor resolves to e.g. the integer 42 rather than the
     * string "42"; a quoted-scalar key needs no equivalent step, since
     * its natural value is already a string, matching key_str exactly. */
    cyaml_node_t *typed = make_typed_scalar(ctx, key_str);
    if (!typed) {
      parse_err(ctx, "out of memory typing dictionary key at position %zu",
                saved_pos);
      _mem_free(ctx->mp, key_str);
      _mem_free(ctx->mp, anchor_name);
      return false;
    }
    if (anchor_name) {
      /* Preserve the key's own typed value for anchor-registration
       * purposes before node_to_dict_key_string() below destroys typed
       * and replaces it with a canonicalized string; mirrors the
       * flow-collection branch's own identical clone-before-canonicalize
       * treatment above. */
      anchor_value = (cyaml_node_t *)cyaml_clone((cyaml)typed);
      if (!anchor_value) {
        parse_err(ctx,
                  "out of memory registering key anchor '%s' at position %zu",
                  anchor_name, ctx->pos);
        __cyaml_destroy((cyaml)typed);
        _mem_free(ctx->mp, key_str);
        _mem_free(ctx->mp, anchor_name);
        return false;
      }
    }
    char *canon_key = node_to_dict_key_string(ctx, typed, "block dictionary");
    if (!canon_key) {
      if (anchor_value) __cyaml_destroy((cyaml)anchor_value);
      _mem_free(ctx->mp, key_str);
      _mem_free(ctx->mp, anchor_name);
      return false;
    }
    _mem_free(ctx->mp, key_str);
    key_str = canon_key;
  }

  *anchor_name_out = anchor_name;
  *key_out = key_str;
  if (is_merge_candidate_out)
    *is_merge_candidate_out =
        from_plain_branch && !had_tag && strcmp(key_str, "<<") == 0;
  if (anchor_value_out)
    *anchor_value_out = anchor_value;
  else if (anchor_value)
    __cyaml_destroy((cyaml)anchor_value);
  return true;
}

/* ========================================================================== */
/*                         FORWARD DECLARATIONS                               */
/* ========================================================================== */

static cyaml_node_t *parse_node(parse_ctx_t *ctx, int indent, bool in_flow,
                                bool seq_ok_at_indent, bool allow_inline_map,
                                bool had_anchor, bool had_tag,
                                const char *resolved_tag);
static cyaml_node_t *parse_node_inner(parse_ctx_t *ctx, int indent,
                                      bool in_flow, bool seq_ok_at_indent,
                                      bool allow_inline_map, bool had_anchor,
                                      bool had_tag, const char *resolved_tag);

/*
 * Thin recursion-depth guard wrapping parse_node_inner (the real dispatch
 * logic, unchanged below).  Every recursive descent into node parsing in
 * this file goes through this one function, so incrementing/decrementing
 * ctx->depth here bounds total call-stack usage regardless of which YAML
 * construct drives the recursion (nested mappings, sequences, explicit
 * keys, anchors, tags, ...), without needing a matching check at each of
 * this file's many individual recursive call sites. Exceeding
 * CYAML_MAX_PARSE_DEPTH is a hard parse error, matching this parser's
 * fail-fast convention elsewhere; ctx->depth is decremented on every exit
 * path, including the depth-exceeded error path itself, so a caller
 * higher up the stack that goes on to report its own, different error
 * always sees a consistent depth count.
 */
static cyaml_node_t *parse_node(parse_ctx_t *ctx, int indent, bool in_flow,
                                bool seq_ok_at_indent, bool allow_inline_map,
                                bool had_anchor, bool had_tag,
                                const char *resolved_tag) {
  if (ctx->depth >= CYAML_MAX_PARSE_DEPTH) {
    parse_err(ctx, "maximum nesting depth (%d) exceeded at position %zu",
              CYAML_MAX_PARSE_DEPTH, ctx->pos);
    return NULL;
  }
  ctx->depth++;
  cyaml_node_t *result =
      parse_node_inner(ctx, indent, in_flow, seq_ok_at_indent, allow_inline_map,
                       had_anchor, had_tag, resolved_tag);
  ctx->depth--;
  return result;
}

/* See this function's own definition, near parse_block_dictionary, for the
 * full doc comment; forward-declared here since parse_flow_dictionary
 * (which needs it too) is defined earlier in the file. */
static bool expand_merge_key(parse_ctx_t *ctx, cyaml_node_t *map);

/* See this function's own definition, near parse_one_dict_entry_key, for
 * the full doc comment; forward-declared here since parse_flow_dictionary
 * (which needs it too, for its own "{? key: value}" explicit-key shorthand)
 * is defined earlier in the file. */
static bool explicit_key_peek_is_merge_candidate(parse_ctx_t *ctx, int indent,
                                                 bool in_flow);

/* ========================================================================== */
/*                         FLOW COLLECTION PARSERS                            */
/* ========================================================================== */

/* Outcome of flow_list_expect_comma_or_close: what parse_flow_list's own
 * caller-side switch should do next. */
typedef enum {
  FLOW_LIST_TAIL_CLOSE,    /* ']' consumed; caller returns the list as-is. */
  FLOW_LIST_TAIL_CONTINUE, /* ',' consumed; caller loops for another entry. */
  FLOW_LIST_TAIL_ERROR     /* ctx->error already set; caller goes to fail. */
} flow_list_tail_t;

/* Shared "after one flow-list element: skip whitespace, then require ']' or
 * ',' " tail, reached identically after both parse_flow_list's own two
 * distinct entry shapes (the "[? key: value]" explicit-pair branch and the
 * bare-element branch below it). Consumes the ']' or ',' itself so the
 * caller only needs to act on the outcome. */
static flow_list_tail_t flow_list_expect_comma_or_close(parse_ctx_t *ctx,
                                                        int indent) {
  if (!flow_skip_ws(ctx, indent)) return FLOW_LIST_TAIL_ERROR;
  if (at_end(ctx)) {
    parse_err(ctx, "unterminated flow list");
    return FLOW_LIST_TAIL_ERROR;
  }
  if (cur(ctx) == ']') {
    ctx->pos++;
    return FLOW_LIST_TAIL_CLOSE;
  }
  if (cur(ctx) != ',') {
    parse_err(ctx, "expected ',' or ']' in flow list at position %zu",
              ctx->pos);
    return FLOW_LIST_TAIL_ERROR;
  }
  ctx->pos++;
  return FLOW_LIST_TAIL_CONTINUE;
}

/* Parse a flow list '[' elem (',' elem)* ']' and return a CYAML_LIST
 * node whose backing cvec holds cyaml_node_t * child pointers. `indent`
 * is the enclosing block value's own indent (-1 at the document root or
 * when this flow list is itself nested inside another flow collection
 * that already established it); see flow_skip_ws's own doc comment. */
static cyaml_node_t *parse_flow_list(parse_ctx_t *ctx, int indent) {
  /* consume '[' */
  ctx->pos++;
  if (!flow_skip_ws(ctx, indent)) return NULL;

  cyaml_node_t *seq = (cyaml_node_t *)cyaml_create_list_mp(ctx->mp);
  if (!seq) return NULL;

  if (!at_end(ctx) && cur(ctx) == ']') {
    ctx->pos++;
    return seq;
  }

  while (1) {
    if (!flow_skip_ws(ctx, indent)) goto fail;
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow list");
      goto fail;
    }
    if (cur(ctx) == ']') {
      ctx->pos++;
      return seq;
    }
    if (cur(ctx) == ',') {
      /* A ',' with no content since '[' or the previous ',' is an empty
       * entry, which (unlike a flow mapping's bare-key-no-value shorthand)
       * has no valid meaning for a sequence entry: every element must
       * actually be present. parse_node/parse_plain_scalar would otherwise
       * silently produce a zero-length string "" here instead of erroring. */
      parse_err(ctx, "empty flow list entry at position %zu", ctx->pos);
      goto fail;
    }

    /* "[? key: value]" (YAML 1.2 sec. 7.4.1, Spec Example 7.20): an
     * explicit-style single-pair entry, mirroring the bare "[key: value]"
     * shorthand below but with the key introduced by '?' (permitting
     * multi-line/non-scalar key content the bare form's implicit-key
     * restriction would otherwise forbid) and the value, if any, by a
     * separate ':'. */
    bool is_explicit_pair_indicator = false;
    if (cur(ctx) == '?') {
      char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
      if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
        is_explicit_pair_indicator = true;
    }
    if (is_explicit_pair_indicator) {
      ctx->pos++;
      if (!flow_skip_ws(ctx, indent)) goto fail;
      /* Mirrors parse_flow_dictionary's own "{? key: value}" handling: a
       * genuine plain, untagged "<<" key here is just as real a merge
       * trigger as the bare "[<<: value]" shorthand just below, or either
       * form's flow-dictionary/block-dictionary counterparts; must be
       * peeked before parse_node consumes the key. */
      bool is_merge_candidate =
          explicit_key_peek_is_merge_candidate(ctx, indent, true);
      cyaml_node_t *key_node =
          parse_node(ctx, indent, true, false, false, false, false, NULL);
      if (!key_node) goto fail;
      char *key_str =
          node_to_dict_key_string(ctx, key_node, "flow sequence entry");
      if (!key_str) goto fail;

      if (!flow_skip_ws(ctx, indent)) {
        _mem_free(ctx->mp, key_str);
        goto fail;
      }
      cyaml_node_t *val;
      if (!at_end(ctx) && cur(ctx) == ':') {
        ctx->pos++;
        if (!flow_skip_ws(ctx, indent)) {
          _mem_free(ctx->mp, key_str);
          goto fail;
        }
        val = parse_node(ctx, indent, true, false, false, false, false, NULL);
        if (!val) {
          _mem_free(ctx->mp, key_str);
          goto fail;
        }
      } else {
        val = node_alloc(CYAML_NULL, ctx->mp);
        if (!val) {
          _mem_free(ctx->mp, key_str);
          goto fail;
        }
      }

      cyaml_node_t *pair = (cyaml_node_t *)cyaml_create_dictionary_mp(ctx->mp);
      if (!pair) {
        _mem_free(ctx->mp, key_str);
        __cyaml_destroy((cyaml)val);
        goto fail;
      }
      if (cyaml_dictionary_set((cyaml)pair, key_str, (cyaml)val) !=
          ccol_success) {
        _mem_free(ctx->mp, key_str);
        __cyaml_destroy((cyaml)pair);
        goto fail;
      }
      _mem_free(ctx->mp, key_str);
      if (is_merge_candidate && !expand_merge_key(ctx, pair)) {
        __cyaml_destroy((cyaml)pair);
        goto fail;
      }

      cyaml_node_t *pep = pair;
      if (cvector_push_back(seq->value.list, &pep) != ccol_success) {
        __cyaml_destroy((cyaml)pair);
        goto fail;
      }

      switch (flow_list_expect_comma_or_close(ctx, indent)) {
        case FLOW_LIST_TAIL_CLOSE:
          return seq;
        case FLOW_LIST_TAIL_ERROR:
          goto fail;
        case FLOW_LIST_TAIL_CONTINUE:
          continue;
      }
    }

    size_t elem_start = ctx->pos;
    /* Peeked before parse_node consumes the element, mirroring parse_flow_
     * dictionary's own implicit-key peek: only a plain, untagged, unquoted
     * scalar reading exactly "<<" is a genuine merge-key trigger (an
     * anchor does not disqualify it, so explicit_key_peek_is_merge_
     * candidate's own anchor handling covers that case too; see try_parse_
     * scalar_dict_key's own doc comment for the general rule this
     * mirrors). A raw property-chain peek is required here, not a single-
     * character check: a tag that follows an anchor ("&y !!str <<: *x")
     * must still be seen and correctly disqualify the key, which checking
     * only the very first character cannot do. */
    bool key_is_merge_candidate =
        explicit_key_peek_is_merge_candidate(ctx, indent, true);
    cyaml_node_t *elem =
        parse_node(ctx, indent, true, false, false, false, false, NULL);
    if (!elem) goto fail;

    /* "[foo: bar]" is shorthand for "[{foo: bar}]": a bare "key: value"
     * pair inside a flow sequence with no surrounding '{'/'}' denotes a
     * single-entry mapping element (YAML 1.2 sec. 7.4.1, ns-flow-pair).
     * Like every implicit key, it must fit on a single line
     * (ns-s-implicit-yaml-key): a ':' found only after crossing a newline
     *; whether that newline was crossed while parse_node itself was
     * still resolving elem (e.g. multi-line plain scalar continuation
     * folding straight up to the ':') or only afterward, while skipping
     * whitespace before finding it; is not this shorthand, just the
     * next real error (a missing ',' between this element and whatever
     * follows). */
    if (!flow_skip_ws(ctx, indent)) {
      __cyaml_destroy((cyaml)elem);
      goto fail;
    }
    bool key_crossed_newline = span_crosses_newline(ctx, elem_start);
    if (!key_crossed_newline && !at_end(ctx) && cur(ctx) == ':') {
      char *key_str = node_to_dict_key_string(ctx, elem, "flow sequence entry");
      if (!key_str) goto fail;
      bool is_merge_candidate = key_is_merge_candidate;
      ctx->pos++; /* consume ':' */
      if (!flow_skip_ws(ctx, indent)) {
        _mem_free(ctx->mp, key_str);
        goto fail;
      }

      cyaml_node_t *val =
          parse_node(ctx, indent, true, false, false, false, false, NULL);
      if (!val) {
        _mem_free(ctx->mp, key_str);
        goto fail;
      }

      cyaml_node_t *pair = (cyaml_node_t *)cyaml_create_dictionary_mp(ctx->mp);
      if (!pair) {
        _mem_free(ctx->mp, key_str);
        __cyaml_destroy((cyaml)val);
        goto fail;
      }
      /* cyaml_dictionary_set always consumes val, destroying it on failure
       * too, so pair (now empty either way) is the only thing left to
       * clean up on that path. */
      if (cyaml_dictionary_set((cyaml)pair, key_str, (cyaml)val) !=
          ccol_success) {
        _mem_free(ctx->mp, key_str);
        __cyaml_destroy((cyaml)pair);
        goto fail;
      }
      _mem_free(ctx->mp, key_str);
      if (is_merge_candidate && !expand_merge_key(ctx, pair)) {
        __cyaml_destroy((cyaml)pair);
        goto fail;
      }
      elem = pair;
    }

    cyaml_node_t *ep = elem;
    if (cvector_push_back(seq->value.list, &ep) != ccol_success) {
      __cyaml_destroy((cyaml)elem);
      goto fail;
    }

    switch (flow_list_expect_comma_or_close(ctx, indent)) {
      case FLOW_LIST_TAIL_CLOSE:
        return seq;
      case FLOW_LIST_TAIL_ERROR:
        goto fail;
      case FLOW_LIST_TAIL_CONTINUE:
        continue;
    }
  }

fail:
  __cyaml_destroy((cyaml)seq);
  return NULL;
}

/*
 * Convert a parsed node into its canonical string form for storage as a
 * dictionary key (this DOM's dictionaries are always char* -> node,
 * backed by a chmap, so every key is ultimately a string).  Always
 * consumes (destroys) key_node.
 *
 * A non-scalar key_node (a list or another dictionary; YAML 1.2 sec.
 * 7.4.1/8.2.2 both permit this) is canonicalized via
 * serialize_flow_canonical (an internal-only variant of
 * cyaml_serialize_flow specifically for this use, see its own doc comment)
 * into its compact flow-YAML text (e.g. "[a, b]", "{x: 1}") and used as the
 * key string; two structurally-equal non-scalar keys therefore collide
 * (matching-flow-output equality) exactly like this DOM already treats,
 * say, the numeric key 1 and the string key "1" as the same string key,
 * regardless of what order either dictionary's own keys happened to be
 * inserted in; this canonicalization is context_label-independent (unlike
 * the previous hard rejection) so it applies uniformly to a block
 * dictionary's explicit key, a flow dictionary key, and a flow sequence's
 * "key: value" shorthand.
 */
static char *node_to_dict_key_string(parse_ctx_t *ctx, cyaml_node_t *key_node,
                                     const char *context_label) {
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
    case CYAML_FLOAT: {
      char tmp[64];
      snprintf(tmp, sizeof(tmp), "%.17g", key_node->value.number);
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
    case CYAML_LIST:
    case CYAML_DICTIONARY:
    default:
      key_str = serialize_flow_canonical(key_node);
      if (!key_str) {
        parse_err(ctx,
                  "out of memory canonicalizing non-scalar key in %s "
                  "at position %zu",
                  context_label, ctx->pos);
      } else if (strlen(key_str) > CYAML_MAX_CANONICAL_KEY_LEN) {
        /* See CYAML_MAX_CANONICAL_KEY_LEN's own doc comment: a non-scalar
         * key nested inside another non-scalar key, many levels deep,
         * grows its own canonical text exponentially in nesting depth
         * (each level re-quotes the text the level below it already
         * produced), independent of and far cheaper to reach than
         * CYAML_MAX_PARSE_DEPTH. */
        parse_err(ctx,
                  "canonical form of a non-scalar key in %s exceeds %d "
                  "bytes at position %zu",
                  context_label, CYAML_MAX_CANONICAL_KEY_LEN, ctx->pos);
        _mem_free(ctx->mp, key_str);
        key_str = NULL;
      }
      break;
  }
  /* The LIST/DICTIONARY branch above always reports its own OOM/oversize
   * failure via parse_err() before falling through here; the five scalar
   * branches have no failure mode of their own besides a bare ccol_strdup
   * OOM, which none of them individually reported, unlike this file's own
   * established convention elsewhere (e.g. try_parse_scalar_dict_key). Not
   * a crash or desync either way (parse_common's own top-level fallback
   * would still correctly report a generic out-of-memory message when
   * ctx->error is still empty), but a real OOM here deserves this call
   * site's own specific diagnostic instead. */
  if (!key_str && !ctx->error[0]) {
    parse_err(ctx,
              "out of memory converting a scalar key to a dictionary "
              "key string in %s at position %zu",
              context_label, ctx->pos);
  }
  __cyaml_destroy((cyaml)key_node);
  return key_str;
}

/*
 * Parse a flow dictionary '{' key ':' value (',' key ':' value)* '}'.
 * A key may be any node type: a scalar (string, integer, null, bool) or a
 * non-scalar (sequence, mapping, flow collection); every key is canonicalized
 * via node_to_dict_key_string() (see that function's own doc comment) for
 * storage in the chmap so that all dictionary keys are uniformly char *.
 */
static cyaml_node_t *parse_flow_dictionary(parse_ctx_t *ctx, int indent) {
  /* consume '{' */
  ctx->pos++;
  if (!flow_skip_ws(ctx, indent)) return NULL;

  cyaml_node_t *map = (cyaml_node_t *)cyaml_create_dictionary_mp(ctx->mp);
  if (!map) return NULL;

  if (!at_end(ctx) && cur(ctx) == '}') {
    ctx->pos++;
    return map;
  }

  /* Tracks whether the entry that most recently wrote the literal key "<<"
   * into `map` was a genuine merge-key trigger. This must be re-derived
   * (overwritten, not OR-accumulated across every entry ever seen) each
   * time an entry's own key text is exactly "<<": a duplicate "<<" key
   * collapses to whichever entry wrote it LAST (cyaml_dictionary_set's own
   * "last value wins" duplicate-key semantics), so an earlier merge-
   * triggering "<<" entry must never keep this true once a later,
   * explicitly quoted/tagged "<<" entry overwrites it with an ordinary
   * literal value; OR-accumulating unconditionally let that earlier
   * entry's candidacy incorrectly force-expand (or spuriously reject) the
   * later, literal entry's own value instead. */
  bool double_lt_is_merge_candidate = false;
  while (1) {
    if (!flow_skip_ws(ctx, indent)) goto fail;
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow dictionary");
      goto fail;
    }
    if (cur(ctx) == '}') {
      ctx->pos++;
      if (double_lt_is_merge_candidate && !expand_merge_key(ctx, map))
        goto fail;
      return map;
    }

    /* "{? key: value}" (YAML 1.2 sec. 7.4.2, ns-flow-map-explicit-entry):
     * a flow dictionary entry's key may be introduced by an explicit '?',
     * exactly like the block-style "? key\n: value" form, permitting the
     * key to itself be a flow collection or to be omitted entirely (a
     * bare "?" with nothing else, i.e. a null key). Without this check, a
     * '?' here would otherwise reach the generic parse_node dispatch
     * below and, since parse_node's own (block-only) explicit-key
     * handling is gated on !in_flow, be scanned as ordinary plain-scalar
     * TEXT instead; silently producing a key that literally starts with
     * "? ", not the real key that follows it. */
    bool is_explicit_key_indicator = false;
    if (cur(ctx) == '?') {
      char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
      if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
        is_explicit_key_indicator = true;
    }

    /* Parse key.  For both the implicit (non-'?') and explicit '?' forms,
     * explicit_key_peek_is_merge_candidate peeks the key's own leading
     * anchor/tag property chain (in either order) before parse_node's own
     * general dispatch consumes it, to confirm a genuine merge-key
     * candidate (see try_parse_scalar_dict_key's own doc comment for the
     * identical rule on the block-dictionary side; and explicit_key_peek_
     * is_merge_candidate's own doc comment for why a raw property-chain
     * peek, not a single-character check, is required: an anchor does NOT
     * disqualify the key, so a tag that happens to follow one ("&y !!str
     * <<: *x") must still be seen and correctly disqualify it, which a
     * peek that only inspects the very first character cannot do). */
    bool implicit_key_is_merge_candidate = false;
    bool explicit_key_is_merge_candidate = false;
    cyaml_node_t *key_node;
    if (is_explicit_key_indicator) {
      ctx->pos++;
      if (!flow_skip_ws(ctx, indent)) goto fail;
      if (!at_end(ctx) &&
          (cur(ctx) == ':' || cur(ctx) == ',' || cur(ctx) == '}')) {
        key_node = node_alloc(CYAML_NULL, ctx->mp);
      } else {
        explicit_key_is_merge_candidate =
            explicit_key_peek_is_merge_candidate(ctx, indent, true);
        key_node =
            parse_node(ctx, indent, true, false, false, false, false, NULL);
      }
    } else {
      implicit_key_is_merge_candidate =
          explicit_key_peek_is_merge_candidate(ctx, indent, true);
      key_node =
          parse_node(ctx, indent, true, false, false, false, false, NULL);
    }
    if (!key_node) goto fail;

    /* Key must be a scalar for our purposes. */
    char *key_str = node_to_dict_key_string(ctx, key_node, "flow dictionary");
    if (!key_str) goto fail;
    bool is_merge_candidate = is_explicit_key_indicator
                                  ? explicit_key_is_merge_candidate
                                  : implicit_key_is_merge_candidate;

    /* "{key}" is shorthand for "{key: null}" (a bare key with no ':' at
     * all), and "{key:}" (a ':' with nothing before the next ',' or '}')
     * likewise has a null value; YAML 1.2 sec. 7.4.2's ns-flow-map-entry
     * allows both the key and the value half of a pair to be omitted. */
    if (!flow_skip_ws(ctx, indent)) {
      _mem_free(ctx->mp, key_str);
      goto fail;
    }
    cyaml_node_t *val;
    if (!at_end(ctx) && cur(ctx) == ':') {
      ctx->pos++;
      if (!flow_skip_ws(ctx, indent)) {
        _mem_free(ctx->mp, key_str);
        goto fail;
      }
      if (!at_end(ctx) && (cur(ctx) == ',' || cur(ctx) == '}')) {
        val = node_alloc(CYAML_NULL, ctx->mp);
      } else {
        val = parse_node(ctx, indent, true, false, false, false, false, NULL);
      }
    } else if (!at_end(ctx) && (cur(ctx) == ',' || cur(ctx) == '}')) {
      val = node_alloc(CYAML_NULL, ctx->mp);
    } else {
      parse_err(ctx, "expected ':' after flow dictionary key at position %zu",
                ctx->pos);
      _mem_free(ctx->mp, key_str);
      goto fail;
    }
    if (!val) {
      _mem_free(ctx->mp, key_str);
      goto fail;
    }

    bool key_is_double_lt = strcmp(key_str, "<<") == 0;
    ccol_retval_t r = cyaml_dictionary_set((cyaml)map, key_str, (cyaml)val);
    _mem_free(ctx->mp, key_str);
    if (r != ccol_success) goto fail;
    if (key_is_double_lt) double_lt_is_merge_candidate = is_merge_candidate;

    if (!flow_skip_ws(ctx, indent)) goto fail;
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow dictionary");
      goto fail;
    }
    if (cur(ctx) == '}') {
      ctx->pos++;
      if (double_lt_is_merge_candidate && !expand_merge_key(ctx, map))
        goto fail;
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
 * map_indent: the column at which dictionary keys/'?' indicators must
 * appear.
 *
 * If first_key is non-NULL, it is the key string of the first entry's
 * implicit ("key: value") key, already read by the caller before realising
 * we have a dictionary rather than a standalone scalar.  first_key_colon_
 * consumed reports whether the caller also already consumed the ':' that
 * follows it (true for an anchored/tagged key parsed via try_parse_
 * scalar_dict_key, which always consumes it; false for the plain/quoted
 * scalar dispatch branches in parse_node, which leave it for this
 * function to find and consume itself).
 *
 * If first_key is NULL, ctx->pos is at map_indent on a line starting with
 * '?' (explicit key style, "? key" / ": value"); the only way parse_node
 * reaches this function without a pre-read key; first_key_colon_consumed
 * is unused in this mode.  Every entry after the first may freely mix
 * explicit and implicit style within the same dictionary, per YAML 1.2
 * sec. 8.2.2.
 *
 * first_key_is_merge_candidate: whether first_key was already confirmed a
 * genuine plain, untagged "<<" merge-key trigger by whichever caller-side
 * key-production site produced it (see try_parse_scalar_dict_key's own
 * doc comment for the exact rule); always false when first_key is NULL,
 * since a NULL first_key means the mapping's first entry uses the '?'
 * explicit-key form, whose own merge candidacy this function determines
 * internally instead (see explicit_key_peek_is_merge_candidate).
 */
static cyaml_node_t *parse_block_dictionary(parse_ctx_t *ctx, int map_indent,
                                            const char *first_key,
                                            bool first_key_colon_consumed,
                                            bool first_key_is_merge_candidate);

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
 * seq_ok_at_indent: whether `indent` is a mapping's own indent (true, so a
 *   '-' sequence value exactly at that column is a legitimate YAML 1.2
 *   sec. 8.2.2 compact-sequence-under-a-key exception) or a sequence's
 *   own indent (false, where a '-' at that exact column is always a
 *   sibling element, never nested content); see at_block_value_col's own
 *   doc comment. Irrelevant when in_flow or indent == -1, but still
 *   forwarded through unconditionally by the '&'/'!' branches below so a
 *   decorated value's own indent-context is never lost.
 * allow_inline_map: whether a plain/quoted scalar found here that turns
 *   out to be immediately followed by ':' may start a NEW block mapping
 *   right here (this call becomes that mapping's first key).  True at
 *   every position where a fresh key is actually expected: the document
 *   root, a sequence element ("- key: value"), explicit-style ('?'/':')
 *   key or value content (verified against a reference parser: this gets
 *   the same "compact" privilege for a nested mapping that it already
 *   does for a nested sequence), and a value that starts on its own,
 *   more-indented line.  False only for an ordinary implicit value's own
 *   SAME-LINE content ("key: value"; passed to the recursive parse_node
 *   call that parses the "value" half): chaining a second ':' there
 *   ("a: b: c") has no valid interpretation and is a hard error, not
 *   silent unbounded nesting; verified against a reference parser, which
 *   rejects this identically whether the chain sits at the document root,
 *   inside a sequence element's own compact mapping, or nested arbitrarily
 *   deep. Irrelevant once in_flow (flow mappings have their own, unrelated
 *   '{'/'}' dispatch); forwarded through unconditionally by the
 *   '&'/'!'/'*' branches below for the same reason seq_ok_at_indent is,
 *   so a decorated value's own inline-mapping privilege (or lack of one)
 *   is never lost.
 * had_anchor: whether an ENCLOSING anchor's own "ordinary content"
 *   recursion already applied it to the exact node position this call is
 *   about to resolve; i.e. this call's result, if it is not itself a key
 *   (see below), would become that enclosing anchor's direct content.
 *   c-ns-properties permits at most one anchor per node; if THIS call also
 *   turns out to need an anchor while had_anchor is already true, that is
 *   two anchors stacked on the same node, which has no valid interpretation
 *   (verified against two independent reference parsers: "&a &b val",
 *   stacking two bare anchors around one scalar with nothing else in
 *   between, is rejected by both, whether the two anchors sit on one line
 *   or are split across a newline). Consulted only at the very top of the
 *   '&' branch itself, specifically at the point where that branch's own
 *   "is this actually a key" fast path has already failed or was skipped;
 *   a property that DOES resolve to a key (e.g. "&node1\n  &k1 key1:
 *   val1", where &k1 decorates the key scalar "key1", a different node
 *   than &node1's own mapping value) is never affected, regardless of
 *   had_anchor, since that path returns before ever consulting it. False
 *   at every other call site, including the '&'/'!' branches' own "is this
 *   a key" fast path and every call site outside these two branches
 *   entirely, all of which start a genuinely fresh node position with no
 *   pending property to conflict with.
 * had_tag: whether an ENCLOSING '!' branch's own recursion already applied
 *   a tag to the exact node position this call is about to resolve, exactly
 *   mirroring had_anchor's own role for anchors; consulted only by the '!'
 *   branch's own two-tags guard and the '*' branch's cannot-carry-a-tag
 *   guard. This is a SEPARATE signal from resolved_tag itself, because a
 *   bare non-specific tag ("!" with nothing after it) resolves to
 *   resolved_tag == NULL (see parse_tag_token's own doc comment: "resolves
 *   to no forced type, treated identically to no tag at all") while still
 *   being a real tag for stacking purposes - "! !!str x" and "! *alias"
 *   must both still be rejected exactly like "!!str !!str x"/"!!str
 *   *alias" are, even though the first tag in each pair carries no
 *   resolved_tag value of its own to make resolved_tag != NULL true.
 * resolved_tag: the fully-resolved tag string an ENCLOSING '!' branch's own
 *   recursion is delegating to this call, or NULL for every ordinary,
 *   untagged call (including one enclosed by a non-specific "!", which
 *   still sets had_tag but leaves resolved_tag NULL). The actual value
 *   this call's own scalar-construction dispatch (see finalize_scalar_node)
 *   consults to override which type gets built in the first place, not
 *   merely metadata attached to an already-finished node afterward.
 *   Threaded through the '&' branch's own recursive calls completely
 *   unchanged from whatever this call itself received (an anchor never
 *   introduces or consumes a tag of its own), which is what correctly
 *   carries a tag all the way down to wherever the actual content ends up
 *   being parsed regardless of how many anchor/tag layers sit in between
 *   (e.g. "!!str &a foo" or "&a !!str foo", c-ns-properties permits either
 *   order).
 */
static cyaml_node_t *parse_node_inner(parse_ctx_t *ctx, int indent,
                                      bool in_flow, bool seq_ok_at_indent,
                                      bool allow_inline_map, bool had_anchor,
                                      bool had_tag, const char *resolved_tag) {
  size_t entry_pos = ctx->pos;
  bool entry_at_line_start = current_col(ctx) == 0;
  if (!in_flow)
    skip_ws_comments(ctx);
  else
    skip_inline_ws(ctx);

  /* line_indent_has_tab() is only meaningful when ctx->pos now sits at the
   * first non-whitespace character following PURE leading indentation;
   * that holds whenever either this skip crossed a newline (landing on a
   * fresh line), or entry_pos was already at column 0 (e.g. the very
   * start of the document, or right after a previous sibling ended
   * exactly at a line boundary); in both cases, everything between the
   * true line start and ctx->pos is genuinely indentation, nothing else.
   * Without both conditions, a same-line call (e.g. skipping the
   * separator between "&anchor" and its own inline value, entry_pos
   * mid-line and no newline crossed) would scan back through real,
   * non-whitespace content earlier on the same line ("key: &x") looking
   * for a tab that has nothing to do with this position at all;
   * same-line separator whitespace legitimately permits tabs (verified
   * against a reference parser; see line_indent_has_tab's own doc comment
   * for the general rule this is the one carved-out exception to). */
  if (!in_flow && !at_end(ctx) &&
      (entry_at_line_start || span_crosses_newline(ctx, entry_pos)) &&
      line_indent_has_tab(ctx)) {
    parse_err(ctx,
              "tab cannot be used as block-structural indentation at "
              "position %zu",
              ctx->pos);
    return NULL;
  }

  if (at_end(ctx)) {
    if (!in_flow) return node_alloc(CYAML_NULL, ctx->mp);
    parse_err(ctx, "unexpected end of input in flow context at position %zu",
              ctx->pos);
    return NULL;
  }

  if (in_flow && at_doc_marker(ctx)) {
    /* c-forbidden (YAML 1.2 sec. 6.9): a '---'/'...' document marker at
     * the start of a line can never be plain scalar content, in any
     * context; but unlike block context (where the enclosing loop
     * simply treats it as the end of the current collection, a
     * perfectly ordinary document boundary), a still-open flow
     * collection has no valid way to end here at all: its own closing
     * ']'/'}' is still expected, not a stream-level document boundary
     * (verified against two independent reference parsers). */
    parse_err(ctx,
              "a document marker cannot appear inside an unclosed "
              "flow collection at position %zu",
              ctx->pos);
    return NULL;
  }

  char c = cur(ctx);

  /* Anchor */
  if (c == '&') {
    /* In block context, "&anchor key: value" anchors just the key scalar,
     * not the entire dictionary that key turns out to introduce; check
     * for this shape before falling through to the generic
     * anchor-then-recurse handling below, which would otherwise let the
     * recursive parse_node call consume every sibling entry too and
     * anchor the whole resulting dictionary. Mirrors
     * parse_one_dict_entry_key's identical handling for a later entry.
     * Gated on allow_inline_map: this fast path always concludes "this is
     * a legitimate key", so it must not fire when this call has no
     * license to open a fresh mapping here in the first place (e.g. "a:
     * &x b: c", a chained implicit value); letting it fall through to
     * ordinary anchor handling below routes it through the same
     * allow_inline_map-aware rejection the plain-scalar dispatch already
     * has. */
    if (!in_flow && allow_inline_map) {
      int col = current_col(ctx);
      char *anchor_name = NULL;
      char *key_str = NULL;
      bool is_merge_candidate = false;
      cyaml_node_t *anchor_value = NULL;
      if (try_parse_scalar_dict_key(ctx, &anchor_name, &key_str,
                                    &is_merge_candidate, &anchor_value)) {
        if (!register_key_anchor(ctx, anchor_name, key_str, anchor_value)) {
          _mem_free(ctx->mp, key_str);
          return NULL;
        }
        cyaml_node_t *map = parse_block_dictionary(
            ctx, col, key_str, /*colon_consumed=*/true, is_merge_candidate);
        _mem_free(ctx->mp, key_str);
        return finalize_collection_node(ctx, map, resolved_tag);
      }
      if (ctx->error[0]) return NULL;
      /* Not a key after all; try_parse_scalar_dict_key already restored
       * ctx->pos, so fall through to ordinary anchor handling below. */
    }

    if (had_anchor) {
      /* This anchor is not decorating a key (the fast path above already
       * ruled that out, or never applies here); it is ordinary content
       * for an ENCLOSING anchor, which already used up the one anchor a
       * single node may carry (see had_anchor's own doc comment). */
      parse_err(ctx, "a node cannot carry two anchors at position %zu",
                ctx->pos);
      return NULL;
    }

    ctx->pos++;
    char *name = NULL;
    if (!parse_anchor_name(ctx, &name)) return NULL;
    size_t after_name_pos = ctx->pos;
    skip_inline_ws(ctx);
    if (ctx->pos == after_name_pos && !at_end(ctx) &&
        (cur(ctx) == '[' || cur(ctx) == '{' ||
         (!in_flow &&
          (cur(ctx) == ',' || cur(ctx) == ']' || cur(ctx) == '}')))) {
      /* ns-anchor-char excludes every c-flow-indicator character
       * unconditionally, so parse_anchor_name always stops right before
       * one; whatever comes next can only be valid content if genuinely
       * separated from the anchor by s-separate (real whitespace). A
       * flow-collection OPENER ('['/'{') glued directly onto the name has
       * no valid interpretation in ANY context (nothing in the grammar
       * lets an anchor's name be immediately followed by a brand-new
       * nested collection with no separator), so it is always rejected,
       * even in flow context (unlike ','/']'/'}', which are legitimate,
       * meaningful flow-collection structure immediately after an anchor
       * INSIDE a flow collection, e.g. "[&a, b]", "[&a]", and are only
       * rejected in block context, where they have no valid meaning at
       * all). Verified against a reference parser, which rejects
       * "[&a{x: 1}, *a]"/"[&a[1,2], *a]" (an anchor glued directly onto
       * a nested collection opener) even though it otherwise accepts an
       * anchor glued onto a flow terminator. */
      parse_err(ctx,
                "unexpected '%c' directly after anchor name at "
                "position %zu",
                cur(ctx), ctx->pos);
      _mem_free(ctx->mp, name);
      return NULL;
    }

    cyaml_node_t *n;
    if (!in_flow && rest_of_line_is_blank(ctx)) {
      /* Nothing else (but possibly a comment) on this line: mirror
       * parse_block_map_node's own "a
       * value on a later line only counts if it's actually more indented
       * (or, for a sequence, exactly as indented)" check before recursing,
       * so a sibling entry at or below `indent` (e.g. "b: 2" following
       * "a: &anchor" at the same column) is never misread as this
       * anchor's own value; an unconditional recursive parse_node call
       * here would let it walk straight past the newline and swallow that
       * sibling (and everything after it) into the anchor instead. */
      skip_ws_comments(ctx);
      bool is_value_col =
          !at_end(ctx) && at_block_value_col(ctx, indent, seq_ok_at_indent);
      if (!is_value_col && !at_end(ctx) && ctx->error[0]) {
        /* at_block_value_col() found a tab, not merely "not eligible". */
        _mem_free(ctx->mp, name);
        return NULL;
      }
      if (at_end(ctx) || !is_value_col) {
        /* Nothing at all follows this anchor. Route through
         * finalize_scalar_node (empty text, implicit_ok=true) rather than
         * synthesizing a bare CYAML_NULL node directly: resolved_tag (an
         * ENCLOSING tag delegating to this anchor, e.g. "!!str &a" with
         * nothing after the anchor either) must still get the chance to
         * override the type of this "empty" content exactly as it would
         * for any other empty scalar position, not be silently dropped
         * just because no anchor content happens to follow. */
        char *empty = ccol_strdup(ctx->mp, "");
        n = empty ? finalize_scalar_node(ctx, empty, resolved_tag, true) : NULL;
      } else {
        /* A value confirmed to start on its own fresh line always gets the
         * "may open a fresh mapping here" privilege, regardless of what
         * allow_inline_map this call itself inherited (mirrors parse_block_
         * map_node's own subsequent-line branch, which hardcodes true for
         * the identical reason: only SAME-LINE chaining after an already-
         * open implicit value is ever restricted). had_anchor=true (this
         * call's own anchor, "used up"); had_tag/resolved_tag are both
         * forwarded unchanged; see parse_node's own doc comment for why. */
        n = parse_node(ctx, indent, in_flow, seq_ok_at_indent, true, true,
                       had_tag, resolved_tag);
      }
    } else if (in_flow && at_eol(ctx)) {
      /* Flow context: nothing else on this line, but flow collections
       * permit wrapping across lines, so the value may still follow on a
       * later one (there is no sibling-entry ambiguity to guard against
       * here the way block context has; a flow collection's own ','/
       * ']'/'}' terminators, not indentation, delimit entries). Must cross
       * the newline via flow_skip_ws (not plain skip_ws_comments), so this
       * continuation line is held to the same s-separate(n,c)
       * indentation/no-tab rules every other flow-collection continuation
       * line in this file already enforces; skip_ws_comments alone would
       * silently accept a tab-indented or under-indented continuation line
       * here that flow_skip_ws would correctly reject anywhere else. */
      if (!flow_skip_ws(ctx, indent)) {
        _mem_free(ctx->mp, name);
        return NULL;
      }
      n = parse_node(ctx, indent, in_flow, seq_ok_at_indent, allow_inline_map,
                     true, had_tag, resolved_tag);
    } else if (!in_flow && at_bare_seq_indicator(ctx)) {
      /* A block sequence cannot start inline right after an anchor on the
       * same line (only a mapping gets that "compact" privilege, and only
       * when introduced by '-' itself, not by a preceding node property);
       * try_parse_scalar_dict_key already ruled out "this is an anchored
       * key" above, so nothing legitimate remains for a bare '-' here. */
      parse_err(ctx,
                "a block sequence cannot start on the same line as an "
                "anchor at position %zu",
                ctx->pos);
      _mem_free(ctx->mp, name);
      return NULL;
    } else if (!in_flow && cur(ctx) == '&') {
      /* This anchor's own same-line content is itself another anchor.
       * c-ns-properties permits at most one anchor per node; the had_anchor
       * check below cannot catch this by itself, because the "is this an
       * anchored key" fast path above would otherwise take priority and
       * silently accept it whenever what follows the second anchor happens
       * to look like a valid key (e.g. "&a &b foo: bar"), the same way it
       * correctly accepts the legitimate cross-line case ("top: &a\n  &b
       * key: val", handled via the rest_of_line_is_blank branch above,
       * which this branch is not). Caught here, before ever recursing,
       * rather than relying on the callee's own had_anchor check, which
       * only fires once the callee's OWN fast-path attempt happens to
       * fail (e.g. when nothing after the second anchor looks like a key). */
      parse_err(ctx, "a node cannot carry two anchors at position %zu",
                ctx->pos);
      _mem_free(ctx->mp, name);
      return NULL;
    } else {
      n = parse_node(ctx, indent, in_flow, seq_ok_at_indent, allow_inline_map,
                     true, had_tag, resolved_tag);
    }
    if (!n) {
      _mem_free(ctx->mp, name);
      return NULL;
    }
    cyaml_node_t *clone = (cyaml_node_t *)cyaml_clone((cyaml)n);
    if (!clone) {
      if (_parse_node_budget_exhausted)
        parse_err(ctx,
                  "anchor '%s' registration exceeded the %d node-allocation "
                  "limit at position %zu; check for exponential anchor "
                  "nesting",
                  name, CYAML_MAX_PARSE_NODES, ctx->pos);
      else
        parse_err(ctx, "out of memory cloning anchor '%s' at position %zu",
                  name, ctx->pos);
      __cyaml_destroy((cyaml)n);
      _mem_free(ctx->mp, name);
      return NULL;
    }
    if (!anchors_store(ctx, name, clone)) {
      /* anchors_store() has already reported the specific error and
       * consumed clone; n is still a fully valid parsed node, but the
       * document as a whole must still fail here, since a later '*name'
       * alias reference to this anchor could otherwise resolve against
       * nothing (or, worse, against a stale entry from an outer scope)
       * instead of getting the real "out of memory" failure. */
      __cyaml_destroy((cyaml)n);
      _mem_free(ctx->mp, name);
      return NULL;
    }
    _mem_free(ctx->mp, name);
    return n;
  }

  /* Alias */
  if (c == '*') {
    /* "*alias : value" uses the alias directly as an implicit key (see
     * try_parse_scalar_dict_key's own identical handling, needed here too
     * since this top-level dispatch is also reached generically; e.g.
     * recursing through an anchor with nothing else on its own line, as
     * "top: &node\n  *alias : value\n" does. Gated on allow_inline_map for
     * the same reason the '&' branch's identical fast path is above. */
    if (!in_flow && allow_inline_map) {
      int col = current_col(ctx);
      char *anchor_name = NULL;
      char *key_str = NULL;
      if (try_parse_scalar_dict_key(ctx, &anchor_name, &key_str, NULL, NULL)) {
        /* Reached only when the dispatch character itself is '*', so this
         * can only ever be the alias-as-key branch inside try_parse_
         * scalar_dict_key, which is structurally never a merge candidate
         * (see that function's own doc comment); no need to even ask for
         * the out-param here. */
        _mem_free(ctx->mp,
                  anchor_name); /* an alias key has no name of its own */
        cyaml_node_t *map = parse_block_dictionary(
            ctx, col, key_str, /*colon_consumed=*/true, false);
        _mem_free(ctx->mp, key_str);
        return finalize_collection_node(ctx, map, resolved_tag);
      }
      if (ctx->error[0]) return NULL;
      /* Not a key after all; try_parse_scalar_dict_key already restored
       * ctx->pos, so fall through to ordinary alias resolution below. */
    }

    if (had_anchor || had_tag) {
      /* c-ns-alias-node is its own top-level alternative in ns-flow-node's
       * grammar, entirely separate from the c-ns-properties branch an
       * anchor/tag decorates; an alias can never itself carry a
       * property at all (verified against two independent reference
       * parsers, both of which reject "&anchor *alias" and "!!tag
       * *alias" identically). This is a different, stricter rule than
       * the two-stacked-properties check in the '&'/'!' branches above:
       * even a SINGLE anchor or tag decorating a bare alias (not an
       * alias-as-key, already handled by the fast path above) is
       * invalid, not just a second one. had_tag (not resolved_tag) is
       * checked here so a bare non-specific "! *alias" is caught too,
       * even though a non-specific tag's own resolved_tag is NULL. */
      parse_err(ctx, "an alias cannot carry an anchor or tag at position %zu",
                ctx->pos);
      return NULL;
    }

    ctx->pos++;
    char *name = NULL;
    if (!parse_anchor_name(ctx, &name)) return NULL;
    cyaml_node_t *anchored = anchors_lookup(ctx, name);
    if (!anchored) {
      parse_err(ctx, "unknown alias '*%s' at position %zu", name, ctx->pos);
      _mem_free(ctx->mp, name);
      return NULL;
    }
    cyaml_node_t *clone = (cyaml_node_t *)cyaml_clone((cyaml)anchored);
    if (!clone && _parse_node_budget_exhausted)
      parse_err(ctx,
                "alias '*%s' expansion exceeded the %d node-allocation "
                "limit at position %zu; check for exponential anchor/alias "
                "nesting",
                name, CYAML_MAX_PARSE_NODES, ctx->pos);
    _mem_free(ctx->mp, name);
    return clone;
  }

  /* Tag (ignored) */
  if (c == '!') {
    /* Mirrors the '&' branch's identical check just above: "!!tag key:
     * value" tags just the key scalar, not the entire dictionary that key
     * turns out to introduce. try_parse_scalar_dict_key already handles a
     * bare tag prefix (no preceding anchor) on its own; but c-ns-
     * properties permits the anchor and tag in EITHER order, so this key
     * may still carry an anchor found via that same call (e.g. "!!str
     * &a1 key:"), which must be registered exactly like the '&' branch
     * does, not silently discarded just because THIS branch was entered
     * via the tag rather than the anchor. Gated on allow_inline_map for
     * the same reason the '&' branch's identical fast path is above. */
    if (!in_flow && allow_inline_map) {
      int col = current_col(ctx);
      char *anchor_name = NULL;
      char *key_str = NULL;
      cyaml_node_t *anchor_value = NULL;
      if (try_parse_scalar_dict_key(ctx, &anchor_name, &key_str, NULL,
                                    &anchor_value)) {
        /* Reached only when the dispatch character itself is '!', so
         * try_parse_scalar_dict_key's own prop-scanning loop always sees
         * a leading tag sigil here, structurally disqualifying this key
         * from ever being a merge candidate (see that function's own doc
         * comment); no need to even ask for the out-param here. */
        if (!register_key_anchor(ctx, anchor_name, key_str, anchor_value)) {
          _mem_free(ctx->mp, key_str);
          return NULL;
        }
        cyaml_node_t *map = parse_block_dictionary(
            ctx, col, key_str, /*colon_consumed=*/true, false);
        _mem_free(ctx->mp, key_str);
        return finalize_collection_node(ctx, map, resolved_tag);
      }
      if (ctx->error[0]) return NULL;
      /* Not a key after all; try_parse_scalar_dict_key already restored
       * ctx->pos, so fall through to ordinary tag handling below. */
    }

    if (had_tag) {
      /* Mirrors the '&' branch's identical had_anchor check above: this
       * tag is ordinary content for an ENCLOSING tag, which already used
       * up the one tag a single node may carry. Checked via had_tag, not
       * resolved_tag, so a bare non-specific enclosing "!" (whose own
       * resolved_tag is NULL - see parse_tag_token's doc comment) still
       * correctly rejects a second, stacked tag underneath it. */
      parse_err(ctx, "a node cannot carry two tags at position %zu", ctx->pos);
      return NULL;
    }

    char *tag = NULL;
    if (!parse_tag_token(ctx, &tag)) return NULL;
    size_t after_tag_pos = ctx->pos;
    skip_inline_ws(ctx);
    if (ctx->pos == after_tag_pos && !at_end(ctx) &&
        (cur(ctx) == '[' || cur(ctx) == '{' ||
         (!in_flow &&
          (cur(ctx) == ',' || cur(ctx) == ']' || cur(ctx) == '}')))) {
      /* Mirrors the '&' branch's identical check above (see its own,
       * fuller doc comment): a flow-collection OPENER ('['/'{') glued
       * directly onto a tag has no valid interpretation in any context
       * (e.g. "!!seq[1,2]" inside "[!!seq[1,2]]"), so it is rejected even
       * in flow context; ','/']'/'}' are legitimate immediately after a
       * tag INSIDE a flow collection (e.g. "[!!str, x]") and are only
       * rejected in block context, where they have no valid meaning at
       * all (e.g. "!!str,xxx", verified against a reference parser). */
      parse_err(ctx, "unexpected '%c' directly after tag at position %zu",
                cur(ctx), ctx->pos);
      _mem_free(ctx->mp, tag);
      return NULL;
    }
    if (ctx->pos == after_tag_pos && !at_end(ctx) && cur(ctx) != '\n' &&
        cur(ctx) != '\r' && cur(ctx) != '#' && cur(ctx) != '!' &&
        cur(ctx) != '&' && cur(ctx) != '*' &&
        !(in_flow && (cur(ctx) == ',' || cur(ctx) == ']' || cur(ctx) == '}'))) {
      /* c-ns-properties requires s-separate(n,c) before any following
       * ns-flow-content; a verbatim or shorthand tag with no separating
       * whitespace at all before ordinary content (e.g. "!<a>b", a quote,
       * a block scalar indicator, or plain-scalar text) has no valid
       * grammar path, verified against a reference parser ("!<a>b" and
       * "!<a>|\n  x\n" are both rejected). The flow-opener case just above
       * already covers '['/'{'; a following '#'/'!'/'&'/'*' is left to
       * the more specific checks elsewhere in this branch (comment,
       * stacked tag, an anchor/alias following the tag). */
      parse_err(ctx,
                "tag must be separated from its content by whitespace at "
                "position %zu",
                ctx->pos);
      _mem_free(ctx->mp, tag);
      return NULL;
    }

    cyaml_node_t *n;
    if (!in_flow && rest_of_line_is_blank(ctx)) {
      /* Nothing else (but possibly a comment) on this line: mirror the
       * '&' branch's identical guard (see its own doc comment above) so a
       * sibling entry at or below `indent` (e.g. a following list entry
       * at the same column as "- !!str" itself) is never misread as
       * this tag's own value. An earlier version of this branch recursed
       * unconditionally whenever the rest of the line was blank, which let
       * it walk straight past the newline and swallow that sibling (and
       * everything after it) into this tag's value instead, exactly the
       * bug the '&' branch's own comment already warns against. */
      skip_ws_comments(ctx);
      bool is_value_col =
          !at_end(ctx) && at_block_value_col(ctx, indent, seq_ok_at_indent);
      if (!is_value_col && !at_end(ctx) && ctx->error[0]) {
        _mem_free(ctx->mp, tag);
        return NULL;
      }
      if (at_end(ctx) || !is_value_col) {
        /* Nothing at all follows this tag: route through
         * finalize_scalar_node (empty text) rather than synthesizing a
         * bare CYAML_NULL directly, so e.g. "!!str" with nothing after it
         * correctly resolves to an empty string, not null. */
        char *empty = ccol_strdup(ctx->mp, "");
        n = empty ? finalize_scalar_node(ctx, empty, tag, true) : NULL;
      } else {
        n = parse_node(ctx, indent, in_flow, seq_ok_at_indent, true, had_anchor,
                       true, tag);
      }
    } else if (in_flow && at_eol(ctx)) {
      /* Flow context: the tagged value may be wrapped onto a following
       * line (see the identical case in the '&' branch above); must cross
       * the newline via flow_skip_ws (not plain skip_ws_comments), so this
       * continuation line is held to the same s-separate(n,c)
       * indentation/no-tab rules every other flow-collection continuation
       * line in this file already enforces; parse_node's own
       * top-of-function skip only covers inline whitespace when in_flow, so
       * the newline must be crossed explicitly here before recursing, or
       * dispatch would see '\n' next and misparse it as an empty plain
       * scalar. There is no sibling-entry ambiguity to guard against here
       * the way the block-context branch above does; a flow collection's
       * own ','/']'/'}' terminators, not indentation, delimit entries. */
      if (!flow_skip_ws(ctx, indent)) {
        _mem_free(ctx->mp, tag);
        return NULL;
      }
      n = parse_node(ctx, indent, in_flow, seq_ok_at_indent, allow_inline_map,
                     had_anchor, true, tag);
    } else if (!in_flow && at_bare_seq_indicator(ctx)) {
      /* Mirrors the '&' branch's identical check above: a block sequence
       * cannot start inline right after a tag on the same line either
       * (only a mapping gets that "compact" privilege, and only when
       * introduced by '-' itself, not by a preceding node property). */
      parse_err(ctx,
                "a block sequence cannot start on the same line as a "
                "tag at position %zu",
                ctx->pos);
      _mem_free(ctx->mp, tag);
      return NULL;
    } else if (!in_flow && cur(ctx) == '!') {
      /* This tag's own same-line content is itself another tag.
       * c-ns-properties permits at most one tag per node; mirrors the '&'
       * branch's identical same-line double-anchor check above (see its
       * own doc comment for why had_tag's check further up cannot catch
       * this by itself). */
      parse_err(ctx, "a node cannot carry two tags at position %zu", ctx->pos);
      _mem_free(ctx->mp, tag);
      return NULL;
    } else {
      n = parse_node(ctx, indent, in_flow, seq_ok_at_indent, allow_inline_map,
                     had_anchor, true, tag);
    }

    if (!n) {
      _mem_free(ctx->mp, tag);
      return NULL;
    }

    /* By this point `tag` has already been consumed at the exact point of
     * construction, however many layers of recursion deep that turned out
     * to be: for a scalar, by finalize_scalar_node; for a collection, by
     * finalize_collection_node (both reached via `tag` being forwarded,
     * unchanged, through any intervening '&' anchor layers; see either
     * function's own doc comment for why attachment cannot be deferred to
     * here, after recursion returns: an anchor sitting between this tag
     * and the actual content would otherwise clone an untagged node for
     * its own anchor table before this point ever ran). This local copy
     * was only ever borrowed by that whole chain, never transferred, so
     * it is unconditionally freed here regardless of which path was
     * taken. */
    _mem_free(ctx->mp, tag);
    return n;
  }

  /* Flow list */
  /* Flow dictionary */
  if (c == '[' || c == '{') {
    int col = current_col(ctx);
    size_t coll_start = ctx->pos;
    cyaml_node_t *coll = (c == '[') ? parse_flow_list(ctx, indent)
                                    : parse_flow_dictionary(ctx, indent);
    if (!coll) return NULL;

    /* In block context, a flow collection immediately followed by ':' is
     * itself a (non-scalar) implicit block mapping key; YAML 1.2 sec.
     * 8.2.2's "compact mapping" form, ns-l-compact-mapping, permits any
     * ns-flow-node as a key here, not just a scalar; mirroring the
     * identical check the plain/quoted-scalar dispatch branches already
     * perform for a scalar key. But like every implicit key (scalar or
     * not), it must fit on a single line (ns-s-implicit-yaml-key); a flow
     * collection that itself spanned multiple lines is a valid standalone
     * value, just never an implicit key, regardless of what follows it. */
    bool spans_multiple_lines = span_crosses_newline(ctx, coll_start);
    if (!in_flow && !spans_multiple_lines) {
      skip_inline_ws(ctx);
      if (!at_end(ctx) && cur(ctx) == ':') {
        char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
        if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
            ctx->pos + 1 >= ctx->len) {
          if (!allow_inline_map) {
            /* Same-line chaining restriction as the scalar-key cases above
             * ("a: [1,2]: c" has no valid interpretation, identically to
             * "a: b: c"; see allow_inline_map's own doc comment). */
            parse_err(ctx,
                      "mapping values are not allowed here "
                      "(position %zu)",
                      ctx->pos);
            __cyaml_destroy((cyaml)coll);
            return NULL;
          }
          char *key_str =
              node_to_dict_key_string(ctx, coll, "block dictionary");
          if (!key_str) return NULL;
          /* A flow collection can never be a merge-key candidate ("<<" is
           * always a plain scalar). */
          cyaml_node_t *map =
              parse_block_dictionary(ctx, col, key_str, false, false);
          _mem_free(ctx->mp, key_str);
          /* See the double-quoted scalar branch's own identical comment
           * for why resolved_tag is unambiguously safe to attach to the
           * resulting mapping here, rather than to the flow collection
           * that turned out to become this mapping's own key. */
          return finalize_collection_node(ctx, map, resolved_tag);
        }
      }
    }
    return finalize_collection_node(ctx, coll, resolved_tag);
  }

  /* Literal block scalar */
  if (c == '|' && !in_flow) {
    ctx->pos++;
    chomp_t chomp;
    int exp_indent;
    if (!parse_block_scalar_header(ctx, &chomp, &exp_indent)) return NULL;
    char *s = NULL;
    if (!parse_block_scalar_content(ctx, indent, chomp, exp_indent, &s))
      return NULL;
    return finalize_scalar_node(ctx, s, resolved_tag, false);
  }

  /* Folded block scalar */
  if (c == '>' && !in_flow) {
    ctx->pos++;
    chomp_t chomp;
    int exp_indent;
    if (!parse_block_scalar_header(ctx, &chomp, &exp_indent)) return NULL;
    char *s = NULL;
    if (!parse_folded_scalar_content(ctx, indent, chomp, exp_indent, &s))
      return NULL;
    return finalize_scalar_node(ctx, s, resolved_tag, false);
  }

  /* Block list (- item) */
  if (c == '-' && !in_flow) {
    if (ctx->pos + 1 < ctx->len) {
      char next = ctx->src[ctx->pos + 1];
      if (next == '\t') {
        /* YAML 1.2 sec. 6.1: tab characters are never valid as
         * block-structural indentation or separation, because different
         * systems treat their width differently; a bare '-' immediately
         * followed by one has no valid interpretation at all (verified
         * against two independent reference parsers, both of which
         * reject this unconditionally, not merely reinterpret '-' as an
         * ordinary plain scalar character). */
        parse_err(ctx,
                  "tab cannot follow a block sequence indicator '-' "
                  "at position %zu",
                  ctx->pos + 1);
        return NULL;
      }
      if (next == ' ' || next == '\n' || next == '\r') {
        int col = current_col(ctx);
        return finalize_collection_node(ctx, parse_block_list(ctx, col),
                                        resolved_tag);
      }
    } else {
      /* '-' at very end of input = single-element list with null value. */
      int col = current_col(ctx);
      return finalize_collection_node(ctx, parse_block_list(ctx, col),
                                      resolved_tag);
    }
  }

  /* Block mapping (explicit "? key" / ": value") */
  if (c == '?' && !in_flow) {
    bool is_explicit_key = false;
    if (ctx->pos + 1 < ctx->len) {
      char next = ctx->src[ctx->pos + 1];
      if (next == '\t') {
        /* See the identical check on the '-' branch above: a tab is
         * never valid as block-structural separation, and a bare '?'
         * immediately followed by one has no valid interpretation
         * (verified against two independent reference parsers). */
        parse_err(ctx,
                  "tab cannot follow an explicit key indicator '?' "
                  "at position %zu",
                  ctx->pos + 1);
        return NULL;
      }
      if (next == ' ' || next == '\n' || next == '\r') is_explicit_key = true;
    } else {
      is_explicit_key = true; /* '?' at very end of input. */
    }
    if (is_explicit_key) {
      if (!allow_inline_map) {
        /* Same-line chaining restriction as every other "this could open a
         * fresh mapping here" case above (plain/quoted-scalar key, flow-
         * collection key): ns-l-block-map-implicit-value has no compact-
         * mapping alternative, so entering a NEW explicit-key mapping from
         * an ordinary implicit value's own same-line content (e.g. "a: ?
         * b\n   : c") has no valid interpretation either, even though the
         * explicit form is otherwise perfectly legal once already inside
         * explicit-style content (where allow_inline_map is already true). */
        parse_err(ctx,
                  "mapping values are not allowed here "
                  "(position %zu)",
                  ctx->pos);
        return NULL;
      }
      int col = current_col(ctx);
      return finalize_collection_node(
          ctx, parse_block_dictionary(ctx, col, NULL, false, false),
          resolved_tag);
    }
  }

  /* Double-quoted scalar */
  if (c == '"') {
    int col = current_col(ctx);
    size_t quote_start = ctx->pos;
    char *s = NULL;
    if (!parse_double_quoted(ctx, &s)) return NULL;
    /* In block context a quoted scalar may be a dictionary key. */
    if (!in_flow) {
      skip_inline_ws(ctx);
      if (!at_end(ctx) && cur(ctx) == ':') {
        char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
        if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
            ctx->pos + 1 >= ctx->len) {
          if (span_crosses_newline(ctx, quote_start)) {
            /* An implicit key is always single-line (ns-s-implicit-yaml-
             * key), exactly like a plain scalar key; a quoted scalar
             * spanning multiple lines has no valid interpretation as a
             * key (verified against two independent reference parsers). */
            parse_err(ctx,
                      "implicit keys cannot span multiple lines at "
                      "position %zu",
                      ctx->pos);
            _mem_free(ctx->mp, s);
            return NULL;
          }
          if (!allow_inline_map) {
            /* Same-line chaining restriction as the plain-scalar case
             * above ("a: 'b': c" has no valid interpretation, identically
             * to "a: b: c"; see allow_inline_map's own doc comment). */
            parse_err(ctx,
                      "mapping values are not allowed here "
                      "(position %zu)",
                      ctx->pos);
            _mem_free(ctx->mp, s);
            return NULL;
          }
          /* A quoted scalar can never be a merge-key candidate (see
           * try_parse_scalar_dict_key's own doc comment: only the plain-
           * scalar production is ever eligible). */
          cyaml_node_t *map = parse_block_dictionary(ctx, col, s, false, false);
          _mem_free(ctx->mp, s);
          /* resolved_tag reaching this "value turns out to be a key"
           * shape can only ever happen via a tag forwarded through one
           * or more '&' anchor layers (a tag directly adjacent to a key
           * is always intercepted earlier, by try_parse_scalar_dict_key's
           * own preamble in the '&'/'!' branches), so there is no
           * ambiguity here about who the tag belongs to: it decorates
           * the whole resulting mapping, exactly like the explicit '?'
           * and flow-collection-as-key cases elsewhere in this
           * function. */
          return finalize_collection_node(ctx, map, resolved_tag);
        }
      }
    }
    /* A quoted scalar is always a string (no implicit typing), unless an
     * explicit tag forces something else. */
    return finalize_scalar_node(ctx, s, resolved_tag, false);
  }

  /* Single-quoted scalar */
  if (c == '\'') {
    int col = current_col(ctx);
    size_t quote_start = ctx->pos;
    char *s = NULL;
    if (!parse_single_quoted(ctx, &s)) return NULL;
    /* In block context a quoted scalar may be a dictionary key. */
    if (!in_flow) {
      skip_inline_ws(ctx);
      if (!at_end(ctx) && cur(ctx) == ':') {
        char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
        if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
            ctx->pos + 1 >= ctx->len) {
          if (span_crosses_newline(ctx, quote_start)) {
            /* See the identical check in the double-quoted branch above. */
            parse_err(ctx,
                      "implicit keys cannot span multiple lines at "
                      "position %zu",
                      ctx->pos);
            _mem_free(ctx->mp, s);
            return NULL;
          }
          if (!allow_inline_map) {
            /* Same-line chaining restriction as the plain-scalar case
             * above ("a: 'b': c" has no valid interpretation, identically
             * to "a: b: c"; see allow_inline_map's own doc comment). */
            parse_err(ctx,
                      "mapping values are not allowed here "
                      "(position %zu)",
                      ctx->pos);
            _mem_free(ctx->mp, s);
            return NULL;
          }
          /* A quoted scalar can never be a merge-key candidate; see the
           * double-quoted branch's own identical comment. */
          cyaml_node_t *map = parse_block_dictionary(ctx, col, s, false, false);
          _mem_free(ctx->mp, s);
          /* See the identical comment in the double-quoted branch above. */
          return finalize_collection_node(ctx, map, resolved_tag);
        }
      }
    }
    return finalize_scalar_node(ctx, s, resolved_tag, false);
  }

  /* '#' can never start a plain scalar in any context (YAML 1.2 sec. 6.6,
   * ns-plain-first categorically excludes it); reaching here with '#'
   * means skip_ws_comments/skip_inline_ws already tried and failed to
   * treat it as a comment (not preceded by whitespace, start-of-input, or
   * a newline), so there is no valid interpretation left. */
  if (c == '#') {
    parse_err(ctx,
              "unexpected '#' at position %zu (a comment must be "
              "preceded by whitespace)",
              ctx->pos);
    return NULL;
  }

  /* '%', '@', and '`' can likewise never start a plain scalar in any
   * context (YAML 1.2 sec. 6.6, ns-plain-first categorically excludes all
   * three c-indicator characters; '@' and '`' are additionally reserved
   * for future use by sec. 5.5). Reaching here with one of them means
   * every earlier, more specific dispatch (directives, tags, anchors,
   * aliases, block/flow indicators, quoted scalars) already declined to
   * claim it, so (exactly like '#' just above) there is no valid
   * interpretation left. needs_quoting() (used by the serializer) already
   * treats all three as requiring quoting on output; without this check,
   * the parser would silently accept unquoted input the library's own
   * serializer would never produce. */
  if (c == '%' || c == '@' || c == '`') {
    parse_err(ctx,
              "unexpected '%c' at position %zu (not a valid plain scalar "
              "start character)",
              c, ctx->pos);
    return NULL;
  }

  /* In flow context, '-'/'?'/':' immediately followed by a flow indicator
   * (or whitespace/EOL/EOF) has no valid interpretation at all; see
   * at_valid_flow_plain_scalar_start's own doc comment. */
  if (in_flow && !at_valid_flow_plain_scalar_start(ctx)) {
    parse_err(ctx,
              "unexpected '%c' at position %zu (not a valid plain "
              "scalar in flow context)",
              c, ctx->pos);
    return NULL;
  }

  /* Plain scalar (or block dictionary key) */
  {
    int col = current_col(ctx);
    char *s = NULL;
    bool hit_real_terminator = false;
    if (!parse_plain_scalar(ctx, in_flow, &s, &hit_real_terminator))
      return NULL;

    /* Check if a ':' value indicator follows (making this a dictionary key).
     * An implicit key is always single-line, so this check must happen
     * before any multi-line continuation is ever attempted below. */
    if (!in_flow) {
      skip_inline_ws(ctx);
      if (!at_end(ctx) && cur(ctx) == ':') {
        char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
        if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
            ctx->pos + 1 >= ctx->len) {
          if (!allow_inline_map) {
            /* This scalar is itself the same-line content of an ordinary
             * implicit value; it has no license to open a further nested
             * mapping here ("a: b: c"; see allow_inline_map's own doc
             * comment above parse_node for the reasoning and the reference-
             * parser verification). */
            parse_err(ctx,
                      "mapping values are not allowed here "
                      "(position %zu)",
                      ctx->pos);
            _mem_free(ctx->mp, s);
            return NULL;
          }
          /* This is a block dictionary key. See the double-quoted branch's
           * own comment above for why resolved_tag is unambiguously safe
           * to attach to the resulting mapping here. Separately: this is
           * the one "becomes a key" site that a genuine plain, untagged
           * "<<" merge-key trigger can actually reach (see try_parse_
           * scalar_dict_key's own doc comment for the exact rule an
           * anchor-decorated "<<" (reached via the '&' branch's own
           * preamble instead) also has to satisfy). resolved_tag here is
           * whatever tag an ENCLOSING '!' layer is delegating down to the
           * resulting mapping as a whole (e.g. "m: !!map\n  <<: *b\n"); it
           * says nothing about whether the KEY "<<" itself carries a tag;
           * a tag directly on the key would already have been
           * intercepted by the '!' branch's own fast path above, never
           * reaching this plain-scalar dispatch at all. Disqualifying the
           * merge whenever the enclosing collection merely happens to be
           * tagged was a bug: it silently left "<<" as a literal,
           * unmerged key instead of expanding it. */
          bool is_merge_candidate = strcmp(s, "<<") == 0;
          /* This plain-scalar key must canonicalize through the exact same
           * core-schema typing node_to_dict_key_string() already applies to
           * every OTHER key-capture site (try_parse_scalar_dict_key's own
           * flow-collection/alias-as-key/anchored-or-tagged-plain-scalar
           * branches, a flow dictionary/sequence key, and an explicit
           * "? key" block key); without this, an unadorned first entry like
           * "~: v" stored the literal text "~" while "? ~\n: v" stored
           * "null" for the identical value, even though YAML 1.2 treats the
           * two notations as exactly equivalent spellings of the same
           * entry. is_merge_candidate above is deliberately computed from
           * the raw text s, matching every other merge-key check in this
           * file (only a literal, untyped "<<" is ever a trigger). */
          cyaml_node_t *typed = make_typed_scalar(ctx, s);
          if (!typed) {
            parse_err(ctx,
                      "out of memory typing dictionary key at position %zu",
                      ctx->pos);
            _mem_free(ctx->mp, s);
            return NULL;
          }
          char *canon_key =
              node_to_dict_key_string(ctx, typed, "block dictionary");
          _mem_free(ctx->mp, s);
          if (!canon_key) return NULL;
          cyaml_node_t *map = parse_block_dictionary(ctx, col, canon_key, false,
                                                     is_merge_candidate);
          _mem_free(ctx->mp, canon_key);
          return finalize_collection_node(ctx, map, resolved_tag);
        }
      }
    }

    /* Not a dictionary key: a plain scalar value may span multiple lines
     * (YAML 1.2 sec. 7.3.3). */
    char *full = NULL;
    if (!parse_plain_scalar_multiline(ctx, in_flow, indent, s,
                                      hit_real_terminator, &full))
      return NULL;

    return finalize_scalar_node(ctx, full, resolved_tag, true);
  }
}

/* Block list */

static cyaml_node_t *parse_block_list(parse_ctx_t *ctx, int seq_indent) {
  cyaml_node_t *seq = (cyaml_node_t *)cyaml_create_list_mp(ctx->mp);
  if (!seq) return NULL;

  while (!at_end(ctx)) {
    /* Check we're at the right indent and the '-' indicator. */
    skip_ws_comments(ctx);
    if (at_end(ctx)) break;

    if (line_indent_has_tab(ctx)) {
      parse_err(ctx,
                "tab cannot be used as block sequence indentation at "
                "position %zu",
                ctx->pos);
      goto fail;
    }
    int col = current_col(ctx);
    if (col != seq_indent) break;
    if (cur(ctx) != '-') break;

    /* Is the '-' a list indicator? */
    bool is_seq_entry = false;
    if (ctx->pos + 1 < ctx->len) {
      char next = ctx->src[ctx->pos + 1];
      if (next == '\t') {
        /* See the identical check in parse_node's own '-' dispatch: a
         * tab is never valid block-structural separation. */
        parse_err(ctx,
                  "tab cannot follow a block sequence indicator '-' "
                  "at position %zu",
                  ctx->pos + 1);
        goto fail;
      }
      if (next == ' ' || next == '\n' || next == '\r') is_seq_entry = true;
    } else {
      is_seq_entry = true; /* '-' at EOF */
    }

    if (!is_seq_entry) break;

    ctx->pos++; /* consume '-' */

    /* Skip optional space after '-'. */
    if (!at_end(ctx) && cur(ctx) == ' ') ctx->pos++;

    if (!at_end(ctx) && cur(ctx) == '\t') {
      /* A tab immediately following the (possibly already-consumed)
       * single separator space is just as invalid as one directly after
       * '-' itself; e.g. "- \t-" (dash, space, tab, dash) has no valid
       * interpretation, matching "-\t-" (verified against two
       * independent reference parsers). Without this check, parse_node's
       * own top-of-function skip_ws_comments would silently cross the
       * tab and dispatch on whatever follows it as if the tab had never
       * been there at all. */
      parse_err(ctx,
                "tab cannot follow a block sequence indicator '-' "
                "at position %zu",
                ctx->pos);
      goto fail;
    }

    /* Determine where the element value is. rest_of_line_is_blank(), not a
     * bare at_eol()/at_end() check, is what correctly treats "- # Empty"
     * (a dash followed only by a trailing comment) the same as a bare
     * "-": cur(ctx) is '#' at this point, not a newline, so at_eol() alone
     * would wrongly conclude real content starts here and hand the
     * comment text itself to parse_node() as if it were this entry's
     * value; which then went on to swallow every following sibling
     * entry as nested content instead of a null entry followed by its
     * own separate siblings. */
    cyaml_node_t *elem;
    if (rest_of_line_is_blank(ctx) || at_end(ctx)) {
      /* Nothing on this line after '-'; peek at the next non-empty line.
       * If it is more indented than seq_indent it is the element value;
       * otherwise (same or less indent, or EOF) the element is null. */
      skip_ws_comments(ctx);
      if (!at_end(ctx) && line_indent_has_tab(ctx)) {
        parse_err(ctx,
                  "tab cannot be used as block sequence indentation at "
                  "position %zu",
                  ctx->pos);
        goto fail;
      }
      if (at_end(ctx) || current_col(ctx) <= seq_indent) {
        elem = node_alloc(CYAML_NULL, ctx->mp);
      } else {
        elem =
            parse_node(ctx, seq_indent, false, false, true, false, false, NULL);
      }
    } else {
      /* The element starts on the same line.  Pass seq_indent so block
       * scalars nested inside the element use the list's indent as
       * their parent_indent, which is what the YAML spec requires.
       * seq_ok_at_indent is false: seq_indent is a sequence's own indent,
       * not a mapping's, so a '-' at exactly that column (reached only
       * through a decorating anchor/tag on this very element) is always a
       * sibling element of this same list, never nested content.
       * allow_inline_map is true: "- key: value" (a compact mapping right
       * after the dash) is always a legitimate, ordinary pattern; the
       * "a: b: c" chaining restriction is specific to an ordinary
       * implicit mapping VALUE's own same-line content, not a sequence
       * element's. */
      elem =
          parse_node(ctx, seq_indent, false, false, true, false, false, NULL);
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

/* Block dictionary */

/*
 * Parse the node occupying a block dictionary key or value position: either
 * on the same line as the '?'/':' indicator that precedes it, or, if
 * nothing follows before end-of-line, on a subsequent more-indented line
 * (otherwise the position holds an implicit null).  Shared by the
 * explicit-key and explicit-value sides of parse_block_dictionary, and by
 * an implicit entry's own value, all three of which share YAML 1.2 sec.
 * 8.2.2's placement grammar for content on a following line.
 *
 * allow_inline_seq distinguishes the one point where explicit and
 * implicit entries genuinely differ for a SEQUENCE: explicit-style
 * content (both the '?' key's own content and the ':' value that follows
 * it) is grammatically s-l+block-indented, whose "compact" alternative
 * permits a bare '-' sequence to start inline on the very same line
 * (verified against a reference parser, not assumed); but an ordinary
 * implicit entry's value ("key: value") is instead ns-l-block-map-
 * implicit-value, which has no such compact alternative at all: a
 * sequence value there must always begin on its own line. The identical
 * asymmetry holds for a nested MAPPING chained on the very same line
 * ("a: b: c" has no valid interpretation and is a hard error, while
 * "? k\n: a: b" is accepted); since both restrictions apply, and lift,
 * together at exactly the same call site, allow_inline_seq is reused
 * directly as parse_node's own allow_inline_map argument below rather
 * than threading a second, always-identical parameter through this
 * function too. Callers pass true only for explicit-style key/value
 * content.
 *
 * is_merge_candidate_out, when non-NULL, receives whether the node about
 * to be parsed is a genuine "<<" merge-key candidate (see
 * explicit_key_peek_is_merge_candidate's own doc comment), determined
 * right here rather than by the caller: this is the one point that has
 * already resolved whether the content starts on this line or a
 * subsequent one, so it is the only position at which a peek's own
 * documented "already at the start of the key's real content"
 * precondition genuinely holds in both cases. Pass NULL for a value
 * position, which can never be a merge key.
 */
static cyaml_node_t *parse_block_map_node(parse_ctx_t *ctx, int map_indent,
                                          bool allow_inline_seq,
                                          bool *is_merge_candidate_out) {
  if (is_merge_candidate_out) *is_merge_candidate_out = false;
  if (rest_of_line_is_blank(ctx)) {
    skip_ws_comments(ctx);
    bool is_value_col =
        !at_end(ctx) && at_block_value_col(ctx, map_indent, true);
    if (!is_value_col && !at_end(ctx) && ctx->error[0]) return NULL;
    if (at_end(ctx) || !is_value_col) return node_alloc(CYAML_NULL, ctx->mp);
    /* ctx is now positioned exactly at the start of the node's own real
     * content, whether that content began on this same call's own line or,
     * as here, was pushed to a fresh one; this is the one position at which
     * explicit_key_peek_is_merge_candidate's own documented precondition
     * ("already confirmed to be the start of a dictionary key's own
     * content") actually holds for BOTH cases, which is why the peek lives
     * here rather than in this function's own caller (a caller-side peek,
     * taken before it's known whether the key's content starts on this
     * line or a later one, would see only whatever follows '?' on the '?'
     * line itself - typically nothing but the newline this branch exists
     * to skip past - and wrongly conclude "not a merge candidate" for
     * every multi-line explicit key, even a genuine "? \n  <<\n: ..."
     * merge). Only requested (non-NULL) by the key call site below; the
     * value call site passes NULL, since a value is never a merge key. */
    if (is_merge_candidate_out)
      *is_merge_candidate_out =
          explicit_key_peek_is_merge_candidate(ctx, map_indent, false);
    /* A value found on its own fresh line is never an ambiguous same-line
     * chain, regardless of entry style, so allow_inline_map is
     * unconditionally true here (unlike the same-line case below). */
    return parse_node(ctx, map_indent, false, true, true, false, false, NULL);
  }
  if (!allow_inline_seq && at_bare_seq_indicator(ctx)) {
    parse_err(ctx,
              "a block sequence cannot start on the same line as its "
              "mapping key at position %zu",
              ctx->pos);
    return NULL;
  }
  if (is_merge_candidate_out)
    *is_merge_candidate_out =
        explicit_key_peek_is_merge_candidate(ctx, map_indent, false);
  return parse_node(ctx, map_indent, false, true, allow_inline_seq, false,
                    false, NULL);
}

/*
 * Peek (non-consuming: restores ctx->pos before returning) at a dictionary
 * entry's own key text to determine whether it is a genuine merge-key
 * trigger: only a plain, untagged scalar reading exactly "<<" counts (an
 * anchor does not disqualify it; a quote, a tag, or a non-scalar key
 * does). Despite the name, this is used at the start of EVERY dictionary
 * key's own content, implicit ("key:"/"{key:...}"/"[key:...]") as well as
 * explicit ("? key" / "{? key:...}"): a single-character check (only
 * inspecting whether the very first byte is a quote or a tag sigil) is
 * not sufficient to rule out a tag, since c-ns-properties permits an
 * anchor and a tag in either order ("&y !!str <<" has a tag despite its
 * first character being '&'); only walking the full anchor/tag property
 * chain, as this function does, correctly sees a tag that follows an
 * anchor. Confirmed against two independent reference parsers that the
 * explicit '? <<' form is a genuine merge trigger under the same
 * conditions as the implicit '<<:' shorthand, in both block and flow
 * context, not a form that is unconditionally exempt from merging
 * regardless of quoting.
 *
 * in_flow selects flow_skip_ws (crosses lines, matching the real flow-
 * dictionary key parse this peek precedes) vs. skip_inline_ws (block
 * context keeps its own single-physical-line key convention); indent is
 * only consulted by flow_skip_ws. The boundary check after "<<" is a
 * deliberately coarser OVER-approximation of scan_plain_scalar_line's own
 * in_flow-conditional termination rules, not an exact mirror of them:
 * scan_plain_scalar_line does not stop at plain internal whitespace (only
 * at EOL, ': '/':'+EOF, an inline ' #' comment, or a flow terminator), so a
 * key like "<< foo" genuinely parses as the single scalar "<< foo", not
 * "<<". Treating any whitespace right after "<<" as candidate-worthy can
 * therefore flag a non-merge key as a false-positive candidate; this is
 * safe because a caller either re-validates the real, fully-parsed key
 * text against the literal string "<<" before trusting the flag (see
 * try_parse_scalar_dict_key/parse_one_dict_entry_key's own strcmp gate),
 * or, for the two parse_flow_list callers that skip that re-check
 * ("[? key: value]" and the bare "[key: value]" compact-mapping
 * shorthand), relies instead on expand_merge_key's own
 * cyaml_dictionary_get(map, "<<") lookup being a safe no-op whenever the
 * freshly-built single-entry map's key isn't actually "<<". The ':'
 * handling (a ':' immediately followed by whitespace/EOF, or, in flow
 * context, by a flow terminator) does match scan_plain_scalar_line's own
 * rule exactly, since a flow key of "<<:" (colon glued on, no separating
 * whitespace) is a single different plain scalar in block context but
 * genuinely resolves to just "<<" in flow context, and getting this one
 * case right (unlike the whitespace over-approximation above) is load-
 * bearing for that distinction.
 *
 * Only ever called at a position already confirmed to be the start of a
 * dictionary key's own content (just past '?' and its required separator
 * for the explicit form; at the key position itself for the implicit
 * form). A malformed anchor/tag, or a flow_skip_ws failure (unterminated
 * flow collection), is simply treated as "not a merge candidate" and left
 * for the real parse (called by this function's caller right after) to
 * detect and report properly; ctx->error is explicitly cleared before
 * returning on either failure path, so this throwaway peek's own
 * diagnostic (parse_anchor_name()/flow_skip_ws() may have already called
 * parse_err()) can never linger and mask a later, unrelated, correctly-
 * guarded ("only set if not already set") error message from a different
 * part of the same parse. This function's own internal use of
 * skip_tag_token() (rather than parse_tag_token()) is safe precisely
 * because of that: it never needs the tag to be valid, only to correctly
 * skip past it so had_tag can be set; genuine tag validation still
 * happens when the real parse runs.
 */
static bool explicit_key_peek_is_merge_candidate(parse_ctx_t *ctx, int indent,
                                                 bool in_flow) {
  size_t saved_pos = ctx->pos;
  bool had_tag = false;
  bool had_anchor = false;
  for (int prop_pass = 0; prop_pass < 2; prop_pass++) {
    if (!at_end(ctx) && cur(ctx) == '&' && !had_anchor) {
      ctx->pos++;
      char *tmp_anchor = NULL;
      if (!parse_anchor_name(ctx, &tmp_anchor)) {
        /* This is only a throwaway peek (see this function's own doc
         * comment): a failure here is never the real, final error for
         * this position - the real parse right after this call is what
         * must report it. Clearing ctx->error (parse_anchor_name() may
         * have set it via parse_err()) prevents this peek's own, possibly
         * stale-by-then diagnostic from masking a later, unrelated,
         * correctly-guarded ("only set if not already set") error message
         * elsewhere in the same parse. */
        ctx->error[0] = '\0';
        ctx->pos = saved_pos;
        return false;
      }
      _mem_free(ctx->mp, tmp_anchor);
      had_anchor = true;
    } else if (!at_end(ctx) && cur(ctx) == '!' && !had_tag) {
      skip_tag_token(ctx);
      had_tag = true;
    } else {
      break;
    }
    bool ws_ok =
        in_flow ? flow_skip_ws(ctx, indent) : (skip_inline_ws(ctx), true);
    if (!ws_ok) {
      /* Same reasoning as the parse_anchor_name() failure above: this
       * peek's own error must not outlive the peek itself. */
      ctx->error[0] = '\0';
      ctx->pos = saved_pos;
      return false;
    }
  }
  bool is_candidate = false;
  if (!had_tag && !at_end(ctx) && ctx->pos + 1 < ctx->len &&
      ctx->src[ctx->pos] == '<' && ctx->src[ctx->pos + 1] == '<') {
    size_t after = ctx->pos + 2;
    if (after >= ctx->len) {
      is_candidate = true;
    } else {
      char c = ctx->src[after];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        is_candidate = true;
      } else if (in_flow && (c == ',' || c == ']' || c == '}')) {
        is_candidate = true;
      } else if (c == ':') {
        if (after + 1 >= ctx->len) {
          is_candidate = true;
        } else {
          char c2 = ctx->src[after + 1];
          if (c2 == ' ' || c2 == '\t' || c2 == '\n' || c2 == '\r')
            is_candidate = true;
          else if (in_flow && (c2 == ',' || c2 == ']' || c2 == '}'))
            is_candidate = true;
        }
      }
    }
  }
  ctx->pos = saved_pos;
  return is_candidate;
}

/*
 * Read one block dictionary entry's key, starting at the current position
 * (which must be at column map_indent).  Handles both the explicit '?
 * key' style and the implicit 'key:' style, mixed freely within one
 * dictionary.
 *
 * On success (return 1): *key_out receives an owned key string and
 * *have_colon_out reports whether a ':' value indicator was found for it
 * (explicit style permits a key with no value at all); ctx->pos is
 * positioned to begin value parsing.
 *
 * Return 0: the implicit-style peek found a scalar not followed by ':';
 * not a dictionary entry at all.  ctx->pos is restored to where this call
 * started, exactly as if it had never been called, so the caller can stop
 * without consuming anything.  A leading '?' is unambiguous and can never
 * yield this outcome.
 *
 * Return -1: a genuine parse error; ctx->error is already set.
 */
static int parse_one_dict_entry_key(parse_ctx_t *ctx, int map_indent,
                                    char **key_out, bool *have_colon_out,
                                    bool *is_explicit_out,
                                    bool *is_merge_candidate_out) {
  char c = cur(ctx);
  *is_merge_candidate_out = false;

  /* '?' is only an explicit-key indicator when followed by whitespace or
   * EOF/EOL (matching parse_node's own '?' dispatch check); otherwise it
   * legitimately starts a plain scalar key (e.g. "?foo: value"). */
  bool is_explicit_key_indicator = false;
  if (c == '?') {
    if (ctx->pos + 1 < ctx->len) {
      char next = ctx->src[ctx->pos + 1];
      if (next == '\t') {
        /* See the identical check in parse_node's own '?' dispatch: a tab
         * is never valid block-structural separation. This must be
         * enforced for every entry of a mapping, not only the first;
         * parse_node's own dispatch only ever sees the first entry; every
         * later entry reaches this function directly from
         * parse_block_dictionary's loop instead, so the check has to be
         * repeated here too. */
        parse_err(ctx,
                  "tab cannot follow an explicit key indicator '?' "
                  "at position %zu",
                  ctx->pos + 1);
        return -1;
      }
      if (next == ' ' || next == '\n' || next == '\r')
        is_explicit_key_indicator = true;
    } else {
      is_explicit_key_indicator = true;
    }
  }

  if (is_explicit_key_indicator) {
    ctx->pos++;

    /* Skip at most one optional separator space; a tab is never a valid
     * substitute for it (matching the identical "- \t-" restriction in
     * parse_block_list), and must still be rejected below even once a
     * genuine space has already been consumed. */
    if (!at_end(ctx) && cur(ctx) == ' ') ctx->pos++;
    if (!at_end(ctx) && cur(ctx) == '\t') {
      parse_err(ctx,
                "tab cannot follow an explicit key indicator '?' "
                "at position %zu",
                ctx->pos);
      return -1;
    }

    /* Determined by parse_block_map_node itself, once it knows whether the
     * key's own content starts on this line or a later one (an explicit
     * key is allowed to span to a fresh line per YAML 1.2 sec. 8.2.2, and
     * a merge-candidate peek taken any earlier than that would only ever
     * see the newline right after '?', never the real key). */
    bool is_merge_candidate = false;
    cyaml_node_t *key_node =
        parse_block_map_node(ctx, map_indent, true, &is_merge_candidate);
    if (!key_node) return -1;
    char *key_str = node_to_dict_key_string(ctx, key_node, "block dictionary");
    if (!key_str) return -1;

    /* The ':' value indicator, if any, is always on its own line at
     * exactly map_indent (YAML 1.2 sec. 8.2.2: l-block-map-explicit-value
     * starts with s-indent(n) ":", never inline after the key). current_col()
     * is being compared against an indentation level here, so this needs the
     * same line_indent_has_tab() rejection every other such comparison in
     * this file already carries (see e.g. parse_block_dictionary's own
     * next-sibling-entry lookup a little further down); current_col() counts
     * a tab as a single byte of width, the same as a space, so a tab-indented
     * ':' could otherwise line up with map_indent by byte offset alone and
     * be silently accepted. */
    skip_ws_comments(ctx);
    bool have_colon = false;
    if (!at_end(ctx) && !at_doc_marker(ctx)) {
      if (line_indent_has_tab(ctx)) {
        parse_err(ctx,
                  "tab cannot be used as block mapping indentation at "
                  "position %zu",
                  ctx->pos);
        _mem_free(ctx->mp, key_str);
        return -1;
      }
      have_colon = (current_col(ctx) == map_indent && cur(ctx) == ':');
    }
    if (have_colon) ctx->pos++;

    *key_out = key_str;
    *have_colon_out = have_colon;
    *is_explicit_out = true;
    *is_merge_candidate_out = is_merge_candidate;
    return 1;
  }

  /* Indicator characters that cannot start any kind of implicit key.  '-' is
   * only a list indicator when followed by whitespace or EOF; otherwise it
   * may legitimately start a plain scalar key (e.g. -key:).  '*' is not
   * excluded here: try_parse_scalar_dict_key handles an alias used directly
   * as a key.  '['/'{' are NOT excluded here either: a flow collection is a
   * valid non-scalar implicit key (YAML 1.2 sec. 8.2.2 permits ns-flow-node
   * in block-key context, not just a scalar; see try_parse_scalar_dict_key's
   * own '['/'{' branch, which parses exactly this); excluding them here
   * would only break every entry after the mapping's first (the first entry
   * is detected directly by parse_node_inner, which has no such filter),
   * making the construct's acceptance depend on entry position alone. */
  if (c == '-') {
    char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
    if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
      return 0;
  } else if (c == '|' || c == '>' || c == '#') {
    return 0;
  }

  /* An implicit key may itself carry an anchor and/or a tag (e.g.
   * "&anchor key: value"); try_parse_scalar_dict_key handles both,
   * restoring ctx->pos on its own if this doesn't turn out to be a key. */
  char *anchor_name = NULL;
  char *key_str = NULL;
  cyaml_node_t *anchor_value = NULL;
  if (!try_parse_scalar_dict_key(ctx, &anchor_name, &key_str,
                                 is_merge_candidate_out, &anchor_value))
    return ctx->error[0] ? -1 : 0;

  if (!register_key_anchor(ctx, anchor_name, key_str, anchor_value)) {
    _mem_free(ctx->mp, key_str);
    return -1;
  }

  *key_out = key_str;
  *have_colon_out = true;
  *is_explicit_out = false;
  return 1;
}

/*
 * Merge one source mapping's entries into target, skipping any key target
 * already has (explicit keys always win over anything merged in; by the
 * time this runs, target already holds every real explicit key, since
 * merge expansion is always a post-pass run only after the whole mapping
 * has otherwise been fully parsed). Each value is cyaml_clone'd before
 * insertion: cyaml_dictionary_set takes ownership of its child
 * unconditionally, and a value pulled from a merge source is still
 * logically owned by that source's own tree (or, for the exact same
 * anchor merged into two different target mappings, by multiple targets
 * at once); inserting the same pointer directly would double-own it.
 *
 * Returns false (with ctx->error set) on a non-mapping source or OOM.
 */
static bool merge_one_source_into(parse_ctx_t *ctx, cyaml_node_t *target,
                                  cyaml_node_t *source) {
  if (!source || source->type != CYAML_DICTIONARY) {
    parse_err(ctx, "merge key source is not a mapping at position %zu",
              ctx->pos);
    return false;
  }
  size_t src_count = chmap_elem_count(source->value.dictionary);
  cmap_iterator *it = chmap_begin_iter_safe(source->value.dictionary);
  if (!it && src_count > 0) {
    /* Non-empty source, but the iterator could not be built even after
     * retrying (see chmap_begin_iter_safe's own doc comment): a real OOM,
     * not "nothing to merge". Silently falling through to the empty-loop
     * "return true" below would report success while merging none of a
     * non-empty source's keys. */
    parse_err(ctx, "out of memory expanding merge key at position %zu",
              ctx->pos);
    return false;
  }
  size_t seen = 0;
  while (it) {
    const char *key = (const char *)it->key_pair->ptr;
    if (!cyaml_dictionary_get((cyaml)target, key)) {
      cyaml_node_t *child = _cyaml_read_child(it->val_pair->ptr);
      cyaml clone = cyaml_clone((cyaml)child);
      if (!clone) {
        parse_err(ctx, "out of memory expanding merge key at position %zu",
                  ctx->pos);
        ccol_iter_destroy(it);
        return false;
      }
      if (cyaml_dictionary_set((cyaml)target, key, clone) != ccol_success) {
        parse_err(ctx, "out of memory expanding merge key at position %zu",
                  ctx->pos);
        ccol_iter_destroy(it);
        return false;
      }
    }
    seen++;
    it = it->_next_fn(it);
  }
  if (seen != src_count) {
    /* chmap_begin_iter_safe() succeeded, but a later _next_fn() call
     * failed partway through (a real OOM, not "nothing more to iterate":
     * see chmap_begin_iter_safe's own doc comment for why these are
     * otherwise indistinguishable). Silently returning true here would
     * report a fully successful merge while some of the source's later
     * keys were never even considered, the exact same contract violation
     * the src_count>0-but-!it case above already guards against. */
    parse_err(ctx, "out of memory expanding merge key at position %zu",
              ctx->pos);
    return false;
  }
  return true;
}

/*
 * Expand a mapping's own "<<" merge-key entry, once the caller has already
 * confirmed a genuine plain, untagged merge-key trigger was seen while
 * parsing map. The merge source may be a single mapping or a sequence of
 * mappings (earlier sources win over later ones on conflict, per this
 * module's own confirmed precedence decision). A merged-in mapping's own
 * "<<" entry, if it had one, is already fully expanded and removed by the
 * time IT was parsed (a nested mapping is always completely finished,
 * including its own merge expansion, before any anchor referencing it is
 * ever registered; see anchors_store's own call sites), so transitive
 * expansion falls out for free with no special-casing needed here.
 *
 * Implemented entirely on top of the existing public dictionary API
 * (cyaml_dictionary_get/_set/_remove) against the now-fully-built map,
 * rather than touching chmap internals directly.
 *
 * Returns false (with ctx->error set) on a malformed merge source or OOM;
 * true otherwise. A no-op if "<<" isn't actually present (defensive; the
 * caller only invokes this when it already knows a candidate was seen).
 */
static bool expand_merge_key(parse_ctx_t *ctx, cyaml_node_t *map) {
  cmap_pair kp = {.ptr = (void *)"<<", .size = sizeof("<<")};
  cmap_pair *vp = NULL;
  if (chmap_get_elem_ref(map->value.dictionary, &kp, &vp) != ccol_success)
    return true;
  cyaml_node_t *merge_val = _cyaml_read_child(vp->ptr);
  /* Detach (but do not yet destroy) map's own "<<" slot before merging any
   * source into map: merge_one_source_into's own dedup check below is
   * "does target already have this key", checked via the public
   * cyaml_dictionary_get(target, key) API. If map's "<<" entry were still
   * attached during the merge loop, a source that legitimately contains
   * its own literal (quoted, non-triggering) "<<" key would be
   * misdetected as "already present in target" - that entry's dedup
   * check would spuriously match map's own not-yet-removed merge-trigger
   * slot, silently dropping the source's real entry, which is then
   * compounded by map's own slot being destroyed outright once merging
   * finishes. Removing the slot from map's chmap up front (without
   * destroying merge_val, which is still needed below) closes this. */
  if (chmap_delete_elem(map->value.dictionary, &kp) != ccol_success) {
    /* Unreachable in practice: chmap_get_elem_ref just confirmed the key
     * is present, and nothing between the two calls can invalidate it. */
    parse_err(ctx, "internal error removing merge key at position %zu",
              ctx->pos);
    return false;
  }

  bool ok = true;
  if (merge_val->type == CYAML_LIST) {
    size_t cnt = cvector_elem_count(merge_val->value.list);
    for (size_t i = 0; i < cnt && ok; i++) {
      cyaml_node_t *src =
          *(cyaml_node_t **)cvector_at(merge_val->value.list, i);
      ok = merge_one_source_into(ctx, map, src);
    }
  } else {
    ok = merge_one_source_into(ctx, map, merge_val);
  }
  __cyaml_destroy((cyaml)merge_val);
  return ok;
}

/*
 * Parse a block dictionary.  See the forward declaration's doc comment for
 * first_key's two modes (pre-read implicit key vs. NULL for an explicit
 * '?'-led first entry), first_key_colon_consumed's meaning, and
 * first_key_is_merge_candidate's meaning.
 */
static cyaml_node_t *parse_block_dictionary(parse_ctx_t *ctx, int map_indent,
                                            const char *first_key,
                                            bool first_key_colon_consumed,
                                            bool first_key_is_merge_candidate) {
  cyaml_node_t *map = (cyaml_node_t *)cyaml_create_dictionary_mp(ctx->mp);
  if (!map) return NULL;

  /* Tracks whether the entry that most recently wrote the literal key "<<"
   * into `map` was a genuine merge-key trigger. This must be re-derived
   * (overwritten, not OR-accumulated across every entry ever seen) each
   * time an entry's own key text is exactly "<<": a duplicate "<<" key
   * collapses to whichever entry wrote it LAST (cyaml_dictionary_set's own
   * "last value wins" duplicate-key semantics), so an earlier merge-
   * triggering "<<" entry must never keep this true once a later,
   * explicitly quoted/tagged "<<" entry overwrites it with an ordinary
   * literal value; OR-accumulating unconditionally let that earlier
   * entry's candidacy incorrectly force-expand (or spuriously reject) the
   * later, literal entry's own value instead. */
  bool double_lt_is_merge_candidate = false;
  const char *key = first_key;
  char *key_owned = NULL; /* owned key for the current iteration */
  bool have_colon;
  /* A pre-read first_key is always implicit style (parse_node's scalar,
   * flow-collection, anchor/tag/alias dispatch branches never reach here
   * for a '?'-led entry; see the forward declaration's own doc comment). */
  bool is_explicit = false;

  if (first_key != NULL) {
    if (strcmp(first_key, "<<") == 0)
      double_lt_is_merge_candidate = first_key_is_merge_candidate;
    if (first_key_colon_consumed) {
      have_colon = true;
    } else if (at_end(ctx) || cur(ctx) != ':') {
      parse_err(ctx, "expected ':' after dictionary key at position %zu",
                ctx->pos);
      goto fail;
    } else {
      ctx->pos++;
      have_colon = true;
    }
  } else {
    bool first_entry_is_merge_candidate = false;
    int r =
        parse_one_dict_entry_key(ctx, map_indent, &key_owned, &have_colon,
                                 &is_explicit, &first_entry_is_merge_candidate);
    if (r <= 0) {
      /* Unreachable in practice: parse_node only reaches here after
       * confirming a '?' at the current position, which parse_one_dict_
       * entry_key can never turn down. Handled defensively regardless. */
      if (r == 0)
        parse_err(ctx, "expected block dictionary key at position %zu",
                  ctx->pos);
      goto fail;
    }
    key = key_owned;
    /* This call site is only ever reached for the '?' explicit-key form
     * (see the forward declaration's own doc comment); its own merge
     * candidacy is only known once parse_one_dict_entry_key has actually
     * read the key, so it is folded in here rather than threaded in via
     * the first_key_is_merge_candidate parameter (which is only ever
     * meaningful for a pre-read implicit first_key). */
    if (strcmp(key, "<<") == 0)
      double_lt_is_merge_candidate = first_entry_is_merge_candidate;
  }

  while (1) {
    /* A tab directly after ':' (or after its own single optional
     * separator space) has no valid interpretation, mirroring the
     * identical "-"/"?" indicator checks elsewhere in this file
     * (verified against a reference parser for both the implicit
     * "key:\tvalue" and explicit ": \tvalue" forms; both are rejected,
     * not merely the explicit one). */
    if (have_colon && !at_end(ctx) && cur(ctx) == '\t') {
      parse_err(ctx, "tab cannot follow a ':' value indicator at position %zu",
                ctx->pos);
      goto fail;
    }
    /* Skip optional space after ':'. */
    if (have_colon && !at_end(ctx) && cur(ctx) == ' ') ctx->pos++;
    if (have_colon && !at_end(ctx) && cur(ctx) == '\t') {
      parse_err(ctx, "tab cannot follow a ':' value indicator at position %zu",
                ctx->pos);
      goto fail;
    }

    /* Parse value.  An explicit key with no ':' at all (have_colon false)
     * is null per YAML 1.2 sec. 8.2.2's "| e-node" alternative, with no
     * further parsing attempted. allow_inline_seq is true only for an
     * explicit entry's value (see parse_block_map_node's own doc comment
     * for why implicit and explicit values genuinely differ here). */
    cyaml_node_t *val =
        have_colon ? parse_block_map_node(ctx, map_indent, is_explicit, NULL)
                   : node_alloc(CYAML_NULL, ctx->mp);

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
    key = NULL;

    /* Look for the next entry at the same indent. */
    skip_ws_comments(ctx);
    if (at_end(ctx)) break;
    if (at_doc_marker(ctx)) break;
    if (line_indent_has_tab(ctx)) {
      parse_err(ctx,
                "tab cannot be used as block mapping indentation at "
                "position %zu",
                ctx->pos);
      goto fail;
    }
    if (current_col(ctx) != map_indent) break;

    bool this_key_is_merge_candidate = false;
    int r =
        parse_one_dict_entry_key(ctx, map_indent, &key_owned, &have_colon,
                                 &is_explicit, &this_key_is_merge_candidate);
    if (r < 0) goto fail;
    if (r == 0) break;
    key = key_owned;
    if (strcmp(key, "<<") == 0)
      double_lt_is_merge_candidate = this_key_is_merge_candidate;
  }

  if (double_lt_is_merge_candidate && !expand_merge_key(ctx, map)) goto fail;

  return map;

fail:
  _mem_free(ctx->mp, key_owned);
  __cyaml_destroy((cyaml)map);
  return NULL;
}

/*
 * Consume one directive line (ctx->pos is at the leading '%').  Every
 * directive name is accepted (YAML 1.2 sec. 6.8.2's ns-reserved-directive
 * permits an unrecognized name with any number of trailing parameters,
 * silently ignored); but "YAML" is given its own strict, spec-mandated
 * grammar (l-yaml-directive ::= "YAML" s-separate-in-line ns-yaml-version):
 * exactly one <major>.<minor> version token, nothing else on the line
 * besides optional trailing whitespace and a comment (which, like any
 * comment, must itself be separated from the version by whitespace; a
 * comment glued directly onto the version with no separating space is not
 * a valid comment at all, just malformed trailing content). Returns false
 * and sets ctx->error on a malformed "%YAML" line; true otherwise (either
 * a valid "%YAML" line, or any other directive name, accepted as-is).
 * *is_yaml_directive_out, if non-NULL, reports whether this line's
 * directive name was exactly "YAML"; needed by the caller to enforce
 * YAML 1.2 sec. 6.8.1's "it is an error to define more than one YAML
 * directive for the same document" rule, which this function itself has
 * no per-document state to track.
 */
static bool parse_directive_line(parse_ctx_t *ctx,
                                 bool *is_yaml_directive_out) {
  size_t start = ctx->pos;
  ctx->pos++; /* consume '%' */
  size_t name_start = ctx->pos;
  while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
         cur(ctx) != '\n' && cur(ctx) != '\r')
    ctx->pos++;
  size_t name_len = ctx->pos - name_start;
  bool is_yaml = name_len == 4 && memcmp(ctx->src + name_start, "YAML", 4) == 0;
  bool is_tag = name_len == 3 && memcmp(ctx->src + name_start, "TAG", 3) == 0;
  if (is_yaml_directive_out) *is_yaml_directive_out = is_yaml;

  if (is_tag) {
    /* A tab is never valid block-structural indentation or separation
     * (YAML 1.2 sec. 6.1); mirrors the identical, already-established
     * "%YAML"-directive checks just below (both the immediate glued-tab
     * check and the post-skip_inline_ws scan of the whole consumed span,
     * since skip_inline_ws() itself still treats a tab as ordinary
     * whitespace and would otherwise silently swallow one past the first
     * character). */
    if (!at_end(ctx) && cur(ctx) == '\t') {
      parse_err(ctx, "tab cannot follow '%%TAG' at position %zu", ctx->pos);
      return false;
    }
    if (at_end(ctx) || cur(ctx) != ' ') {
      parse_err(ctx, "malformed %%TAG directive at position %zu", start);
      return false;
    }
    size_t ws1_start = ctx->pos;
    skip_inline_ws(ctx);
    if (memchr(ctx->src + ws1_start, '\t', ctx->pos - ws1_start) != NULL) {
      parse_err(ctx, "tab cannot follow '%%TAG' at position %zu", ws1_start);
      return false;
    }

    size_t handle_start = ctx->pos;
    while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
           cur(ctx) != '\n' && cur(ctx) != '\r')
      ctx->pos++;
    size_t handle_len = ctx->pos - handle_start;
    if (handle_len == 0) {
      parse_err(ctx, "malformed %%TAG directive at position %zu", start);
      return false;
    }
    char *handle = _mem_alloc(ctx->mp, handle_len + 1);
    if (!handle) return false;
    memcpy(handle, ctx->src + handle_start, handle_len);
    handle[handle_len] = '\0';

    /* A %TAG handle is exactly "!" (primary), exactly "!!" (secondary), or
     * "!" + one-or-more word-chars (alnum/'-') + "!" (named); unlike a
     * shorthand tag token's own handle, there is no ambiguity to resolve
     * here since a directive's handle is a whole, whitespace-delimited
     * word on its own. */
    bool handle_valid = false;
    if (handle_len == 1 && handle[0] == '!') {
      handle_valid = true;
    } else if (handle_len == 2 && handle[0] == '!' && handle[1] == '!') {
      handle_valid = true;
    } else if (handle_len >= 3 && handle[0] == '!' &&
               handle[handle_len - 1] == '!') {
      handle_valid = true;
      for (size_t i = 1; i < handle_len - 1; i++) {
        char hc = handle[i];
        bool is_word = (hc >= 'a' && hc <= 'z') || (hc >= 'A' && hc <= 'Z') ||
                       (hc >= '0' && hc <= '9') || hc == '-';
        if (!is_word) {
          handle_valid = false;
          break;
        }
      }
    }
    if (!handle_valid) {
      parse_err(ctx, "malformed %%TAG handle '%s' at position %zu", handle,
                start);
      _mem_free(ctx->mp, handle);
      return false;
    }

    /* The handle-to-prefix separator gets the identical tab rejection as
     * the directive-name-to-handle separator above; a tab was previously
     * accepted here as an equally valid separator character, unlike every
     * other same-line-vs-content-boundary case in this file. */
    if (!at_end(ctx) && cur(ctx) == '\t') {
      parse_err(ctx, "tab cannot follow a %%TAG handle at position %zu",
                ctx->pos);
      _mem_free(ctx->mp, handle);
      return false;
    }
    if (at_end(ctx) || cur(ctx) != ' ') {
      parse_err(ctx, "malformed %%TAG directive at position %zu", start);
      _mem_free(ctx->mp, handle);
      return false;
    }
    size_t ws2_start = ctx->pos;
    skip_inline_ws(ctx);
    if (memchr(ctx->src + ws2_start, '\t', ctx->pos - ws2_start) != NULL) {
      parse_err(ctx, "tab cannot follow a %%TAG handle at position %zu",
                ws2_start);
      _mem_free(ctx->mp, handle);
      return false;
    }

    size_t prefix_start = ctx->pos;
    /* Unlike the "extra content after the prefix" scan further down, '#'
     * is NOT a stopping character here: YAML 1.2's ns-uri-char (the tag
     * prefix's own grammar, sec. 5.6/5.7) explicitly permits '#' as an
     * ordinary URI character, and a comment can only ever start after
     * genuine separating whitespace (s-b-comment), never glued directly
     * onto the prefix with nothing between them. Stopping at '#'
     * unconditionally here (as this loop previously did) silently
     * truncated any prefix containing one, e.g. a fragment identifier, and
     * treated the rest as an ordinary trailing comment with no separator
     * at all - mirrors parse_tag_token's/skip_tag_token's own suffix scan,
     * which already treats '#' as an ordinary tag character. */
    while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
           cur(ctx) != '\n' && cur(ctx) != '\r')
      ctx->pos++;
    size_t prefix_len = ctx->pos - prefix_start;
    if (prefix_len == 0) {
      parse_err(ctx, "malformed %%TAG prefix at position %zu", start);
      _mem_free(ctx->mp, handle);
      return false;
    }
    char *prefix = _mem_alloc(ctx->mp, prefix_len + 1);
    if (!prefix) {
      _mem_free(ctx->mp, handle);
      return false;
    }
    memcpy(prefix, ctx->src + prefix_start, prefix_len);
    prefix[prefix_len] = '\0';

    /* Nothing may follow the prefix except whitespace and, only once
     * separated from the prefix by that whitespace, a comment; mirrors
     * the identical "extra content after version" check the %YAML branch
     * above already enforces (YAML 1.2 sec. 6.8.2's l-tag-directive ends
     * in the same s-l-comments production every other directive line
     * does, not the free-form ns-reserved-directive parameter list an
     * unrecognized directive name gets). Without this, e.g. "%TAG !e!
     * tag:example.com,2000:app/ garbage-text\n" silently discarded
     * "garbage-text" instead of being rejected. */
    if (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
        cur(ctx) != '\n' && cur(ctx) != '\r' && cur(ctx) != '#') {
      parse_err(ctx, "extra content after %%TAG prefix at position %zu",
                ctx->pos);
      _mem_free(ctx->mp, handle);
      _mem_free(ctx->mp, prefix);
      return false;
    }
    size_t ws3_start = ctx->pos;
    skip_inline_ws(ctx);
    if (memchr(ctx->src + ws3_start, '\t', ctx->pos - ws3_start) != NULL) {
      parse_err(ctx, "tab cannot follow a %%TAG prefix at position %zu",
                ws3_start);
      _mem_free(ctx->mp, handle);
      _mem_free(ctx->mp, prefix);
      return false;
    }
    if (!at_end(ctx) && cur(ctx) != '\n' && cur(ctx) != '\r' &&
        cur(ctx) != '#') {
      parse_err(ctx, "extra content after %%TAG prefix at position %zu",
                ctx->pos);
      _mem_free(ctx->mp, handle);
      _mem_free(ctx->mp, prefix);
      return false;
    }

    /* YAML 1.2 sec. 6.8.2: "it is an error to define the same handle
     * more than once", mirroring the identical, already-established rule
     * for a duplicate %YAML directive (sec. 6.8.1); verified against
     * two independent reference parsers, both of which reject this even
     * when the second directive repeats the exact same prefix. */
    ccol_retval_t set_rv = tag_handles_set(ctx, handle, prefix);
    _mem_free(ctx->mp, handle);
    _mem_free(ctx->mp, prefix);
    if (set_rv == ccol_key_already_present) {
      parse_err(ctx, "duplicate %%TAG directive for handle at position %zu",
                start);
      return false;
    }
    if (set_rv != ccol_success) return false;

    skip_to_eol(ctx);
    return true;
  }

  if (is_yaml) {
    if (!at_end(ctx) && cur(ctx) == '\t') {
      /* A tab is never valid block-structural indentation or separation
       * (YAML 1.2 sec. 6.1), and a %YAML directive's own separator is no
       * exception (verified against a reference parser: "%YAML \t1.1" is
       * rejected, not merely the version-glued-directly-on "%YAML\t1.1"
       * case this same check also already covered). */
      parse_err(ctx, "tab cannot follow '%%YAML' at position %zu", ctx->pos);
      return false;
    }
    if (at_end(ctx) || cur(ctx) != ' ') {
      parse_err(ctx, "malformed %%YAML directive at position %zu", start);
      return false;
    }
    size_t ws_start = ctx->pos;
    skip_inline_ws(ctx);
    /* skip_inline_ws() itself still treats tabs as ordinary whitespace
     * (see its own doc comment for why it must, generally); checking
     * cur(ctx) only AFTER the call would miss a tab it already consumed
     * as part of this same run, so the whole consumed span is scanned
     * for one instead, mirroring flow_skip_ws's identical pattern. */
    if (memchr(ctx->src + ws_start, '\t', ctx->pos - ws_start) != NULL) {
      parse_err(ctx, "tab cannot follow '%%YAML' at position %zu", ws_start);
      return false;
    }
    size_t ver_start = ctx->pos;
    while (!at_end(ctx) && cur(ctx) >= '0' && cur(ctx) <= '9') ctx->pos++;
    bool has_major = ctx->pos > ver_start;
    bool ok = has_major && !at_end(ctx) && cur(ctx) == '.';
    if (ok) {
      ctx->pos++;
      size_t minor_start = ctx->pos;
      while (!at_end(ctx) && cur(ctx) >= '0' && cur(ctx) <= '9') ctx->pos++;
      ok = ctx->pos > minor_start;
    }
    if (!ok) {
      parse_err(ctx, "malformed %%YAML version at position %zu", start);
      return false;
    }
    /* Nothing may follow except whitespace and, only once separated from
     * the version by that whitespace, a comment. */
    if (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
        cur(ctx) != '\n' && cur(ctx) != '\r') {
      parse_err(ctx, "extra content after %%YAML version at position %zu",
                ctx->pos);
      return false;
    }
    size_t ws3_start = ctx->pos;
    skip_inline_ws(ctx);
    /* Same "scan the whole consumed span, not just the byte after the
     * call" reasoning as every other separator in this function (see the
     * ws_start check above): a tab here is just as much invalid
     * block-structural separation as one before the version number. */
    if (memchr(ctx->src + ws3_start, '\t', ctx->pos - ws3_start) != NULL) {
      parse_err(ctx, "tab cannot follow a %%YAML version at position %zu",
                ws3_start);
      return false;
    }
    if (!at_end(ctx) && cur(ctx) != '\n' && cur(ctx) != '\r' &&
        cur(ctx) != '#') {
      parse_err(ctx, "extra content after %%YAML version at position %zu",
                ctx->pos);
      return false;
    }
  }

  skip_to_eol(ctx);
  return true;
}

/* Consume a '---' document-start marker whose three dashes the caller has
 * already confirmed are present at ctx->pos (via at_doc_marker() plus a
 * literal "---" check). Advances past the marker and its own trailing
 * separator whitespace/newline, and reports via *same_line_content_out
 * whether real content follows on the same physical line.
 *
 * A tab is never valid block-structural indentation or separation (YAML 1.2
 * sec. 6.1); '---'s own separator is no exception, matching every other
 * block-structural indicator already rejecting one elsewhere in this file
 * (verified against a reference parser: "---\tfoo" is rejected). Returns
 * false (with ctx->error set) only for that rejection.
 */
static bool consume_doc_start_marker(parse_ctx_t *ctx,
                                     bool *same_line_content_out) {
  ctx->pos += 3;
  size_t ws_start = ctx->pos;
  skip_inline_ws(ctx);
  /* skip_inline_ws() itself still treats a tab as ordinary whitespace (see
   * its own doc comment); checking cur(ctx) only after the call would miss
   * one already consumed as part of this same run, so the whole consumed
   * span is scanned for one instead, mirroring parse_directive_line's
   * identical pattern for the '%YAML'/'%TAG' separators. */
  if (memchr(ctx->src + ws_start, '\t', ctx->pos - ws_start) != NULL) {
    parse_err(ctx,
              "tab cannot follow '---' document start marker at position "
              "%zu",
              ws_start);
    return false;
  }
  *same_line_content_out = !rest_of_line_is_blank(ctx);
  if (!at_end(ctx) && at_eol(ctx)) skip_newline(ctx);
  skip_ws_comments(ctx);
  return true;
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
 * *consumed_end_marker reports whether a trailing '...' was found and
 * consumed; YAML 1.2 sec. 6.9 (l-yaml-stream) permits the following
 * document to omit its own '---' only when it is directly preceded by one
 * or more '...' end markers, so the caller needs this to decide whether to
 * require '---' before the next document.
 *
 * Returns NULL on parse error; ctx->error is set in that case.  The anchor
 * table is always destroyed before returning.
 */
static cyaml_node_t *parse_one_document(parse_ctx_t *ctx,
                                        bool *consumed_end_marker) {
  /* Whether real root content directly follows an explicit '---' marker
   * on that SAME physical line (as opposed to '---' alone, with content
   * starting on a later line). YAML 1.2's grammar gives root content
   * reached this way only the "flow-in-block" alternative of s-l+block-
   * node; a plain/quoted scalar or a flow collection; never the
   * "block-in-block" alternative a block mapping/sequence needs, which
   * requires s-l-comments (i.e. only whitespace/a comment/a newline)
   * directly after '---' (or after any node properties decorating it).
   * Verified against two independent reference parsers, which both
   * reject e.g. "--- a: b" and "--- - a" while accepting "---\na: b" and
   * bare "a: b" with no marker at all. */
  bool doc_marker_same_line_content = false;

  /* Whether this document already consumed its own leading '---'. A
   * directive can only ever precede the '---' it configures (YAML 1.2 sec.
   * 6.8.1); once a document's own start marker has been consumed, a '%'
   * found afterward can never belong to THIS document; it can only be the
   * next document's own directive prefix, meaning this document is empty
   * and ends right here. Without tracking this, a bare '---' immediately
   * followed (on a later line) by a directive-carrying document had that
   * next document's entire "%directive\n---\ncontent" silently absorbed as
   * if it were this document's own trailing content, losing the empty
   * document this '---' actually introduced. */
  bool consumed_leading_dashes = false;

  /* Accept optional leading document-start marker.  Per YAML 1.2 the '---'
   * token is only a document-start indicator when its three dashes are
   * followed by whitespace, a comment, or EOF; '---X' where X is any other
   * character is a plain scalar, not a marker. */
  if (at_doc_marker(ctx) && memcmp(ctx->src + ctx->pos, "---", 3) == 0) {
    if (!consume_doc_start_marker(ctx, &doc_marker_same_line_content)) {
      anchors_destroy(ctx);
      tag_handles_destroy(ctx);
      return NULL;
    }
    consumed_leading_dashes = true;
  }

  /* Accept %YAML / %TAG directives silently, but a directive always
   * requires an explicit '---' document-start marker to follow (YAML 1.2
   * sec. 6.8.1: directives are associated with a specific document, never
   * with "no document" / an implicit empty one); reaching EOF or a plain
   * '...' end marker without one is a syntax error, not an empty document,
   * so this must be tracked and enforced explicitly rather than just
   * falling out of the loop. Gated on !consumed_leading_dashes: a document
   * that already consumed its own '---' can never also own a directive
   * (see consumed_leading_dashes's own doc comment above) so a '%' found
   * there must be left untouched for the NEXT parse_one_document call to
   * pick up as the following document's own prefix. */
  bool saw_directive = false;
  bool saw_yaml_directive = false;
  bool consumed_doc_start_after_directive = false;
  while (!consumed_leading_dashes && !at_end(ctx) && cur(ctx) == '%') {
    saw_directive = true;
    bool is_yaml_directive = false;
    if (!parse_directive_line(ctx, &is_yaml_directive)) {
      /* A %TAG directive may have already populated ctx->tag_handles
       * before this specific directive line failed (e.g. a later,
       * malformed %TAG after an earlier, valid one); this function's own
       * normal exit isn't reached from here, so that table (and, for
       * symmetry/future-proofing, the anchor table, though it can never
       * actually be populated this early) must be cleaned up on every
       * early-return path through this directive-parsing section, not
       * just the one at the bottom of the function. */
      anchors_destroy(ctx);
      tag_handles_destroy(ctx);
      return NULL;
    }
    if (is_yaml_directive) {
      /* YAML 1.2 sec. 6.8.1: "it is an error to define more than one
       * YAML directive for the same document, even if both occurrences
       * give the same version." */
      if (saw_yaml_directive) {
        parse_err(ctx,
                  "duplicate %%YAML directive for the same document "
                  "at position %zu",
                  ctx->pos);
        anchors_destroy(ctx);
        tag_handles_destroy(ctx);
        return NULL;
      }
      saw_yaml_directive = true;
    }
    skip_ws_comments(ctx);
    if (at_doc_marker(ctx) && memcmp(ctx->src + ctx->pos, "---", 3) == 0) {
      if (!consume_doc_start_marker(ctx, &doc_marker_same_line_content)) {
        anchors_destroy(ctx);
        tag_handles_destroy(ctx);
        return NULL;
      }
      consumed_doc_start_after_directive = true;
      consumed_leading_dashes = true;
      break;
    }
  }
  if (saw_directive && !consumed_doc_start_after_directive) {
    parse_err(ctx,
              "directive requires an explicit '---' document start at "
              "position %zu",
              ctx->pos);
    anchors_destroy(ctx);
    tag_handles_destroy(ctx);
    return NULL;
  }

  /* Empty document: at EOF, immediately at another document boundary, or
   * immediately at a fresh '%' directive line (column 0). The last case is
   * not covered by at_doc_marker(), which only recognizes literal '---'/
   * '...'; but '%' is a c-indicator character, so a plain scalar can
   * never legitimately start with it; a bare '%' reached here can only be
   * the next document's own directive prefix, never this document's
   * content. Without this check, two consecutive directive-only documents
   * (e.g. "%YAML 1.2\n---\n%YAML 1.2\n---\n") had the second document's
   * directive line silently mis-consumed as the first document's own
   * scalar content, since parse_node() has no directive-syntax awareness
   * of its own. */
  cyaml_node_t *root;
  if (at_end(ctx) || at_doc_marker(ctx) ||
      (current_col(ctx) == 0 && cur(ctx) == '%')) {
    root = node_alloc(CYAML_NULL, ctx->mp);
  } else if (doc_marker_same_line_content && at_bare_seq_indicator(ctx)) {
    /* A block sequence has no "flow-in-block" alternative either, so it
     * cannot start directly on the same line as '---' any more than a
     * block mapping can (see doc_marker_same_line_content's own doc
     * comment). */
    parse_err(ctx,
              "a block sequence cannot start on the same line as "
              "its document start marker at position %zu",
              ctx->pos);
    root = NULL;
  } else {
    /* -1 is the document root's own indentation sentinel (YAML 1.2 sec.
     * 8.1.1.1): the root node has no enclosing block context, so a block
     * scalar at column 0 (e.g. immediately after "--- >") is still more
     * indented than its "parent" and must not be rejected as empty.
     * allow_inline_map is false only when real content sits directly on
     * the same line as '---': an implicit "key: value" mapping there has
     * no valid grammar path either (same underlying rule as the bare
     * sequence check above), so it must be reported as an ordinary
     * chained-mapping-value error rather than silently accepted. */
    root = parse_node(ctx, -1, false, false, !doc_marker_same_line_content,
                      false, false, NULL);
  }

  /* Anchor table and %TAG handle table are both scoped to one document;
   * reset them before returning. */
  anchors_destroy(ctx);
  tag_handles_destroy(ctx);

  if (!root) return NULL;

  /* Consume optional trailing '...' document-end marker(s).  YAML 1.2 sec.
   * 6.9's l-yaml-stream grammar groups one-or-more consecutive '...'
   * markers (l-document-suffix+) as a single unit of separator material
   * between documents, not as one document boundary per marker; a second,
   * immediately-redundant '...' found here (nothing but whitespace/
   * comments between it and the one just consumed) belongs to THIS same
   * document's own closing, not to a fresh, separate (and, per the empty-
   * document check above, spuriously empty) document of its own. Without
   * looping here, "a: 1\n...\n...\n" left the second '...' for the next
   * parse_one_document call to find, which would then re-trigger the
   * empty-document check above and fabricate a phantom extra CYAML_NULL
   * document out of what is really just repeated separator noise; PyYAML,
   * cross-checked directly, parses this as a single document.
   *
   * at_doc_marker() enforces column 0 so an indented '...' sequence is not
   * silently swallowed as a document boundary (it would then be reported
   * as trailing content by the outer parse_common loop). */
  skip_ws_comments(ctx);
  *consumed_end_marker = false;
  while (at_doc_marker(ctx) && memcmp(ctx->src + ctx->pos, "...", 3) == 0) {
    ctx->pos += 3;
    size_t end_ws_start = ctx->pos;
    skip_inline_ws(ctx);
    /* A tab is never valid block-structural indentation or separation
     * (YAML 1.2 sec. 6.1); '...'s own separator is no exception, matching
     * '---'s identical rejection in consume_doc_start_marker() above
     * (verified against a reference parser: "...\t" is rejected). The
     * whole consumed span is scanned rather than just the byte after the
     * call, since skip_inline_ws() itself still treats a tab as ordinary
     * whitespace. */
    if (memchr(ctx->src + end_ws_start, '\t', ctx->pos - end_ws_start) !=
        NULL) {
      parse_err(ctx,
                "tab cannot follow '...' document end marker at position "
                "%zu",
                end_ws_start);
      __cyaml_destroy((cyaml)root);
      return NULL;
    }
    /* Nothing but whitespace and, optionally, a comment may share the
     * '...' marker's own line; real content there (e.g. "... invalid")
     * has no valid interpretation and must not be silently left for the
     * caller to misread as the start of a bare next document. */
    if (!at_end(ctx) && !at_eol(ctx) && cur(ctx) != '#') {
      parse_err(ctx,
                "unexpected content after '...' document end marker "
                "at position %zu",
                ctx->pos);
      __cyaml_destroy((cyaml)root);
      return NULL;
    }
    if (!at_end(ctx) && at_eol(ctx)) skip_newline(ctx);
    *consumed_end_marker = true;
    skip_ws_comments(ctx);
  }

  return root;
}

/*
 * Sets *err_str (if err_str is non-NULL) to a heap copy of msg, allocated
 * through mp. Centralizes the "if (err_str) *err_str = ccol_strdup(...)"
 * pattern repeated at every failure return in parse_common() below.
 *
 * If ccol_strdup() itself fails (a cascading second allocation failure under
 * the same OOM condition that caused, or accompanies, the original one being
 * reported), *err_str is left NULL: the only allocator available to retry is
 * the very one that just failed, so there is no allocation this function
 * could perform that is any more likely to succeed than the one that didn't
 * (a fault-injecting test allocator with an exhausted budget, in particular,
 * fails every call from that point on regardless of size). This NULL is
 * always safe for a caller to pass to free() or cyaml_serialize_free_mp()
 * (both treat a NULL argument as a no-op), so a genuine parse failure is
 * never mistaken for success: the return value (NULL) is what a caller must
 * check, with *err_str only ever providing optional extra detail.
 */
static void set_err_str(char **err_str, ccol_memmgmt_procs_t *mp,
                        const char *msg) {
  if (!err_str) return;
  *err_str = ccol_strdup(mp, msg);
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
 * On failure *err_str (if non-NULL) is set to a heap-allocated message,
 * allocated through mp, that the caller must free with
 * cyaml_serialize_free_mp() (passing the same mp).  Any successfully parsed
 * documents are destroyed before returning NULL.
 */
static cyaml parse_common(const char *src, size_t len, char **err_str,
                          ccol_memmgmt_procs_t *mp) {
  if (!src) {
    set_err_str(err_str, mp, "null input");
    return NULL;
  }
  parse_ctx_t ctx = {
      .src = src, .pos = 0, .len = len, .error = "", .mp = mp, .anchors = NULL};
  parse_node_budget_arm();

  /* Eagerly allocate line_start_pos()'s own position cache before any
   * parsing function runs; see that function's own doc comment for why
   * this allocation specifically is fatal-on-failure (unlike a later
   * growth of the same cache, which degrades gracefully instead). */
  ctx.line_starts = _mem_alloc(mp, 64 * sizeof(size_t));
  if (!ctx.line_starts) {
    set_err_str(err_str, mp, "out of memory");
    parse_node_budget_disarm();
    return NULL;
  }
  ctx.line_starts_cap = 64;
  ctx.line_starts[0] = 0;
  ctx.line_starts_len = 1;

  /* Skip optional BOM. The BOM itself is not part of the first line's
   * content for column-tracking purposes, so line_starts[0]/line_scan_pos
   * must be advanced to 3 along with ctx.pos - otherwise line_start_pos()
   * keeps reporting the first physical line as starting at byte 0, and
   * every column computed on that line (including at_doc_marker()'s
   * column-0 check) comes out 3 bytes too high. */
  if (len >= 3 && (unsigned char)src[0] == 0xEF &&
      (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) {
    ctx.pos = 3;
    ctx.line_starts[0] = 3;
    ctx.line_scan_pos = 3;
  }

  skip_ws_comments(&ctx);
  /* The stream's very own leading whitespace (before the first document's
   * own "---"/directives/content) is, by construction, pure indentation
   * from true column 0; see parse_node's own identical entry_at_line_start
   * check for why this is safe to apply here unconditionally. Without
   * this, a document beginning with a tab (e.g. "\ta: 1\n") had that tab
   * silently consumed here, before parse_one_document/parse_node ever got
   * a chance to reject it. */
  if (!at_end(&ctx) && line_indent_has_tab(&ctx)) {
    char buf[96];
    snprintf(buf, sizeof(buf),
             "tab cannot be used as block-structural indentation at "
             "position %zu",
             ctx.pos);
    set_err_str(err_str, mp, buf);
    parse_node_budget_disarm();
    _mem_free(mp, ctx.line_starts);
    return NULL;
  }

  /* Temporary array that accumulates parsed document roots. */
  cyaml_node_t **docs = NULL;
  size_t ndocs = 0;
  size_t dcap = 0;
  bool prev_consumed_end_marker = false;

  while (!at_end(&ctx)) {
    /* After the first document, the next document must start with '---',
     * UNLESS the previous document was itself terminated by a '...' end
     * marker, in which case a bare (no '---') document may follow
     * directly (YAML 1.2 sec. 6.9, l-yaml-stream: a document with no
     * preceding '...' must be explicit; one that does follow '...' may be
     * any of bare/directive/explicit). Bare content with no preceding
     * '...' is ambiguous trailing material. */
    if (ndocs > 0 && !prev_consumed_end_marker) {
      if (!at_doc_marker(&ctx) || memcmp(ctx.src + ctx.pos, "---", 3) != 0) {
        char buf[80];
        snprintf(buf, sizeof(buf), "trailing content at position %zu", ctx.pos);
        set_err_str(err_str, mp, buf);
        goto fail;
      }
    }

    bool consumed_end_marker = false;
    cyaml_node_t *root = parse_one_document(&ctx, &consumed_end_marker);
    if (!root) {
      if (ctx.error[0]) {
        set_err_str(err_str, mp, ctx.error);
      } else {
        /* Every genuine syntax rejection in this parser reports a
         * specific diagnostic via parse_err() before returning failure
         * (this file's own established, audited convention throughout);
         * the only way to reach here with ctx.error still empty is either
         * an allocation that failed somewhere deep in the call chain
         * without a diagnostic of its own to report (many low-level DOM
         * construction helpers, e.g. node_alloc()/cyaml_create_
         * dictionary_mp(), are shared with the public, non-parsing API
         * and have no parse_ctx_t to report through in the first place),
         * or this thread's CYAML_MAX_PARSE_NODES budget (see that macro's
         * own doc comment) running out at a construction site with the
         * identical no-parse_ctx_t limitation - node_alloc() itself has
         * no way to report through ctx.error either, for the same reason.
         * Distinguishing the two here, rather than reporting every case
         * as a generic allocation failure, matters because the node-
         * budget case is not actually an OOM at all: it fires with
         * memory freely available, and a caller seeing "out of memory"
         * for an ordinary, non-pathological large document would have no
         * way to tell the two apart. */
        char buf[128];
        if (_parse_node_budget_exhausted)
          snprintf(buf, sizeof(buf),
                   "document exceeded the %d node-allocation limit at "
                   "position %zu",
                   CYAML_MAX_PARSE_NODES, ctx.pos);
        else
          snprintf(buf, sizeof(buf),
                   "out of memory (unreported allocation failure) at "
                   "position %zu",
                   ctx.pos);
        set_err_str(err_str, mp, buf);
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
        set_err_str(err_str, mp, "out of memory");
        goto fail;
      }
      docs = nd;
      dcap = new_cap;
    }
    docs[ndocs++] = root;
    prev_consumed_end_marker = consumed_end_marker;

    skip_ws_comments(&ctx);
  }

  /* Empty input: return a null node, matching the behaviour of parsing an
   * empty plain scalar (YAML 1.2 core schema: empty value resolves to null). */
  if (ndocs == 0) {
    _mem_free(mp, docs);
    cyaml_node_t *empty = node_alloc(CYAML_NULL, mp);
    if (!empty) {
      /* *err_str must not be set to NULL until success is actually
       * confirmed; every other OOM path in this function sets a real
       * message before returning NULL, and a caller following the
       * documented "*err_str is NULL only on success" contract must not see
       * a NULL/NULL pair here either. */
      set_err_str(err_str, mp, "out of memory");
      parse_node_budget_disarm();
      _mem_free(mp, ctx.line_starts);
      return NULL;
    }
    if (err_str) *err_str = NULL;
    parse_node_budget_disarm();
    _mem_free(mp, ctx.line_starts);
    return (cyaml)empty;
  }

  /* Single document: return as-is (backwards compatible). */
  if (ndocs == 1) {
    cyaml_node_t *result = docs[0];
    _mem_free(mp, docs);
    if (err_str) *err_str = NULL;
    parse_node_budget_disarm();
    _mem_free(mp, ctx.line_starts);
    return (cyaml)result;
  }

  /* Multiple documents: wrap in a CYAML_LIST. */
  cyaml_node_t *list = (cyaml_node_t *)cyaml_create_list_mp(mp);
  if (!list) {
    set_err_str(err_str, mp, "out of memory");
    goto fail;
  }
  for (size_t i = 0; i < ndocs; i++) {
    cyaml_node_t *elem = docs[i];
    if (cvector_push_back(list->value.list, &elem) != ccol_success) {
      /* Destroy remaining docs not yet adopted by the list. */
      for (size_t j = i; j < ndocs; j++) __cyaml_destroy((cyaml)docs[j]);
      _mem_free(mp, docs);
      __cyaml_destroy((cyaml)list);
      set_err_str(err_str, mp, "out of memory");
      parse_node_budget_disarm();
      _mem_free(mp, ctx.line_starts);
      return NULL;
    }
  }
  _mem_free(mp, docs);
  if (err_str) *err_str = NULL;
  parse_node_budget_disarm();
  _mem_free(mp, ctx.line_starts);
  return (cyaml)list;

fail:
  parse_node_budget_disarm();
  for (size_t i = 0; i < ndocs; i++) __cyaml_destroy((cyaml)docs[i]);
  _mem_free(mp, docs);
  _mem_free(mp, ctx.line_starts);
  return NULL;
}

/* Parse a null-terminated YAML string.  mp may be NULL for the default
 * allocator.  On failure *err_str (if non-NULL) receives a heap-allocated
 * error message, allocated through mp, that the caller must free with
 * cyaml_serialize_free_mp() (passing the same mp), per cyaml.h's own
 * documented contract. */
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
 * Returns true when n's tag is one of the seven YAML 1.2 core-schema tag
 * URIs AND it actually matches n's own type (e.g. CYAML_TAG_INT on a
 * CYAML_INTEGER node, or CYAML_TAG_SEQ on a CYAML_LIST node). Such a tag is
 * never emitted on serialize: this serializer's own output discipline
 * (needs_quoting()'s exhaustive misread-as-another-type coverage,
 * yb_append_double()'s own float/integer disambiguation guard, and every
 * other type's fixed, unambiguous literal form) already guarantees the
 * value round-trips through its implicit type unaided, so re-emitting a
 * tag that only restates it would be pure redundancy.
 *
 * That round-trip guarantee does NOT hold for a core-schema tag that does
 * NOT match n's actual type: cyaml_node_set_tag() is explicitly documented
 * to allow attaching e.g. CYAML_TAG_STR to a CYAML_INTEGER node without
 * coercing its value ("now type-inconsistent tag string"), and a mismatched
 * CYAML_TAG_SEQ/_MAP can likewise be attached to any node via the same
 * function. Treating every one of the seven URIs as unconditionally
 * omittable regardless of whether it actually matches would silently drop
 * that tag on every serialize call, with no error and nothing in the
 * output to reconstruct it from; such a tag must be treated exactly like a
 * custom tag instead (still emitted, in verbatim !<...> form) so it is
 * never silently lost.
 */
static bool tag_matches_node_type(const cyaml_node_t *n) {
  if (!n->tag) return false;
  cyaml_node_type_t expected;
  if (strcmp(n->tag, CYAML_TAG_NULL) == 0)
    expected = CYAML_NULL;
  else if (strcmp(n->tag, CYAML_TAG_BOOL) == 0)
    expected = CYAML_BOOL;
  else if (strcmp(n->tag, CYAML_TAG_INT) == 0)
    expected = CYAML_INTEGER;
  else if (strcmp(n->tag, CYAML_TAG_FLOAT) == 0)
    expected = CYAML_FLOAT;
  else if (strcmp(n->tag, CYAML_TAG_STR) == 0)
    expected = CYAML_STRING;
  else if (strcmp(n->tag, CYAML_TAG_SEQ) == 0)
    expected = CYAML_LIST;
  else if (strcmp(n->tag, CYAML_TAG_MAP) == 0)
    expected = CYAML_DICTIONARY;
  else
    return false; /* not a core-schema tag at all: never redundant */
  return n->type == expected;
}

/*
 * Returns true if the string s can be emitted as a plain YAML scalar without
 * quoting: it does not look like null/bool/number, does not contain
 * indicator characters, and is not empty.
 */
static bool needs_quoting(const char *s) {
  if (!s || s[0] == '\0') return true;

  /* Strings that would be misinterpreted as other types. "<<" is included
   * unconditionally (both in key and value position, even though the
   * ambiguity it guards against, merge-key detection, is only ever
   * gated on key position): a single shared check here is safer than
   * special-casing only the key-emission call sites and risking a future
   * value-position use case being missed; the cost is one harmless,
   * unnecessary quote on the rare literal scalar value "<<". */
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
      strcmp(s, ".NAN") == 0 || strcmp(s, "<<") == 0)
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
   * are not caught by the digit or the "+.inf" checks above. strtod() sets
   * errno=ERANGE both on true overflow (result clamped to +-infinity) and on
   * a legitimate underflow to a valid subnormal or to 0.0 (a correctly-
   * computed, still-numeric result); only the former means the string does
   * NOT parse as a genuine float. A bare "errno != ERANGE" check here
   * disagreed with try_parse_float_scalar()'s own identical ERANGE-vs-
   * actual-overflow distinction (see that function's own doc comment), so a
   * CYAML_STRING value like "+.1e-400" was serialized unquoted and silently
   * reparsed as CYAML_FLOAT 0.0 instead of round-tripping as a string. */
  if (fc == '+' && s[1] == '.') {
    char *endp = NULL;
    errno = 0;
    double dval = strtod(s, &endp);
    if (endp != s && *endp == '\0' &&
        !(errno == ERANGE &&
          (dval == __builtin_inf() || dval == -__builtin_inf())))
      return true;
  }
  if (fc == '.') return true;

  while (*p) {
    char c = *p;
    if (c == '\n' || c == '\r') return true; /* multiline -> block scalar */
    /* A raw C0 control byte (other than tab) has no valid unescaped
     * representation in ANY scalar style (see is_disallowed_control_byte's
     * own doc comment); emitting one directly into a plain scalar would
     * produce output no conformant YAML 1.2 parser (this one now included,
     * since parse_plain_scalar's own scanner rejects it) can read back.
     * Routing it through yb_append_yaml_dquoted() instead is what actually
     * escapes it, via that function's own "\xXX" fallback for c < 0x20. */
    if (is_disallowed_control_byte(c)) return true;
    /* c-flow-indicator (YAML 1.2 sec. 7.4): all five of ',' '[' ']' '{' '}'
     * are plain-scalar terminators in flow context, so any string containing
     * one, anywhere, must be quoted to survive a flow round-trip. Applied
     * unconditionally here (not just when actually serializing in flow
     * style), matching this function's own established precedent of quoting
     * for the strictest context it may be used in; block-style output pays
     * only a harmless, unnecessary quote in exchange, since block-context
     * plain scalars tolerate all five characters unrestricted. */
    if (c == ',' || c == '[' || c == ']' || c == '{' || c == '}') return true;
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
        /* DEL (0x7F), like every C0 control byte, has no valid unescaped
         * representation (see is_disallowed_control_byte's own doc
         * comment); escape it the same way. */
        if (c < 0x20 || c == 0x7F) {
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
  /* Once a prior sibling/ancestor call has already set b->oom (whether via
   * the depth guard just below or a genuine allocation failure), every
   * further yb_append_* call is already a safe no-op; but without this
   * check, the traversal itself is not short-circuited, so a wide tree of
   * many independently-deep branches would still pay full recursion cost
   * (and, for a dictionary sibling, a real chmap_begin_iter_safe
   * allocation and, in canonical mode, a further _mem_calloc + qsort) for
   * every remaining sibling, each independently re-discovering the same
   * already-known failure. This defeats the whole point of
   * CYAML_MAX_SERIALIZE_DEPTH bounding cost against a pathological tree:
   * the guard would otherwise only bound one branch's own depth, not the
   * total work across every sibling branch. */
  if (b->oom) return;
  if (depth > CYAML_MAX_SERIALIZE_DEPTH) {
    /* See CYAML_MAX_SERIALIZE_DEPTH's own doc comment: a legitimately
     * acyclic tree can still be too deep for the stack to survive
     * recursing into it; reported through the same oom flag every other
     * serialization failure already uses. */
    b->oom = true;
    return;
  }
  if (!n) {
    yb_append_cstr(b, "~");
    return;
  }

  /* A custom (non-core-schema) tag always serializes in its fully-resolved
   * verbatim form; a %TAG shorthand it may originally have been written
   * with is parse-time-only state that doesn't persist on the tree, so
   * there is nothing to expand a shorthand back from here. A collection's
   * tag sits on its own line, indented to match the entries that follow
   * (mirroring how this parser's own '&'/'!' branches already accept a
   * node property with content starting on a later, more-indented line);
   * a scalar's tag is a same-line prefix immediately before its value. */
  if (n->tag && !tag_matches_node_type(n)) {
    if (n->type == CYAML_LIST || n->type == CYAML_DICTIONARY) {
      yb_indent(b, depth);
      yb_append_c(b, '!');
      yb_append_c(b, '<');
      yb_append_cstr(b, n->tag);
      yb_append_cstr(b, ">\n");
    } else {
      yb_append_c(b, '!');
      yb_append_c(b, '<');
      yb_append_cstr(b, n->tag);
      yb_append_cstr(b, "> ");
    }
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
      size_t seen = 0;
      cmap_iterator *it = chmap_begin_iter_safe(n->value.dictionary);
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
        seen++;
        it = it->_next_fn(it);
      }
      if (seen != cnt) {
        /* chmap_begin_iter_safe() (or a later _next_fn call) failed to
         * reach every entry: a real OOM, not "nothing more to iterate"
         * (see that helper's own doc comment). Silently falling through
         * would let cyaml_serialize() return a "successful" string
         * missing one or more of this dictionary's entries, violating its
         * documented "NULL on OOM" contract; flag the buffer as OOM
         * instead, mirroring how every other dictionary-walking call site
         * in this file (cyaml_clone, merge_one_source_into,
         * serialize_flow's own canonical branch) already treats this
         * exact situation. */
        b->oom = true;
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

/* A single (key, value) pair captured out of a dictionary's chmap for
 * canonical-mode sorting; see serialize_flow's own "canonical" parameter
 * and serialize_flow_canonical's doc comment below. key is borrowed:
 * it points into the dictionary's own persistent per-entry storage,
 * valid for as long as the dictionary itself is not mutated. */
typedef struct {
  const char *key;
  cyaml_node_t *val;
} _flow_dict_entry_t;

static int _flow_dict_entry_cmp(const void *a, const void *b) {
  const _flow_dict_entry_t *ea = (const _flow_dict_entry_t *)a;
  const _flow_dict_entry_t *eb = (const _flow_dict_entry_t *)b;
  return strcmp(ea->key, eb->key);
}

/*
 * Recursively emit compact single-line flow YAML for the subtree rooted at n.
 * Lists are enclosed in '[ ]', dictionaries in '{ }'.  Null is emitted as
 * '~'. String quoting follows the same needs_quoting() rules as block output so
 * the result is always a valid parseable YAML value.
 *
 * canonical controls dictionary entry order only (lists are already
 * order-significant and never reordered): false (the public
 * cyaml_serialize_flow() path) preserves the map's own chashmap iteration
 * order exactly, unchanged from this function's original behavior; true
 * (serialize_flow_canonical's own path, see its doc comment) instead emits
 * every dictionary's entries sorted lexicographically by key, at every
 * level of nesting (recursive calls forward the same canonical value
 * unchanged), so the result is a pure function of content, independent of
 * insertion order.
 *
 * depth mirrors serialize_block's own identical parameter and
 * CYAML_MAX_SERIALIZE_DEPTH guard (see that constant's own doc comment);
 * see the doc comment on that macro for why this is not redundant with
 * CYAML_MAX_PARSE_DEPTH.
 */
static void serialize_flow(ybuf_t *b, cyaml_node_t *n, bool canonical,
                           int depth) {
  /* See serialize_block's own identical check and doc comment: without
   * this, a failure already recorded anywhere in this call is silently
   * re-discovered by every remaining sibling branch instead of aborting
   * the whole traversal immediately. */
  if (b->oom) return;
  if (depth > CYAML_MAX_SERIALIZE_DEPTH) {
    b->oom = true;
    return;
  }
  if (!n) {
    yb_append_cstr(b, "~");
    return;
  }
  /* See serialize_block's own identical comment; flow context has no
   * indentation concerns, so a custom (or type-mismatched core-schema) tag
   * is always a simple same-line prefix regardless of scalar vs.
   * collection. */
  if (n->tag && !tag_matches_node_type(n)) {
    yb_append_c(b, '!');
    yb_append_c(b, '<');
    yb_append_cstr(b, n->tag);
    yb_append_cstr(b, "> ");
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
        serialize_flow(b, child, canonical, depth + 1);
      }
      yb_append_c(b, ']');
      break;
    }
    case CYAML_DICTIONARY: {
      yb_append_c(b, '{');
      size_t cnt = chmap_elem_count(n->value.dictionary);
      if (canonical && cnt > 1) {
        /* Collect into a plain array first rather than sorting in place
         * over the iterator: chashmap's own iteration order is exactly
         * what canonical mode exists to not depend on, so the entries
         * must be fully materialized before any ordering decision is
         * made. Sorted lexicographically by key via qsort, matching this
         * codebase's own precedent for a small ad hoc array sort
         * (clogger.c's rotated-file-name ordering uses the identical
         * qsort-plus-strcmp-comparator shape). */
        _flow_dict_entry_t *entries =
            _mem_calloc(b->m_procs, cnt, sizeof(*entries));
        if (!entries) {
          b->oom = true;
          break;
        }
        size_t idx = 0;
        cmap_iterator *it = chmap_begin_iter_safe(n->value.dictionary);
        while (it) {
          entries[idx].key = (const char *)it->key_pair->ptr;
          entries[idx].val = _cyaml_read_child(it->val_pair->ptr);
          idx++;
          it = it->_next_fn(it);
        }
        if (idx != cnt) {
          /* chashmap_begin_iter's own small allocation failed even after
           * chmap_begin_iter_safe's retries: a real, if rare, OOM. Treat
           * this the same as any other allocation failure encountered
           * while building this string, rather than silently emitting a
           * dictionary missing entries. */
          _mem_free(b->m_procs, entries);
          b->oom = true;
          break;
        }
        qsort(entries, cnt, sizeof(*entries), _flow_dict_entry_cmp);
        for (size_t i = 0; i < cnt; i++) {
          if (i > 0) yb_append_cstr(b, ", ");
          if (needs_quoting(entries[i].key))
            yb_append_yaml_dquoted(b, entries[i].key);
          else
            yb_append_cstr(b, entries[i].key);
          yb_append_cstr(b, ": ");
          serialize_flow(b, entries[i].val, canonical, depth + 1);
        }
        _mem_free(b->m_procs, entries);
      } else {
        /* Either the ordinary (non-canonical) public path, which
         * preserves chashmap's own iteration order unchanged, or a
         * canonical-mode dictionary with 0 or 1 entries, for which no
         * order-dependence exists to correct in the first place. */
        size_t idx = 0;
        cmap_iterator *it = chmap_begin_iter_safe(n->value.dictionary);
        while (it) {
          if (idx > 0) yb_append_cstr(b, ", ");
          const char *key = (const char *)it->key_pair->ptr;
          if (needs_quoting(key))
            yb_append_yaml_dquoted(b, key);
          else
            yb_append_cstr(b, key);
          yb_append_cstr(b, ": ");
          cyaml_node_t *child = _cyaml_read_child(it->val_pair->ptr);
          serialize_flow(b, child, canonical, depth + 1);
          idx++;
          it = it->_next_fn(it);
        }
        if (idx != cnt) {
          /* See the canonical branch's own identical check above: a real
           * OOM (chmap_begin_iter_safe, or a later _next_fn call, failing
           * to reach every entry), not "nothing more to iterate". Without
           * this, cyaml_serialize_flow() could return a "successful"
           * string silently missing one or more of this dictionary's
           * entries instead of honoring its documented "NULL on OOM"
           * contract. */
          b->oom = true;
        }
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
  serialize_flow(&b, n, false, 0);
  if (b.oom) {
    _mem_free(b.m_procs, b.buf);
    return NULL;
  }
  return b.buf;
}

/* Internal-only counterpart to cyaml_serialize_flow(), used exclusively by
 * node_to_dict_key_string() to canonicalize a non-scalar value used as a
 * dictionary key. Unlike the public function, every dictionary encountered
 * (at any nesting level within n) is emitted with its entries sorted
 * lexicographically by key rather than in chashmap's own insertion-order-
 * dependent iteration order, so two structurally-equal non-scalar keys
 * built via different insertion sequences always canonicalize to the
 * identical string and correctly collide as the same dictionary key. Never
 * exposed publicly: cyaml_serialize_flow()'s own documented, unsorted
 * output order is deliberately left completely unchanged for every other
 * caller. Returns NULL on OOM, matching cyaml_serialize_flow()'s own
 * contract. */
static char *serialize_flow_canonical(cyaml_node_t *n) {
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  ybuf_t b;
  yb_init(&b, mp);
  serialize_flow(&b, n, true, 0);
  if (b.oom) {
    _mem_free(b.m_procs, b.buf);
    return NULL;
  }
  return b.buf;
}

/* Release a string returned by cyaml_serialize or cyaml_serialize_flow using
 * the matching allocator (mp == NULL for the default allocator). Also the
 * documented way to release an *err_str produced by cyaml_parse()/_parse_n()
 * on failure; s may be NULL (see set_err_str()'s own doc comment for when
 * that happens), which _mem_free() already treats as a safe no-op. */
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
 * Strictly parse a "#N" path component's digit string (digits points just
 * past the '#'). Unlike a bare strtol() call, this rejects any leading
 * whitespace or an explicit sign before the first digit (strtol() itself
 * silently accepts both), matching this module's own documented path
 * grammar for a list index ("a component beginning with '#' followed by
 * digits") exactly: "#  3" and "#+3" are malformed, not index 3.
 *
 * Returns false (idx_out untouched) for an empty or malformed digit
 * string, or a value that does not fit in a non-negative long; true with
 * *idx_out set otherwise.
 */
static bool parse_list_index_component(const char *digits, long *idx_out) {
  if (digits[0] < '0' || digits[0] > '9') return false;
  char *endp;
  errno = 0;
  long idx = strtol(digits, &endp, 10);
  if (*endp != '\0' || idx < 0 || errno == ERANGE) return false;
  *idx_out = idx;
  return true;
}

/* Why navigate_y() failed to reach the end of a path (only meaningful when
 * it returns NULL). Lets a ccol_retval_t-returning caller (_cyaml_set_typed,
 * _cyaml_delete) distinguish a caller-side path-construction error
 * (ccol_invalid_args: wrong parent type, malformed "#N" index syntax, or an
 * empty path component) from a well-formed but absent path component
 * (ccol_key_not_found), at ANY position in the path, not just at the leaf.
 * CYAML_NAV_EMPTY_COMPONENT mirrors the leaf-position empty-component check
 * _cyaml_set_typed()/_cyaml_delete() already apply to their own leaf
 * component (a path syntax with nothing between two dots, or a leading
 * dot, has no valid interpretation - it is never treated as addressing a
 * literal empty-string dictionary key), so the same input is rejected the
 * same way regardless of whether the empty component happens to be an
 * intermediate one or the leaf. */
typedef enum {
  CYAML_NAV_NOT_FOUND = 0,
  CYAML_NAV_WRONG_TYPE,
  CYAML_NAV_MALFORMED_INDEX,
  CYAML_NAV_EMPTY_COMPONENT,
} cyaml_nav_fail_t;

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
 *
 * fail_reason_out, if non-NULL, is set (only on a NULL return) to why
 * navigation stopped short; unused by callers (_cyaml_get) that have no way
 * to surface a distinct error code anyway.
 */
static cyaml navigate_y(cyaml root, char *path_copy,
                        cyaml_nav_fail_t *fail_reason_out) {
  cyaml node = root;
  char *p = path_copy;
  cyaml_nav_fail_t fail_reason = CYAML_NAV_NOT_FOUND;

  while (node) {
    char *dot = path_find_unescaped_dot(p);
    if (dot) *dot = '\0';

    if (p[0] == '\0') {
      fail_reason = CYAML_NAV_EMPTY_COMPONENT;
      node = NULL;
      break;
    }

    path_unescape_component(p);

    cyaml_node_t *n = (cyaml_node_t *)node;

    if (n->type == CYAML_LIST && p[0] == '#') {
      long idx;
      if (!parse_list_index_component(p + 1, &idx)) {
        fail_reason = CYAML_NAV_MALFORMED_INDEX;
        node = NULL;
        break;
      }
      void *slot = cvector_at(n->value.list, (size_t)idx);
      node = slot ? *(cyaml *)slot : NULL;
      if (!node) fail_reason = CYAML_NAV_NOT_FOUND;
    } else if (n->type == CYAML_DICTIONARY) {
      cmap_pair kp = {.ptr = p, .size = strlen(p) + 1};
      cmap_pair *vp = NULL;
      if (chmap_get_elem_ref(n->value.dictionary, &kp, &vp) != ccol_success) {
        fail_reason = CYAML_NAV_NOT_FOUND;
        node = NULL;
      } else {
        node = _cyaml_read_child(vp->ptr);
      }
    } else {
      fail_reason = CYAML_NAV_WRONG_TYPE;
      node = NULL;
    }

    if (!dot) break;
    p = dot + 1;
  }
  if (!node && fail_reason_out) *fail_reason_out = fail_reason;
  return node;
}

/* Maps a navigate_y() failure reason onto the ccol_retval_t a
 * ccol_retval_t-returning caller (_cyaml_set_typed, _cyaml_delete) reports
 * for a failed parent-path resolution: a caller-side path-construction
 * error (wrong parent type, malformed "#N" syntax) is ccol_invalid_args;
 * a well-formed but absent component is ccol_key_not_found. */
static ccol_retval_t nav_fail_to_retval(cyaml_nav_fail_t reason) {
  switch (reason) {
    case CYAML_NAV_WRONG_TYPE:
    case CYAML_NAV_MALFORMED_INDEX:
    case CYAML_NAV_EMPTY_COMPONENT:
      return ccol_invalid_args;
    case CYAML_NAV_NOT_FOUND:
    default:
      return ccol_key_not_found;
  }
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
  cyaml result = navigate_y(root, copy, NULL);
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
 * typeof("hi") == char[N]; the raw pointer is normalised to const char **
 * so node_reinit_scalar always receives one consistent form.
 *
 * Existing scalar nodes at the leaf are mutated in-place (node_reinit_scalar).
 * New keys are allocated and inserted.  Intermediate containers are never
 * created automatically; the parent node must already exist.
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
    cyaml_nav_fail_t fail_reason = CYAML_NAV_NOT_FOUND;
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    parent = navigate_y(root, copy, &fail_reason);
    _mem_free(mp, copy);
    if (!leaf_copy) return ccol_not_enough_memory;
    if (!parent) {
      _mem_free(mp, leaf_copy);
      return nav_fail_to_retval(fail_reason);
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
      ret = node_reinit_scalar(existing, type, raw, raw_size, is_signed);
    } else {
      cyaml_node_t *new_node = NULL;
      ccol_retval_t make_r = node_make_scalar(type, raw, raw_size, is_signed,
                                              pn->m_procs, &new_node);
      if (make_r != ccol_success) {
        _mem_free(mp, leaf_copy);
        return make_r;
      }
      cmap_pair vp = {.ptr = &new_node, .size = sizeof(new_node)};
      ccol_retval_t r = chmap_insert_elem(pn->value.dictionary, &kp, &vp);
      /* chmap_insert_elem's own documented contract never returns
       * ccol_key_already_present (only ccol_success, ccol_invalid_args,
       * ccol_container_full, or ccol_not_enough_memory), and this call
       * site is additionally only reached once chmap_get_elem_ref just
       * above already confirmed the key is absent; so unlike
       * chmap_insert's own generic macro (which upserts and must treat an
       * already-present key as success), any non-success return here is a
       * genuine failure: new_node was never adopted by the map and must
       * be freed, not silently treated as if it had been inserted. */
      if (r != ccol_success) {
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
      long idx;
      if (!parse_list_index_component(leaf_comp + 1, &idx)) {
        ret = ccol_invalid_args;
      } else {
        void *slot = cvector_at(pn->value.list, (size_t)idx);
        if (!slot) {
          /* A syntactically valid index that simply doesn't exist is an
           * absent path component, exactly like a missing dictionary key
           * (the CYAML_DICTIONARY branch above, which creates the leaf
           * rather than erroring, has no equivalent case: there is no
           * sensible way to "create" an arbitrary out-of-range list slot
           * without knowing what to fill the gap before it with), not a
           * malformed-path error; mirrors _cyaml_delete's identical
           * reasoning and return value for the same situation. */
          ret = ccol_key_not_found;
        } else {
          cyaml_node_t *existing = *(cyaml_node_t **)slot;
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
 * Public implementation of the cyaml_delete(root, path) macro.
 *
 * Splits the path on the LAST unescaped dot to identify the parent node and
 * the leaf key.  When no dot is present, root is the parent.  The leaf key
 * is unescaped before use so '\.' and '\\' work identically to cyaml_get and
 * cyaml_set.
 *
 * Removes and deep-frees the addressed node.  Returns:
 *   ccol_success          - node removed and freed.
 *   ccol_invalid_args     - NULL root/path, empty path, a non-container
 *                           (wrong-type) node encountered before the leaf,
 *                           or malformed "#N" index syntax, at any path
 *                           component (leaf or intermediate).
 *   ccol_key_not_found    - a syntactically well-formed path component (a
 *                           dictionary key or a "#N" index) is absent, at
 *                           any position, including a syntactically valid
 *                           but out-of-range list index.
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
    cyaml_nav_fail_t fail_reason = CYAML_NAV_NOT_FOUND;
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    parent = navigate_y(root, copy, &fail_reason);
    _mem_free(mp, copy);
    if (!leaf_copy) return ccol_not_enough_memory;
    if (!parent) {
      _mem_free(mp, leaf_copy);
      return nav_fail_to_retval(fail_reason);
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
      long idx;
      if (!parse_list_index_component(leaf_comp + 1, &idx)) {
        ret = ccol_invalid_args;
      } else if ((size_t)idx >= cvector_elem_count(pn->value.list)) {
        /* A syntactically valid index that simply doesn't exist is an
         * absent path component, exactly like a missing dictionary key,
         * not a malformed-path error; see this function's own doc comment
         * and _cyaml_delete's public doc comment in cyaml.h. */
        ret = ccol_key_not_found;
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
