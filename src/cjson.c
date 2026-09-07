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
#include <locale.h>
#include <math.h>
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
 *  - type     : one of the seven cjson_node_type_t values
 *  - attached : true once this node has been successfully pushed/set into a
 *               CJSON_LIST or CJSON_DICTIONARY parent (see cjson_list_push() /
 *               cjson_dictionary_set() below).  A tree built through the
 *               public API only ever has each node reachable from exactly one
 *               parent slot; this flag is what lets cjson_list_push() /
 *               cjson_dictionary_set() refuse to accept a child that already
 *               has a parent, rather than silently creating a second owner of
 *               the same node.  Without this guard, two container slots
 *               referencing the same node both independently destroy it when
 *               their own parent is torn down: a real, reproducible double
 *               free (confirmed to corrupt the thread-local node-pool
 *               free-list into a self-referencing cycle under the default
 *               allocator, hanging __attribute__((destructor)) teardown, and
 *               to segfault directly under a custom allocator). Freshly
 *               allocated nodes (node_alloc(), including the pool fast path
 *               after its own memset) start with attached == false.
 *  - m_procs  : allocator used for this node and its owned strings
 *  - value    : a union sized to the largest member (8 bytes on 64-bit)
 *     boolean : bool        -> CJSON_BOOL
 *     integer : long long   -> CJSON_INTEGER
 *     number  : double      -> CJSON_FLOAT
 *     string  : char *      -> CJSON_STRING  (heap-allocated, owned)
 *     list   : cvec         -> CJSON_LIST  (cvec of cjson_node_t *)
 *     dictionary  : chmap   -> CJSON_DICTIONARY  (chmap char* --> cjson_node_t)
 * *)
 */
typedef struct cjson_node_t {
  cjson_node_type_t type;
  bool attached;
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
 * Dynamic string buffer used exclusively for JSON serialization output,
 * backed by common.h's ccol_growbuf_t (the same growable-byte-buffer type
 * cyaml.c's own ybuf_t uses); the sb_* names are kept as thin forwarding
 * wrappers so every existing call site in this file needs no change. All
 * sb_* operations are no-ops once oom is set, allowing callers to defer
 * error checking to the end of a serialization pass rather than testing
 * after every append.
 */
typedef ccol_growbuf_t sbuf_t;

static inline void sb_init(sbuf_t *sb, ccol_memmgmt_procs_t *mp) {
  ccol_growbuf_init(sb, mp);
}

static inline void sb_append(sbuf_t *sb, const char *data, size_t n) {
  ccol_growbuf_append(sb, data, n);
}

static inline void sb_append_c(sbuf_t *sb, char c) {
  ccol_growbuf_append_c(sb, c);
}

static inline void sb_append_cstr(sbuf_t *sb, const char *s) {
  ccol_growbuf_append_cstr(sb, s);
}

/* Initialise with a pre-sized capacity (at least 64 bytes). */
static inline void sb_init_hint(sbuf_t *sb, ccol_memmgmt_procs_t *mp,
                                size_t hint) {
  ccol_growbuf_init_hint(sb, mp, hint);
}

/* ========================================================================== */
/*                    LOCALE-INDEPENDENT NUMBER I/O                           */
/* ========================================================================== */

/*
 * JSON (RFC 8259 sec. 6) requires the ASCII '.' as the sole decimal
 * separator, unconditionally, regardless of the host process's locale.
 * strtod() and the printf-family "%g" conversion are both locale-sensitive
 * (the radix character follows the active LC_NUMERIC category; see
 * strtod(3) / printf(3)): a process that has called setlocale()/uselocale()
 * to a locale using ',' (or anything else) as its decimal point would
 * otherwise have parse_number() silently mis-parse "3.14" as 3 (strtod()
 * stops at the first unrecognised byte with no reported error) and
 * format_double() emit unparseable garbage instead of valid JSON.
 *
 * Every strtod() call and every "%g"-family snprintf() call in this file
 * goes through _cjson_strtod_c()/_cjson_snprintf_g_c() below, which force
 * the C locale for the duration of that one call via uselocale(); this is a
 * per-thread switch (unlike setlocale(), it does not affect the process-wide
 * locale any other thread observes) that is safe to nest/recurse, since each
 * call saves and restores exactly the locale that was active immediately
 * before it.
 *
 * The "C" locale_t object is created once, lazily, and kept for the
 * process's lifetime (never freed), the same established pattern this
 * codebase already uses for other rarely-mutated, process-wide singletons
 * (e.g. ctls.c's own lazily-generated self-signed root RSA key).
 */
static once_flag_t _cjson_locale_once = ONCE_INIT;
static locale_t _cjson_c_locale = (locale_t)0;

static void _cjson_locale_init(void) {
  _cjson_c_locale = newlocale(LC_ALL_MASK, "C", (locale_t)0);
}

/* Switches the calling thread to the C locale for a subsequent strtod()/
 * snprintf() call. *switched is set to false (no switch performed) when the
 * "C" locale_t object itself could not be constructed (e.g. a broken libc
 * locale installation); the caller then falls back to whatever locale is
 * already active rather than crashing on a NULL locale_t. */
static locale_t _cjson_enter_c_locale(bool *switched) {
  call_once(_cjson_locale_once, _cjson_locale_init);
  if (!_cjson_c_locale) {
    *switched = false;
    return (locale_t)0;
  }
  *switched = true;
  return uselocale(_cjson_c_locale);
}

static void _cjson_leave_c_locale(bool switched, locale_t prev) {
  if (switched) uselocale(prev);
}

/* Locale-independent strtod(): always interprets '.' as the decimal point,
 * regardless of the calling thread's ambient LC_NUMERIC. */
static double _cjson_strtod_c(const char *nptr) {
  bool switched;
  locale_t prev = _cjson_enter_c_locale(&switched);
  double v = strtod(nptr, NULL);
  _cjson_leave_c_locale(switched, prev);
  return v;
}

/* Locale-independent snprintf() for format_double()'s own two "%.15g" /
 * "%.17g" double conversions: always emits '.' as the decimal point,
 * regardless of the calling thread's ambient LC_NUMERIC. Takes a fixed
 * choice between the two literals format_double() needs (rather than an
 * arbitrary caller-supplied format string) so the snprintf() call below
 * always has a literal format argument, avoiding -Wformat-nonliteral by
 * construction instead of suppressing it. */
static void _cjson_snprintf_g_c(char *buf, size_t cap, bool wide_precision,
                                double val) {
  bool switched;
  locale_t prev = _cjson_enter_c_locale(&switched);
  if (wide_precision)
    snprintf(buf, cap, "%.17g", val);
  else
    snprintf(buf, cap, "%.15g", val);
  _cjson_leave_c_locale(switched, prev);
}

/* ========================================================================== */
/*                         INTERNAL HELPERS                                   */
/* ========================================================================== */

/*
 * chmap_entry's SSO storage is naturally aligned, so this memcpy is
 * defense-in-depth rather than a live alignment requirement.  Kept because
 * it costs nothing and matches the same pattern used by
 * cyaml/clrucache/cthreadcomm/chttpclient for their own chmap-backed
 * pointer storage.
 */
static inline cjson_node_t *_cjson_read_child(const void *src) {
  cjson_node_t *p;
  memcpy(&p, src, sizeof(p));
  return p;
}

/*
 * chashmap_begin_iter() returns NULL both when the map is genuinely empty
 * and when its own small internal iterator-struct allocation fails, with no
 * way for a caller passing NULL for err to tell the two apart. This wrapper
 * disambiguates by retrying the allocation itself a handful of times before
 * giving up, on the chance a transient allocator failure clears; it still
 * returns NULL if the map is genuinely empty or if the retries are
 * exhausted, but a caller can distinguish the latter case by checking
 * chmap_elem_count() again itself if it needs to.
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

/*
 * Explicit, heap-backed worklist used by node_clear_value() / node_clear() /
 * __cjson_destroy() below to tear down an entire subtree without recursing
 * once per nesting level. A tree reaching any of them need not have come
 * from cjson_parse() at all (which is separately bounded by
 * CJSON_MAX_PARSE_DEPTH, see the PARSER section further down): it can be
 * built to arbitrary depth directly via the public cjson_list_push() /
 * cjson_dictionary_set() API. Destruction has no "fail cleanly" contract to
 * fall back on (node_clear() and __cjson_destroy() are both void; a caller
 * cannot be handed back a still-owned, half-freed tree to try again), so
 * bounding recursion the way cjson_clone() / serialize_node() do (returning
 * an error past a depth cap) is not an option here: it would only trade an
 * immediate crash for a silent, permanent memory leak of everything past
 * the cap. An explicit worklist avoids both failure modes by keeping the
 * native call stack at O(1) depth regardless of how deep or wide the tree
 * actually is.
 */
typedef struct destroy_worklist {
  cjson_node_t **items;
  size_t cap;
  size_t len;
} destroy_worklist_t;

/* Push child onto wl, growing it as needed (plain realloc/free; the
 * worklist is transient scratch state with no ties to any node's own
 * m_procs). A NULL child is silently ignored, matching every other
 * "destroy this child pointer" call site in this file.
 *
 * On a worklist-growth allocation failure, child is torn down immediately
 * via an ordinary recursive __cjson_destroy() call instead of being
 * deferred. This can only reintroduce depth-proportional stack usage for
 * that one child's own subtree, and only when BOTH conditions hold at
 * once: the tree is deep enough to matter, AND the allocator is
 * simultaneously unable to grow a small scratch array. That compound
 * failure is accepted as a rare, graceful degradation back to the
 * pre-existing recursive behavior rather than engineered around further;
 * it is categorically narrower than the unconditional stack-overflow bug
 * this worklist exists to fix, which triggers on depth alone with no
 * memory pressure required at all. */
static void destroy_worklist_push(destroy_worklist_t *wl, cjson_node_t *child) {
  if (!child) return;
  if (wl->len == wl->cap) {
    size_t new_cap = wl->cap == 0 ? 32 : wl->cap * 2;
    cjson_node_t **grown = mem_realloc(wl->items, new_cap * sizeof(*wl->items));
    if (!grown) {
      __cjson_destroy((cjson)child);
      return;
    }
    wl->items = grown;
    wl->cap = new_cap;
  }
  wl->items[wl->len++] = child;
}

/* Destructor callback for chmap_destroy_with_dtor(), used by
 * node_clear_value()'s own CJSON_DICTIONARY case below: enqueues one
 * dictionary entry's child node onto the worklist threaded through
 * dtor_ctx instead of recursing into it directly, so a dictionary value is
 * drained by the same iterative worklist as everything else. See
 * chmap_destroy_with_dtor's own doc comment (chashmap.h) for why this is
 * the allocation-free way to reach every child during teardown, unlike
 * enumerating the dictionary via chashmap_begin_iter() first (which needs
 * its own small allocation that can itself fail under sustained OOM,
 * silently leaking every already-inserted child that allocation failure
 * prevents ever being reached). */
static void _cjson_enqueue_dict_child(cmap_pair *val_pair, void *dtor_ctx) {
  cjson_node_t *child = _cjson_read_child(val_pair->ptr);
  destroy_worklist_push((destroy_worklist_t *)dtor_ctx, child);
}

/* Push item onto the growable stack array (its current length and capacity
 * given by len and cap), growing it via plain realloc (transient scratch
 * state, with no ties to any node's own m_procs; same rationale as
 * destroy_worklist_push above). Returns false on allocation failure,
 * leaving stack, cap, and len all unchanged. */
static bool cycle_stack_push(cjson_node_t ***stack, size_t *cap, size_t *len,
                             cjson_node_t *item) {
  if (*len == *cap) {
    size_t new_cap = *cap == 0 ? 32 : *cap * 2;
    cjson_node_t **grown = mem_realloc(*stack, new_cap * sizeof(**stack));
    if (!grown) return false;
    *stack = grown;
    *cap = new_cap;
  }
  (*stack)[(*len)++] = item;
  return true;
}

/*
 * Returns true if needle is reachable from haystack's own subtree (haystack
 * itself included), i.e. haystack == needle or needle is a descendant of
 * haystack somewhere inside its list/dictionary contents.
 *
 * cjson_list_push()/cjson_dictionary_set() use this to reject a push/set
 * that would create a cycle: attaching child as a new descendant of target
 * is only safe when target is not ALREADY reachable by descending from
 * child itself. If it is, target becomes both a new ancestor of child (via
 * the edge this call is about to add) and an existing descendant of it (via
 * the pre-existing subtree); a genuine graph cycle. The "attached" field
 * (see this file's own top-of-file doc comment) rejects a child that
 * already has SOME parent, or that is literally the same node as the
 * target; on its own it does not reject a not-yet-attached node (e.g. the
 * root of a whole tree the caller is still holding, never itself pushed
 * anywhere) being pushed into one of its own descendants; this closes
 * that gap. Once such a cycle exists, __cjson_destroy()'s own
 * worklist-driven teardown (see destroy_worklist_t's own doc comment)
 * revisits and frees the same node twice, since freeing an ancestor does
 * not make it unreachable through the cycle's own back-edge; confirmed via
 * a direct, minimal reproduction (a 2-node cycle triggers glibc's own
 * double-free abort inside __cjson_destroy()) before this check was added.
 *
 * Iterative (an explicit heap-backed stack, not recursion), matching
 * destroy_worklist_t's own reasoning exactly: child's own subtree can be
 * arbitrarily deep, built directly through the public API to well past any
 * safe native stack depth, so a recursive search here would reintroduce the
 * identical stack-overflow risk destroy_worklist_t exists to avoid.
 *
 * *incomplete is set to true when the search could not be run to
 * completion (a stack-growth allocation failure, or a dictionary whose
 * iterator could not be built even after chmap_begin_iter_safe's own
 * retries) without having found needle yet; the caller must treat that
 * exactly like any other allocation failure (report ccol_not_enough_memory
 * and refuse the operation) rather than assume "not found yet" means
 * "safe", since a real cycle past the point the search gave up would then
 * go undetected.
 *
 * needle->attached short-circuits the whole search to O(1): a node can only
 * ever be reached by descending from some other node if it was itself
 * successfully inserted as somebody's child at least once (the only place
 * "attached" is ever set to true; it is never cleared afterwards, see the
 * struct's own doc comment). So needle->attached == false proves needle has
 * no parent at all yet, which makes it unreachable from ANY node's subtree,
 * haystack's included, without walking a single byte of haystack. This is
 * not a heuristic: it is what makes the overwhelmingly common "wrap an
 * already-built subtree in a brand-new, still-unattached outer container"
 * pattern (e.g. building a deeply nested structure bottom-up, one
 * cjson_list_push() per level) cost O(1) per call instead of O(size of the
 * subtree so far); without it, that ordinary, publicly-documented usage
 * degrades to O(n^2) for n such calls, confirmed by direct measurement
 * before this short-circuit was added (build_nested_list_via_api(20000) in
 * tests/cjson/tests.c going from well under 100ms to over a second).
 */
static bool node_reaches(cjson_node_t *haystack, const cjson_node_t *needle,
                         bool *incomplete) {
  *incomplete = false;
  if ((const cjson_node_t *)haystack == needle) return true;
  if (!needle->attached) return false;
  if (haystack->type != CJSON_LIST && haystack->type != CJSON_DICTIONARY)
    return false;

  cjson_node_t **stack = NULL;
  size_t cap = 0, len = 0;
  bool found = false;

  if (!cycle_stack_push(&stack, &cap, &len, haystack)) {
    *incomplete = true;
    return false;
  }

  while (len > 0 && !found) {
    cjson_node_t *n = stack[--len];
    if ((const cjson_node_t *)n == needle) {
      found = true;
      break;
    }
    if (n->type == CJSON_LIST) {
      size_t cnt = cvector_elem_count(n->value.list);
      for (size_t i = 0; i < cnt; i++) {
        cjson_node_t *c = *(cjson_node_t **)cvector_at(n->value.list, i);
        if (!cycle_stack_push(&stack, &cap, &len, c)) {
          *incomplete = true;
          goto done;
        }
      }
    } else if (n->type == CJSON_DICTIONARY) {
      size_t dict_cnt = chmap_elem_count(n->value.dictionary);
      cmap_iterator *it = chmap_begin_iter_safe(n->value.dictionary);
      if (!it && dict_cnt > 0) {
        /* Non-empty dictionary, but no iterator even after retrying: a real
         * OOM, not "nothing to walk" (see chmap_begin_iter_safe's own doc
         * comment). */
        *incomplete = true;
        goto done;
      }
      size_t seen = 0;
      while (it) {
        cjson_node_t *c = _cjson_read_child(it->val_pair->ptr);
        if (!cycle_stack_push(&stack, &cap, &len, c)) {
          ccol_iter_destroy(it);
          *incomplete = true;
          goto done;
        }
        seen++;
        it = it->_next_fn(it);
      }
      if (seen != dict_cnt) {
        *incomplete = true;
        goto done;
      }
    }
  }

done:
  mem_free(stack);
  return found;
}

/*
 * Thread-local free-list pool for cjson_node_t.
 *
 * The pool is exclusively for nodes whose m_procs == NULL (default allocator).
 * Custom-allocator nodes bypass the pool entirely; they are allocated and
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
  n->attached = false;
  n->m_procs = mp;
  return n;
}

/* Thread-local-storage key used solely to trigger _node_pool_drain on any
 * thread's exit; wraps pthread_key_t via common.h's thread_ls_* macros, per
 * this codebase's own standing rule that every pthread_* use in library code
 * must go through a common.h wrapper rather than calling the raw API
 * directly. */
static thread_ls_key_t _pool_pthread_key;

/* True once _do_pool_key_init() has successfully created _pool_pthread_key
 * and initialized _pool_key_rwlock below; false again once _pool_key_fini()
 * has deleted the key. node_free's cold path guards its thread_ls_set call
 * with this flag (under _pool_key_rwlock, see that lock's own doc comment)
 * to avoid calling into a deleted key when a background thread is still
 * active during a concurrent dlclose. It also doubles as the sole signal of
 * whether the one-time init actually succeeded: it is left false (its
 * static initial value) if either pthread call in _do_pool_key_init() fails
 * (e.g. genuine resource exhaustion, such as PTHREAD_KEYS_MAX already
 * reached process-wide), so a caller must never assume the rwlock/key are
 * valid to use without checking it first. */
static atomic_bool _pool_key_live = false;

/* Guards the load-and-act pair in node_free's cold path against
 * _pool_key_fini's own flag-flip-then-key-delete sequence (the DSO
 * destructor below, taken as the sole writer). A bare atomic_load-then-act
 * on _pool_key_live is not sufficient on its own: the destructor can run
 * between node_free's load and its own thread_ls_set call, deleting the key
 * out from under a pthread_setspecific() already in flight (UB per POSIX).
 * Holding this lock across the load-and-act keeps the two mutually
 * exclusive, so thread_ls_set either fully completes before the key is
 * deleted or never runs at all. Lazily initialized inside
 * _do_pool_key_init() (guarded by the once_flag_t below), per this
 * codebase's own standing rule against static/constant lock initializers. */
static rw_lock_t _pool_key_rwlock;

static void _node_pool_drain(void *);

/* Lazy key init: runs exactly once, the first time node_free() actually
 * needs the pool. Leaves _pool_key_live false (and _pool_key_rwlock
 * uninitialized) if either pthread call fails, so node_free's own read side
 * never touches either primitive without first confirming init succeeded. */
static once_flag_t _pool_key_once = ONCE_INIT;

static void _do_pool_key_init(void) {
  if (rw_lock_init(_pool_key_rwlock) != 0) return;
  if (thread_ls_key_create(_pool_pthread_key, _node_pool_drain) != 0) return;
  atomic_store(&_pool_key_live, true);
}

/* Called when the DSO is unloaded (dlclose or process exit).
 * - Drains the calling thread's own pool first so it is freed before the
 *   DSO's code mapping disappears.
 * - Flips the liveness flag and deletes the key under the write lock, so
 *   this can never interleave with a concurrent node_free()'s own
 *   read-locked use of the key (see _pool_key_rwlock's own doc comment):
 *   thread_ls_set either completes fully before the key goes away, or never
 *   runs at all (unlike a bare atomic flag, which only narrows the race
 *   window without actually closing it).
 * - Deletes the key to prevent PTHREAD_KEYS_MAX exhaustion on repeated
 *   dlopen/dlclose cycles.
 * A no-op if _do_pool_key_init() never ran (no cjson function that touches
 * the pool was ever called on any thread), since _pool_key_rwlock would not
 * be validly initialized in that case. */
__attribute__((destructor)) static void _pool_key_fini(void) {
  if (!atomic_load(&_pool_key_live)) return;
  _node_pool_drain(NULL);
  rw_lock_wrlock(_pool_key_rwlock);
  atomic_store(&_pool_key_live, false);
  thread_ls_key_delete(_pool_pthread_key);
  rw_lock_unlock(_pool_key_rwlock);
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
  /* Per this codebase's own rule for every pthread/once_flag_t primitive:
   * never rely on call-graph reasoning to skip the once-guard in a function
   * that directly touches _pool_key_rwlock; that exact reasoning is what
   * has broken silently before elsewhere in this codebase the moment a new
   * call path appeared. */
  call_once(_pool_key_once, _do_pool_key_init);
  if (_node_pool_sz == 0) {
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
      if (atomic_load(&_pool_key_live))
        thread_ls_set(_pool_pthread_key, (void *)1);
      rw_lock_unlock(_pool_key_rwlock);
    }
  }
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

#ifdef RUNNING_UNIT_TESTS
/* Exposes the calling thread's own node-pool free-list size for white-box
 * unit tests that verify the pool's cap-eviction behavior (_NODE_POOL_CAP)
 * and per-thread isolation. Not part of the public API. */
size_t cjson_debug_pool_size(void) { return (size_t)_node_pool_sz; }
#endif

/* Deep-free n's own value payload (string bytes, or a list/dictionary
 * container), pushing any direct children onto wl rather than recursing
 * into them. Leaves n->type and n itself untouched. */
static void node_clear_value(cjson_node_t *n, destroy_worklist_t *wl) {
  switch (n->type) {
    case CJSON_STRING:
      _mem_free(n->m_procs, n->value.string);
      n->value.string = NULL;
      break;
    case CJSON_LIST: {
      size_t cnt = cvector_elem_count(n->value.list);
      for (size_t i = 0; i < cnt; i++) {
        cjson_node_t *child = *(cjson_node_t **)cvector_at(n->value.list, i);
        destroy_worklist_push(wl, child);
      }
      __cvector_destroy(n->value.list);
      n->value.list = NULL;
      break;
    }
    case CJSON_DICTIONARY: {
      /* chmap_destroy_with_dtor (chashmap.h), not an enumerate-then-destroy
       * pass via chashmap_begin_iter: that helper allocates its own small
       * iterator struct, which can itself fail under sustained OOM and, with
       * no allocation left to reach the remaining entries, would silently
       * leak every child past that point. This walks the map's own internal
       * storage directly and cannot fail to reach a live entry. */
      chmap_destroy_with_dtor(n->value.dictionary, _cjson_enqueue_dict_child,
                              wl);
      n->value.dictionary = NULL;
      break;
    }
    default:
      break;
  }
}

/* Drain wl until empty, fully destroying (node struct included) every node
 * it contains; each drained node's own children (enqueued by
 * node_clear_value() above) keep the worklist fed until the whole subtree
 * referenced by wl's initial contents is gone. A plain loop, not
 * recursion: this is what keeps node_clear()/__cjson_destroy()'s own stack
 * usage independent of tree depth. */
static void destroy_worklist_drain(destroy_worklist_t *wl) {
  while (wl->len > 0) {
    cjson_node_t *n = wl->items[--wl->len];
    node_clear_value(n, wl);
    node_free(n);
  }
}

/* Deep-free the value resources of a node without freeing the node itself.
 * Used by node_reinit_scalar() to discard an existing node's old
 * list/dictionary value before overwriting it in place with a new scalar
 * (e.g. via cjson_set()); the old value may be an arbitrarily deep tree
 * (see destroy_worklist_t's own doc comment above), so every descendant
 * below n's direct children is torn down through the same iterative
 * worklist __cjson_destroy() uses, not through recursion. */
static void node_clear(cjson_node_t *n) {
  destroy_worklist_t wl = {NULL, 0, 0};
  node_clear_value(n, &wl);
  destroy_worklist_drain(&wl);
  mem_free(wl.items);
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

  /* Reject composite types (CJSON_LIST / CJSON_DICTIONARY): the post-clear
   * assignment switch covers only scalars, and reaching its default branch
   * after node_clear would leave the node in an inconsistent state.  This is
   * also the enforcement point for cjson_set()'s documented accepted-type
   * list: a C value whose type _cjson_type_of() (cjson.h) does not recognise
   * arrives here as _CJSON_TYPE_UNSUPPORTED, which likewise falls through to
   * the default branch and is rejected before anything is touched, rather
   * than being silently written as CJSON_NULL. */
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

  /* cjson_set()'s _Generic dispatch (_cjson_type_of() in cjson.h) maps ANY
   * void pointer-typed C expression to CJSON_NULL, not just the literal
   * NULL; without this check, a caller who accidentally passes a live,
   * non-NULL void pointer (rather than one of the documented bool, integer,
   * float, double, char pointer, const char pointer, or NULL types) would
   * silently have it discarded, with the leaf written as JSON null and no
   * diagnostic at all. Reject it instead, matching cjson_set()'s own
   * documented accepted-type list. Only checked when raw actually carries
   * a full pointer-sized payload (raw_size == sizeof(void*)), which is
   * exactly what the macro always supplies for this type; a direct
   * _cjson_set_typed() call passing
   * raw == NULL / raw_size == 0 for a genuine "no payload" null is left
   * untouched. */
  if (type == CJSON_NULL && raw && raw_size == sizeof(void *) &&
      *(void **)raw != NULL)
    return ccol_invalid_args;

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

  /* Validate bool raw_size before touching the node. Reached only via a
   * direct, macro-bypassing call to _cjson_set_typed(): the cjson_set()
   * macro always supplies sizeof(bool) for a bool-typed C expression, so a
   * mismatch here can only come from a caller of the public backend
   * function itself. Without this check, *(bool *)raw would read past a
   * narrower raw buffer (or read a garbage byte out of a wider one) and
   * store whatever bit pattern results into a _Bool object, which is a
   * trap representation unless the byte is exactly 0 or 1. */
  if (type == CJSON_BOOL && raw_size != sizeof(bool)) return ccol_invalid_args;

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

  /* === Mutation: all pre-validation passed, cannot fail from here === */
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

/* Free a node and all its descendants, iteratively (see destroy_worklist_t's
 * own doc comment for why: a tree reaching here need not have come from
 * cjson_parse() at all, and has no depth bound when built directly through
 * the public mutation API). Safe to call on NULL. Does NOT null the
 * caller's pointer; use the cjson_destroy() macro wrapper for that.
 *
 * n itself is cleared and freed directly, without ever passing through
 * destroy_worklist_push(): that function's own OOM fallback recursively
 * calls __cjson_destroy() on the item it failed to enqueue, and n is this
 * very call's own argument, so pushing it there would risk this function
 * calling itself on the same node under simultaneous OOM. Every genuine
 * descendant is unaffected by this and is still drained through the
 * worklist exactly as node_clear() itself uses it. */
void __cjson_destroy(cjson node) {
  if (!node) return;
  cjson_node_t *n = (cjson_node_t *)node;

  destroy_worklist_t wl = {NULL, 0, 0};
  node_clear_value(n, &wl);
  node_free(n);

  destroy_worklist_drain(&wl);
  mem_free(wl.items);
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
 * unconditionally: if the push fails because of OOM or a wrong node type,
 * child is destroyed before returning the error code so the caller never has
 * to track ownership across error paths.
 *
 * child must not already be attached to a list/dictionary parent (including
 * arr itself, or arr's own ancestor chain, i.e. child must not already
 * contain arr somewhere within its own subtree, checked via node_reaches()
 * below); a node reachable from two parent slots would be independently
 * destroyed by each parent's own teardown, corrupting the heap (a double
 * free, and, under the default thread-local node-pool allocator, a
 * self-referencing free-list cycle); a graph cycle (arr already reachable
 * from within child) corrupts the heap the identical way once torn down,
 * since freeing one node along the cycle does not make it unreachable
 * through the cycle's own back-edge. This case returns ccol_invalid_args and
 * leaves child completely untouched (still owned by whatever it was already
 * attached to, or still a free-standing tree of its own); it is one of the
 * two failure modes that do NOT destroy child (the other being OOM inside
 * the cycle check itself, reported as ccol_not_enough_memory), since child
 * is not this call's to destroy in either case.
 */
ccol_retval_t cjson_list_push(cjson arr, cjson child) {
  if (!child) return ccol_invalid_args;
  cjson_node_t *c = (cjson_node_t *)child;

  cjson_node_t *n = (arr && ((cjson_node_t *)arr)->type == CJSON_LIST)
                        ? (cjson_node_t *)arr
                        : NULL;
  if (!n) {
    /* arr is NULL or not a CJSON_LIST: an already-attached child must still
     * be left completely untouched here (see this function's own doc
     * comment above); destroying it would free memory still owned by
     * whatever it is currently attached to, corrupting that tree. Only a
     * fresh, unattached child is destroyed, since ownership transfers
     * unconditionally on every other failure path. */
    if (!c->attached) __cjson_destroy(child);
    return ccol_invalid_args;
  }

  if (c->attached || c == n) {
    /* Already owned elsewhere (or child is arr itself): reject without
     * touching it. See this function's own doc comment above. */
    return ccol_invalid_args;
  }
  bool cycle_incomplete;
  if (node_reaches(c, (const cjson_node_t *)n, &cycle_incomplete)) {
    /* arr is already reachable from within child's own subtree: attaching
     * child here would make arr both a new ancestor and an existing
     * descendant of child, a genuine cycle. Reject without touching either
     * node. See node_reaches()'s own doc comment above. */
    return ccol_invalid_args;
  }
  if (cycle_incomplete) {
    /* The cycle check itself could not be completed (OOM); refuse rather
     * than risk silently letting an undetected cycle through. */
    return ccol_not_enough_memory;
  }
  ccol_retval_t r = cvector_push_back(n->value.list, &c);
  /* Ownership of child transfers unconditionally; free it on failure so the
   * caller does not have to track ownership across error paths. */
  if (r != ccol_success) {
    __cjson_destroy(child);
    return r;
  }
  c->attached = true;
  return ccol_success;
}

/* Return the element at position index (borrowed; do not destroy it
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
 * destroyed after the new pointer is safely in place; the slot is never
 * left dangling.
 *
 * child must not already be attached to a list/dictionary parent (including
 * obj itself, or obj's own ancestor chain: attaching child would make obj
 * both a new ancestor and an existing descendant of it, a cycle), UNLESS it
 * is exactly the value already stored under key (see cjson_list_push's own
 * doc comment for why an already-attached child is otherwise rejected;
 * accepting it would give the same node two owners, each of which
 * independently destroys it, corrupting the heap; a cycle corrupts the heap
 * the same way, since freeing an ancestor does not make it unreachable
 * through the cycle's own back-edge). The already-stored-under-key case is
 * instead accepted as a harmless no-op, specifically so
 * `cjson_dictionary_set(obj, k, cjson_dictionary_get(obj, k))` (setting a
 * key to its own current value) succeeds rather than being treated as an
 * illegal re-parent.
 */
ccol_retval_t cjson_dictionary_set(cjson obj, const char *key, cjson child) {
  if (!child) return ccol_invalid_args;
  cjson_node_t *c = (cjson_node_t *)child;

  cjson_node_t *n = (obj && ((cjson_node_t *)obj)->type == CJSON_DICTIONARY)
                        ? (cjson_node_t *)obj
                        : NULL;
  if (!n || !key) {
    /* obj is NULL/wrong type, or key is NULL: this can never be the "set a
     * key to its own current value" no-op below (that requires a valid
     * obj/key to look the current value up), so an already-attached child
     * here is a genuine double-attach attempt and must be left completely
     * untouched, for the same reason cjson_list_push() leaves it untouched
     * on its own analogous early-reject paths; destroying it would free
     * memory still owned by whatever it is currently attached to,
     * corrupting that tree. Only a fresh, unattached child is destroyed,
     * since ownership transfers unconditionally on every other failure
     * path. */
    if (!c->attached) __cjson_destroy(child);
    return ccol_invalid_args;
  }

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};

  /* Snapshot the old child pointer before the insert overwrites the slot. */
  cmap_pair *old_vp = NULL;
  cjson_node_t *old_child = NULL;
  if (chmap_get_elem_ref(n->value.dictionary, &kp, &old_vp) == ccol_success)
    old_child = _cjson_read_child(old_vp->ptr);

  if (c == old_child) return ccol_success; /* Setting a key to its own value. */

  if (c->attached || c == n) {
    /* Already owned elsewhere (or child is obj itself): reject without
     * touching it, or the slot it is already stored in. */
    return ccol_invalid_args;
  }

  bool cycle_incomplete;
  if (node_reaches(c, (const cjson_node_t *)n, &cycle_incomplete)) {
    /* obj is already reachable from within child's own subtree: attaching
     * child here would make obj both a new ancestor and an existing
     * descendant of child, a genuine cycle. Reject without touching either
     * node. See node_reaches()'s own doc comment. */
    return ccol_invalid_args;
  }
  if (cycle_incomplete) {
    /* The cycle check itself could not be completed (OOM); refuse rather
     * than risk silently letting an undetected cycle through. */
    return ccol_not_enough_memory;
  }

  cmap_pair vp = {.ptr = &c, .size = sizeof(c)};
  ccol_retval_t r = chmap_insert_elem(n->value.dictionary, &kp, &vp);
  if (r == ccol_success || r == ccol_key_already_present) {
    /* Insert succeeded: it is now safe to release the displaced old child. */
    c->attached = true;
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

/*
 * Maximum recursion depth for clone_node() below. A tree handed to
 * cjson_clone() need not have come from cjson_parse() at all: it can be
 * built directly, to an arbitrary depth, via the public cjson_list_push() /
 * cjson_dictionary_set() API, so CJSON_MAX_PARSE_DEPTH (which only bounds
 * the parser's own recursion, defined later in this file) gives no
 * protection here. Without an independent cap, a legitimately acyclic but
 * deep tree recurses clone_node() without bound and crashes the process via
 * stack overflow instead of returning NULL as documented. Set to the same
 * value as CJSON_MAX_PARSE_DEPTH so a tree exactly at the parser's own
 * limit can still be cloned without spuriously failing; kept as its own
 * constant only because CJSON_MAX_PARSE_DEPTH is defined later in this
 * file, after clone_node().
 */
#define CJSON_CLONE_MAX_DEPTH 500

/* Produce an independent deep copy of the subtree rooted at src, depth
 * levels below the original cjson_clone() call (0 at the root). See
 * CJSON_CLONE_MAX_DEPTH's own doc comment for why this cap exists
 * independently of the parser's CJSON_MAX_PARSE_DEPTH. The clone uses the
 * same allocator (m_procs) as the source. On OOM (or on exceeding the depth
 * cap) any partially-built clone is destroyed before NULL is returned. */
static cjson clone_node(cjson_node_t *src, int depth) {
  if (depth > CJSON_CLONE_MAX_DEPTH) return NULL;
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
        cjson child_copy = clone_node(child, depth + 1);
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
      size_t src_count = chmap_elem_count(src->value.dictionary);
      cmap_iterator *it = chmap_begin_iter_safe(src->value.dictionary);
      if (!it && src_count > 0) {
        /* Non-empty source, but the iterator could not be built even after
         * retrying (see chmap_begin_iter_safe's own doc comment): a real
         * OOM, not "nothing to clone". Returning dst here would silently
         * produce a "successful" clone missing every key, violating this
         * function's own documented "NULL on allocation failure" contract.
         * __cjson_destroy(dst) below is safe regardless of how far this
         * loop got, since node_clear's own CJSON_DICTIONARY case routes
         * through chmap_destroy_with_dtor (chashmap.h), which never needs
         * to allocate to reach dst's already-inserted entries. */
        __cjson_destroy(dst);
        return NULL;
      }
      while (it) {
        const char *key = (const char *)it->key_pair->ptr;
        cjson_node_t *child = _cjson_read_child(it->val_pair->ptr);
        cjson child_copy = clone_node(child, depth + 1);
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

/* Produce an independent deep copy of the entire subtree rooted at node.
 * See CJSON_CLONE_MAX_DEPTH's own doc comment above: a tree nested more
 * than CJSON_CLONE_MAX_DEPTH levels deep is reported as an allocation
 * failure (NULL), the same as any other clone failure. */
cjson cjson_clone(cjson node) {
  if (!node) return NULL;
  return clone_node((cjson_node_t *)node, 0);
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
 * depth           : current parse_value() recursion depth (0 at the root),
 *                   bounded by CJSON_MAX_PARSE_DEPTH below.
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
  unsigned int depth;
} parse_ctx_t;

/*
 * Maximum recursive-descent nesting depth accepted by cjson_parse() and
 * friends. parse_value() recurses into parse_list()/parse_dictionary(),
 * which each recurse back into parse_value() for every element/value they
 * contain, once per nesting level of the input with no other bound; a
 * document containing many thousands of nested '[' or '{' characters (a
 * few tens of KB of text) exhausts the process stack and crashes rather
 * than returning a parse error, confirmed empirically well below any depth
 * that would raise practical suspicion. 500 comfortably covers any
 * realistic hand- or machine-generated JSON document while keeping
 * worst-case parse-time stack usage bounded to a small, fixed amount.
 */
#define CJSON_MAX_PARSE_DEPTH 500

/* Format a human-readable parse error into ctx->error.  Only the last call
 * survives; earlier messages are silently overwritten.
 *
 * Declared with __attribute__((format(printf, 2, 3))) rather than suppressing
 * -Wformat-nonliteral at the vsnprintf call site: every call in this file
 * passes a string literal, so the attribute lets the compiler check every
 * call site's format string against its arguments (a real class of bug, e.g.
 * "%d" on a size_t) instead of just silencing the warning. */
static void parse_err(parse_ctx_t *ctx, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void parse_err(parse_ctx_t *ctx, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
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

/* ------------------------------------------------------------------ null */
/* Consume the literal "null" token and return a CJSON_NULL node. */
static cjson_node_t *parse_null(parse_ctx_t *ctx) {
  if (ctx->pos + 4 > ctx->len || memcmp(ctx->src + ctx->pos, "null", 4) != 0) {
    parse_err(ctx, "expected 'null' at position %zu", ctx->pos);
    return NULL;
  }
  ctx->pos += 4;
  return node_alloc(CJSON_NULL, ctx->mp);
}

/* ------------------------------------------------------------------ bool */
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

/* ---------------------------------------------------------------- number */
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
    /* RFC 8259 sec. 6: a leading zero may not be followed by more digits. */
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

  /*
   * Build a null-terminated token for strtoll / strtod.  A 360-byte stack
   * buffer (covering the full decimal form of any IEEE 754 double, ~328
   * chars worst case, and any LLONG_MIN, 20 chars, without any allocation)
   * handles every realistic JSON number.  RFC 8259 sec. 6 places no length
   * limit on a number literal, so a syntactically valid but far longer
   * token (e.g. many redundant leading/trailing zeros on an otherwise
   * ordinary, finite value) is still accepted, via a heap allocation sized
   * exactly to the token, rather than rejected outright.
   */
  size_t tok_len = ctx->pos - start;
  char stack_tok[360];
  char *tok = stack_tok;
  char *heap_tok = NULL;
  if (tok_len >= sizeof(stack_tok)) {
    heap_tok = _mem_alloc(ctx->mp, tok_len + 1);
    if (!heap_tok) return NULL;
    tok = heap_tok;
  }
  memcpy(tok, ctx->src + start, tok_len);
  tok[tok_len] = '\0';

  cjson_node_t *result = NULL;
  if (is_float) {
    double dval = _cjson_strtod_c(tok);
    if (isinf(dval)) {
      parse_err(ctx, "number out of range at position %zu", start);
    } else {
      cjson_node_t *n = node_alloc(CJSON_FLOAT, ctx->mp);
      if (n) n->value.number = dval;
      result = n;
    }
  } else {
    /* Try integer first; fall back to double if out of range. */
    char *endp;
    errno = 0;
    long long ival = strtoll(tok, &endp, 10);
    if (*endp == '\0' && errno != ERANGE) {
      cjson_node_t *n = node_alloc(CJSON_INTEGER, ctx->mp);
      if (n) n->value.integer = ival;
      result = n;
    } else {
      double dval = _cjson_strtod_c(tok);
      if (isinf(dval)) {
        parse_err(ctx, "number out of range at position %zu", start);
      } else {
        cjson_node_t *n = node_alloc(CJSON_FLOAT, ctx->mp);
        if (n) n->value.number = dval;
        result = n;
      }
    }
  }
  _mem_free(ctx->mp, heap_tok);
  return result;
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
    /* Fast path: no escapes, no control chars; single alloc+memcpy. */
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
            /* High surrogate; expect \uXXXX low surrogate. Remember where
             * we are right after this high surrogate's own 4 hex digits,
             * so that if the following \uXXXX turns out not to actually be
             * a low surrogate we can rewind here instead of discarding it:
             * without the rewind, a lookahead \u escape that merely FAILS
             * to be a valid low surrogate (e.g. it is itself a second high
             * surrogate, or an ordinary BMP character) would be silently
             * swallowed into this single replacement character rather than
             * being reprocessed as its own independent character on the
             * next iteration of the enclosing loop. */
            size_t after_high = ctx->pos;
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
                ctx->pos = after_high; /* not a low surrogate: rewind */
                cp = 0xFFFD;           /* invalid low surrogate */
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

/* ----------------------------------------------------------------- list */
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
    elem->attached = true;
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

/* ------------------------------------------------------------ dictionary */
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
    val->attached = true;
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

/* --------------------------------------------------------- value dispatch */
/* Skip whitespace then branch on the first character to call the appropriate
 * sub-parser.  Handles all seven JSON value types. */
static cjson_node_t *parse_value_dispatch(parse_ctx_t *ctx) {
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

/*
 * Thin recursion-depth-tracking wrapper around parse_value_dispatch(): every
 * recursive descent into a nested value (a list element, a dictionary
 * value, or, transitively, anything nested inside those) goes through
 * this exact function, since parse_list()/parse_dictionary() call
 * parse_value(), never parse_value_dispatch() directly.  This is what makes
 * CJSON_MAX_PARSE_DEPTH a genuine bound on native call-stack usage rather
 * than a bound on only one kind of nesting.
 */
static cjson_node_t *parse_value(parse_ctx_t *ctx) {
  if (ctx->depth >= CJSON_MAX_PARSE_DEPTH) {
    parse_err(ctx, "maximum nesting depth (%u) exceeded at position %zu",
              CJSON_MAX_PARSE_DEPTH, ctx->pos);
    return NULL;
  }
  ctx->depth++;
  cjson_node_t *r = parse_value_dispatch(ctx);
  ctx->depth--;
  return r;
}

/* ---------------------------------------------------- public parse entry */
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
  parse_ctx_t ctx = {
      .src = src, .pos = 0, .len = len, .error = "", .mp = mp, .depth = 0};
  cjson_node_t *root = parse_value(&ctx);
  if (!root) {
    if (err_str) {
      /* Every genuine syntax rejection in this parser already reports a
       * specific diagnostic via parse_err() before returning failure; the
       * only way to reach here with ctx.error still empty is an allocation
       * failure that had nowhere of its own to report through (node_alloc(),
       * a raw _mem_alloc(), or a cvector/chmap insert failing). Report that
       * honestly rather than the misleading, alarming-sounding "unknown
       * parse error", which would suggest a malformed document rather than
       * memory pressure. */
      const char *msg;
      char oom_buf[80];
      if (ctx.error[0]) {
        msg = ctx.error;
      } else {
        snprintf(oom_buf, sizeof(oom_buf),
                 "out of memory (unreported allocation failure) at position "
                 "%zu",
                 ctx.pos);
        msg = oom_buf;
      }
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
  _cjson_snprintf_g_c(buf, cap, false, val);
  if (_cjson_strtod_c(buf) != val) {
    _cjson_snprintf_g_c(buf, cap, true, val);
  }
  /* Guarantee the output is recognisable as a floating-point literal so that
   * parsing it back yields CJSON_FLOAT, not CJSON_INTEGER.  %.Ng strips the
   * decimal point for whole-number values (e.g. 1.0 -> "1"), which would
   * parse back as CJSON_INTEGER.  Appending ".0" fixes this; the longest
   * affected case is +/-1e14 (15 digits + ".0\0" = 18 bytes, well within the
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
 * Maximum recursion depth for serialize_node() below. Mirrors
 * CJSON_CLONE_MAX_DEPTH for the identical reason: a tree handed to
 * cjson_serialize()/cjson_serialize_pretty() need not have come from
 * cjson_parse() at all, it can be built directly, to an arbitrary depth,
 * via the public cjson_list_push()/cjson_dictionary_set() API, so
 * CJSON_MAX_PARSE_DEPTH (which only bounds the parser's own recursion)
 * gives no protection here. Set to the same value (500) so a tree exactly
 * at the parser's own limit still serializes without spuriously failing.
 */
#define CJSON_MAX_SERIALIZE_DEPTH 500

/*
 * Recursively emit JSON text for the subtree rooted at n into sb.
 * indent: spaces per indentation level (0 = compact, no whitespace added).
 * depth:  current nesting depth; the top-level caller passes 0.
 * A NULL node pointer is emitted as the literal "null".
 */
static void serialize_node(sbuf_t *sb, cjson_node_t *n, unsigned int indent,
                           unsigned int depth) {
  /* Once a prior sibling/ancestor call has already set sb->oom (whether via
   * the depth guard just below or a genuine allocation failure), every
   * further sb_append_* call is already a safe no-op; but without this
   * check, the traversal itself is not short-circuited, so a wide tree of
   * many independently-deep branches would still pay full recursion cost
   * for every remaining sibling, each independently re-discovering the same
   * already-known failure. This defeats the whole point of
   * CJSON_MAX_SERIALIZE_DEPTH bounding cost against a pathological tree:
   * the guard would otherwise only bound one branch's own depth, not the
   * total work across every sibling branch. */
  if (sb->oom) return;
  if (depth > CJSON_MAX_SERIALIZE_DEPTH) {
    /* See CJSON_MAX_SERIALIZE_DEPTH's own doc comment: a legitimately
     * acyclic tree can still be too deep for the stack to survive
     * recursing into it; reported through the same oom flag every other
     * serialization failure already uses. */
    sb->oom = true;
    return;
  }
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
      size_t cnt = chmap_elem_count(n->value.dictionary);
      size_t idx = 0;
      cmap_iterator *it = chmap_begin_iter_safe(n->value.dictionary);
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
      if (idx != cnt) {
        /* chmap_begin_iter_safe() failed to reach every entry after
         * retrying: a real OOM, not "nothing more to iterate" (see that
         * helper's own doc comment). Silently falling through would let
         * cjson_serialize()/cjson_serialize_pretty() return a "successful"
         * string missing one or more of this dictionary's entries,
         * violating their documented "NULL on OOM" contract. */
        sb->oom = true;
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
 * Strictly parse the decimal digits following '#' in a "#N" list-index path
 * component (i.e. digits points one past the '#').  Returns true and sets
 * *out on success; returns false (leaving *out unspecified) for a
 * non-numeric, negative, or out-of-range index, or a bare '#' (empty
 * digits).
 *
 * A bare strtol() call tolerates leading whitespace and an explicit '+'
 * sign before the digits (both are part of its own documented grammar),
 * which would otherwise silently accept a component like "#  5" or "#+5"
 * as a well-formed index; inconsistent with this path syntax's own
 * documented "malformed ('#N' index, non-numeric...)" rejection contract.
 * Requiring the very first byte to already be an ASCII digit rules out
 * whitespace, '+', '-', and the bare-'#' case up front, so the strtol()
 * call below can never itself skip or interpret anything before the digits
 * it consumes.
 */
static bool parse_list_index_component(const char *digits, size_t *out) {
  if (digits[0] < '0' || digits[0] > '9') return false;
  char *endp;
  errno = 0;
  long idx = strtol(digits, &endp, 10);
  if (*endp != '\0' || idx < 0 || errno == ERANGE) return false;
  *out = (size_t)idx;
  return true;
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
 *
 * *empty_component (when non-NULL) is set to true when ANY component of the
 * path is empty (consecutive dots, e.g. "a..b", or a trailing dot), no
 * matter where in the path it occurs relative to an earlier component that
 * already failed to resolve; it is left false for every other reason
 * navigate() returns NULL (a key/index genuinely absent, a malformed "#N"
 * index, or a type mismatch), and is never set at all on success. This lets
 * callers that split ccol_invalid_args (a syntax error) from
 * ccol_key_not_found (a syntactically valid but absent component) classify
 * an empty component the same way regardless of position: the whole path is
 * still scanned for a later empty component even after an earlier,
 * well-formed component has already failed to resolve (cur == NULL), so a
 * genuine syntax error is never masked by an unrelated "not found" outcome
 * reported for an earlier, unrelated component. Only the actual dictionary/
 * list lookups are skipped once cur is NULL, since there is nothing left to
 * look a further component up in.
 */
static cjson navigate(cjson root, char *path_copy, bool *empty_component) {
  if (empty_component) *empty_component = false;
  cjson cur = root;
  char *p = path_copy;

  while (1) {
    char *dot = path_find_unescaped_dot(p);
    if (dot) *dot = '\0';

    /* Empty component: consecutive dots ("a..b") or trailing dot ("a.").
     * Checked unconditionally; see this function's own doc comment above
     * for why this must not be skipped once cur is already NULL. */
    if (p[0] == '\0') {
      if (empty_component) *empty_component = true;
      cur = NULL;
      break;
    }

    path_unescape_component(p);

    if (cur) {
      cjson_node_t *n = (cjson_node_t *)cur;

      if (n->type == CJSON_LIST && p[0] == '#') {
        size_t idx;
        if (!parse_list_index_component(p + 1, &idx)) {
          cur = NULL;
        } else {
          void *slot = cvector_at(n->value.list, idx);
          cur = slot ? *(cjson *)slot : NULL;
        }
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
  cjson result = navigate(root, copy, NULL);
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
 * Intermediate containers are never created automatically; the parent must
 * already exist.
 *
 * Returns:
 *   ccol_success           - leaf created or updated.
 *   ccol_invalid_args      - NULL root/path, an empty path or path component
 *                            (leading/trailing/consecutive dots), a parent
 *                            type incompatible with the leaf component (a
 *                            non-'#N' leaf on a list parent, or any leaf on
 *                            a scalar parent), a malformed '#N' index
 *                            (non-numeric, negative, or bare '#'), or val's
 *                            C type is not one of cjson_set()'s accepted
 *                            types.
 *   ccol_key_not_found     - the parent path, or a syntactically valid but
 *                            out-of-range list index, is absent. A list,
 *                            unlike a dictionary, has no way to auto-extend
 *                            to fit an arbitrary index, so this remains an
 *                            error rather than creating the leaf.
 *   ccol_not_enough_memory - allocation failure.
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

  if (!last_dot) {
    /* No dot at all: copy already holds an unmutated, byte-identical copy of
     * path, so it can be reused directly as leaf_copy instead of paying for
     * a second, redundant strdup of the same string. */
    leaf_copy = copy;
  } else {
    *last_dot = '\0';
    if (copy[0] == '\0') {
      /* Leading dot: parent path is empty, which is a syntax error. */
      _mem_free(mp, copy);
      return ccol_invalid_args;
    }
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    if (!leaf_copy) {
      _mem_free(mp, copy);
      return ccol_not_enough_memory;
    }
  }

  /* Validate and unescape the leaf component BEFORE ever navigating to the
   * parent path: an empty leaf (a trailing dot on the overall path, e.g.
   * "a.") is always a syntax error and must be reported as ccol_invalid_args
   * unconditionally, even when the parent path itself also fails to resolve
   * (e.g. "missing." where "missing" does not exist); a genuinely absent
   * parent must never mask a leaf syntax error as ccol_key_not_found. */
  path_unescape_component(leaf_copy);
  if (leaf_copy[0] == '\0') {
    if (leaf_copy != copy) _mem_free(mp, leaf_copy);
    _mem_free(mp, copy);
    return ccol_invalid_args;
  }
  const char *leaf_comp = leaf_copy;

  cjson parent;
  if (!last_dot) {
    parent = root;
  } else {
    bool empty_component = false;
    parent = navigate(root, copy, &empty_component);
    _mem_free(mp, copy);
    if (!parent) {
      _mem_free(mp, leaf_copy);
      /* An empty intermediate component (e.g. "a..b") is a syntax error,
       * classified the same way an empty leaf component already is above,
       * not conflated with an ordinary absent-but-well-formed component. */
      return empty_component ? ccol_invalid_args : ccol_key_not_found;
    }
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
        new_node->attached = true;
        ret = ccol_success;
      }
    }
  } else if (pn->type == CJSON_LIST) {
    if (leaf_comp[0] != '#') {
      ret = ccol_invalid_args;
    } else {
      size_t idx;
      if (!parse_list_index_component(leaf_comp + 1, &idx)) {
        ret = ccol_invalid_args;
      } else {
        void *slot = cvector_at(pn->value.list, idx);
        if (!slot) {
          /* Syntactically valid index, but the element itself is absent: a
           * "not found" condition, matching _cjson_delete()'s own
           * identical classification of the same situation (see its own
           * list branch below), not a malformed-path syntax error. A list,
           * unlike a dictionary, has no way to auto-extend to fit an
           * arbitrary index, so this remains a real error; it is just
           * classified the same way regardless of which of these two
           * path-mutation functions reports it. */
          ret = ccol_key_not_found;
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
 *   ccol_invalid_args     - NULL root/path, an empty path or path component
 *                           (leading/trailing/consecutive dots), a non-'#N'
 *                           leaf component on a list parent, or a
 *                           malformed '#N' index (non-numeric, negative, or
 *                           bare '#').
 *   ccol_key_not_found    - the parent path, or a syntactically valid leaf
 *                           key / index, is absent.
 *   ccol_not_enough_memory - strdup failed.
 */
ccol_retval_t _cjson_delete(cjson root, const char *path) {
  if (!root || !path || path[0] == '\0') return ccol_invalid_args;

  ccol_memmgmt_procs_t *mp = ((cjson_node_t *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return ccol_not_enough_memory;

  char *last_dot = path_find_last_unescaped_dot(copy);
  char *leaf_copy;

  if (!last_dot) {
    /* No dot at all: copy already holds an unmutated, byte-identical copy of
     * path, so it can be reused directly as leaf_copy instead of paying for
     * a second, redundant strdup of the same string. */
    leaf_copy = copy;
  } else {
    *last_dot = '\0';
    if (copy[0] == '\0') {
      /* Leading dot: parent path is empty, which is a syntax error. */
      _mem_free(mp, copy);
      return ccol_invalid_args;
    }
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    if (!leaf_copy) {
      _mem_free(mp, copy);
      return ccol_not_enough_memory;
    }
  }

  /* Validate and unescape the leaf component BEFORE ever navigating to the
   * parent path: an empty leaf (a trailing dot on the overall path, e.g.
   * "a.") is always a syntax error and must be reported as ccol_invalid_args
   * unconditionally, even when the parent path itself also fails to resolve
   * (e.g. "missing." where "missing" does not exist); a genuinely absent
   * parent must never mask a leaf syntax error as ccol_key_not_found. */
  path_unescape_component(leaf_copy);
  if (leaf_copy[0] == '\0') {
    if (leaf_copy != copy) _mem_free(mp, leaf_copy);
    _mem_free(mp, copy);
    return ccol_invalid_args;
  }
  const char *leaf_comp = leaf_copy;

  cjson parent;
  if (!last_dot) {
    parent = root;
  } else {
    bool empty_component = false;
    parent = navigate(root, copy, &empty_component);
    _mem_free(mp, copy);
    if (!parent) {
      _mem_free(mp, leaf_copy);
      /* See the identical comment in _cjson_set_typed(): an empty
       * intermediate component is a syntax error, not an ordinary
       * absent-but-well-formed component. */
      return empty_component ? ccol_invalid_args : ccol_key_not_found;
    }
  }

  cjson_node_t *pn = (cjson_node_t *)parent;
  ccol_retval_t ret;

  if (pn->type == CJSON_DICTIONARY) {
    ret = cjson_dictionary_remove(parent, leaf_comp);
  } else if (pn->type == CJSON_LIST) {
    if (leaf_comp[0] != '#') {
      ret = ccol_invalid_args;
    } else {
      size_t idx;
      if (!parse_list_index_component(leaf_comp + 1, &idx)) {
        ret = ccol_invalid_args;
      } else if (idx >= cvector_elem_count(pn->value.list)) {
        /* Syntactically valid index, but the element itself is absent: a
         * "not found" condition per this function's own documented
         * contract ("ccol_key_not_found if any path component is
         * absent"), not a syntax error. Checked here rather than left to
         * cjson_list_remove()'s own ccol_invalid_args-for-out-of-bounds
         * return, which is correct for that function's own direct-call
         * contract but would otherwise leak that narrower contract into
         * this function's differently-documented one. */
        ret = ccol_key_not_found;
      } else {
        ret = cjson_list_remove(parent, idx);
      }
    }
  } else {
    ret = ccol_invalid_args;
  }

  _mem_free(mp, leaf_copy);
  return ret;
}
