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
#include <float.h>
#include <internal/cascii.h>
#include <internal/cgrowbuf.h>
#include <internal/chashinsert.h>
#include <internal/cnumlocale.h>
#include <internal/cprocsintern.h>
#include <internal/cstrutil.h>
#include <internal/ctlsmodel.h>
#include <internal/cutf8.h>
#include <internal/test_helpers/cyaml.h>
#include <stdarg.h>
#include <stdatomic.h>

/* ========================================================================== */
/*                         INTERNAL DOM NODE                                  */
/* ========================================================================== */

/*
 * The full definition of the DOM node. This struct is also the public handle
 * type (cyaml == cyaml_node_t *), so unlike cjson's tagged_node_t it isn't
 * fully opaque, but the caller still must never read or write these fields
 * directly and needs to go through the public API.
 *
 * Layout:
 *  type     : one of the seven cyaml_node_type_t values.
 *  m_procs  : the allocator for this node and for the strings it owns. NULL
 *             means the default allocator. Any other value is an interned
 *             copy from ccol_procs_intern(), never the pointer the caller
 *             gave, so the caller's struct can go out of scope while the
 *             tree is in use. Every public entry point that takes a caller's
 *             procs interns it before any node exists.
 *  tag      : an owned, fully resolved YAML tag string (for example
 *             "tag:yaml.org,2002:str", a custom verbatim tag, or a resolved
 *             shorthand tag), or NULL when this node has no tag. Only
 *             __cyaml_destroy frees it, never node_clear, and the doc
 *             comment of node_clear explains why the two operations must
 *             stay separate.
 *  value    : a union of the scalar and the composite payloads.
 *    list : cvec of cyaml_node_t *   (CYAML_LIST)
 *    dictionary  : chmap char*->cyaml_node_t* (CYAML_DICTIONARY)
 */
/* attached: true once the node is stored in a CYAML_LIST or CYAML_DICTIONARY
 * parent. In a tree built through the public API exactly one parent slot can
 * reach each node, and this flag lets cyaml_list_push() and
 * cyaml_dictionary_set() refuse a child that already has a parent. Without
 * it they would quietly give the node a second owner, and when the two
 * parents are torn down each slot destroys the node on its own, which is a
 * double free. The realistic way to get there is a borrowed reference from
 * cyaml_list_get() or cyaml_dictionary_get(), since those two are how the API
 * names a node that already has a parent.
 *
 * A new node starts with attached == false because node_alloc() zeroes the
 * struct on both of its paths (the calloc path, and the memset in the pool
 * fast path). The flag is never cleared once set, because a node only leaves
 * a parent when it is destroyed. Aliases are safe here, since an alias
 * resolves to a deep clone and not to a second reference to the anchored
 * node.
 *
 * The flag sits in the same position as the matching flag of cjson_node_t.
 * On LP64 it uses padding that already exists after `type`, so the struct
 * stays at 32 bytes and every other offset is unchanged. ILP32 has no such
 * padding, so there the node grows from 20 to 24 bytes and the later fields
 * move. No code outside this file can see that, because cyaml_node_t is
 * opaque and no public macro computes a size or an offset from it.
 *
 * anchored: true once a parse has registered this node in its anchor table
 * (anchors_store()). The table points at the node itself, which stays in the
 * tree, and an alias copies it when the parser reads the alias. So a parse
 * that drops a subtree before it ends (the old value of a repeated key, the
 * value of a merge key, a key node that turns into key text) keeps every
 * anchored node of that subtree alive until the anchor table goes away; see
 * parse_discard(). The flag means nothing outside a parse, and it sits in the
 * padding after `attached` on both LP64 and ILP32, so it costs no space. */
typedef struct cyaml_node_t {
  cyaml_node_type_t type;
  bool attached;
  bool anchored;
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

/* The tag of the YAML merge-key type. A mapping key "<<" that carries this
 * tag, as the "!!merge" shorthand or in verbatim form, is a merge key just
 * like an untagged plain "<<". */
#define _CYAML_TAG_MERGE "tag:yaml.org,2002:merge"
/* The resolved form of the non-specific tag, a bare "!". No written tag
 * resolves to this string, since parse_tag_token() refuses one that does and
 * cyaml_node_set_tag() refuses it too, so it marks the bare "!" alone. A
 * scalar under it is a string and a collection keeps its kind, and no node
 * ever carries it (see finalize_scalar_node and finalize_collection_node). */
#define _CYAML_TAG_NON_SPECIFIC "!"

/* ========================================================================== */
/*                         SERIALIZATION BUFFER                               */
/* ========================================================================== */

/* A dynamic string buffer for YAML serialization, backed by the
 * ccol_growbuf_t of cgrowbuf.h (the same growable byte buffer that cjson.c's
 * own sbuf_t uses). The yb_* names are thin forwarding wrappers so this file
 * has one short local name per buffer operation. Once oom is set, every
 * yb_* operation is a safe no-op. */
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

/* Every read of a buffer's backing store goes through one of the three
 * helpers below, and each of them decides on its own whether it can read the
 * buffer's content at all. A failed append latches the buffer's OOM flag and
 * leaves the backing store in a state that no caller may index, so making
 * that decision locally keeps these call sites correct on their own, without
 * depending on how the buffer is implemented. */

/* Drops trailing spaces and tabs, down to `floor` but never past it. */
static inline void yb_trim_trailing_inline_ws(ybuf_t *b, size_t floor) {
  if (b->oom) return;
  while (b->len > floor &&
         (b->buf[b->len - 1] == ' ' || b->buf[b->len - 1] == '\t'))
    b->buf[--b->len] = '\0';
}

/* Appends every byte of `src` to `dst`. A src with no readable content adds
 * nothing, and dst's OOM flag already makes the append a no-op; the caller
 * reports the failure of src separately. */
static inline void yb_append_buf(ybuf_t *dst, const ybuf_t *src) {
  if (src->oom || src->len == 0) return;
  ccol_growbuf_append(dst, src->buf, src->len);
}

/* True when b holds readable content and its last byte is c. */
static inline bool yb_ends_with_char(const ybuf_t *b, char c) {
  return !b->oom && b->len > 0 && b->buf[b->len - 1] == c;
}

#ifdef RUNNING_UNIT_TESTS
/* Runs each of the three helpers above on a buffer in the state a failed
 * append leaves behind: the OOM flag latched, no backing store, and the old
 * content length still there. Each helper must answer from the flag alone
 * and index nothing, which is what keeps it correct on its own.
 * Returns one bit per helper, so the value is 7 when all three answered
 * without reading anything: bit 1 is the trim, bit 2 the buffer-to-buffer
 * append, and bit 4 the last-byte test. Remove any one of those guards and
 * the matching call below becomes a null dereference. */
unsigned cyaml_debug_growbuf_guard_bits(void) {
  /* Read through a volatile, so the buffer's shape is a run-time value and
   * the compiler actually runs the helpers instead of folding them away. */
  char *volatile absent_store = NULL;
  volatile size_t stale_len = 8;
  unsigned bits = 0;

  ybuf_t hostile = {.buf = absent_store,
                    .len = stale_len,
                    .cap = 0,
                    .oom = true,
                    .m_procs = NULL};
  yb_trim_trailing_inline_ws(&hostile, 0);
  if (hostile.len == stale_len) bits |= 1u;

  ybuf_t dst;
  yb_init(&dst, NULL);
  yb_append_buf(&dst, &hostile);
  if (!dst.oom && dst.len == 0) bits |= 2u;
  _ccol_mem_free(dst.m_procs, dst.buf);

  if (!yb_ends_with_char(&hostile, '\n')) bits |= 4u;
  return bits;
}
#endif

/* ========================================================================== */
/*                         INTERNAL HELPERS                                   */
/* ========================================================================== */

/*
 * Pins a helper into its callers on the parse path.
 *
 * The parser descends through an explicit frame stack instead of the call
 * stack, so the whole parser compiles into one very large driver function,
 * and GCC spends that function's inlining budget elsewhere. Each helper that
 * carries this macro was picked by measuring instruction counts over a whole
 * parse, not by looking at it: the direction of the effect can't be
 * predicted, and some plausible candidates measured worse. Measure again
 * before you add another helper, and before you remove one.
 */
#define _CYAML_PARSE_HOT static inline __attribute__((always_inline))

/*
 * The SSO storage of chmap_entry is naturally aligned, so this memcpy is
 * defense in depth and not something the alignment needs today. We keep it
 * because cjson uses the same pattern.
 */
static inline cyaml_node_t *_cyaml_read_child(const void *src) {
  cyaml_node_t *p;
  memcpy(&p, src, sizeof(p));
  return p;
}

/*
 * The members of a dictionary, in insertion order. The dictionary map keeps
 * every live entry on one list in that order: a new key goes after all the
 * others, replacing the value of a key keeps its place, and a delete takes it
 * out. That's why parsing, cyaml_dictionary_set(), cyaml_clone(), the
 * serializers and cyaml_dictionary_first() all see the members in the order
 * in which their keys first arrived. A walk allocates nothing and can't fail;
 * ccol_chmap_entry_read() reads an entry and gives the one after it.
 */
static inline const ccol_chmap_entry_ref *dict_first_entry(cyaml_node_t *n) {
  return ccol_chmap_oldest_entry(n->value.dictionary);
}

/* ========================================================================== */
/*                         THREAD-LOCAL NODE POOL                             */
/* ========================================================================== */

/*
 * A thread-local free-list pool for cyaml_node_t, the same as the one in
 * cjson.c. node_free() puts a node from the default allocator
 * (m_procs == NULL) back into this pool instead of freeing it at once,
 * which spreads the cost of calloc and free over many operations and helps a
 * workload that parses and destroys lots of documents.
 *
 * The pool holds at most _CYAML_POOL_CAP nodes. Any node above that count is
 * freed at once, and a node from a custom allocator never goes into the pool.
 *
 * A pthread destructor registered with _pool_key drains the pool when a
 * thread exits. Without it a worker thread would leak the nodes in its pool
 * for the life of the process.
 */
#define _CYAML_POOL_CAP 512U
#if defined(_CCOL_EMULATE_DARWIN_TLS)
/* One thread's pool and its error buffer. The pointer to this block is the
 * value of the pool key, so _pool_drain gets it as its argument and reads no
 * __thread variable (see ctlsmodel.h). A thread gets a block with its first
 * pooled node or its first stored error message, and has a value on the key
 * exactly as long as it has a block. */
typedef struct {
  cyaml_node_t *head;
  unsigned sz;
  char *err_buf;
} _cyaml_tls_t;
#else
static __thread cyaml_node_t *_pool_head = NULL;
static __thread unsigned _pool_sz = 0;
/* True once this thread has set its value on the pool key. Setting it once is
 * enough for the key's destructor to run at thread exit, so the lock below is
 * taken once per thread and not every time the thread's pool runs empty.
 * Taking it on every refill would write the lock's cache line once per parse
 * on every thread, and that line can hold data that every node allocation
 * reads, like the once flag of this subsystem. */
static __thread bool _pool_key_armed = false;
#endif /* _CCOL_EMULATE_DARWIN_TLS */
#ifdef RUNNING_UNIT_TESTS
/* How many times any thread has taken the pool key lock to arm the key. */
atomic_ulong _cyaml_pool_key_lock_count_for_tests;
#endif /* RUNNING_UNIT_TESTS */

/* Guards the pthread_setspecific calls. It becomes true once the key is
 * created and false before the key is deleted. Without it a background
 * thread that is running during a dlclose could call into a deleted key. */
static atomic_bool _pool_key_live = false;

/* True once the shared object's destructor has run. A thread whose first
 * pooled call comes after that, such as an application destructor that
 * destroys a tree, must not create the key, since nothing would delete it or
 * drain the pool it arms. _do_pool_key_init() then leaves the pool unarmed
 * and every node goes straight back to the allocator. */
static atomic_bool _pool_key_finalized = false;
static _ccol_thread_ls_key_t _pool_key;

/* Serializes two operations: "use the key while it is live", the read side
 * in node_free, which takes this lock as a reader, and "set _pool_key_live
 * to false and delete the key", the DSO destructor below and the only
 * writer. An atomic_load of _pool_key_live followed by acting on it isn't
 * enough, because the destructor can run between node_free's load and its
 * _ccol_thread_ls_set call and delete the key under a pthread_setspecific()
 * that is already in flight, which POSIX makes undefined. The lock covers
 * the load and the action together so the two exclude each other, and
 * _ccol_thread_ls_set either completes before the key is deleted or doesn't
 * run at all. The lock is initialized lazily inside _do_pool_key_init(),
 * under the same _pool_key_once below that guards the rest of the one-time
 * setup of this subsystem, since this codebase never gives a lock a static
 * or constant initializer. */
static _ccol_rw_lock_t _pool_key_rwlock;

static void _pool_drain(void *);

/*
 * The thread-local budget of node allocations left for the top-level
 * parse_common() call running on this thread. (size_t)-1 means "no parse is
 * running on this thread"; that's the default, and node_alloc() treats it as
 * unlimited. The doc comment of CYAML_MAX_PARSE_NODES, further below near
 * parse_ctx_t, explains why the budget exists. Only parse_node_budget_arm()
 * and _disarm(), which sit next to that macro, write it. parse_common() calls
 * _arm() once at entry and _disarm() on every exit path, so a value that a
 * parse has decremented never leaks into unrelated node creation after the
 * parse returns (a plain cyaml_create_list_mp() call, or a later, separate
 * parse on the same thread).
 */
static __thread size_t _parse_node_budget = (size_t)-1;

/* node_alloc() sets this flag when it refuses an allocation because
 * _parse_node_budget has reached zero, but not on a real allocator OOM. That
 * lets the call sites that clone a node to resolve an alias, which are the
 * main thing this budget bounds, report a specific, useful diagnostic.
 * Without the flag they could only report the general "out of memory" that
 * node_alloc's NULL return gives them. */
static __thread bool _parse_node_budget_exhausted = false;

/*
 * The thread-local budget of DOM bytes left for the top-level parse_common()
 * call running on this thread. It works like _parse_node_budget above:
 * (size_t)-1 means "no parse is running" and parse_bytes_charge() treats that
 * as unlimited. Only parse_byte_budget_arm() and _disarm() write it. The doc
 * comment of CYAML_MAX_PARSE_BYTES, further below near parse_ctx_t, explains
 * what this budget bounds that the node count alone can't.
 */
static __thread size_t _parse_byte_budget = (size_t)-1;

/* parse_bytes_charge() sets this flag when it refuses a charge because
 * _parse_byte_budget ran out, but not on a real allocator OOM, so the call
 * sites this budget mainly bounds can report a specific, useful diagnostic.
 * Without it they could only report the general "out of memory" that a NULL
 * return gives them. It is the byte-count counterpart of
 * _parse_node_budget_exhausted. */
static __thread bool _parse_byte_budget_exhausted = false;

/*
 * The approximate heap cost in bytes that the byte budget charges for each
 * kind of DOM allocation. Each figure is at or above the real cost of the
 * matching allocation, as measured for this library's cvec and chmap on
 * LP64, so the budget bounds real memory and never understates it.
 * _CYAML_ALLOC_OVERHEAD covers a typical allocator's per-block header and
 * size rounding, the two container base figures cover the backing
 * allocations of an empty cvec and an empty chmap, and
 * _CYAML_BYTES_PER_DICT_ENTRY covers one chmap entry and its share of the
 * amortized growth of the bucket array.
 *
 * The element slots of a list aren't charged on their own. A slot holds a
 * single pointer to a child node, and that child already costs at least
 * _CYAML_BYTES_PER_NODE, so the backing array can never be more than a small
 * constant fraction of the cost of the elements it indexes.
 */
#define _CYAML_ALLOC_OVERHEAD ((size_t)16)
#define _CYAML_BYTES_PER_NODE (sizeof(cyaml_node_t) + _CYAML_ALLOC_OVERHEAD)
#define _CYAML_BYTES_PER_LIST_BASE ((size_t)192)
#define _CYAML_BYTES_PER_DICT_BASE ((size_t)320)
#define _CYAML_BYTES_PER_DICT_ENTRY ((size_t)192)

/*
 * Charges bytes against this thread's parse byte budget. Returns true and
 * deducts the bytes when the budget is unarmed (no parse running, the common
 * case for every direct caller of the public API) or has room. Returns false
 * and latches _parse_byte_budget_exhausted when the charge would take the
 * budget past its limit, and every caller reports that like an allocation
 * failure.
 */
static inline bool parse_bytes_charge(size_t bytes) {
  if (_parse_byte_budget == (size_t)-1) return true;
  if (bytes > _parse_byte_budget) {
    _parse_byte_budget = 0;
    _parse_byte_budget_exhausted = true;
    return false;
  }
  _parse_byte_budget -= bytes;
  return true;
}

/* Whether parse_bytes_charge(bytes) would refuse, without charging and
 * without latching _parse_byte_budget_exhausted. */
static inline bool parse_bytes_would_refuse(size_t bytes) {
  return _parse_byte_budget != (size_t)-1 && bytes > _parse_byte_budget;
}

/*
 * Gives bytes back to this thread's parse byte budget, for when the budget
 * accepted a charge but the allocation then failed. Charging before the
 * allocation is on purpose: the budget bounds how much memory one parse may
 * hold at a time, so a request that is already past the limit must be
 * refused before its bytes reach the allocator. The refund keeps that order
 * exact, so bytes charged for an allocation that never happened stay
 * available to the rest of the parse. That way the byte budget agrees with
 * the node COUNT budget, which needs no refund of its own since node_alloc()
 * only decrements the count once a node exists.
 *
 * No current path depends on the refund: every parse treats a failed DOM
 * allocation as fatal and unwinds to parse_common(), which disarms both
 * budgets, and each top-level parse arms them again from scratch. The refund
 * is a standing defence that keeps the accounting exact as soon as some
 * speculative path learns to recover from a failed allocation.
 */
static inline void parse_bytes_refund(size_t bytes) {
  if (_parse_byte_budget != (size_t)-1) _parse_byte_budget += bytes;
}

/*
 * ccol_strdup(), but charging the bytes of the copy against the parse byte
 * budget first, so the budget that bounds the node structs also bounds a
 * string copied into the DOM (the text of a scalar, or the tag of a node).
 * Returns NULL when the budget refuses the charge, just as for an allocation
 * failure.
 */
_CYAML_PARSE_HOT char *strdup_charged(ccol_memmgmt_procs_t *mp, const char *s) {
  size_t n = strlen(s) + 1 + _CYAML_ALLOC_OVERHEAD;
  if (!parse_bytes_charge(n)) return NULL;
  char *copy = ccol_strdup(mp, s);
  if (!copy) parse_bytes_refund(n);
  return copy;
}

#if defined(_CCOL_EMULATE_DARWIN_TLS)
/* The calling thread's block, or NULL when the thread has none. */
static _cyaml_tls_t *_cyaml_tls_peek(void) {
  if (!atomic_load_explicit(&_pool_key_live, memory_order_acquire)) return NULL;
  return (_cyaml_tls_t *)_ccol_thread_ls_get(_pool_key);
}

/* The calling thread's block. When the thread has none we make one and set it
 * as the value of the pool key. Gives NULL when the key doesn't exist or
 * memory runs out. The caller must already have run the key's once-guard. */
static _cyaml_tls_t *_cyaml_tls_get(void) {
  _cyaml_tls_t *t = _cyaml_tls_peek();
  if (t || !atomic_load(&_pool_key_live)) return t;
  t = (_cyaml_tls_t *)calloc(1, sizeof(*t));
  if (!t) return NULL;
  bool set = false;
  _ccol_rw_lock_rdlock(_pool_key_rwlock);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&_cyaml_pool_key_lock_count_for_tests, 1,
                            memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
  if (atomic_load(&_pool_key_live) && _ccol_thread_ls_set(_pool_key, t) == 0)
    set = true;
  _ccol_rw_lock_unlock(_pool_key_rwlock);
  if (!set) {
    free(t);
    return NULL;
  }
  return t;
}
#else
/* Sets the value of the pool key for the calling thread, once per thread, so
 * that _pool_drain runs when the thread exits. The caller must already have
 * run the key's once-guard. It is always inlined so node_free() keeps the
 * same shape as with the code written in place. */
static inline __attribute__((always_inline)) void _pool_key_arm(void) {
  if (!_pool_key_armed && atomic_load(&_pool_key_live)) {
    _ccol_rw_lock_rdlock(_pool_key_rwlock);
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add_explicit(&_cyaml_pool_key_lock_count_for_tests, 1,
                              memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
    if (atomic_load(&_pool_key_live) &&
        _ccol_thread_ls_set(_pool_key, (void *)1) == 0)
      _pool_key_armed = true;
    _ccol_rw_lock_unlock(_pool_key_rwlock);
  }
}

/* The per-thread message buffer of a failed parse (see cyaml_err_storage()).
 * The thread-exit drain frees it together with the node pool. */
static __thread char *cyaml_err_buf;
#endif /* _CCOL_EMULATE_DARWIN_TLS */

/* Lazy key init, run exactly once on the first node_alloc call.
 * _pool_key_live gates pthread_setspecific in node_free and also the
 * destructor below, so a process that never calls a cyaml function pays
 * nothing. The flag is also the only sign that this one-time init worked: it
 * keeps its static initial value false when either pthread call below fails,
 * which is a real resource exhaustion, for example in a process that has
 * already reached PTHREAD_KEYS_MAX. So never use the rwlock or the key before
 * checking this flag; the cold path of node_free is the matching read side. */
static _ccol_once_flag_t _pool_key_once = _CCOL_ONCE_INIT;

static void _do_pool_key_init(void) {
  if (atomic_load(&_pool_key_finalized)) return;
  if (_ccol_rw_lock_init(_pool_key_rwlock) != 0) return;
  if (_ccol_thread_ls_key_create(_pool_key, _pool_drain) != 0) return;
  atomic_store(&_pool_key_live, true);
}

/* The DSO destructor. It drains the calling thread's pool, marks the key dead
 * so a concurrent thread skips the pthread_setspecific call, and then deletes
 * the key; without the delete, repeated dlopen and dlclose cycles use up
 * PTHREAD_KEYS_MAX. It always sets _pool_key_finalized first. The
 * _pool_key_live guard makes the rest of the function a no-op when no code
 * ever called a cyaml function, because then _do_pool_key_init never ran and
 * _pool_key_rwlock was never initialized either. The flag change and the key
 * deletion both happen under the write lock, so they can never interleave
 * with the read-locked use of the key in a concurrent node_free(); see the
 * doc comment of _pool_key_rwlock. */
__attribute__((destructor)) static void _pool_key_fini(void) {
  atomic_store(&_pool_key_finalized, true);
  if (!atomic_load(&_pool_key_live)) return;
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  _cyaml_tls_t *t = _cyaml_tls_peek();
  if (t) {
    (void)_ccol_thread_ls_set(_pool_key, NULL);
    _pool_drain(t);
  }
#else
  _pool_drain(NULL);
#endif
  _ccol_rw_lock_wrlock(_pool_key_rwlock);
  atomic_store(&_pool_key_live, false);
  _ccol_thread_ls_key_delete(_pool_key);
  _ccol_rw_lock_unlock(_pool_key_rwlock);
}

/* Allocates a new node, preferring a recycled one from the thread-local pool
 * when mp == NULL. The node comes back with the given type tag and a zeroed
 * value union. Returns NULL on an allocation failure, which includes a parse
 * on this thread that has used up its _parse_node_budget (see the doc comment
 * of that variable). */
_CYAML_PARSE_HOT cyaml_node_t *node_alloc(cyaml_node_type_t type,
                                          ccol_memmgmt_procs_t *mp) {
  _ccol_call_once(_pool_key_once, _do_pool_key_init);
  if (_parse_node_budget != (size_t)-1 && _parse_node_budget == 0) {
    _parse_node_budget_exhausted = true;
    return NULL;
  }
  /* Charge the node struct together with the empty backing container that a
   * list or dictionary node allocates right after this returns
   * (cyaml_create_list_mp() and cyaml_create_dictionary_mp() are the only two
   * callers that pass those types). A container's backing store costs several
   * times the node struct it hangs from, so a budget that only counted the
   * struct would miss most of the real memory of a tree with many
   * containers. */
  size_t node_bytes = _CYAML_BYTES_PER_NODE;
  if (type == CYAML_LIST)
    node_bytes += _CYAML_BYTES_PER_LIST_BASE;
  else if (type == CYAML_DICTIONARY)
    node_bytes += _CYAML_BYTES_PER_DICT_BASE;
  if (!parse_bytes_charge(node_bytes)) return NULL;
  cyaml_node_t *n;
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  _cyaml_tls_t *t = mp == NULL ? _cyaml_tls_peek() : NULL;
  if (t && t->head) {
    n = t->head;
    cyaml_node_t *next;
    memcpy(&next, (cyaml_node_t **)n, sizeof(next));
    t->head = next;
    t->sz--;
#else
  if (mp == NULL && _pool_head) {
    n = _pool_head;
    cyaml_node_t *next;
    memcpy(&next, (cyaml_node_t **)n, sizeof(next));
    _pool_head = next;
    _pool_sz--;
#endif
    memset(n, 0, sizeof(*n));
  } else {
    n = _ccol_mem_calloc(mp, 1, sizeof(*n));
    if (!n) {
      /* The budget accepted the charge above for a node that never came to
       * exist, so give the bytes back for the rest of this parse; see the doc
       * comment of parse_bytes_refund(). */
      parse_bytes_refund(node_bytes);
      return NULL;
    }
  }
  /* Only charge the budget once a node exists, whether it came from the pool
   * or from a fresh allocation. Decrementing before the allocation attempt
   * above would burn one unit of budget for good on a node that never came to
   * exist, every time the allocator fails briefly (under a test that injects
   * a single OOM fault, for example), and the budget would then understate how
   * many nodes the rest of this parse may create. The byte budget keeps the
   * same property through parse_bytes_refund(); see its doc comment. */
  if (_parse_node_budget != (size_t)-1) _parse_node_budget--;
  n->type = type;
  n->m_procs = mp;
  return n;
}

/* Puts a node back into the thread-local pool when m_procs == NULL and the
 * pool isn't full, and otherwise frees it directly through its own
 * allocator. */
_CYAML_PARSE_HOT void node_free(cyaml_node_t *n) {
  if (n->m_procs != NULL) {
    _ccol_mem_free(n->m_procs, n);
    return;
  }
  /* Every function that touches _pool_key_rwlock directly runs the once-guard
   * itself, as with every pthread primitive and every _ccol_once_flag_t in
   * this codebase. An argument from the call graph never replaces it: "every
   * node that gets here came from node_alloc(), and node_alloc() already ran
   * this guard" stops being true, without anything reporting it, the moment a
   * new call path reaches this point. */
  _ccol_call_once(_pool_key_once, _do_pool_key_init);
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  _cyaml_tls_t *t = _cyaml_tls_get();
  if (!t || t->sz >= _CYAML_POOL_CAP) {
    free(n);
    return;
  }
  memcpy((cyaml_node_t **)n, &t->head, sizeof(t->head));
  t->head = n;
  t->sz++;
#else
  if (_pool_sz >= _CYAML_POOL_CAP) {
    free(n);
    return;
  }
  if (_pool_sz == 0) {
    /* The cold path. It runs when this thread's pool is empty, and it only
     * takes the lock until this thread has armed the key once (see
     * _pool_key_armed). The doc comment of _pool_key_rwlock explains why a
     * lock has to cover the load and the action, and why an atomic check
     * alone isn't enough. The outer check here takes no lock; it only skips
     * _pool_key_rwlock entirely when _do_pool_key_init() never completed
     * (after a real pthread resource exhaustion, for example), because the
     * rwlock is then never initialized and locking it is undefined behavior.
     * That outer check doesn't reopen the race the inner lock closes.
     * _pool_key_live only goes from false to true inside _do_pool_key_init(),
     * which the _ccol_call_once() above has already run to completion with
     * the usual happens-before guarantee of pthread_once, so a true value
     * here proves the rwlock is fully initialized. The rwlock also stays
     * valid memory for the rest of the process even if a concurrent DSO
     * unload sets the flag back to false right after this check; the inner
     * check under the lock is what makes racing against that later change to
     * false safe. */
    _pool_key_arm();
    /* Only a pool whose key is armed gets drained at thread exit. If the key
     * can't be armed (no pthread key left in the process, the set fails, or
     * the shared object's destructor has already deleted the key and drained
     * this thread), nothing would ever drain this node, so it goes straight
     * back to the allocator. An armed thread never gets to this test, since
     * the arm call above jumps right past it. */
    if (!_pool_key_armed) {
      free(n);
      return;
    }
  }
  memcpy((cyaml_node_t **)n, &_pool_head, sizeof(_pool_head));
  _pool_head = n;
  _pool_sz++;
#endif /* _CCOL_EMULATE_DARWIN_TLS */
}

/* Walks the pool's free-list and calls free() on every node. The pthread
 * destructor calls this when a thread exits, and it also runs on a DSO
 * unload.
 *
 * The drain also clears _pool_key_armed. The C library resets the key's value
 * to NULL before it calls this destructor, so from then on the key is unarmed
 * for this thread. Another thread-specific destructor can run after this one
 * on the same thread and free nodes (the destructor of an application key
 * whose value holds a document, for example). The first such free then finds
 * an empty pool and sets the key again, and the C library runs one more
 * destructor round that drains those nodes too. If we left the flag true,
 * that free would skip the set and the nodes would sit in a pool that no
 * destructor drains. */
#if defined(_CCOL_EMULATE_DARWIN_TLS)
/* Under _CCOL_EMULATE_DARWIN_TLS the drain frees the block that arg names.
 * The C library has already cleared the key's value, so a node that another
 * key's destructor frees later makes a new block and sets the value again,
 * and the C library calls this destructor once more for it. */
static void _pool_drain(void *arg) {
  _cyaml_tls_t *t = (_cyaml_tls_t *)arg;
  if (!t) return;
  cyaml_node_t *n = t->head;
  while (n) {
    cyaml_node_t *next;
    memcpy(&next, (cyaml_node_t **)n, sizeof(next));
    free(n);
    n = next;
  }
  free(t->err_buf);
  free(t);
}
#else
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
  free(cyaml_err_buf);
  cyaml_err_buf = NULL;
  _pool_key_armed = false;
}
#endif /* _CCOL_EMULATE_DARWIN_TLS */

#ifdef RUNNING_UNIT_TESTS
/* The size of the calling thread's node-pool free-list. White-box unit tests
 * use it to check how the pool evicts a node above its cap (_CYAML_POOL_CAP)
 * and that each thread's pool stays separate. It isn't part of the public
 * API. */
#if defined(_CCOL_EMULATE_DARWIN_TLS)
size_t cyaml_debug_pool_size(void) {
  _cyaml_tls_t *t = _cyaml_tls_peek();
  return t ? (size_t)t->sz : 0;
}
#else
size_t cyaml_debug_pool_size(void) { return (size_t)_pool_sz; }
#endif
#endif

/*
 * An explicit worklist on the heap. node_clear() and __cyaml_destroy() below
 * use it to tear down a whole subtree without recursing once per level of
 * nesting. A tree that reaches them need not come from cyaml_parse(),
 * which CYAML_MAX_NESTING_DEPTH bounds: the public cyaml_list_push() and
 * cyaml_dictionary_set() can build a tree of any depth. A destroy has no way
 * to fail cleanly either, since both functions are void and we can't hand
 * the caller back a half-freed tree to try again. That's where they differ
 * from clone_node(), serialize_block() and serialize_flow(), so capping the
 * recursion with CYAML_MAX_NESTING_DEPTH the way those three do isn't an
 * option here: it would only swap an immediate crash for a permanent, silent
 * leak of everything past the cap. The worklist avoids both, because it keeps
 * the native call stack at O(1) depth for a tree of any depth and width, and
 * for any number of levels it drains.
 */
typedef struct destroy_worklist {
  cyaml_node_t **items;
  size_t cap;
  size_t len;
  /* true for parse_discard()'s worklist: a child that can't go onto the
   * list must keep its anchored nodes too. */
  bool rescue;
} destroy_worklist_t;

static void parse_discard(cyaml_node_t *n);

/* Pushes child onto wl, growing wl when it is full. The growth uses the
 * default allocator because the worklist is short-lived scratch state with
 * nothing to do with the m_procs of any node. A NULL child is silently
 * ignored, as at every other "destroy this child pointer" call site in this
 * file.
 *
 * When the worklist can't grow, we tear the child down at once with an
 * ordinary recursive __cyaml_destroy() call instead of deferring it. That can
 * bring back stack use proportional to depth, but only for that one child's
 * subtree, and only when BOTH conditions hold at once: the tree is deep
 * enough to matter AND the allocator can't grow a small scratch array. We
 * accept that combined failure as a rare, graceful degradation and don't
 * engineer around it further, since it is much narrower than the stack
 * exhaustion the worklist prevents, which needs depth alone and no memory
 * pressure at all. */
static void destroy_worklist_push(destroy_worklist_t *wl, cyaml_node_t *child) {
  if (!child) return;
  if (wl->len == wl->cap) {
    size_t new_cap = wl->cap == 0 ? 32 : wl->cap * 2;
    cyaml_node_t **grown =
        _ccol_default_mem_realloc(wl->items, new_cap * sizeof(*wl->items));
    if (!grown) {
      if (wl->rescue)
        parse_discard(child);
      else
        __cyaml_destroy((cyaml)child);
      return;
    }
    wl->items = grown;
    wl->cap = new_cap;
  }
  wl->items[wl->len++] = child;
}

/* The destructor callback for chmap_destroy_with_dtor(), used by the
 * CYAML_DICTIONARY case of node_clear_value(). It pushes the child node of
 * one dictionary entry onto the worklist that dtor_ctx carries instead of
 * recursing into it, so dictionary values drain through the same worklist
 * loop as everything else. */
static void _cyaml_enqueue_dict_child(const cmap_pair *val_pair,
                                      void *dtor_ctx) {
  cyaml_node_t *child = _cyaml_read_child(val_pair->ptr);
  destroy_worklist_push((destroy_worklist_t *)dtor_ctx, child);
}

/* Deep-frees the value payload of n (the string bytes, or a list or
 * dictionary container). Each direct child is pushed onto wl instead of
 * being recursed into, and n->type, n->tag and n itself are left alone. */
_CYAML_PARSE_HOT void node_clear_value(cyaml_node_t *n,
                                       destroy_worklist_t *wl) {
  switch (n->type) {
    case CYAML_STRING:
      _ccol_mem_free(n->m_procs, n->value.string);
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

/* Drains wl until it is empty, fully destroying every node in it, tag and
 * node struct included. node_clear_value() above pushes the children of each
 * drained node onto the worklist, so it keeps getting work until the whole
 * subtree of the initial contents of wl is gone. It's a plain loop rather
 * than recursion, and that's what keeps the stack use of __cyaml_destroy()
 * independent of the depth of the tree. */
static void destroy_worklist_drain(destroy_worklist_t *wl) {
  while (wl->len > 0) {
    cyaml_node_t *n = wl->items[--wl->len];
    node_clear_value(n, wl);
    _ccol_mem_free(n->m_procs, n->tag);
    n->tag = NULL;
    node_free(n);
  }
}

/* Deep-frees the resources of the value but not the node struct itself.
 * node_reinit_scalar() uses it to drop the old list or dictionary value of a
 * node before writing a new scalar into the node in place, for a cyaml_set()
 * call for example. The old value can be a tree of any depth (see the doc
 * comment of destroy_worklist_t above), so every node below the direct
 * children of n goes through the same worklist loop as in __cyaml_destroy()
 * and not through recursion. */
static void node_clear(cyaml_node_t *n) {
  destroy_worklist_t wl = {NULL, 0, 0, false};
  node_clear_value(n, &wl);
  destroy_worklist_drain(&wl);
  _ccol_default_mem_free(wl.items);
}

/*
 * Whether tag is one of the seven core-schema tag URIs of YAML 1.2. When it
 * is, *expected gets the only node type that the tag can decorate:
 * cyaml_parse() accepts a core tag on exactly that type and refuses it, or
 * converts the value, on any other type, so a core tag is valid on a node
 * only while the node's type is *expected. A tag that isn't a core tag
 * decorates any node, and for it we return false.
 */
static bool core_tag_expected_type(const char *tag,
                                   cyaml_node_type_t *expected) {
  /* Every core tag starts with the same 18-byte prefix, so one strncmp()
   * rules out most custom tags before any suffix comparison runs. */
  static const char prefix[] = "tag:yaml.org,2002:";
  if (strncmp(tag, prefix, sizeof(prefix) - 1) != 0) return false;
  const char *suffix = tag + sizeof(prefix) - 1;
  if (strcmp(suffix, "null") == 0)
    *expected = CYAML_NULL;
  else if (strcmp(suffix, "bool") == 0)
    *expected = CYAML_BOOL;
  else if (strcmp(suffix, "int") == 0)
    *expected = CYAML_INTEGER;
  else if (strcmp(suffix, "float") == 0)
    *expected = CYAML_FLOAT;
  else if (strcmp(suffix, "str") == 0)
    *expected = CYAML_STRING;
  else if (strcmp(suffix, "seq") == 0)
    *expected = CYAML_LIST;
  else if (strcmp(suffix, "map") == 0)
    *expected = CYAML_DICTIONARY;
  else
    return false;
  return true;
}

/* Drops the tag of n when it is a core-schema tag that doesn't match the type
 * of n. Every entry point that changes the type of an existing node calls
 * this afterwards; a custom tag always stays. Without it, a node parsed from
 * "port: !!int 8080" and then set to the string "auto" serializes as
 * "port: !<tag:yaml.org,2002:int> auto", which cyaml_parse() refuses, and a
 * node tagged !!str and then set to 8080 comes back from a round trip as the
 * string "8080". */
static void node_drop_stale_core_tag(cyaml_node_t *n) {
  cyaml_node_type_t expected;
  if (n->tag && core_tag_expected_type(n->tag, &expected) &&
      expected != n->type) {
    _ccol_mem_free(n->m_procs, n->tag);
    n->tag = NULL;
  }
}

/*
 * True when the C string s, which a caller hands to the public API to store
 * as a scalar value, a dictionary key or a tag, is well-formed UTF-8. NULL is
 * accepted so a caller can check an optional string; each entry point
 * decides for itself what a NULL means.
 *
 * Every Unicode scalar value is accepted, including the characters that
 * c-printable leaves out: a C0 control, DEL, a C1 control and U+FFFE or
 * U+FFFF each have an escape in a double-quoted scalar, and the serializer
 * writes every string that holds one double-quoted with that escape (a tag
 * writes it percent-escaped), so the tree still serializes to a document the
 * parser accepts and that reads back as the same text. Only a byte sequence
 * that isn't UTF-8 names no character that any escape could represent, so
 * that is what gets refused. The NUL character can't occur, since the
 * string ends at it.
 */
static inline bool cyaml_api_string_ok(const char *s) {
  return !s || ccol_utf8_is_valid(s, strlen(s));
}

/*
 * Checks a scalar payload described by cyaml_set() or by a direct
 * _cyaml_set_typed() call, without touching any node. Returns ccol_success
 * when node_reinit_scalar_checked() can store the payload, which can then
 * only fail for lack of memory. ccol_invalid_args reports an unsupported or
 * composite `type`, including _CYAML_TYPE_UNSUPPORTED, the sentinel that
 * _cyaml_type_of() in cyaml.h gives for a C value of a type that cyaml_set()
 * doesn't document. It also reports a raw_size that matches no real integer
 * or float width, a raw_size other than sizeof(bool) for CYAML_BOOL, a NULL
 * raw for any type other than CYAML_NULL, and a CYAML_STRING that isn't
 * well-formed UTF-8.
 *
 * This is where the list of types that cyaml_set() documents gets enforced.
 * Without it, an unsupported type would reach the store as whatever the
 * default case of _cyaml_type_of() gives, and that value would be silently
 * written as a real value instead of the call being refused.
 */
static ccol_retval_t scalar_payload_check(cyaml_node_type_t type, void *raw,
                                          size_t raw_size) {
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

  /* Every accepted type except CYAML_NULL carries a payload that we read
   * through raw, and a NULL raw has nothing to read, so every size check below
   * would dereference it. Only CYAML_NULL accepts raw == NULL, which is how a
   * direct _cyaml_set_typed() call says "no payload". */
  if (type != CYAML_NULL && !raw) return ccol_invalid_args;

  /* The _Generic dispatch of cyaml_set() maps ANY C expression of a void
   * pointer type to CYAML_NULL, not just the literal NULL. A non-NULL void
   * pointer is a caller mistake that would otherwise be thrown away and
   * written as a null with success reported, so we refuse it, as cjson_set()
   * does. The macro always passes raw_size == sizeof(void *) for this type;
   * a direct call with raw == NULL and raw_size == 0 is a real null and
   * passes. */
  if (type == CYAML_NULL && raw && raw_size == sizeof(void *)) {
    void *ptr_val;
    memcpy(&ptr_val, raw, sizeof(ptr_val));
    if (ptr_val != NULL) return ccol_invalid_args;
  }

  /* cyaml_set() always passes sizeof(bool) for a bool expression, so only a
   * direct call to _cyaml_set_typed() can get a mismatch here. Without this
   * check *(bool *)raw reads past a narrower buffer, or reads one garbage
   * byte of a wider one and stores that bit pattern in a _Bool, which is a
   * trap representation unless the byte is 0 or 1. */
  if (type == CYAML_BOOL && raw_size != sizeof(bool)) return ccol_invalid_args;

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

  /* For CYAML_STRING, raw names an object that is a pointer to char, just as
   * it names an integer object for CYAML_INTEGER and a floating object for
   * CYAML_FLOAT, so we check its width the same way before reading anything
   * through it. Without this check a caller of _cyaml_set_typed() that
   * describes a narrower object would have sizeof(const char *) bytes read
   * out of it, and whatever follows would go to strdup_charged() as a
   * string. */
  if (type == CYAML_STRING && raw_size != sizeof(const char *))
    return ccol_invalid_args;
  /* A string value must be well-formed UTF-8, so the tree only holds text
   * that the serializer can write and every YAML reader can read back; see
   * cyaml_api_string_ok(). */
  if (type == CYAML_STRING && !cyaml_api_string_ok(*(const char **)raw))
    return ccol_invalid_args;
  return ccol_success;
}

/*
 * Writes a payload that scalar_payload_check() has already accepted over an
 * existing scalar node, deep-freeing everything the old content owned.
 * Returns ccol_success, or ccol_not_enough_memory when copying a string
 * fails; the copy runs before node_clear(), so that failure leaves the
 * existing node completely unchanged.
 */
static ccol_retval_t node_reinit_scalar_checked(cyaml_node_t *n,
                                                cyaml_node_type_t type,
                                                void *raw, size_t raw_size,
                                                bool is_signed) {
  char *new_str = NULL;
  if (type == CYAML_STRING) {
    const char *s = *(const char **)raw;
    if (s) {
      new_str = strdup_charged(n->m_procs, s);
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
        /* Every read goes through memcpy into a fixed-width integer,
         * because the caller's object can be a long, an int64_t or any other
         * type of that width, and reading it through a different integer
         * type isn't allowed by C11 6.5p7. */
        switch (raw_size) {
          case 1: {
            int8_t x;
            memcpy(&x, raw, sizeof(x));
            v = x;
            break;
          }
          case 2: {
            int16_t x;
            memcpy(&x, raw, sizeof(x));
            v = x;
            break;
          }
          case 4: {
            int32_t x;
            memcpy(&x, raw, sizeof(x));
            v = x;
            break;
          }
          case 8: {
            int64_t x;
            memcpy(&x, raw, sizeof(x));
            v = x;
            break;
          }
        }
      } else {
        switch (raw_size) {
          case 1: {
            uint8_t x;
            memcpy(&x, raw, sizeof(x));
            v = (long long)x;
            break;
          }
          case 2: {
            uint16_t x;
            memcpy(&x, raw, sizeof(x));
            v = (long long)x;
            break;
          }
          case 4: {
            uint32_t x;
            memcpy(&x, raw, sizeof(x));
            v = (long long)x;
            break;
          }
          case 8: {
            unsigned long long u;
            memcpy(&u, raw, sizeof(u));
            if (u > (unsigned long long)LLONG_MAX) {
              /* No long long can hold it, so it becomes the nearest double,
               * the same node that parsing its decimal literal gives, since
               * that literal overflows the core schema's integer grammar and
               * resolves as a float. */
              n->type = CYAML_FLOAT;
              n->value.number = (double)u;
              node_drop_stale_core_tag(n);
              return ccol_success;
            }
            v = (long long)u;
            break;
          }
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
  node_drop_stale_core_tag(n);
  return ccol_success;
}

/* Writes a new typed value over an existing scalar node. Every check runs
 * before anything touches the node, so a refused call leaves the node
 * completely unchanged. The return codes are those of scalar_payload_check()
 * and node_reinit_scalar_checked(). */
static ccol_retval_t node_reinit_scalar(cyaml_node_t *n, cyaml_node_type_t type,
                                        void *raw, size_t raw_size,
                                        bool is_signed) {
  ccol_retval_t r = scalar_payload_check(type, raw, raw_size);
  if (r != ccol_success) return r;
  return node_reinit_scalar_checked(n, type, raw, raw_size, is_signed);
}

/* Allocates a new node with the caller's allocator from a payload that
 * scalar_payload_check() has already accepted. On success *out gets the new
 * node. The only possible failure is ccol_not_enough_memory (for the node or
 * for the copy of a string), and *out is NULL then. */
static ccol_retval_t node_make_scalar(cyaml_node_type_t type, void *raw,
                                      size_t raw_size, bool is_signed,
                                      ccol_memmgmt_procs_t *mp,
                                      cyaml_node_t **out) {
  *out = NULL;
  cyaml_node_t *n = node_alloc(CYAML_NULL, mp);
  if (!n) return ccol_not_enough_memory;
  ccol_retval_t r =
      node_reinit_scalar_checked(n, type, raw, raw_size, is_signed);
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

/* Swaps *mp for its interned copy so no node keeps a pointer to the caller's
 * struct. Returns false when the intern table is full; *mp is NULL then and
 * the caller must fail without creating anything. */
static inline bool cyaml_intern_procs(ccol_memmgmt_procs_t **mp) {
  if (*mp == NULL) return true;
  return ccol_procs_intern(*mp, mp) == ccol_success;
}

/*
 * The factory functions, one per node type. mp may be NULL for the default
 * allocator, and all of them return NULL on an allocation failure.
 *
 * cyaml_create_string_mp with val == NULL gives a CYAML_NULL node.
 * cyaml_create_list_mp and cyaml_create_dictionary_mp give empty containers,
 * backed by a cvec for the list and a chmap for the dictionary.
 */
static cyaml cyaml_create_null_interned(ccol_memmgmt_procs_t *mp) {
  return (cyaml)node_alloc(CYAML_NULL, mp);
}

static cyaml cyaml_create_bool_interned(bool val, ccol_memmgmt_procs_t *mp) {
  cyaml_node_t *n = node_alloc(CYAML_BOOL, mp);
  if (n) n->value.boolean = val;
  return (cyaml)n;
}

static cyaml cyaml_create_int_interned(long long val,
                                       ccol_memmgmt_procs_t *mp) {
  cyaml_node_t *n = node_alloc(CYAML_INTEGER, mp);
  if (n) n->value.integer = val;
  return (cyaml)n;
}

static cyaml cyaml_create_double_interned(double val,
                                          ccol_memmgmt_procs_t *mp) {
  cyaml_node_t *n = node_alloc(CYAML_FLOAT, mp);
  if (n) n->value.number = val;
  return (cyaml)n;
}

static cyaml cyaml_create_string_interned(const char *val,
                                          ccol_memmgmt_procs_t *mp) {
  if (!val) return cyaml_create_null_interned(mp);
  cyaml_node_t *n = node_alloc(CYAML_STRING, mp);
  if (!n) return NULL;
  n->value.string = strdup_charged(mp, val);
  if (!n->value.string) {
    node_free(n);
    return NULL;
  }
  return (cyaml)n;
}

static cyaml cyaml_create_list_interned(ccol_memmgmt_procs_t *mp) {
  cyaml_node_t *n = node_alloc(CYAML_LIST, mp);
  if (!n) return NULL;
  n->value.list = cvector_create_full(sizeof(cyaml_node_t *), mp, NULL);
  if (!n->value.list) {
    node_free(n);
    return NULL;
  }
  return (cyaml)n;
}

static cyaml cyaml_create_dictionary_interned(ccol_memmgmt_procs_t *mp) {
  cyaml_node_t *n = node_alloc(CYAML_DICTIONARY, mp);
  if (!n) return NULL;
  char *err = NULL;
  n->value.dictionary =
      ccol_chmap_create_compact(ccol_string, ccol_pointer, mp, &err);
  if (!n->value.dictionary) {
    node_free(n);
    return NULL;
  }
  return (cyaml)n;
}

cyaml cyaml_create_null_mp(ccol_memmgmt_procs_t *mp) {
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_null_interned(mp);
}

cyaml cyaml_create_bool_mp(bool val, ccol_memmgmt_procs_t *mp) {
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_bool_interned(val, mp);
}

cyaml cyaml_create_int_mp(long long val, ccol_memmgmt_procs_t *mp) {
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_int_interned(val, mp);
}

cyaml cyaml_create_double_mp(double val, ccol_memmgmt_procs_t *mp) {
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_double_interned(val, mp);
}

cyaml cyaml_create_string_mp(const char *val, ccol_memmgmt_procs_t *mp) {
  if (!cyaml_api_string_ok(val)) return NULL;
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_string_interned(val, mp);
}

cyaml cyaml_create_list_mp(ccol_memmgmt_procs_t *mp) {
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_list_interned(mp);
}

cyaml cyaml_create_dictionary_mp(ccol_memmgmt_procs_t *mp) {
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_dictionary_interned(mp);
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/* Frees a node and everything below it with a loop instead of recursion (the
 * doc comment of destroy_worklist_t above explains why): a tree that gets
 * here doesn't have to come from cyaml_parse(), and one built directly
 * through the public mutation API has no depth bound. It's safe to call on
 * NULL. It does NOT set the caller's pointer to NULL; the cyaml_destroy()
 * macro wrapper does that.
 *
 * n itself is cleared and freed directly, so it never goes through
 * destroy_worklist_push(). That function's OOM fallback calls
 * __cyaml_destroy() recursively on the item it couldn't add to the list, and
 * n is the argument of this very call, so pushing n there could mean calling
 * ourselves on the same node during an OOM. Real nodes below n aren't
 * affected: the worklist drains each of them, the same way node_clear()
 * uses it. */
void __cyaml_destroy(cyaml node) {
  if (!node) return;
  cyaml_node_t *n = (cyaml_node_t *)node;

  destroy_worklist_t wl = {NULL, 0, 0, false};
  node_clear_value(n, &wl);
  _ccol_mem_free(n->m_procs, n->tag);
  n->tag = NULL;
  node_free(n);

  destroy_worklist_drain(&wl);
  _ccol_default_mem_free(wl.items);
}

/* ========================================================================== */
/*                         LEAF VALUE ACCESS                                  */
/* ========================================================================== */

/* The type tag of the node, or CYAML_NULL for a NULL handle. */
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
    _ccol_mem_free(n->m_procs, n->tag);
    n->tag = NULL;
    return ccol_success;
  }
  /* The empty string isn't a tag. The serializer writes a custom tag in its
   * verbatim "!<...>" form, so an empty tag would come out as "!<>", which
   * parse_tag_token() refuses as an empty verbatim tag. Accepting it here
   * would build a DOM that this library can't parse back from its own
   * serialization, which breaks the round trip the tag accessors promise.
   * Use NULL to clear a tag. */
  if (tag[0] == '\0') return ccol_invalid_args;
  /* "!" alone is the non-specific tag. The parser never stores it on a node
   * and no written tag may name it, since the serializer would write it as
   * "!<!>", which cyaml_parse() refuses. */
  if (strcmp(tag, _CYAML_TAG_NON_SPECIFIC) == 0) return ccol_invalid_args;
  /* A tag is text, so it must be well-formed UTF-8; see
   * cyaml_api_string_ok(). */
  if (!cyaml_api_string_ok(tag)) return ccol_invalid_args;
  /* A core-schema tag is only valid on the one type it names, because any
   * other pairing serializes to a document that cyaml_parse() refuses or
   * reads back as a different type. See core_tag_expected_type(). */
  cyaml_node_type_t expected;
  if (core_tag_expected_type(tag, &expected) && expected != n->type)
    return ccol_invalid_args;
  char *copy = strdup_charged(n->m_procs, tag);
  if (!copy) return ccol_not_enough_memory;
  _ccol_mem_free(n->m_procs, n->tag);
  n->tag = copy;
  return ccol_success;
}

/*
 * The typed value accessors. Each one calls _ccol_fatal_err() for a wrong
 * type and for NULL, so guard the call with cyaml_type() when the type isn't
 * known for sure at compile time.
 */
bool cyaml_bool_val(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_BOOL)
    _ccol_fatal_err("cyaml_bool_val: node is %s, expected CYAML_BOOL",
                    cyaml_type_str(node));
  return n->value.boolean;
}

long long cyaml_int_val(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_INTEGER)
    _ccol_fatal_err("cyaml_int_val: node is %s, expected CYAML_INTEGER",
                    cyaml_type_str(node));
  return n->value.integer;
}

double cyaml_double_val(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_FLOAT)
    _ccol_fatal_err("cyaml_double_val: node is %s, expected CYAML_FLOAT",
                    cyaml_type_str(node));
  return n->value.number;
}

const char *cyaml_str_val(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_STRING)
    _ccol_fatal_err("cyaml_str_val: node is %s, expected CYAML_STRING",
                    cyaml_type_str(node));
  return n->value.string;
}

/* The number of elements of a list, or the number of key and value pairs of
 * a dictionary. Both call _ccol_fatal_err() when the node has the wrong
 * type.
 */
size_t cyaml_list_len(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_LIST)
    _ccol_fatal_err("cyaml_list_len: node is %s, expected CYAML_LIST",
                    cyaml_type_str(node));
  return cvector_elem_count(n->value.list);
}

size_t cyaml_dictionary_size(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_DICTIONARY)
    _ccol_fatal_err(
        "cyaml_dictionary_size: node is %s, expected CYAML_DICTIONARY",
        cyaml_type_str(node));
  return chmap_elem_count(n->value.dictionary);
}

/* ========================================================================== */
/*                          LIST / DICTIONARY MANIPULATION                    */
/* ========================================================================== */

/*
 * Appends child to the list. Each return code carries one ownership rule:
 * ccol_invalid_args always means that the arguments were refused and child
 * wasn't touched at all, so the caller keeps it, while every other failure
 * (an OOM or a full list) takes ownership of child and destroys it before
 * returning. The whole point of splitting it into two classes is that a
 * caller can act on the code alone, since nothing else about the call tells
 * the classes apart, and a wrong guess either leaks a subtree or frees it
 * twice. This matches cjson_list_push with one difference: here we don't walk
 * child's subtree looking for seq. cyaml.h documents that a cycle built that
 * way isn't checked, so there is no refusal here for a container found
 * inside a subtree.
 */
ccol_retval_t cyaml_list_push(cyaml seq, cyaml child) {
  if (!child) return ccol_invalid_args;
  cyaml_node_t *c = (cyaml_node_t *)child;
  /* Every argument refusal answers ccol_invalid_args and leaves child alone,
   * including when several refusals apply at once. A caller can pass one
   * handle as both seq and child where that handle isn't a CYAML_LIST; both
   * the self-attach test and the kind test are true for that call, and both
   * give the same answer. We compare against seq and not n, so the check
   * holds even when seq is NULL or the wrong kind of node. Destroying a child
   * that is already attached would free memory its current parent owns and
   * corrupt that parent's tree. */
  if (c->attached || (const void *)c == (const void *)seq) {
    /* Some other parent already owns child, or child is seq itself. Accepting
     * it would give the node two owners, and each would free it when its own
     * parent is destroyed. */
    return ccol_invalid_args;
  }
  if (!seq) return ccol_invalid_args;
  cyaml_node_t *n = (cyaml_node_t *)seq;
  if (n->type != CYAML_LIST) return ccol_invalid_args;
  /* cvector_push_back only answers ccol_invalid_args for a NULL element, and
   * it gets &c (the address of a local), so it can't answer that code here.
   * That matters because ccol_invalid_args is this function's "child
   * untouched" code, while the branch below has already destroyed child;
   * without this property the two classes would share one code. */
  ccol_retval_t r = cvector_push_back(n->value.list, &c);
  if (r != ccol_success) {
    __cyaml_destroy(child);
    return r;
  }
  c->attached = true;
  return r;
}

/* A borrowed reference to the element at position index. NULL when index is
 * out of bounds, and when seq isn't a CYAML_LIST node. */
cyaml cyaml_list_get(cyaml seq, size_t index) {
  if (!seq) return NULL;
  cyaml_node_t *n = (cyaml_node_t *)seq;
  if (n->type != CYAML_LIST) return NULL;
  void *slot = cvector_at(n->value.list, index);
  if (!slot) return NULL;
  return *(cyaml *)slot;
}

/* Replaces the child that the dictionary slot slot_vp describes with child.
 * slot_vp is the pair that ccol_chmap_insert_or_get_elem() or
 * chmap_get_elem_ref() filled for a key that was already present. The old
 * child is freed first (through parse_discard(), which keeps its anchored
 * nodes while a parse runs) and then the new pointer goes into the slot, so
 * the slot never names freed memory once this returns. Freeing the old child
 * changes no map that holds the slot, so slot_vp->ptr stays valid across the
 * free. child must have no parent, and it becomes attached here. */
static void dict_slot_replace_child(const cmap_pair *slot_vp,
                                    cyaml_node_t *child) {
  cyaml_node_t *old_child = _cyaml_read_child(slot_vp->ptr);
  parse_discard(old_child);
  memcpy((void *)slot_vp->ptr, &child, sizeof(child));
  child->attached = true;
}

/* Inserts or replaces the value for key in map. Each return code carries one
 * ownership rule, as in cyaml_list_push(): ccol_invalid_args always means
 * that the arguments were refused and child wasn't touched, so the caller
 * keeps it, while every other failure takes ownership of child and destroys
 * it before returning. Passing back the exact node the key already holds is a
 * harmless no-op that reports ccol_success. For a key that is already
 * present the old value is freed and the slot pointed at child with nothing
 * in between that can fail, so the slot never dangles once we return. */
static ccol_retval_t dictionary_set_impl(cyaml map, const char *key,
                                         cyaml child) {
  if (!child) return ccol_invalid_args;
  cyaml_node_t *c = (cyaml_node_t *)child;
  /* Every argument refusal answers ccol_invalid_args and leaves child alone,
   * including when several refusals apply at once. A caller can pass one
   * handle as both map and child where that handle isn't a CYAML_DICTIONARY,
   * and both the self-attach test and the kind test are true for that call.
   * We compare against map and not n, so it holds even when map is NULL or
   * the wrong kind of node. Only the self-check is decided up here: the
   * "already attached" refusal has to stay below the c == old_child no-op,
   * because only an attached child can reach that no-op (see the comment of
   * that check). */
  if ((const void *)c == (const void *)map) return ccol_invalid_args;
  if (!map || !key) return ccol_invalid_args;
  cyaml_node_t *n = (cyaml_node_t *)map;
  if (n->type != CYAML_DICTIONARY) return ccol_invalid_args;

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};

  if (c->attached) {
    /* Only an attached child can be the node key already holds, so only this
     * branch looks the key up before deciding. Setting a key to the value it
     * already holds is the one case where an attached child is accepted, and
     * there's nothing to do and nothing to free: destroying "the old value"
     * there would destroy the very node the dictionary correctly points at.
     * Any other attached child belongs to some other parent, so we refuse the
     * call and touch neither child nor the slot. child == map was already
     * refused at the top of the function. */
    cmap_pair held_vp;
    if (chmap_get_elem_ref(n->value.dictionary, &kp, &held_vp) ==
            ccol_success &&
        _cyaml_read_child(held_vp.ptr) == c)
      return ccol_success;
    return ccol_invalid_args;
  }

  /* A new key costs a dictionary entry against the DOM byte budget of a
   * running parse (see CYAML_MAX_PARSE_BYTES). A dictionary stores its own
   * copy of the key bytes, so an entry costs more than the child node that
   * this call has already been charged for, and an alias expansion that
   * copies a mapping copies every one of its keys too. We charge first so the
   * insert below can be one hash and one probe, and give the bytes back when
   * the key turns out to be present already. A charge that would be refused
   * is decided with a lookup first, because a key that is present needs no
   * new entry and must neither fail nor latch the exhausted budget. Outside a
   * parse the budget is unarmed and the charge never refuses. */
  size_t entry_bytes = _CYAML_BYTES_PER_DICT_ENTRY + kp.size;
  if (parse_bytes_would_refuse(entry_bytes)) {
    cmap_pair held_vp;
    if (chmap_get_elem_ref(n->value.dictionary, &kp, &held_vp) ==
        ccol_success) {
      dict_slot_replace_child(&held_vp, c);
      return ccol_success;
    }
  }
  if (!parse_bytes_charge(entry_bytes)) {
    /* We report the refusal the same way as the insert failure below,
     * ownership rule included. */
    __cyaml_destroy(child);
    return ccol_not_enough_memory;
  }

  /* ccol_chmap_insert_or_get_elem only answers ccol_invalid_args in three
   * cases, exactly like chmap_insert_elem: a NULL map, a pair that is NULL or
   * has size zero, and a key whose size doesn't agree with a fixed-width key
   * type. This map belongs to the node and cyaml_create_dictionary_mp() built
   * it with a ccol_string key, which has no fixed width, so the width check
   * always passes; the map is therefore separately chained and the value-size
   * check of open addressing is never reached. kp describes the key validated
   * here with size strlen(key) + 1, and vp describes &c, so none of the three
   * cases can happen. That matters because ccol_invalid_args is this
   * function's "child untouched" code, while the failure branch below has
   * already destroyed child; without this property the two classes would
   * share one code. */
  cmap_pair vp = {.ptr = &c, .size = sizeof(c)};
  cmap_pair old_vp;
  ccol_retval_t r =
      ccol_chmap_insert_or_get_elem(n->value.dictionary, &kp, &vp, &old_vp);
  if (r == ccol_success) {
    c->attached = true;
    return ccol_success;
  }
  parse_bytes_refund(entry_bytes);
  if (r == ccol_key_already_present) {
    dict_slot_replace_child(&old_vp, c);
    return ccol_success;
  }
  __cyaml_destroy((cyaml)child);
  return r;
}

/* The public entry point. Like every other argument refusal, a key that
 * isn't well-formed UTF-8 is refused with ccol_invalid_args and child is left
 * untouched. The parser calls dictionary_set_impl() directly, because the
 * stream check of parse_common() has already proved every key it builds. */
ccol_retval_t cyaml_dictionary_set(cyaml map, const char *key, cyaml child) {
  if (!cyaml_api_string_ok(key)) return ccol_invalid_args;
  return dictionary_set_impl(map, key, child);
}

/* Looks key up in map and gives a borrowed reference to its child. NULL when
 * the key isn't there, when map is NULL, and when map isn't a
 * CYAML_DICTIONARY node. */
cyaml cyaml_dictionary_get(cyaml map, const char *key) {
  if (!map || !key) return NULL;
  cyaml_node_t *n = (cyaml_node_t *)map;
  if (n->type != CYAML_DICTIONARY) return NULL;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair vp;
  if (chmap_get_elem_ref(n->value.dictionary, &kp, &vp) != ccol_success)
    return NULL;
  return _cyaml_read_child(vp.ptr);
}

/*
 * Removes the element at position index from a list and deep-frees it.
 *
 * The element at index is destroyed, every later element moves left by one
 * position (O(n) in the number of elements after index), and then
 * cvector_pop_back makes the vector one element shorter.
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

/* ========================================================================== */
/*                         DICTIONARY ITERATION                               */
/* ========================================================================== */

/* The cursor reads a member's successor at the moment it steps onto that
 * member. Removing the current member frees only that member's entry, so the
 * successor the cursor holds stays valid. */
bool cyaml_dictionary_next(cyaml_dictionary_iter *it) {
  if (!it) return false;
  const ccol_chmap_entry_ref *e = (const ccol_chmap_entry_ref *)it->_cyaml_next;
  if (!e) {
    it->key = NULL;
    it->value = NULL;
    return false;
  }
  const cmap_pair *kp, *vp;
  it->_cyaml_next = ccol_chmap_entry_read(e, &kp, &vp);
  it->key = (const char *)kp->ptr;
  it->value = (cyaml)_cyaml_read_child(vp->ptr);
  return true;
}

bool cyaml_dictionary_first(cyaml dict, cyaml_dictionary_iter *it) {
  if (!it) return false;
  cyaml_node_t *n = (cyaml_node_t *)dict;
  it->_cyaml_next =
      (n && n->type == CYAML_DICTIONARY) ? dict_first_entry(n) : NULL;
  return cyaml_dictionary_next(it);
}

/*
 * Removes the entry with the given key from a dictionary and deep-frees it.
 *
 * We read the child pointer out before deleting the map entry, so the
 * subtree is only freed once the hash table no longer points at it.
 */
ccol_retval_t cyaml_dictionary_remove(cyaml map, const char *key) {
  if (!map || !key) return ccol_invalid_args;
  cyaml_node_t *n = (cyaml_node_t *)map;
  if (n->type != CYAML_DICTIONARY) return ccol_invalid_args;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair vp;
  if (chmap_get_elem_ref(n->value.dictionary, &kp, &vp) != ccol_success)
    return ccol_key_not_found;
  cyaml_node_t *child = _cyaml_read_child(vp.ptr);
  ccol_retval_t r = chmap_delete_elem(n->value.dictionary, &kp);
  if (r == ccol_success) __cyaml_destroy((cyaml)child);
  return r;
}

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

/* Attaches a strdup copy of src_tag to the dst clone, which is already fully
 * built. Returns false and destroys dst on an OOM; also returns false when
 * dst itself is NULL because the base constructor already failed. A NULL
 * src_tag is a successful no-op, since a node with no tag gives a clone with
 * no tag. clone_shallow(), the one step that copies every node of a clone,
 * calls it for each node it makes, so no node shape can miss its tag. */
static bool clone_attach_tag(cyaml dst, const char *src_tag,
                             ccol_memmgmt_procs_t *mp) {
  if (!dst) return false;
  if (!src_tag) return true;
  char *t = strdup_charged(mp, src_tag);
  if (!t) {
    __cyaml_destroy(dst);
    return false;
  }
  ((cyaml_node_t *)dst)->tag = t;
  return true;
}

/*
 * The largest number of nested containers (lists and dictionaries) that a
 * tree may have on any path from its root, the root counted when it is a
 * container. Scalars, tags and anchors take no level. One constant serves
 * cyaml_parse*(), cyaml_clone() and every serializer, so each of them accepts
 * exactly the trees the others accept: a parse never builds a tree that a
 * clone or a serialization refuses, and the text of every serialization
 * parses back. A tree with 500 nested containers, with anything inside the
 * innermost one, is accepted, and a 501st container is refused.
 *
 * It's a policy limit on the work and worklist memory of one call and has
 * nothing to do with the stack: the parser, clone_node() and both
 * serializers walk with explicit stacks of frames, which keep the native
 * call stack at O(1) depth for a tree of any depth. A tree built past this
 * limit through the public cyaml_list_push() and cyaml_dictionary_set() makes
 * a clone or a serialization return NULL.
 */
#define CYAML_MAX_NESTING_DEPTH 500

/*
 * How many frames of an explicit walk stack its owner keeps in its own
 * locals. For a deeper tree, walk_stack_grow below moves the stack onto the
 * heap. The walk pushes one frame per container level, so a document of
 * ordinary shape is walked without touching the allocator at all.
 */
#define _CYAML_WALK_INLINE_FRAMES 16

/*
 * Grows the capacity of an explicit walk stack to new_cap, which must be
 * larger than *cap, and returns the new storage. stack points at the
 * caller's inline array as long as that is big enough; the first growth
 * copies those frames onto the heap and every later growth reallocates in
 * place. On failure it returns NULL and leaves *cap exactly as it was, so the
 * caller can walk and free everything it has already pushed. It returns the
 * new storage instead of writing it through a void **, so each caller can
 * assign it to its own frame pointer type.
 *
 * A *_MAX_DEPTH cap bounds the depth of every caller, so new_cap stays far
 * below the point where new_cap * elem_size could overflow.
 */
static void *walk_stack_grow_to(void *stack, size_t *cap, size_t new_cap,
                                void *inline_base, size_t elem_size,
                                ccol_memmgmt_procs_t *mp) {
  void *grown;
  if (stack == inline_base) {
    grown = _ccol_mem_alloc(mp, new_cap * elem_size);
    if (!grown) return NULL;
    memcpy(grown, inline_base, *cap * elem_size);
  } else {
    grown = _ccol_mem_realloc(mp, stack, new_cap * elem_size);
    if (!grown) return NULL;
  }
  *cap = new_cap;
  return grown;
}

/* walk_stack_grow_to() with twice the current capacity. */
static void *walk_stack_grow(void *stack, size_t *cap, void *inline_base,
                             size_t elem_size, ccol_memmgmt_procs_t *mp) {
  return walk_stack_grow_to(stack, cap, *cap * 2, inline_base, elem_size, mp);
}

/*
 * One level of clone_node()'s explicit walk stack. It holds a source
 * container and its clone, which starts empty and receives the copies of the
 * source's children. A list frame walks src by index, and a dictionary frame
 * walks src through `next`, which is NULL when no entry is left and also when
 * the walk could never reach the entries. `idx` counts how many entries the
 * walk actually saw, which tells a walk that stopped early from one that
 * completed.
 */
typedef struct {
  cyaml_node_t *src;
  cyaml_node_t *dst;
  /* A list: the next index to copy, and the element count. */
  size_t idx;
  size_t count;
  /* A dictionary: the next entry to copy, in insertion order. */
  const ccol_chmap_entry_ref *next;
} _clone_frame_t;

/* Copies the node src itself and none of its children: a scalar comes back
 * complete, and a list or dictionary comes back empty for the caller to fill.
 * The tag is attached here, so the returned node is finished except for its
 * children. Returns NULL on an allocation failure and leaves nothing
 * behind. */
static cyaml clone_shallow(cyaml_node_t *src) {
  ccol_memmgmt_procs_t *mp = src->m_procs;
  cyaml dst;
  switch (src->type) {
    case CYAML_NULL:
      dst = cyaml_create_null_interned(mp);
      break;
    case CYAML_BOOL:
      dst = cyaml_create_bool_interned(src->value.boolean, mp);
      break;
    case CYAML_INTEGER:
      dst = cyaml_create_int_interned(src->value.integer, mp);
      break;
    case CYAML_FLOAT:
      dst = cyaml_create_double_interned(src->value.number, mp);
      break;
    case CYAML_STRING:
      dst = cyaml_create_string_interned(src->value.string, mp);
      break;
    case CYAML_LIST:
      dst = cyaml_create_list_interned(mp);
      break;
    case CYAML_DICTIONARY:
      dst = cyaml_create_dictionary_interned(mp);
      break;
    default:
      return NULL;
  }
  return clone_attach_tag(dst, src->tag, mp) ? dst : NULL;
}

/* Starts the walk of the container src, whose children get copied into the
 * empty clone dst. A dictionary is walked in insertion order and every copied
 * entry is appended to dst, so the clone keeps the source's order. The walk
 * allocates nothing. */
static void clone_frame_open(_clone_frame_t *f, cyaml_node_t *src,
                             cyaml_node_t *dst) {
  f->src = src;
  f->dst = dst;
  f->idx = 0;
  f->count = 0;
  f->next = NULL;
  if (src->type == CYAML_LIST)
    f->count = cvector_elem_count(src->value.list);
  else
    f->next = dict_first_entry(src);
}

/* Makes an independent deep copy of the subtree rooted at src. An explicit
 * stack of container frames drives the walk instead of recursion, so the
 * native call stack stays at O(1) depth for a source of any depth, and
 * max_levels bounds the work and not the stack.
 *
 * max_levels is the largest number of nested containers the copy may have on
 * any path, its root counted when the root is a container. A source past it
 * makes this return NULL with *too_deep set; every other failure is an OOM or
 * an exhausted parse budget and leaves *too_deep false. On success *height is
 * the largest number of nested containers on any path of the copy, 0 for a
 * scalar. The walk looks at every node anyway, so it measures the
 * height on the way, and the check and the measurement cost one comparison
 * each per container.
 *
 * A shallow copy of each child is made and attached to its parent's clone
 * before the child's subtree is walked, so a failure at any point leaves
 * everything built so far reachable from `root`, and a single
 * __cyaml_destroy() call frees all of it. clone_shallow() attaches a tag as
 * it creates each node. The order of the allocations isn't visible to a
 * caller, because a clone either completes or is destroyed whole. */
static cyaml clone_node(cyaml_node_t *src, size_t max_levels, bool *too_deep,
                        size_t *height) {
  ccol_memmgmt_procs_t *mp = src->m_procs;
  *too_deep = false;
  *height = 0;
  if ((src->type == CYAML_LIST || src->type == CYAML_DICTIONARY) &&
      max_levels == 0) {
    *too_deep = true;
    return NULL;
  }
  cyaml root = clone_shallow(src);
  if (!root) return NULL;
  if (src->type != CYAML_LIST && src->type != CYAML_DICTIONARY) return root;

  _clone_frame_t inline_frames[_CYAML_WALK_INLINE_FRAMES];
  _clone_frame_t *stack = inline_frames;
  size_t cap = _CYAML_WALK_INLINE_FRAMES;
  clone_frame_open(&stack[0], src, (cyaml_node_t *)root);
  size_t sp = 1;
  size_t max_sp = 1;
  bool failed = false;

  while (!failed && sp > 0) {
    _clone_frame_t *f = &stack[sp - 1];
    cyaml_node_t *child;
    const cmap_pair *key = NULL;

    if (f->src->type == CYAML_LIST) {
      if (f->idx >= f->count) {
        sp--;
        continue;
      }
      child = *(cyaml_node_t **)cvector_at(f->src->value.list, f->idx);
      f->idx++;
    } else {
      if (!f->next) {
        sp--;
        continue;
      }
      /* key addresses the storage the source dictionary keeps for the entry.
       * The clone never changes the source, so key stays valid. */
      const cmap_pair *vp;
      f->next = ccol_chmap_entry_read(f->next, &key, &vp);
      child = _cyaml_read_child(vp->ptr);
    }

    /* The sp open frames are the containers above this child, so a child
     * that is a container itself is container number sp + 1 on its path. A
     * scalar takes no level. */
    if (sp >= max_levels &&
        (child->type == CYAML_LIST || child->type == CYAML_DICTIONARY)) {
      *too_deep = true;
      failed = true;
      break;
    }

    cyaml_node_t *cc = (cyaml_node_t *)clone_shallow(child);
    if (!cc) {
      failed = true;
      break;
    }
    /* The child goes straight into f->dst's backing container instead of
     * through cyaml_list_push() or cyaml_dictionary_set(), because every
     * question those two ask is already answered here: f->dst is a container
     * of the right kind that this call has just built, cc is a new copy that
     * no parent owns and can't be f->dst, and a key read out of the source
     * dictionary is unique inside it, so there's never an old value to free.
     * One thing is still open and we keep it: the DOM byte charge of a new
     * dictionary entry, since an alias expansion clones whole mappings and
     * CYAML_MAX_PARSE_BYTES is what bounds that. */
    if (key) {
      if (!parse_bytes_charge(_CYAML_BYTES_PER_DICT_ENTRY + key->size)) {
        __cyaml_destroy((cyaml)cc);
        failed = true;
        break;
      }
      cmap_pair vp = {.ptr = &cc, .size = sizeof(cc)};
      if (chmap_insert_elem(f->dst->value.dictionary, key, &vp) !=
          ccol_success) {
        __cyaml_destroy((cyaml)cc);
        failed = true;
        break;
      }
    } else if (cvector_push_back(f->dst->value.list, &cc) != ccol_success) {
      __cyaml_destroy((cyaml)cc);
      failed = true;
      break;
    }
    /* The clone owns this node from now on. Mark it so that a later attach of
     * it through a borrowed reference is refused; without the mark that
     * attach would give the node a second owner. */
    cc->attached = true;

    if (child->type == CYAML_LIST || child->type == CYAML_DICTIONARY) {
      /* Don't use f after this point, since growing the stack can move it. */
      if (sp == cap) {
        _clone_frame_t *grown =
            walk_stack_grow(stack, &cap, inline_frames, sizeof(*stack), mp);
        if (!grown) {
          failed = true;
          break;
        }
        stack = grown;
      }
      clone_frame_open(&stack[sp], child, cc);
      sp++;
      if (sp > max_sp) max_sp = sp;
    }
  }

  if (stack != inline_frames) _ccol_mem_free(mp, stack);
  if (failed) {
    __cyaml_destroy(root);
    return NULL;
  }
  *height = max_sp;
  return root;
}

/* Makes an independent deep copy of the subtree rooted at node. The clone
 * takes the allocator and the tag of the source node, and each of its
 * dictionaries keeps the member order of its source. On an OOM, or for a
 * source that nests more than CYAML_MAX_NESTING_DEPTH containers deep, the
 * partly built clone is destroyed and NULL comes back. */
cyaml cyaml_clone(cyaml node) {
  if (!node) return NULL;
  bool too_deep;
  size_t height;
  return clone_node((cyaml_node_t *)node, CYAML_MAX_NESTING_DEPTH, &too_deep,
                    &height);
}

/* ========================================================================== */
/*                         PARSER INTERNALS                                   */
/* ========================================================================== */

/*
 * One change that a speculative parse made to the anchor table. name is an
 * owned copy of the anchor name. old is the node the name pointed at before
 * the change, or NULL when the change added the name.
 */
typedef struct {
  char *name;
  cyaml_node_t *old;
} _anchor_undo_entry_t;

/*
 * The changes a speculative parse made to the anchor table, in order (see
 * anchor_speculation_begin()). A small inline array holds the entries of a
 * typical flow collection, so a speculation that stores few anchors
 * allocates nothing for its log.
 */
#define _ANCHOR_UNDO_INLINE 4
typedef struct {
  _anchor_undo_entry_t *entries;
  size_t len;
  size_t cap;
  _anchor_undo_entry_t inline_entries[_ANCHOR_UNDO_INLINE];
} _anchor_undo_t;

/*
 * The parse context.
 *
 * anchors: a lazily created chmap (char* -> cyaml_node_t*) that points at
 * each anchored node without owning it. It stays NULL until the first & is
 * found. At the end of each document the map and the kept list are freed
 * (see anchors_destroy()).
 *
 * tag_handles: a lazily created chmap (char* -> char*) that maps a %TAG
 * handle ("!", "!!", or "!name!") to its resolved prefix. It lives exactly
 * as long as anchors: it belongs to one document, and the end of
 * parse_one_document destroys it and sets it back to NULL. Unlike anchors,
 * the table itself starts out truly empty even once it is created, because
 * tag_handles_lookup falls back to the two default YAML 1.2 handles itself
 * when the table has no explicit entry, so shorthand resolution always has a
 * base case, even in a document with no %TAG directive. The doc comment of
 * tag_handles_ensure explains why this matters for detecting a duplicate
 * %TAG directive.
 *
 * indent_stack: not needed, because the recursive descent passes the
 * indentation as an integer argument.
 *
 * depth: the number of containers around the node being read. Each
 * collection frame holds one level while it is on the frame stack, and so
 * does the one-entry mapping that a "key: value" pair inside a flow sequence
 * makes while its value is read (see _pframe_open and flow_pair_level_take).
 * Scalars, tags and anchors hold no level, so this count is exactly the
 * container depth that cyaml_clone() and the serializers measure, and
 * CYAML_MAX_NESTING_DEPTH bounds it the same way for all of them. Each
 * document of a stream after the first starts at 1 instead of 0, because the
 * stream's list encloses it (see parse_common).
 *
 * line_starts, line_starts_len, line_starts_cap, line_scan_pos and
 * line_cache_disabled back the amortized line-boundary cache of
 * line_start_pos() (see the doc comment of that function). line_starts holds
 * the byte offset of every line start found so far in increasing order, and
 * line_starts[0] is first_line_start, which is 0 or the offset past a
 * byte-order mark. line_scan_pos is the highest ctx->pos the cache covers.
 * parse_common() allocates the cache once, before any parse function runs,
 * and a failure there is a fatal parse failure and not a degraded fallback.
 * line_cache_disabled latches true, for good, only when a LATER growth of
 * the cache fails under an OOM; from then on line_start_pos() falls back to
 * a direct backward scan per call for the rest of the parse.
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
  /* The deepest container depth the running document reached, alias
     expansions included. A stream of several documents wraps every document
     in one more list, so parse_common() refuses a first document that
     reached CYAML_MAX_NESTING_DEPTH once a second document follows. */
  size_t peak_depth;
  /* The last answer line_start_pos() gave, and the position it was asked
     about. Which line a byte offset belongs to is a property of the document
     text, which never changes during a parse, so a repeat question about the
     same offset can always be answered from here, whatever the line cache has
     scanned in the meantime. The parser asks the same question several times
     per position (indentation comparisons, tab checks, and the speculative
     paths that ask again after a rewind), and that's what makes one
     remembered answer worth its two fields. memo_pos starts at SIZE_MAX, an
     offset no document can reach, so the first call can't hit a stale
     entry. */
  size_t memo_pos;
  size_t memo_line_start;
  /* Where the content of this stream begins: 0, or the offset past a
     byte-order mark. The first line starts here and not at offset 0, and both
     implementations of "where does the line holding this byte begin" have to
     agree on that. The cached one does by construction, because line_starts[0]
     starts with this value, while the backward scan needs it as a floor.
     Without the floor the scan walks past the mark and reports a line start
     three bytes early, so every column on the first line is three too large,
     which is enough to break the block structure the columns decide and not
     just a wrongly reported position. */
  size_t first_line_start;
  size_t *line_starts;
  size_t line_starts_len;
  size_t line_starts_cap;
  size_t line_scan_pos;
  bool line_cache_disabled;
  /* The undo log of the running speculative parse, or NULL when none is
     running. While it is set, anchors_store() records every change to the
     anchor table here, along with the node the change replaced. */
  _anchor_undo_t *anchor_undo;
  /* The nodes that the anchor table points at but no tree holds any more
     (see parse_discard()), owned here until anchors_destroy(). Every
     anchors_store() reserves one slot, so kept_cap >= kept_reserved, and a
     node goes into the list without an allocation that could fail. */
  cyaml_node_t **kept;
  size_t kept_len;
  size_t kept_cap;
  size_t kept_reserved;
} parse_ctx_t;

/* The parse running on this thread while it has an anchor table, or NULL.
 * parse_discard() reads it, because dict_slot_replace_child(), which the
 * public API shares with the parser, has no parse_ctx_t of its own. */
static __thread parse_ctx_t *_parse_discard_ctx;

/* Hands n to ctx's kept list, in the slot that anchors_store() reserved. */
static void parse_keep(parse_ctx_t *ctx, cyaml_node_t *n) {
  _ccol_assert(ctx->kept_len < ctx->kept_cap);
  ctx->kept[ctx->kept_len++] = n;
}

/*
 * Frees the subtree n that a running parse drops, but keeps every anchored
 * node in it, since the anchor table points at those nodes and a later alias
 * copies them. Each anchored node goes whole into the parse's kept list, and
 * anchors_destroy() frees it together with the table. Every other node of
 * the subtree is freed immediately, so the walk costs what a destroy costs.
 * Outside a parse, and while the parse has no anchor table, this is just
 * __cyaml_destroy().
 */
static void parse_discard(cyaml_node_t *n) {
  if (!n) return;
  parse_ctx_t *ctx = _parse_discard_ctx;
  if (!ctx) {
    __cyaml_destroy((cyaml)n);
    return;
  }
  destroy_worklist_t wl = {NULL, 0, 0, true};
  cyaml_node_t *node = n;
  for (;;) {
    if (node->anchored) {
      parse_keep(ctx, node);
    } else {
      node_clear_value(node, &wl);
      _ccol_mem_free(node->m_procs, node->tag);
      node->tag = NULL;
      node_free(node);
    }
    if (wl.len == 0) break;
    node = wl.items[--wl.len];
  }
  _ccol_default_mem_free(wl.items);
}

/*
 * The largest number of frames that one parse's frame stack may hold. Each
 * container the parse builds holds one collection frame, and the node that
 * turns out to be that container holds at most three node frames on top of
 * its parent's frame: the node itself, the node its anchor decorates and the
 * node its tag decorates. A scalar holds the same node frames and no
 * collection frame. So CYAML_MAX_NESTING_DEPTH bounds the stack at
 * 4 * (CYAML_MAX_NESTING_DEPTH + 1) frames. The driver only checks this bound
 * when it grows the stack and never grows it past the bound, so a construct
 * that pushes node frames without a container can't grow it past the bound
 * either.
 */
#define _CYAML_PARSE_MAX_FRAMES (4 * (CYAML_MAX_NESTING_DEPTH + 1))

/*
 * The largest length in bytes of the canonical flow-YAML text of a non-scalar
 * dictionary key (see the doc comment of node_to_dict_key_string).
 * CYAML_MAX_NESTING_DEPTH doesn't cover the reason for this limit. A
 * non-scalar key can be a mapping whose single key is another such mapping,
 * nested N levels deep, and each enclosing level puts double quotes again
 * around the text that the level below made. So the length of the canonical
 * string grows as O(2^N) in the nesting depth, about 2^N bytes at depth N,
 * where one would expect O(N). The depth cap alone can't bound that, because
 * real deep nesting needs hundreds of levels, so a generous depth value must
 * allow them, and that is far past the 30 or so levels where this pattern
 * already produces gigabyte strings. A document that deliberately stays under
 * the depth cap could then use all the memory and run for an unbounded time.
 * This check applies to the canonical text itself instead of the nesting
 * depth, so it catches exactly that pattern, and nothing else, at its real
 * source, whatever CYAML_MAX_NESTING_DEPTH is. The builder of the canonical
 * text stops at the first byte past this length, so a refused key costs at
 * most this many bytes of text whatever its shape and however many aliases
 * it expands. 64 KiB is far above the canonical text of any real non-scalar
 * key in ordinary use, while it stops the O(2^N) pattern at a shallow depth
 * that is cheap to refuse; depth 20 is already past it.
 */
#define CYAML_MAX_CANONICAL_KEY_LEN (64 * 1024)

/*
 * The largest number of cyaml_node_t allocations one top-level parse_common()
 * call may make. Past that, node_alloc() refuses every further allocation and
 * returns NULL just as for a real allocator OOM. Neither
 * CYAML_MAX_NESTING_DEPTH nor CYAML_MAX_CANONICAL_KEY_LEN covers the reason
 * for this limit. cyaml_clone() makes a real, unshared deep copy, and the
 * parser uses it to make an independent copy of the anchored subtree at
 * every *alias reference (registering the anchor itself copies nothing),
 * and again to expand a merge key. A document that nests aliases of aliases
 * can therefore make the final count of live nodes grow exponentially with
 * the number of anchor levels, while the source text and the parser's
 * recursion depth both stay small; that's the classic "billion laughs" shape
 * of entity expansion. CYAML_MAX_NESTING_DEPTH bounds the nesting depth but
 * not the fan-out of clones and siblings, and this limit bounds how many node
 * structs one parse can create. Any real large document fits, whether a
 * person wrote it or a program generated it, since it would need millions of
 * distinct scalar values to get close. Being a count, this limit says nothing
 * about how many bytes each node brings along; CYAML_MAX_PARSE_BYTES below
 * bounds that, and the two limits are enforced together.
 */
#define CYAML_MAX_PARSE_NODES (4 * 1000 * 1000)

/*
 * The node allocations one parse may make grow with the input: at least
 * CYAML_MAX_PARSE_NODES, and at least this many per byte of input. The factor
 * is the next power of two at or above 1.5 times the most that any document
 * without aliases, merge keys or nested non-scalar keys allocates per byte of
 * its text. That worst case, measured by this budget, is a flow sequence of
 * empty pairs ("[:,:,:]"): a mapping, its null key and its null value for
 * every two bytes, so 1.5 nodes per byte. The shapes left out amplify their
 * input on purpose: an anchor and an alias copy a subtree, a merge key copies
 * the members of its source, and the canonical text of a nested non-scalar
 * key doubles at each level. Every node and byte those copies make is also
 * charged to the amplification budget (see parse_amp_enter()), which keeps
 * the fixed limit CYAML_MAX_PARSE_NODES whatever the input length. So a long
 * input raises the limit for the nodes its text describes, and never the
 * limit for its expansion.
 */
#define CYAML_PARSE_NODES_PER_INPUT_BYTE 4

/*
 * The largest total DOM memory in bytes that one top-level parse_common()
 * call may charge. Past that, every further node, string, tag and dictionary
 * entry is refused just as for a real allocator OOM. The limit exists
 * because CYAML_MAX_PARSE_NODES counts nodes, and a node isn't a fixed amount
 * of memory: a scalar owns its text, a tagged node owns its tag, a dictionary
 * entry owns a copy of its key, and a container node owns a backing cvec or
 * chmap several times the size of the node struct. Alias expansion deep
 * copies all of it, so the node count alone bounds one kind of amplification
 * but not amplification in BYTES. A few kilobytes of input can hold one big
 * anchored scalar, and an alias of it through two nested levels then gets to
 * gigabytes of live DOM with a node count far below the limit above. The byte
 * budget is charged at every DOM allocation site (see the callers of
 * parse_bytes_charge()), so both halves of that shape are bounded.
 *
 * 256 MiB is the floor of the limit. The limit grows with the input, at
 * CYAML_PARSE_BYTES_PER_INPUT_BYTE bytes per input byte, so a large honest
 * document parses whatever its size. The bytes that anchors, aliases, merge
 * keys and collection keys copy are also charged to the amplification
 * budget, which stays at 256 MiB whatever the input length, so padding an
 * input doesn't raise what its expansion may use. Like the node limit it only
 * applies to a parse, never to a tree a caller builds directly through the
 * public API.
 */
#define CYAML_MAX_PARSE_BYTES ((size_t)256 * 1024 * 1024)

/*
 * The DOM bytes one parse may charge grow with the input: at least
 * CYAML_MAX_PARSE_BYTES, and at least this many bytes per byte of input. The
 * factor comes about the same way as CYAML_PARSE_NODES_PER_INPUT_BYTE: the
 * same "[:,:,:]" shape charges about 331 bytes per input byte, the most of
 * any document without aliases, merge keys or nested non-scalar keys, and
 * 1.5 times that rounds up to 512. Ordinary documents cost far less; a large
 * Kubernetes List of generated manifests costs about 20 bytes per input
 * byte. A fixed limit alone would refuse such an honest document once it
 * passes about 13 MB. Expansion through copies hits the fixed amplification
 * budget instead, whatever the input length.
 */
#define CYAML_PARSE_BYTES_PER_INPUT_BYTE 512

/* Arm and disarm both parse budgets, _parse_node_budget and
 * _parse_byte_budget, for the parse_common() call running on this thread.
 * Both are declared in the THREAD-LOCAL NODE POOL section above, next to
 * node_alloc. They are always armed and disarmed together, because they bound
 * two halves of one property and neither is enough alone. A disarm sets both
 * back to "unlimited" instead of leaving whatever remains, so a value that a
 * parse decremented can never leak into unrelated node creation on this
 * thread after that parse returns. */
/* The two limits of the parse running on this thread, as
 * parse_node_budget_arm() computed them. The parse diagnostics name them. */
static __thread size_t _parse_node_limit = CYAML_MAX_PARSE_NODES;
static __thread size_t _parse_byte_limit = CYAML_MAX_PARSE_BYTES;

/* Gives max(floor, per_byte * input_len), saturated below (size_t)-1, which
 * is the "unarmed" sentinel of both budgets. The saturation matters on ILP32,
 * where 512 times an input of 8 MiB or more doesn't fit a size_t. */
static inline size_t parse_budget_for_input(size_t floor, size_t per_byte,
                                            size_t input_len) {
  size_t cap = (size_t)-1 - 1;
  size_t scaled = input_len > cap / per_byte ? cap : input_len * per_byte;
  return scaled > floor ? scaled : floor;
}

#ifdef RUNNING_UNIT_TESTS
/* The floors of the two limits. A test lowers them so a document of a few
 * kilobytes can show how the limits scale with the input, and puts them back
 * before it returns. The test suite declares both extern. */
size_t cyaml_test_parse_node_floor = CYAML_MAX_PARSE_NODES;
size_t cyaml_test_parse_byte_floor = CYAML_MAX_PARSE_BYTES;
#define _CYAML_PARSE_NODE_FLOOR cyaml_test_parse_node_floor
#define _CYAML_PARSE_BYTE_FLOOR cyaml_test_parse_byte_floor

size_t cyaml_debug_parse_budget_for_input(size_t floor, size_t per_byte,
                                          size_t input_len) {
  return parse_budget_for_input(floor, per_byte, input_len);
}
#else
#define _CYAML_PARSE_NODE_FLOOR ((size_t)CYAML_MAX_PARSE_NODES)
#define _CYAML_PARSE_BYTE_FLOOR CYAML_MAX_PARSE_BYTES
#endif

/*
 * The amplification budget of the parse running on this thread. Every node
 * and DOM byte that the parser makes by COPYING something it has already
 * built is charged here as well as to the ordinary budgets above: the copy
 * that resolves an alias, the copy that a merge key takes of each merged
 * value, and the canonical text of a key that is a collection (registering
 * an anchor copies nothing, so it charges nothing). Those are the only ways
 * a parse can make more DOM than its input describes, so they are the only
 * charges that must not grow with the input. The ordinary budgets scale
 * with the input length so a large honest document parses, while the
 * amplification budget keeps the fixed floors
 * CYAML_MAX_PARSE_NODES and CYAML_MAX_PARSE_BYTES whatever the input length.
 * So padding a nested-alias document with a long comment buys it nothing,
 * and its expansion hits the same fixed limit as the unpadded document. A
 * limit derived from the input length is a limit the sender controls, which
 * is why this one has to stay fixed.
 *
 * node_alloc() and parse_bytes_charge() know nothing about this budget, so
 * the ordinary allocation path pays nothing for it. parse_amp_enter()
 * narrows the two ordinary budgets to the smaller of what is left of each
 * and of the amplification budget, and parse_amp_leave() charges what the
 * copy used to both. Regions can nest; only the outermost one narrows and
 * settles.
 */
static __thread size_t _parse_amp_node_budget;
static __thread size_t _parse_amp_byte_budget;
static __thread size_t _parse_amp_saved_node_budget;
static __thread size_t _parse_amp_saved_byte_budget;
static __thread size_t _parse_amp_start_node_budget;
static __thread size_t _parse_amp_start_byte_budget;
static __thread unsigned _parse_amp_depth;

/* Set by parse_amp_leave() when a region ended with a refused charge and the
 * amplification budget was the binding limit, alone or tied with the
 * ordinary one, so the diagnostics at the copy sites name the amplification
 * limit. It stays set until the next arm or disarm, since every refusal fails
 * the parse. */
static __thread bool _parse_amp_limit_hit;

static inline void parse_amp_enter(void) {
  if (_parse_node_budget == (size_t)-1) return;
  if (_parse_amp_depth++ != 0) return;
  _parse_amp_saved_node_budget = _parse_node_budget;
  _parse_amp_saved_byte_budget = _parse_byte_budget;
  if (_parse_amp_node_budget < _parse_node_budget)
    _parse_node_budget = _parse_amp_node_budget;
  if (_parse_amp_byte_budget < _parse_byte_budget)
    _parse_byte_budget = _parse_amp_byte_budget;
  _parse_amp_start_node_budget = _parse_node_budget;
  _parse_amp_start_byte_budget = _parse_byte_budget;
}

static inline void parse_amp_leave(void) {
  if (_parse_node_budget == (size_t)-1) return;
  if (--_parse_amp_depth != 0) return;
  /* A refund inside the region can only give back what the region charged,
   * so the remaining budget never goes above the region's start. */
  size_t used_nodes = _parse_amp_start_node_budget - _parse_node_budget;
  size_t used_bytes = _parse_amp_start_byte_budget - _parse_byte_budget;
  bool amp_binding_nodes =
      _parse_amp_node_budget <= _parse_amp_saved_node_budget;
  bool amp_binding_bytes =
      _parse_amp_byte_budget <= _parse_amp_saved_byte_budget;
  _parse_amp_node_budget -= used_nodes;
  _parse_amp_byte_budget -= used_bytes;
  _parse_node_budget = _parse_amp_saved_node_budget - used_nodes;
  _parse_byte_budget = _parse_amp_saved_byte_budget - used_bytes;
  if ((_parse_node_budget_exhausted && amp_binding_nodes) ||
      (_parse_byte_budget_exhausted && amp_binding_bytes))
    _parse_amp_limit_hit = true;
}

/* clone_node() inside an amplification region. Every copy the parser makes of
 * a node it has already built (an alias expansion) goes through here, passing
 * the levels left below the place where the copy lands. */
static cyaml_node_t *parse_clone_levels(cyaml_node_t *src, size_t max_levels,
                                        bool *too_deep, size_t *height) {
  parse_amp_enter();
  cyaml_node_t *copy =
      (cyaml_node_t *)clone_node(src, max_levels, too_deep, height);
  parse_amp_leave();
  return copy;
}

static inline void parse_node_budget_arm(size_t input_len) {
  _parse_node_limit = parse_budget_for_input(
      _CYAML_PARSE_NODE_FLOOR, CYAML_PARSE_NODES_PER_INPUT_BYTE, input_len);
  _parse_byte_limit = parse_budget_for_input(
      _CYAML_PARSE_BYTE_FLOOR, CYAML_PARSE_BYTES_PER_INPUT_BYTE, input_len);
  _parse_node_budget = _parse_node_limit;
  _parse_node_budget_exhausted = false;
  _parse_byte_budget = _parse_byte_limit;
  _parse_byte_budget_exhausted = false;
  _parse_amp_node_budget = _CYAML_PARSE_NODE_FLOOR;
  _parse_amp_byte_budget = _CYAML_PARSE_BYTE_FLOOR;
  _parse_amp_depth = 0;
  _parse_amp_limit_hit = false;
}
#ifdef RUNNING_UNIT_TESTS
/* What the last parse on this thread charged against its two budgets, as
 * parse_node_budget_disarm() found them. A test divides these by the input
 * length to check the per-byte factors against the worst honest shapes. */
__thread size_t cyaml_test_last_parse_nodes_charged;
__thread size_t cyaml_test_last_parse_bytes_charged;
/* The largest capacity a parse's frame stack reached on this thread. A test
 * resets it and checks it against _CYAML_PARSE_MAX_FRAMES. */
__thread size_t cyaml_test_peak_parse_frame_cap;
#endif

static inline void parse_node_budget_disarm(void) {
#ifdef RUNNING_UNIT_TESTS
  if (_parse_node_budget != (size_t)-1) {
    cyaml_test_last_parse_nodes_charged =
        _parse_node_limit - _parse_node_budget;
    cyaml_test_last_parse_bytes_charged =
        _parse_byte_limit - _parse_byte_budget;
  }
#endif
  /* The end of each document already clears this, but a parse that leaves by
   * any path clears it again, so no later call on this thread can reach the
   * stack frame of a parse that has returned. */
  _parse_discard_ctx = NULL;
  _parse_node_budget = (size_t)-1;
  _parse_node_budget_exhausted = false;
  _parse_byte_budget = (size_t)-1;
  _parse_byte_budget_exhausted = false;
  _parse_amp_depth = 0;
  _parse_amp_limit_hit = false;
}

/* Formats a parse error into ctx->error. Only the last call counts, since an
 * earlier message is silently overwritten.
 *
 * The __attribute__((format(printf, 2, 3))) below is what keeps
 * -Wformat-nonliteral quiet here without suppressing it. The warning fires
 * whenever a printf-family function (vsnprintf here) gets a format string
 * that isn't a literal at the call site, because then the compiler can't
 * check that string against the varargs. fmt is such a string, being a
 * parameter of this function. GCC and Clang both special-case the
 * "pass-through wrapper" pattern, though: when the enclosing function
 * carries the printf-like attribute for the same parameter, the compiler
 * accepts the inner call as correct by construction and moves the real check
 * to every call site of parse_err() itself, where fmt IS a literal. That's
 * better than a suppression because it adds a real compile-time check of the
 * format string and the argument types, under -Werror, to every parse_err()
 * call in this file, and that catches a real class of bug (a "%d" for a
 * size_t, for example), while a suppression catches nothing. */
static void parse_err(parse_ctx_t *ctx, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void parse_err(parse_ctx_t *ctx, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
  va_end(ap);
}

/* Reports a container that would go past CYAML_MAX_NESTING_DEPTH. Every
 * depth-limit refusal names the same limit with the same words. */
static void parse_depth_err(parse_ctx_t *ctx) {
  parse_err(ctx, "maximum nesting depth (%d) exceeded at position %zu",
            CYAML_MAX_NESTING_DEPTH, ctx->pos);
}

/* A "key: value" pair inside a flow sequence becomes a one-entry mapping one
 * level below the sequence, so it takes a nesting level of its own, just as
 * a collection frame does. The flow-list frame holds that level while it
 * reads the pair's value, and the key text the frame holds meanwhile marks
 * it for the cleanup sweep. Returns false, with the error set, when the
 * mapping would go past the limit. */
static inline bool flow_pair_level_take(parse_ctx_t *ctx) {
  if (ctx->depth >= CYAML_MAX_NESTING_DEPTH) {
    parse_depth_err(ctx);
    return false;
  }
  if (++ctx->depth > ctx->peak_depth) ctx->peak_depth = ctx->depth;
  return true;
}

/*
 * The largest number of nested containers on any path of the tree n, its
 * root counted when it is a container, and 0 for a scalar. A tree a parse
 * builds nests at most CYAML_MAX_NESTING_DEPTH containers, so a fixed stack
 * of that many frames is enough for the walk and it allocates nothing. A
 * deeper tree gives CYAML_MAX_NESTING_DEPTH + 1.
 */
static size_t parse_tree_height(cyaml_node_t *n) {
  if (n->type != CYAML_LIST && n->type != CYAML_DICTIONARY) return 0;
  struct {
    cyaml_node_t *n;
    size_t idx;
    const ccol_chmap_entry_ref *next;
  } stack[CYAML_MAX_NESTING_DEPTH];
  size_t sp = 0, height = 0;
  stack[sp].n = n;
  stack[sp].idx = 0;
  stack[sp].next = n->type == CYAML_DICTIONARY ? dict_first_entry(n) : NULL;
  sp++;
  height = 1;
  while (sp > 0) {
    cyaml_node_t *child;
    if (stack[sp - 1].n->type == CYAML_LIST) {
      cvec list = stack[sp - 1].n->value.list;
      if (stack[sp - 1].idx >= cvector_elem_count(list)) {
        sp--;
        continue;
      }
      child = *(cyaml_node_t **)cvector_at(list, stack[sp - 1].idx++);
    } else {
      if (!stack[sp - 1].next) {
        sp--;
        continue;
      }
      const cmap_pair *kp, *vp;
      stack[sp - 1].next = ccol_chmap_entry_read(stack[sp - 1].next, &kp, &vp);
      child = _cyaml_read_child(vp->ptr);
    }
    if (child->type != CYAML_LIST && child->type != CYAML_DICTIONARY) continue;
    if (sp == CYAML_MAX_NESTING_DEPTH) return CYAML_MAX_NESTING_DEPTH + 1;
    stack[sp].n = child;
    stack[sp].idx = 0;
    stack[sp].next =
        child->type == CYAML_DICTIONARY ? dict_first_entry(child) : NULL;
    sp++;
    if (sp > height) height = sp;
  }
  return height;
}

/*
 * A collection that becomes a dictionary key sits below the mapping that
 * holds the key, so its containers count from that mapping's level, whatever
 * form the key takes. An implicit block mapping key and the key of a
 * "key: value" pair in a flow sequence are both read before their mapping
 * exists, at the depth of the mapping's parent, so once the parser knows the
 * node is a key this check adds the mapping's level to the key's height.
 * Returns false, with the error set, when the key would reach past
 * CYAML_MAX_NESTING_DEPTH.
 *
 * The deepest container of the key stands at ctx->depth plus its height,
 * which ctx->peak_depth already covers, so the mapping's level can only take
 * the key to the limit or past it when the document has come within one
 * level of the limit, and only then do we walk the key to measure its height
 * (and raise ctx->peak_depth to the new deepest level).
 */
static bool parse_key_fits_below_mapping(parse_ctx_t *ctx, cyaml_node_t *key) {
  if (ctx->peak_depth + 1 < CYAML_MAX_NESTING_DEPTH) return true;
  size_t top = ctx->depth + 1 + parse_tree_height(key);
  if (top > CYAML_MAX_NESTING_DEPTH) {
    parse_depth_err(ctx);
    return false;
  }
  if (top > ctx->peak_depth) ctx->peak_depth = top;
  return true;
}

/*
 * Every byte of input text that a parse error message quotes goes through
 * cyaml_err_echo(), so every message is printable ASCII whatever the input
 * holds. Otherwise a raw control byte (an ESC that starts a terminal
 * sequence, a line break that forges a second log line, a NUL) would reach
 * the caller's logs, and a byte of 0x80 or above would make the message
 * invalid UTF-8, which a JSON or syslog sink refuses or mangles. Decoded
 * scalar text and decoded tags can hold any of those, and an anchor name or
 * a single character can be part of a multi-byte sequence.
 *
 * The echo writes a byte from 0x20 to 0x7E as it is and any other byte as
 * <0xNN>. It holds at most CYAML_ERR_ECHO_BUF - 1 bytes; a longer text is cut
 * at a whole byte, never inside a <0xNN>, and ends with "...". The three
 * helper macros give the echo its own buffer, a compound literal that lives
 * until the end of the enclosing block, so a call site can write
 * parse_err(ctx, "... '%s' ...", _cyaml_echo(name)).
 */
#define CYAML_ERR_ECHO_BUF 72

static const char *cyaml_err_echo(const char *s, size_t n, char *out) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    bool plain = c >= 0x20 && c <= 0x7E;
    size_t need = plain ? 1 : 6;
    /* Keep room for the "..." that a later cut would need. */
    size_t reserve = i + 1 < n ? 3 : 0;
    if (o + need + reserve > CYAML_ERR_ECHO_BUF - 1) {
      memcpy(out + o, "...", 3);
      o += 3;
      break;
    }
    if (plain) {
      out[o++] = (char)c;
    } else {
      out[o++] = '<';
      out[o++] = '0';
      out[o++] = 'x';
      out[o++] = hex[c >> 4];
      out[o++] = hex[c & 0x0F];
      out[o++] = '>';
    }
  }
  out[o] = '\0';
  return out;
}

#define _cyaml_echo_n(s, n) \
  cyaml_err_echo((s), (n), (char[CYAML_ERR_ECHO_BUF]){0})
#define _cyaml_echo(s) \
  _cyaml_echo_n(((s) ? (s) : ""), ((s) ? strlen(s) : (size_t)0))
#define _cyaml_echo_c(c) _cyaml_echo_n(&(const char){(char)(c)}, 1)

/* Position helpers */

/* at_end: true once the parser has read all of the input.
 * cur:    the current character without moving forward, or '\0' at the end
 *         of the input. */
static inline bool at_end(parse_ctx_t *ctx) { return ctx->pos >= ctx->len; }

static inline char cur(parse_ctx_t *ctx) {
  return at_end(ctx) ? '\0' : ctx->src[ctx->pos];
}

#ifdef RUNNING_UNIT_TESTS
/* Forces the degraded, cache-less line-start path, the same path that a
   failed growth of line_starts latches. Allocation pressure alone can't reach
   the one class of position where the two implementations could disagree:
   the latch needs at least 64 line boundaries first, and by then no query
   lands on the first line, which is the only line a byte-order mark moves.
   The test suite declares this variable extern and the public header
   doesn't, the same way the other modules reach their own test-only
   accessors. */
bool cyaml_test_force_line_cache_disabled = false;
#endif

/* The fallback implementation of line_start_pos(), for the degraded mode. It
 * scans backward from ctx->pos one byte at a time, down to
 * ctx->first_line_start, which costs O(column) per call; the cache of
 * line_start_pos() exists to avoid that cost. Only the degraded mode calls
 * it: a cache that has been turned off, and the call in which a growth of
 * the cache fails and turns it off. */
static size_t line_start_pos_backward_scan(parse_ctx_t *ctx) {
  size_t p = ctx->pos;
  while (p > ctx->first_line_start && ctx->src[p - 1] != '\n' &&
         ctx->src[p - 1] != '\r')
    p--;
  return p;
}

/* Extends ctx->line_starts to cover every line boundary up to, but not
 * including, the byte offset up_to, appending one entry for each '\n' or
 * '\r' it crosses, the same line-boundary rule as
 * line_start_pos_backward_scan(). The caller always passes an up_to greater
 * than ctx->line_scan_pos. On an allocation failure the cache is torn down
 * and ctx->line_cache_disabled latches true for good, so the caller,
 * line_start_pos(), falls back to a direct scan for this call and every
 * later one instead of retrying the same doomed allocation each time. */
static void line_starts_extend(parse_ctx_t *ctx, size_t up_to) {
  size_t p = ctx->line_scan_pos;
  while (p < up_to) {
    char c = ctx->src[p];
    p++;
    if (c == '\n' || c == '\r') {
      if (ctx->line_starts_len == ctx->line_starts_cap) {
        size_t new_cap = ctx->line_starts_cap ? ctx->line_starts_cap * 2 : 64;
        size_t *grown = _ccol_mem_realloc(ctx->mp, ctx->line_starts,
                                          new_cap * sizeof(size_t));
        if (!grown) {
          _ccol_mem_free(ctx->mp, ctx->line_starts);
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

static size_t line_start_pos_uncached(parse_ctx_t *ctx);

/* Gives the byte offset where the physical line holding ctx->pos starts: the
 * position right after the nearest '\n' or '\r' before ctx->pos, or
 * ctx->first_line_start (0, or the offset past a byte order mark) when no
 * such byte comes before ctx->pos. current_col() and line_indent_has_tab()
 * share it because both need exactly this "where did the current line
 * begin" answer. parse_node_inner() calls it once per parsed node, and lots
 * of other places call it too. A lone '\r' counts as a line boundary as well
 * as '\n', like in skip_to_eol(), at_eol() and skip_newline() elsewhere in
 * this file, since the parser never normalizes CR and LF before parsing. For
 * a "\r\n" ending the scan always reaches the '\n' first, because it is the
 * later of the two bytes, so the line correctly starts right after the pair
 * and never right after the '\r' alone.
 *
 * A plain backward scan, one byte at a time, costs O(column) per call. A
 * flow collection drives one such call per sibling, and nothing bounds how
 * many siblings share a line; any other construct that keeps
 * parse_node_inner on one physical line, with no newline crossed, does the
 * same. That alone would make the whole parse O(n^2) in the line length, so
 * a flow list of a few tens of thousands of elements on one line would take
 * tens of seconds. None of CYAML_MAX_NESTING_DEPTH, CYAML_MAX_PARSE_NODES and
 * CYAML_MAX_CANONICAL_KEY_LEN bounds that, because none of them bounds the
 * fan-out of siblings on one line.
 *
 * An amortized cache, ctx->line_starts, keeps that cost linear. It is a
 * sorted, only-growing list of every line-start offset found so far.
 * ctx->pos moves forward almost always: the parser goes back rarely and only
 * by a small, bounded distance, to try the token right before it again
 * (try_parse_scalar_dict_key() is one example). So the common case only
 * extends the cache forward over bytes no scan has seen yet, and the total
 * scan work of the whole parse is O(n), where the plain scan would cost O(n)
 * per call.
 *
 * The last entry answers a query that has moved past everything the cache
 * has seen, because the extend above has just made that entry the right one.
 * A speculative parse can rewind to an earlier position, and a query from
 * there needs a binary search for the greatest recorded line start that is
 * <= ctx->pos. Unlike a scheme that only remembers the most recent line
 * start, the search is correct whether ctx->pos is ahead of or behind the
 * highest point the parse reached, and it stays correct across a backward
 * jump into a region already scanned, without scanning anything again.
 *
 * parse_common() makes the first allocation of the cache itself, once,
 * before any parse function runs, and a failure there is a fatal parse
 * failure reported at once, like every other early setup step of this
 * parser. It's a real, load-bearing allocation that this function has no way
 * around, so its failure has to be reported honestly instead of being hidden
 * by a graceful fallback deep in a hot per-node path with no error channel of
 * its own. Only a LATER growth failure falls back to a per-call direct scan
 * for the rest of the parse. That failure comes from line_starts_extend()
 * once a large document has used up the cache's small first capacity; the
 * parse gets there step by step, deep into an otherwise successful parse,
 * under lasting memory pressure, and there a slower but correct fallback is a
 * better trade than throwing away all the completed work. The first
 * allocation is different, since it's all or nothing and the parse hasn't
 * started yet. */
static size_t line_start_pos(parse_ctx_t *ctx) {
  if (ctx->pos == ctx->memo_pos) {
#ifdef RUNNING_UNIT_TESTS
    /* The memo rests on one property: the line a byte belongs to is a
       property of the document text, and that text doesn't change during a
       parse. That property is all of the memo's correctness and no call site
       shows it, so wherever tests run this check compares the remembered
       answer against a fresh computation. Without it a wrong memo would get
       past every other check in the file, because the fast-path assertion
       below only compares two readings of the same array and a memo hit
       never reaches it. */
    _ccol_assert(ctx->memo_line_start == line_start_pos_uncached(ctx));
#endif
    return ctx->memo_line_start;
  }

  size_t result = line_start_pos_uncached(ctx);
  ctx->memo_pos = ctx->pos;
  ctx->memo_line_start = result;
  return result;
}

static size_t line_start_pos_uncached(parse_ctx_t *ctx) {
  if (ctx->line_cache_disabled) return line_start_pos_backward_scan(ctx);

  if (ctx->pos > ctx->line_scan_pos) {
    line_starts_extend(ctx, ctx->pos);
    if (ctx->line_cache_disabled) return line_start_pos_backward_scan(ctx);
    /* The extend stops exactly at ctx->pos and appends one entry per line
       boundary it crosses, so every entry is at or below ctx->pos. The last
       entry is therefore the greatest one there can be, so it's the answer
       and no search is needed. Nearly every call takes this path, because a
       parse moves forward through the document far more often than it
       rewinds. */
    size_t answer = ctx->line_starts[ctx->line_starts_len - 1];
#ifdef RUNNING_UNIT_TESTS
    /* The invariant above is load-bearing and not obvious, so wherever tests
       run we compare it against the general search. Without the check, a
       future change that let an entry past ctx->pos into the cache would
       silently return a line start from further down the document, which
       shows up as a wrong indentation level far from its cause. */
    {
      size_t vlo = 0, vhi = ctx->line_starts_len;
      while (vlo + 1 < vhi) {
        size_t vmid = vlo + (vhi - vlo) / 2;
        if (ctx->line_starts[vmid] <= ctx->pos)
          vlo = vmid;
        else
          vhi = vmid;
      }
      _ccol_assert(answer == ctx->line_starts[vlo]);
    }
#endif
    return answer;
  }

  /* ctx->pos is at or behind the point the cache has already scanned, the
     state a rewinding speculative parse leaves behind. The cache then holds
     entries past ctx->pos, which this path must exclude, so it searches. */
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

/* The 0-based column of ctx->pos on its line, i.e. the distance from the
 * line start that line_start_pos() gives. The parser uses it to find the
 * indentation levels of block collections. */
_CYAML_PARSE_HOT int current_col(parse_ctx_t *ctx) {
  if (ctx->pos == 0) return 0;
  size_t p = line_start_pos(ctx);
  size_t col = ctx->pos - p;
  /* Every caller only uses this to compare indentation levels, against
   * another int or a small explicit indent indicator, and never as a byte
   * count, so we clamp it at INT_MAX. Without the clamp a line longer than
   * INT_MAX bytes would wrap into an implementation-defined int that can be
   * negative. The clamp keeps all of those comparisons well defined for a
   * physical line of any length. */
  return col > (size_t)INT_MAX ? INT_MAX : (int)col;
}

/* True when there is a tab anywhere between the start of the current line and
 * ctx->pos. YAML 1.2 section 6.1 makes indentation spaces only: a tab may
 * separate a flow node from what comes before it on its line, but the column
 * of a block collection can't be measured across a tab. So the start of every
 * block sequence entry and every block mapping entry refuses a tab anywhere
 * before it on its line, and "\ta: 1", "-\t- a" and "key:\tb: c" all fail.
 *
 * This test is narrower on purpose than "does skip_ws_comments or
 * skip_inline_ws see a tab", because those two shared functions also handle
 * whitespace INSIDE content that is already open, such as the line folding of
 * a quoted scalar, where a tab is real data and not indentation. Merging the
 * two cases gives a real infinite loop, because those callers need the shared
 * skip to always move forward. Instead the parser calls this helper
 * explicitly, and only at the points that really are a block-structural
 * decision about indentation. */
static bool line_indent_has_tab(parse_ctx_t *ctx) {
  size_t p = line_start_pos(ctx);
  return memchr(ctx->src + p, '\t', ctx->pos - p) != NULL;
}

/* The indentation of the current line, for comparing against the indent of
 * an enclosing block construct, when ctx->pos is at the first character after
 * the leading whitespace of its line. YAML 1.2 section 6.1 makes indentation
 * spaces only: s-indent(n) is n spaces, and a tab after it is separation
 * whitespace (s-separate-in-line) and not indentation. When the leading
 * whitespace holds a tab this gives the number of spaces before the first
 * tab. In every other case it gives current_col(), which is also the answer
 * when ctx->pos isn't at the start of its line's content.
 *
 * Only a flow node (a scalar, a flow collection or an alias) may follow a
 * tab. A block collection may not, since its column would be ambiguous, and
 * the start of every block collection refuses a tab before it with
 * line_indent_has_tab() or block_collection_line_has_tab(). */
static int line_space_indent(parse_ctx_t *ctx) {
  size_t p = line_start_pos(ctx);
  size_t spaces = 0;
  bool tab = false;
  for (size_t q = p; q < ctx->pos; q++) {
    char c = ctx->src[q];
    if (c == '\t') {
      tab = true;
    } else if (c != ' ') {
      return current_col(ctx);
    } else if (!tab) {
      spaces++;
    }
  }
  if (!tab) return current_col(ctx);
  return spaces > (size_t)INT_MAX ? INT_MAX : (int)spaces;
}

/* Skips only horizontal whitespace (spaces and tabs) without crossing a
 * newline. We need it because block parsing is indentation-sensitive and the
 * column of the next token means something there.
 *
 * It skips a tab in every case on purpose, because it also handles whitespace
 * inside content that is already open, like the blank-line fold logic of
 * parse_double_quoted, where a tab is real data and not block-structural
 * indentation. A version that always stopped at a tab would give a real
 * infinite loop: the fold loop of parse_double_quoted calls it and needs it
 * to always move forward, and a tab left unread breaks that. Instead, a tab
 * is refused at each call site that really is a block-structural position,
 * with current_col() and line_indent_has_tab() (see that helper's doc
 * comment), and not inside this shared, lower-level function. */
static void skip_inline_ws(parse_ctx_t *ctx) {
  while (!at_end(ctx)) {
    char c = cur(ctx);
    if (c == ' ' || c == '\t')
      ctx->pos++;
    else
      break;
  }
}

/* Drops the rest of the current line along with the newline that ends it. A
 * lone '\r', a lone '\n' and a "\r\n" pair all end the line, as in at_eol()
 * and skip_newline() elsewhere in this file. The parser never normalizes CR
 * and LF before parsing, so a bare CR counts as a line ending here too,
 * not just an LF. */
static void skip_to_eol(parse_ctx_t *ctx) {
  while (!at_end(ctx) && cur(ctx) != '\n' && cur(ctx) != '\r') ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '\r') ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '\n') ctx->pos++;
}

static inline bool at_eol(parse_ctx_t *ctx) {
  return at_end(ctx) || cur(ctx) == '\n' || cur(ctx) == '\r';
}

/* True when the byte span [start, ctx->pos) holds a line break: a '\n', a
 * bare '\r', or a "\r\n" pair. This parser never normalizes CR and LF in its
 * input, so a bare CR has to count as a line break here, exactly as in
 * skip_to_eol(), at_eol() and skip_newline() above; checking for '\n' alone
 * would silently miss a document that only uses bare CR line endings inside
 * the span. Every "did this token span more than one physical line" decision
 * goes through this function: the single-line rule for an implicit key, and
 * the indentation and tab check for a continuation line of a flow
 * collection.
 *
 * It reads no byte of the span, because line_start_pos() already answers the
 * question: the start of the line holding ctx->pos is after `start` exactly
 * when a line break falls inside the span, since that line start is by
 * definition the byte after the last break before ctx->pos. Every caller
 * passes a `start` saved from an earlier ctx->pos, so `start` is never below
 * ctx->first_line_start (the floor both implementations of "where does this
 * line begin" share, past any byte-order mark) and never above ctx->pos. The
 * assertions below check both bounds and the equivalence itself, because all
 * of this function's correctness rests on a property of line_start_pos() that
 * no call site shows. The equivalence must hold on the forward paths and
 * just as much on the speculative paths, which rewind ctx->pos into a region
 * the parser has already scanned. */
static bool span_crosses_newline(parse_ctx_t *ctx, size_t start) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_assert(start >= ctx->first_line_start && start <= ctx->pos);
  _ccol_assert((line_start_pos(ctx) > start) ==
               (memchr(ctx->src + start, '\n', ctx->pos - start) != NULL ||
                memchr(ctx->src + start, '\r', ctx->pos - start) != NULL));
#endif
  return line_start_pos(ctx) > start;
}

/*
 * True when nothing but inline whitespace, and maybe a trailing '#' comment,
 * is left before the next newline or the end of the input, so the line holds
 * no more real content. That's true even when cur(ctx) isn't '\n' or the end
 * yet, for example when cur(ctx) == '#'. ctx->pos isn't changed.
 *
 * The parser uses it for "does this part of the entry (the value, the anchor
 * or the tag) start on THIS line or a later one" decisions, which a comment
 * between the indicator and the newline must not fool. In "a: # comment\n
 * b: c\n", for example, the value really starts on the next line although the
 * character right after "a: " is '#' and not '\n'.
 */
static bool rest_of_line_is_blank(parse_ctx_t *ctx) {
  /* The start of the stream is ctx->first_line_start, as in
   * skip_ws_comments(). No current caller asks at that position, since each
   * one runs after an indicator or a token, but the test keeps the two
   * comment rules the same rule. */
  size_t p = ctx->pos;
  while (p < ctx->len && (ctx->src[p] == ' ' || ctx->src[p] == '\t')) p++;
  if (p < ctx->len && ctx->src[p] == '#' &&
      (p == ctx->first_line_start || ctx->src[p - 1] == ' ' ||
       ctx->src[p - 1] == '\t' || ctx->src[p - 1] == '\n' ||
       ctx->src[p - 1] == '\r')) {
    while (p < ctx->len && ctx->src[p] != '\n' && ctx->src[p] != '\r') p++;
  }
  return p >= ctx->len || ctx->src[p] == '\n' || ctx->src[p] == '\r';
}

/* Skips any mix of whitespace (newlines included) and '#' comments. The
 * parser uses it between top-level structural tokens where indentation
 * doesn't matter, such as between the elements of a flow collection.
 *
 * The YAML spec only lets a '#' start a comment after whitespace or at the
 * start of the input, so a '#' that touches the token before it
 * ("]#comment" or "\"value\"#comment", for example) is no comment at all.
 * Such a '#' is left in place so the caller can refuse it as content
 * that doesn't belong there instead of silently reading it. */
static void skip_ws_comments(parse_ctx_t *ctx) {
  while (!at_end(ctx)) {
    char c = cur(ctx);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ctx->pos++;
    } else if (c == '#' && (ctx->pos == ctx->first_line_start ||
                            ctx->src[ctx->pos - 1] == ' ' ||
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
 * Like skip_ws_comments, but for content inside a flow collection. The
 * s-separate(n,c) grammar of YAML 1.2 covers the flow-in and flow-out
 * contexts, and inside it s-separate-lines(n) needs every line the skip
 * crosses to be indented strictly more than `indent`. `indent` is the indent
 * of the enclosing block value, the same parameter that parse_node and
 * parse_block_scalar_content pass along, with -1 as the sentinel for the
 * document root, which has no enclosing block context.
 *
 * Only the final line it lands on is checked, not every blank line it
 * crosses on the way. That's enough for every case this parser needs to tell
 * apart, as a reference parser confirms: "k: {\nk\n:\nv\n}" is refused,
 * because its continuation lines sit at column 0, no more indented than the
 * column 0 of "k:" itself, while the same content with one more space of
 * indent throughout is accepted.
 *
 * A line that starts with ']' or '}' exactly at column `indent` is the one
 * exception: it closes a flow collection under the key or the "- " that
 * opened it, which libyaml, and so PyYAML, Ansible and go-yaml, accept.
 * Refusing it would break files that those tools read without a complaint,
 * so we accept it as well.
 *

 * Returns false and sets ctx->error when the check fails, and true
 * otherwise, including when no newline was crossed at all, which matches the
 * unconditional success of plain skip_ws_comments.
 */
_CYAML_PARSE_HOT bool flow_skip_ws(parse_ctx_t *ctx, int indent) {
  size_t start = ctx->pos;
  skip_ws_comments(ctx);
  if (!at_end(ctx)) {
    bool crossed_newline = span_crosses_newline(ctx, start);
    /* s-flow-line-prefix(n) is the production for a continuation across a
     * line: s-indent(n), which is spaces only, then optional separation,
     * which may hold tabs. So line_space_indent() measures the spaces before
     * the first tab, and a line such as "  \tvalue" is indented by two.
     *
     * One line is exempt: a line whose content starts with the closing
     * indicator of a flow collection may sit exactly at the indent of the
     * enclosing block value, after spaces only. That's the usual way to close
     * a flow collection that spans lines ("key: [" on one line, the elements
     * indented below, and "]" back under "key"), and libyaml accepts it.
     * Every other flow content at that column, and a closing indicator
     * further left or after a tab, is refused. */
    int col = crossed_newline ? line_space_indent(ctx) : 0;
    if (crossed_newline && col <= indent &&
        !(col == indent && (cur(ctx) == ']' || cur(ctx) == '}') &&
          !line_indent_has_tab(ctx))) {
      parse_err(ctx,
                "flow collection continuation line must be indented "
                "more than %d at position %zu",
                indent, ctx->pos);
      return false;
    }
  }
  return true;
}

/* Reads exactly one newline: a CR, an LF, or a CRLF pair. */
static void skip_newline(parse_ctx_t *ctx) {
  if (!at_end(ctx) && cur(ctx) == '\r') ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '\n') ctx->pos++;
}

/* A look-ahead that reads nothing. True when the rest of the current line, up
 * to the next line break or the end of the input, holds nothing but inline
 * whitespace (spaces and tabs), i.e. a truly blank line. The line folding of
 * double-quoted and single-quoted scalars uses it to find a blank
 * continuation line, which must fold to a newline and not a space even
 * when the line carries some trailing whitespace of its own, a lone tab for
 * example. A plain cur(ctx) == '\n' or '\r' check right after skip_newline()
 * can't see past that whitespace to the line break behind it. */
static bool at_blank_line(parse_ctx_t *ctx) {
  size_t p = ctx->pos;
  while (p < ctx->len && (ctx->src[p] == ' ' || ctx->src[p] == '\t')) p++;
  return p >= ctx->len || ctx->src[p] == '\r' || ctx->src[p] == '\n';
}

/*
 * True for a C0 control byte (0x00 to 0x1F) other than tab (0x09), and for
 * DEL (0x7F). YAML 1.2 keeps all of them out of literal, unescaped scalar
 * content in every style. c-printable, the production under the nb-char
 * grammar of every scalar style that JSON doesn't cover, is
 * "#x9 | #xA | #xD | [#x20-#x7E] | ...", and its range stops at 0x7E, one
 * byte short of DEL. nb-json, which only governs double-quoted content, is
 * wider: "#x9 | [#x20-#x10FFFF]", with no gap around 0x7F at all, so going
 * by the grammar text alone nb-json seems to allow a raw DEL byte there. But
 * PyYAML refuses a raw DEL the same way in every style (double-quoted,
 * single-quoted, plain and literal block), reporting "unacceptable character
 * #x007f: special characters are not allowed", because it filters that byte
 * at the character stream level before reading any style-specific grammar.
 * That filter wins over the wider text of nb-json for this one byte.
 *
 * The serializer asks this predicate about the bytes of a string it is about
 * to write: a string holding one of these bytes, or a line break, goes out
 * double-quoted, where each of them becomes an escape. An explicit "\x01" or
 * "\u0001" escape in a double-quoted scalar is the one way to put such a byte
 * into a document. The parser refuses the raw byte for the whole stream; see
 * cyaml_first_refused_char() below.
 */
static inline bool is_disallowed_control_byte(char c) {
  unsigned char u = (unsigned char)c;
  return (u < 0x20 && u != '\t') || u == 0x7F;
}

/* ========================================================================== */
/*                    STREAM CHARACTER SET (YAML 1.2 SEC. 5.1)                */
/* ========================================================================== */

/*
 * A YAML 1.2 stream is UTF-8 text made only of c-printable characters:
 *
 *   #x9 | #xA | #xD | [#x20-#x7E] | #x85 | [#xA0-#xD7FF] | [#xE000-#xFFFD]
 *   | [#x10000-#x10FFFF]
 *
 * parse_common() checks the whole input against that rule once, before
 * parsing anything, and refuses the document at the first byte that breaks
 * it. The refused bytes are an ill-formed UTF-8 sequence (truncated, a stray
 * continuation byte, an overlong form, an encoded surrogate, a code point
 * above U+10FFFF), a C0 control other than TAB, LF and CR, DEL, a C1 control
 * other than NEL, and the noncharacters U+FFFE and U+FFFF. The rule covers
 * every byte of the stream: scalars of every style, keys, comments, tags,
 * anchors, directives and the space between tokens. An escape inside a
 * double-quoted scalar is another matter: "\x01", "\x80" and "\uFFFE" are
 * c-printable text that names such a character, and the parser decodes it
 * into the value.
 *
 * Every scanner runs after this check, so none of them needs to test a byte
 * against the rule again. That includes the guarantee that a raw NUL, which
 * would silently end the string early, never reaches a token the parser
 * stores as a NUL-terminated string (an anchor name, a tag, a scalar). One
 * check up front is cheaper than a test in every scanner, and it keeps the
 * rule in one place, where it's easy to get right.
 *
 * The check reads sixteen bytes at a time. A block whose bytes are all TAB,
 * LF, CR or 0x20 to 0x7E is accepted with one vector compare, and any other
 * block (one with non-ASCII text or a refused byte) is walked one character
 * at a time. The vector types are GCC and Clang vector extensions; a target
 * without a vector unit runs the same code on scalar registers.
 */
typedef unsigned char cyaml_u8x16 __attribute__((vector_size(16)));
typedef signed char cyaml_s8x16 __attribute__((vector_size(16)));

/* True when each of the 16 bytes at p is TAB, LF, CR or in 0x20..0x7E. */
static inline bool cyaml_block_is_printable_ascii(const unsigned char *p) {
  cyaml_u8x16 v;
  memcpy(&v, p, sizeof(v));
  cyaml_s8x16 bad =
      ((v < 0x20) & (v != 0x09) & (v != 0x0A) & (v != 0x0D)) | (v >= 0x7F);
  uint64_t lo, hi;
  memcpy(&lo, &bad, sizeof(lo));
  memcpy(&hi, (const char *)&bad + sizeof(lo), sizeof(hi));
  return (lo | hi) == 0;
}

/* True for an ASCII byte that c-printable leaves out: a C0 control other than
 * TAB, LF and CR, and DEL. */
static inline bool cyaml_is_refused_ascii(unsigned char c) {
  return (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) || c == 0x7F;
}

/* The offset of the first byte of p[0..len) that starts an ill-formed UTF-8
 * sequence or a character outside c-printable, or len when there is none. */
static size_t cyaml_first_refused_char(const unsigned char *p, size_t len) {
  size_t i = 0;
  for (;;) {
    while (len - i >= 16 && cyaml_block_is_printable_ascii(p + i)) i += 16;
    size_t stop = len - i >= 16 ? i + 16 : len;
    while (i < stop) {
      unsigned char c = p[i];
      if (c < 0x80) {
        if (cyaml_is_refused_ascii(c)) return i;
        i++;
        continue;
      }
      bool ok;
      size_t step = ccol_utf8_step(p + i, len - i, &ok);
      if (!ok) return i;
      /* A well-formed sequence of two or three bytes. A C1 control is C2 80
       * to C2 9F, and NEL (C2 85) is the one c-printable member of that range.
       * U+FFFE and U+FFFF are EF BF BE and EF BF BF. */
      if (c == 0xC2 && p[i + 1] <= 0x9F && p[i + 1] != 0x85) return i;
      if (c == 0xEF && p[i + 1] == 0xBF && p[i + 2] >= 0xBE) return i;
      i += step;
    }
    if (i >= len) return len;
  }
}

/* Writes into msg the diagnostic for the character at p[at] that
 * cyaml_first_refused_char() refused. Line and column both count from 1, and
 * a line ends at LF, at CR, or at a CRLF pair, which are YAML 1.2's line
 * breaks. The column counts characters rather than bytes, and a byte order
 * mark at the start of the stream doesn't count. Every byte of the message is
 * printable ASCII, with input bytes shown as 0xNN. */
static void cyaml_describe_refused_char(const unsigned char *p, size_t len,
                                        size_t at, char *msg, size_t msg_size) {
  size_t line = 1, line_start = 0;
  for (size_t i = 0; i < at; i++) {
    if (p[i] == '\n' || (p[i] == '\r' && (i + 1 >= len || p[i + 1] != '\n'))) {
      line++;
      line_start = i + 1;
    }
  }
  size_t col = 1;
  for (size_t i = line_start; i < at; i++)
    if ((p[i] & 0xC0) != 0x80) col++;
  if (line_start == 0 && at >= 3 && p[0] == 0xEF && p[1] == 0xBB &&
      p[2] == 0xBF)
    col--;

  unsigned char c = p[at];
  unsigned char c1 = at + 1 < len ? p[at + 1] : 0;
  if (c < 0x80) {
    snprintf(msg, msg_size,
             "raw control character 0x%02x is not allowed in a YAML stream "
             "(line %zu, column %zu); write it as an escape sequence in a "
             "double-quoted scalar",
             c, line, col);
    return;
  }
  bool ok;
  (void)ccol_utf8_step(p + at, len - at, &ok);
  if (ok) {
    if (c == 0xC2)
      snprintf(msg, msg_size,
               "C1 control character U+00%02X (bytes 0xc2 0x%02x) is not "
               "allowed in a YAML stream (line %zu, column %zu); write it as "
               "an escape sequence in a double-quoted scalar",
               c1, c1, line, col);
    else
      snprintf(msg, msg_size,
               "noncharacter U+FFF%c (bytes 0xef 0xbf 0x%02x) is not allowed "
               "in a YAML stream (line %zu, column %zu); write it as an "
               "escape sequence in a double-quoted scalar",
               p[at + 2] == 0xBE ? 'E' : 'F', p[at + 2], line, col);
    return;
  }

  /* Same classification as in cjson, through the shared helper. */
  char detail[64];
  const char *what =
      ccol_utf8_describe_ill_formed(p, len, at, detail, sizeof(detail));
  snprintf(msg, msg_size,
           "invalid UTF-8 in a YAML stream (line %zu, column %zu): %s (%s)",
           line, col, what, detail);
}

/* True when ctx->pos is at a document-start marker ('---') or a document-end
 * marker ('...'): those three characters at column 0, followed by whitespace
 * or the end of the input. A '#' that touches the marker with no whitespace
 * in between does NOT count. That follows the rule of skip_ws_comments() and
 * rest_of_line_is_blank() elsewhere in this file that real whitespace has to
 * come before a comment, and also YAML 1.2's s-l-comments grammar, which
 * needs an s-separate-in-line before a c-nb-comment-text, so "---#x" is
 * ordinary plain scalar content and not a marker followed by a comment. The
 * parser uses this to stop the block collection parsers at a document
 * boundary and to find where the next document of a multi-document stream
 * starts. */
_CYAML_PARSE_HOT bool at_doc_marker(parse_ctx_t *ctx) {
  if (current_col(ctx) != 0) return false;
  if (ctx->pos + 3 > ctx->len) return false;
  const char *p = ctx->src + ctx->pos;
  if (memcmp(p, "---", 3) != 0 && memcmp(p, "...", 3) != 0) return false;
  if (ctx->pos + 3 == ctx->len) return true;
  char nx = ctx->src[ctx->pos + 3];
  return nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r';
}

/* True when ctx->pos is at a UTF-8 byte order mark (EF BB BF) at column 0.
 * YAML 1.2 section 5.2 says a byte order mark must not appear inside a
 * document, and the stream grammar only allows one in a document prefix,
 * before a document. So inside a document a mark at the start of a line ends
 * the block structure like a document marker, and the stream loop of
 * parse_common() then only accepts it before a '---' or at the end of the
 * input. The first byte is tested first, so the common case costs one
 * comparison. */
static inline bool at_col0_bom(parse_ctx_t *ctx) {
  return ctx->len - ctx->pos >= 3 &&
         (unsigned char)ctx->src[ctx->pos] == 0xEF &&
         (unsigned char)ctx->src[ctx->pos + 1] == 0xBB &&
         (unsigned char)ctx->src[ctx->pos + 2] == 0xBF && current_col(ctx) == 0;
}

/* Anchor / alias helpers */

/*
 * Anchor table management.
 *
 * A lazily created chmap (char* -> cyaml_node_t*) holds the anchors (&name).
 * The table points at the anchored node itself, which stays where the
 * document put it, so registering an anchor copies nothing. An alias (*name)
 * resolves to a deep clone of that node, so every expansion gives an
 * independent subtree and no two parents share one node; the clone is the
 * only place where an anchor costs work proportional to its size, and the
 * amplification budget charges it there.
 *
 * The table owns no node. A node it points at stays alive until the table
 * goes: either a tree holds it, or parse_discard() moved it into the parse's
 * kept list when its tree dropped it, and anchors_destroy() frees that list.
 *
 * anchors_ensure: allocate the map lazily, on first use.
 * anchors_store:  point name at a node, forgetting any earlier node.
 * anchors_lookup: give a borrowed reference to the anchored node, or NULL.
 * anchors_destroy: free the map and the kept list.
 *
 * The end of each document destroys the table, on success and on failure
 * alike.
 */
static bool anchors_ensure(parse_ctx_t *ctx) {
  if (ctx->anchors) return true;
  char *err = NULL;
  ctx->anchors = chmap_create_mp(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                 ccol_string, ccol_pointer, ctx->mp, &err);
  if (!ctx->anchors) return false;
  _parse_discard_ctx = ctx;
  return true;
}

/* Reserves one more kept-list slot for the node the caller is about to
 * register. Returns false on an OOM. */
static bool anchors_reserve_kept(parse_ctx_t *ctx) {
  if (ctx->kept_reserved == ctx->kept_cap) {
    size_t new_cap = ctx->kept_cap ? ctx->kept_cap * 2 : 16;
    cyaml_node_t **grown =
        _ccol_mem_realloc(ctx->mp, ctx->kept, new_cap * sizeof(*grown));
    if (!grown) return false;
    ctx->kept = grown;
    ctx->kept_cap = new_cap;
  }
  ctx->kept_reserved++;
  return true;
}

/* Appends an entry for name, with old set to NULL, to the undo log of the
 * running speculation. Returns the entry, which stays valid until the next
 * append, or NULL on an OOM. */
static _anchor_undo_entry_t *anchor_undo_push(parse_ctx_t *ctx,
                                              const char *name) {
  _anchor_undo_t *u = ctx->anchor_undo;
  if (u->len == u->cap) {
    size_t new_cap = u->cap * 2;
    _anchor_undo_entry_t *grown;
    if (u->entries == u->inline_entries) {
      grown = _ccol_mem_alloc(ctx->mp, new_cap * sizeof(*grown));
      if (grown) memcpy(grown, u->entries, u->len * sizeof(*grown));
    } else {
      grown = _ccol_mem_realloc(ctx->mp, u->entries, new_cap * sizeof(*grown));
    }
    if (!grown) return NULL;
    u->entries = grown;
    u->cap = new_cap;
  }
  char *copy = ccol_strdup(ctx->mp, name);
  if (!copy) return NULL;
  _anchor_undo_entry_t *e = &u->entries[u->len++];
  e->name = copy;
  e->old = NULL;
  return e;
}

/*
 * Points name at node, which the caller keeps: either a tree holds it, or the
 * caller hands it to parse_keep() right after a successful store. The node
 * is marked anchored and one kept-list slot is reserved for it, so
 * parse_discard() can keep it later without allocating. An earlier node of
 * the same name is just forgotten, since the table never owned it.
 *
 * Returns false on an OOM (from the lazy table creation in anchors_ensure(),
 * from the reservation, or from the insert itself) and sets a parse error
 * with parse_err(). The node is then not registered and the caller still
 * owns it. Reporting the failure matters: a caller that treated a store as
 * always successful would hand back a "successful" parse in which a later
 * '*name' alias fails with a confusing "unknown alias" instead of the real
 * "out of memory" cause.
 *
 * While a speculative parse runs (ctx->anchor_undo is set), every store is
 * also recorded in its undo log along with the node the name pointed at
 * before.
 */
static bool anchors_store(parse_ctx_t *ctx, const char *name,
                          cyaml_node_t *node) {
  if (!anchors_ensure(ctx) || !anchors_reserve_kept(ctx)) {
    parse_err(ctx, "out of memory registering anchor '%s' at position %zu",
              _cyaml_echo(name), ctx->pos);
    return false;
  }
  _anchor_undo_entry_t *undo = NULL;
  if (ctx->anchor_undo) {
    undo = anchor_undo_push(ctx, name);
    if (!undo) {
      parse_err(ctx, "out of memory registering anchor '%s' at position %zu",
                _cyaml_echo(name), ctx->pos);
      return false;
    }
  }
  cmap_pair kp = {.ptr = (void *)name, .size = strlen(name) + 1};
  /* One hash and one probe either insert the anchor or hand back the slot of
   * an anchor the document defines again. The later definition wins, so the
   * slot then points at the new node. */
  cmap_pair vp = {.ptr = &node, .size = sizeof(node)};
  cmap_pair old_vp;
  ccol_retval_t ins =
      ccol_chmap_insert_or_get_elem(ctx->anchors, &kp, &vp, &old_vp);
  if (ins == ccol_success) {
    node->anchored = true;
    return true;
  }
  if (ins == ccol_key_already_present) {
    if (undo) undo->old = _cyaml_read_child(old_vp.ptr);
    memcpy(old_vp.ptr, &node, sizeof(node));
    node->anchored = true;
    return true;
  }
  /* The insert failed, so the old node stays in the map and the log entry
   * describing the store goes away. */
  if (undo) {
    ctx->anchor_undo->len--;
    _ccol_mem_free(ctx->mp, undo->name);
  }
  parse_err(ctx, "out of memory registering anchor '%s' at position %zu",
            _cyaml_echo(name), ctx->pos);
  return false;
}

/*
 * A speculative parse reads content that may turn out to be something else,
 * and then the parser reads it again. There is only one: parsing a flow
 * collection that may be an implicit block mapping key (see
 * _try_parse_scalar_dict_key). Such a parse has side effects that must not
 * survive a rejection. It stores the anchors it defines, so an alias in the
 * second reading would resolve to an anchor that the document only defines
 * later; it may move nodes into the kept list; and it charges the parse
 * budgets for a tree that the rejection frees, so the second reading would
 * pay again.
 *
 * anchor_speculation_begin() records the budgets and the length of the kept
 * list and starts an undo log for the anchor table.
 * anchor_speculation_reject() puts the anchor table and the budgets back
 * exactly as they were and frees what the speculation kept, while
 * anchor_speculation_accept() keeps every change; both end the speculation.
 * A rejection allocates nothing, so it can't fail. When a parse function
 * gains a new side effect, give it an undo here as well, or a rejected guess
 * leaves a trace that the second reading can see.
 */

typedef struct {
  _anchor_undo_t undo;
  _anchor_undo_t *outer;
  size_t kept_len;
  size_t node_budget;
  size_t byte_budget;
  size_t amp_node_budget;
  size_t amp_byte_budget;
  bool node_exhausted;
  bool byte_exhausted;
  bool amp_limit_hit;
} _anchor_speculation_t;

static void anchor_speculation_begin(parse_ctx_t *ctx,
                                     _anchor_speculation_t *sp) {
  sp->undo.entries = sp->undo.inline_entries;
  sp->undo.len = 0;
  sp->undo.cap = _ANCHOR_UNDO_INLINE;
  sp->outer = ctx->anchor_undo;
  sp->kept_len = ctx->kept_len;
  sp->node_budget = _parse_node_budget;
  sp->byte_budget = _parse_byte_budget;
  sp->amp_node_budget = _parse_amp_node_budget;
  sp->amp_byte_budget = _parse_amp_byte_budget;
  sp->node_exhausted = _parse_node_budget_exhausted;
  sp->byte_exhausted = _parse_byte_budget_exhausted;
  sp->amp_limit_hit = _parse_amp_limit_hit;
  ctx->anchor_undo = &sp->undo;
}

static void anchor_speculation_end(parse_ctx_t *ctx,
                                   _anchor_speculation_t *sp) {
  for (size_t i = 0; i < sp->undo.len; i++)
    _ccol_mem_free(ctx->mp, sp->undo.entries[i].name);
  if (sp->undo.entries != sp->undo.inline_entries)
    _ccol_mem_free(ctx->mp, sp->undo.entries);
  ctx->anchor_undo = sp->outer;
}

static void anchor_speculation_accept(parse_ctx_t *ctx,
                                      _anchor_speculation_t *sp) {
  anchor_speculation_end(ctx, sp);
}

/* The caller has already destroyed the tree the speculation built, so the
 * slots the log restores may name freed nodes; we only write those slots
 * and never read a node through them. */
static void anchor_speculation_reject(parse_ctx_t *ctx,
                                      _anchor_speculation_t *sp) {
  /* Undo the stores in reverse order, so a name the speculation stored twice
   * ends up with the node it held before the first store. A name the
   * speculation added keeps its slot with a NULL node, which anchors_lookup()
   * reports as an unknown anchor; that keeps the rejection free of any
   * allocation a delete from the map could make. */
  for (size_t i = sp->undo.len; i-- > 0;) {
    _anchor_undo_entry_t *e = &sp->undo.entries[i];
    cmap_pair kp = {.ptr = e->name, .size = strlen(e->name) + 1};
    cmap_pair vp;
    if (chmap_get_elem_ref(ctx->anchors, &kp, &vp) == ccol_success)
      memcpy(vp.ptr, &e->old, sizeof(e->old));
  }
  /* Every node the speculation kept belongs to the rejected reading, since
   * only the anchors the log has just taken back point at it. */
  while (ctx->kept_len > sp->kept_len)
    __cyaml_destroy((cyaml)ctx->kept[--ctx->kept_len]);
  _parse_node_budget = sp->node_budget;
  _parse_byte_budget = sp->byte_budget;
  _parse_amp_node_budget = sp->amp_node_budget;
  _parse_amp_byte_budget = sp->amp_byte_budget;
  _parse_node_budget_exhausted = sp->node_exhausted;
  _parse_byte_budget_exhausted = sp->byte_exhausted;
  _parse_amp_limit_hit = sp->amp_limit_hit;
  anchor_speculation_end(ctx, sp);
}

/* A slot holding a NULL node is a name that a rejected speculation added and
 * took back (see anchor_speculation_reject()), so it reads as unknown. */
static cyaml_node_t *anchors_lookup(parse_ctx_t *ctx, const char *name) {
  if (!ctx->anchors) return NULL;
  cmap_pair kp = {.ptr = (void *)name, .size = strlen(name) + 1};
  cmap_pair vp;
  if (chmap_get_elem_ref(ctx->anchors, &kp, &vp) != ccol_success) return NULL;
  return _cyaml_read_child(vp.ptr);
}

/* Frees the map, which owns no node, and every node of the kept list. A
 * failed parse gets here too, and then the table may point at nodes the
 * failure has already freed, which nothing reads. */
static void anchors_destroy(parse_ctx_t *ctx) {
  if (_parse_discard_ctx == ctx) _parse_discard_ctx = NULL;
  for (size_t i = 0; i < ctx->kept_len; i++)
    __cyaml_destroy((cyaml)ctx->kept[i]);
  _ccol_mem_free(ctx->mp, ctx->kept);
  ctx->kept = NULL;
  ctx->kept_len = 0;
  ctx->kept_cap = 0;
  ctx->kept_reserved = 0;
  if (!ctx->anchors) return;
  __chmap_destroy(ctx->anchors);
  ctx->anchors = NULL;
}

/* %TAG handle table */

/*
 * tag_handles_ensure, _set, _lookup and _destroy manage a table that maps a
 * %TAG handle to a prefix. It lives exactly as long as the anchors table of
 * anchors_ensure, _store, _lookup and _destroy: created lazily and destroyed
 * at the end of parse_one_document. Unlike anchors, though, this table starts
 * out truly empty, without the two default YAML 1.2 handles, because
 * tag_handles_lookup falls back to the fixed defaults itself when the table
 * has no explicit entry. So tag_handles_set can use chmap_insert_elem's
 * "already present" signal directly to spot a second %TAG directive that
 * defines the same handle again within one document, with no separate
 * structure to track it. The first %TAG for any handle, "!" and "!!"
 * included, always inserts cleanly because the table starts with nothing in
 * it.
 */
static bool tag_handles_ensure(parse_ctx_t *ctx) {
  if (ctx->tag_handles) return true;
  char *err = NULL;
  ctx->tag_handles = chmap_create_mp(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                     ccol_string, ccol_string, ctx->mp, &err);
  return ctx->tag_handles != NULL;
}

/* Returns ccol_success on a clean first insert, and ccol_key_already_present
 * when an earlier %TAG directive of the same document has already set this
 * handle explicitly, which the caller then refuses as a duplicate
 * definition. Returns some other ccol_retval_t on an allocation failure. */
static ccol_retval_t tag_handles_set(parse_ctx_t *ctx, const char *handle,
                                     const char *prefix) {
  if (!tag_handles_ensure(ctx)) return ccol_not_enough_memory;
  cmap_pair kp = {.ptr = (void *)handle, .size = strlen(handle) + 1};
  cmap_pair vp = {.ptr = (void *)prefix, .size = strlen(prefix) + 1};
  return chmap_insert_elem(ctx->tag_handles, &kp, &vp);
}

/* A borrowed pointer to the resolved prefix for `handle`: the one a %TAG
 * directive registered, if there is such an entry, or else the built-in YAML
 * 1.2 default for "!" or "!!". NULL when `handle` is neither registered nor
 * one of the two built-in handles, i.e. nothing defines its name. */
static const char *tag_handles_lookup(parse_ctx_t *ctx, const char *handle) {
  if (ctx->tag_handles) {
    cmap_pair kp = {.ptr = (void *)handle, .size = strlen(handle) + 1};
    cmap_pair vp;
    if (chmap_get_elem_ref(ctx->tag_handles, &kp, &vp) == ccol_success)
      return (const char *)vp.ptr;
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

/* Reads the anchor or alias name after a '&' or a '*'. A name is one or more
 * characters that aren't whitespace, flow indicators or '#'. parse_common()
 * has already refused every character outside c-printable, so a raw NUL can
 * never end the stored name early. Returns false and sets a parse error with
 * parse_err() on an empty name and on an allocation failure. Always setting
 * ctx->error on failure matters beyond the direct callers:
 * try_parse_scalar_dict_key() calls this speculatively and uses ctx->error[0]
 * alone to tell "a real error" from "not a key after all", rather than a
 * restored ctx->pos, because on this path the scan loop here has already
 * moved past that point. Without ctx->error set on an OOM, that caller would
 * misclassify the failure and carry on parsing from a corrupt position, and
 * the document wouldn't fail cleanly. */
static bool parse_anchor_name(parse_ctx_t *ctx, char **name_out) {
  size_t start = ctx->pos;
  while (!at_end(ctx)) {
    char c = cur(ctx);
    /* An anchor name ends at whitespace, at a flow indicator, or at a
     * comment. */
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' ||
        c == '[' || c == ']' || c == '{' || c == '}' || c == '#')
      break;
    /* A ':' followed by whitespace or the end of the input is a mapping value
     * indicator and not part of the anchor or alias name, the same rule as in
     * scan_plain_scalar_line, where a colon and a space, or a colon and an end
     * of line, ends the token. That lets a caller use an anchor or alias
     * directly as a mapping key with no space before the colon; "*x: y" is by
     * far the most natural way to write that, since an ordinary key is
     * written "key: value" the same way. We read it as the name "x" alone and
     * not "x:", and a reference parser agrees: it resolves "*x: y" to the
     * alias "x" used as a key instead of reporting an unknown alias "x:". A
     * bare ':' with no whitespace after it stays ordinary name content, for
     * example in an anchor name that looks like a URL, which is the same
     * exception scan_plain_scalar_line has. */
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
  char *name = _ccol_mem_alloc(ctx->mp, len + 1);
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
 * try_parse_null_scalar, try_parse_bool_scalar, try_parse_int_scalar and
 * try_parse_float_scalar are the core-schema type tests that
 * make_typed_scalar tries one after another to resolve an untagged scalar.
 * They are separate functions that can be called on their own, instead of
 * one block inside make_typed_scalar, so that an explicit tag can force
 * exactly one of them, as finalize_scalar_node below does, without trying
 * every other type first the way make_typed_scalar's ordered fallthrough
 * does.
 */
static bool try_parse_bool_scalar(const char *s, bool *out) {
  /* Dispatch on the first character before any comparison runs. Every scalar
     in a document comes through here and almost none of them is a boolean,
     so the cost is decided by how fast we can rule out a non-boolean. A
     chain of strcmp calls only rules it out after entering the C library
     once per spelling, paying for a call and its setup each time, even when
     the very first byte already settles the question. A switch on that byte
     settles it with one load and one branch, and only the two or three
     spellings still possible then need a comparison. The accepted set is
     exactly what the YAML 1.2 core schema accepts: the all-lower form, the
     capitalized form and the all-upper form, and nothing else. */
  switch (s[0]) {
    case 't':
      if (strcmp(s, "true") == 0) break;
      return false;
    case 'T':
      if (strcmp(s, "True") == 0 || strcmp(s, "TRUE") == 0) break;
      return false;
    case 'f':
      if (strcmp(s, "false") == 0) {
        *out = false;
        return true;
      }
      return false;
    case 'F':
      if (strcmp(s, "False") == 0 || strcmp(s, "FALSE") == 0) {
        *out = false;
        return true;
      }
      return false;
    default:
      return false;
  }
  *out = true;
  return true;
}

_CYAML_PARSE_HOT bool try_parse_int_scalar(const char *s, long long *out) {
  /* The core schema's int grammar is [-+]?[0-9]+ for a decimal value, plus the
   * 0x and 0o forms, so every int starts with a digit or a sign. strtoll()
   * and strtoull() below accept more than that grammar, because the C
   * standard has them silently skip every byte that isspace() accepts (space,
   * tab, LF, VT, FF and CR) before the subject sequence. A double-quoted
   * scalar can carry any of those through an escape, so without this check
   * " 42" and "\v42" would quietly be accepted as 42 when they must fail.
   * The check names the bytes that may start an int instead of those that
   * may not, so no whitespace byte can slip past it, and it also ends the
   * test of every ordinary string at its first byte.
   * trim_trailing_ws_for_numeric_tag, which calls this for an explicit !!int
   * tag, only trims trailing whitespace and never leading whitespace for
   * exactly this reason. Every caller, implicit resolution included, gets the
   * same strict behavior. */
  const unsigned char first = (unsigned char)s[0];
  if (!((unsigned)(first - '0') < 10u || first == '+' || first == '-'))
    return false;

  char *endp = NULL;
  long long ival = 0;
  errno = 0;

  /* The core schema gives the 0x and 0o forms no sign, so "-0x1" and "+0o7"
   * are strings and these two branches test the text from its first byte. */
  const char *p = s;
  if (p[0] == '0' && p[1] == 'x') {
    /* The hex form. Its core-schema grammar is exactly "0x" [0-9a-fA-F]+, with
     * no room for a sign or anything else between the prefix and the digits.
     * The 'x' must be lowercase, while the digits are explicitly
     * case-insensitive. An uppercase "0X" prefix has no int representation
     * in the core schema at all, so it falls through to the decimal
     * branch below, which correctly refuses it, since strtoll with an
     * explicit base of 10 never reads "0X..." as hex. strtoull() accepts more
     * than the grammar above, because the C standard lets it take an optional
     * '+' or '-' before the digits it reads. Without a check a malformed
     * literal like "0x-0" or "0x+5" would quietly be accepted as a valid
     * integer of magnitude 0 or 5, when it must fall back to CYAML_STRING.
     * So we validate every digit after the prefix by hand first, and the
     * octal branch below does the same. */
    const char *digits = p + 2;
    if (*digits == '\0') return false;
    for (const char *d = digits; *d; d++) {
      bool is_hex_digit = (*d >= '0' && *d <= '9') ||
                          (*d >= 'a' && *d <= 'f') || (*d >= 'A' && *d <= 'F');
      if (!is_hex_digit) return false;
    }
    unsigned long long uval = strtoull(digits, &endp, 16);
    if (endp == digits || *endp != '\0' || errno == ERANGE) return false;
    /* strtoull only refuses a magnitude that overflows ULLONG_MAX (2^64-1). A
     * value between 2^63 and 2^64-1 fits an unsigned 64-bit word but is above
     * LLONG_MAX, so we refuse it too and the scalar is a string, like every
     * other hex literal beyond the integer range. Otherwise it would silently
     * be read as some other value: 0xFFFFFFFFFFFFFFFF would become -1, and
     * 0x8000000000000000 would become negative. */
    if (uval > (unsigned long long)LLONG_MAX) return false;
    ival = (long long)uval;
  } else if (p[0] == '0' && p[1] == 'o') {
    /* The octal form. As in the hex branch above the 'o' has to be lowercase,
     * and an uppercase "0O" prefix falls through to the decimal branch below,
     * which correctly refuses it. The comment of the hex branch explains why
     * we validate every digit by hand before calling strtoull() instead of
     * trusting it to refuse a malformed literal like "0o-0" or "0o+7" on its
     * own. */
    const char *digits = p + 2;
    if (*digits == '\0') return false;
    for (const char *d = digits; *d; d++) {
      if (*d < '0' || *d > '7') return false;
    }
    unsigned long long uval = strtoull(digits, &endp, 8);
    if (endp == digits || *endp != '\0' || errno == ERANGE) return false;
    if (uval > (unsigned long long)LLONG_MAX) return false;
    ival = (long long)uval;
  } else {
    /* The decimal form. We pass the whole original string, optional sign
     * included, so strtoll handles LLONG_MIN (-9223372036854775808)
     * correctly; calling strtoll on p, the substring without the sign, would
     * overflow for 2^63. */
    ival = strtoll(s, &endp, 10);
    if (endp == s || *endp != '\0' || errno == ERANGE) return false;
  }
  *out = ival;
  return true;
}

/* Handles the special float values (.inf, -.inf and .nan) as well as ordinary
 * parsing with strtod, following the YAML 1.2 core schema, which only has the
 * dot-prefixed special values. On a C99 platform strtod also accepts a bare
 * "nan", "inf" or "infinity", and all three are excluded here because they
 * aren't YAML floats.
 *
 * strtod() follows LC_NUMERIC. Every caller runs inside the "C" locale scope
 * that cyaml_parse_n_mp(), cyaml_parse_mp(), cyaml_serialize() and
 * cyaml_serialize_flow() open, so '.' is always the decimal point here. */
_CYAML_PARSE_HOT bool try_parse_float_scalar(const char *s, double *out) {
  /* Same check as in try_parse_int_scalar above and for the same reason: every
   * core-schema float, the special forms included, starts with a digit, a
   * sign or '.', and strtod() below silently skips every leading byte that
   * isspace() accepts. Without this check " 3.5" and "\f3.5" would be
   * accepted as 3.5 instead of failing. */
  const unsigned char first = (unsigned char)s[0];
  if (!((unsigned)(first - '0') < 10u || first == '+' || first == '-' ||
        first == '.'))
    return false;

  /* Every spelling of infinity and not-a-number starts with '.', '+' or '-',
     so one look at the first character excludes every ordinary number here
     before any comparison runs; see try_parse_bool_scalar for why that
     matters on a path every scalar of the document takes. A leading '+' or
     '-' that isn't one of these spellings falls through to the numeric
     parse below, which is why those two cases don't return early. */
  if (s[0] == '.' || s[0] == '+' || s[0] == '-') {
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
  }

  const char *q = s;
  if (*q == '+' || *q == '-') q++;
  /* Dispatch on the first byte before any comparison runs. Every spelling the
     four case-insensitive comparisons here can match starts with n, N, i or
     I, so for any other scalar one load and one branch settle all of them,
     and a plain string is what dominates a real document. Without this guard
     every scalar would run up to four comparisons just to be ruled out. The
     comparisons only fold ASCII letters (cascii.h), so a Turkish or any other
     locale can't change which words match. */
  const char qc = *q;
  if (qc == 'n' || qc == 'N' || qc == 'i' || qc == 'I') {
    if (ccol_ascii_casecmp(q, "nan") == 0 ||
        ccol_ascii_casecmp(q, "inf") == 0 ||
        ccol_ascii_casecmp(q, "infinity") == 0)
      return false;
    /* glibc's strtod() also accepts C99's nan(n-char-sequence) syntax, for
     * example "nan(123)" and "nan()", which isn't part of the core schema's
     * float grammar at all; only the dot-prefixed forms above are. Without a
     * guard the general strtod() call below would quietly accept it as a NaN
     * float and drop the original text, so every distinct "nan(...)"
     * spelling would collide onto one canonical dictionary key. We refuse
     * the whole of q here so it falls through to CYAML_STRING like any other
     * token that isn't a number. */
    if (ccol_ascii_ncasecmp(q, "nan(", 4) == 0) return false;
  }

  /* The core schema has no float form for any text starting with "0x", "0o",
   * "0X" or "0O", whatever follows and whatever sign comes before: a
   * lowercase prefix with digits in the integer range is an int
   * (try_parse_int_scalar() reads it) and every other such text is a string,
   * an unsigned literal beyond the integer range included. strtod() reads
   * more than that, namely C99's hex integers and hex floats ("0x10" is 16.0
   * and "0x1p3" is 8.0) with either case of the prefix, so we refuse such
   * text before strtod() can see it. */
  if (q[0] == '0' && (q[1] == 'x' || q[1] == 'o' || q[1] == 'X' || q[1] == 'O'))
    return false;

  char *endp = NULL;
  double dval = strtod(s, &endp);
  if (endp == s || *endp != '\0') return false;
  /* The text matches the core-schema float syntax, so it's a float whatever
   * its magnitude. strtod() sets errno to ERANGE in two cases and both give
   * the value the float really is: an overflow gives positive or negative
   * infinity ("1e400" is +inf, as in PyYAML), and an underflow gives the
   * nearest subnormal value or 0.0. */
  *out = dval;
  return true;
}

static bool is_null_scalar_text(const char *s) {
  /* Test the first character first, for the same reason as in
     try_parse_bool_scalar. */
  switch (s[0]) {
    case '\0':
      return true;
    case '~':
      return s[1] == '\0';
    case 'n':
      return strcmp(s, "null") == 0;
    case 'N':
      return strcmp(s, "Null") == 0 || strcmp(s, "NULL") == 0;
    default:
      return false;
  }
}

/*
 * Tries to parse s as a typed scalar of the YAML 1.2 core schema, and gives a
 * newly allocated node that the caller owns.
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

  /* The fallback to a string. */
  cyaml_node_t *n = node_alloc(CYAML_STRING, mp);
  if (!n) return NULL;
  n->value.string = strdup_charged(mp, s);
  if (!n->value.string) {
    node_free(n);
    return NULL;
  }
  return n;
}

/* Tag-driven scalar type resolution */

/*
 * Removes every trailing whitespace character and newline of text, in place.
 * It runs before a forced !!int or !!float tag tries to parse text as a
 * number. Without it the chomped content of a literal or folded block scalar
 * would never match, for any chomp mode, and KEEP mode keeps several trailing
 * newlines. Measuring against PyYAML's construct_yaml_int and
 * construct_yaml_float confirms it: both accept a KEEP-chomped "42\n\n\n" as
 * 42, because Python's own int() and float() accept the whitespace. A
 * narrower rule that trims exactly one trailing newline is too strict against
 * that same reference, since it refuses exactly the KEEP-chomped case with
 * several newlines that PyYAML accepts. It is on purpose NOT used for
 * !!bool: PyYAML's construct_yaml_bool does a strict dictionary lookup with no
 * trim and refuses the same shape (measured as well), so trimming there would
 * only differ from the reference in the other direction. */
static void trim_trailing_ws_for_numeric_tag(char *text) {
  size_t len = strlen(text);
  while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
                     text[len - 1] == ' ' || text[len - 1] == '\t'))
    len--;
  text[len] = '\0';
}

/*
 * An explicit !!bool tag accepts a wider set of words than this module's
 * implicit typing and ignores case. The implicit default, through
 * try_parse_bool_scalar, only accepts true, True, TRUE, false, False and
 * FALSE. Measuring against PyYAML's construct_yaml_bool confirms the wider
 * set: it matches yes, no, true, false, on and off, ignoring case, but not
 * the single letters y and n, for every loader. An explicit tag is a more
 * deliberate act by the author, so it gets PyYAML's wider set instead of the
 * narrow one implicit resolution uses.
 */
static bool try_parse_bool_scalar_explicit(const char *s, bool *out) {
  if (ccol_ascii_casecmp(s, "true") == 0 || ccol_ascii_casecmp(s, "yes") == 0 ||
      ccol_ascii_casecmp(s, "on") == 0) {
    *out = true;
    return true;
  }
  if (ccol_ascii_casecmp(s, "false") == 0 || ccol_ascii_casecmp(s, "no") == 0 ||
      ccol_ascii_casecmp(s, "off") == 0) {
    *out = false;
    return true;
  }
  return false;
}

/*
 * Builds a CYAML_STRING node that takes over *text, the text a scalar scanner
 * accumulated in a growable buffer. On success *text becomes NULL because the
 * node owns it; on failure *text stays with the caller.
 *
 * A scanner buffer starts at 256 bytes and doubles, so it's usually far
 * larger than its content. The node keeps the text shrunk to its exact
 * length, and the parse's byte budget is charged for that length, the same
 * way strdup_charged() charges every other DOM string. Without the shrink a
 * short quoted scalar would keep its whole scanner buffer for the life of the
 * document, and the budget would count none of it. A shrink the allocator
 * refuses leaves the text where it is, which is fine too.
 *
 * Returns NULL when the byte budget refuses the charge and when the node
 * allocation fails. Neither case sets ctx->error.
 */
static cyaml_node_t *string_node_adopting_text(parse_ctx_t *ctx, char **text) {
  size_t size = strlen(*text) + 1;
  size_t charge = size + _CYAML_ALLOC_OVERHEAD;
  if (!parse_bytes_charge(charge)) return NULL;
  cyaml_node_t *n = node_alloc(CYAML_STRING, ctx->mp);
  if (!n) {
    parse_bytes_refund(charge);
    return NULL;
  }
  char *fit = _ccol_mem_realloc(ctx->mp, *text, size);
  n->value.string = fit ? fit : *text;
  *text = NULL;
  return n;
}

/*
 * Finishes a scalar node from its raw text, letting an explicit tag override
 * the default type where such a tag applies. It takes ownership of `text`:
 * text is either freed or moved into the returned node, and the caller must
 * not touch it again after this call. `resolved_tag` is borrowed and not
 * owned here; a NULL `resolved_tag` means the scalar has no tag.
 * `implicit_ok` is true only for the natural default of a plain scalar, the
 * implicit core-schema resolution of make_typed_scalar, and false for the
 * four quoted and block styles, whose natural default is always
 * CYAML_STRING with no implicit typing.
 *
 * Returns NULL on an OOM without changing ctx->error, like every other
 * allocation-failure return in this file. Also returns NULL, with ctx->error
 * set, on a real disagreement between the tag and the content; that's a hard
 * parse failure for the whole document, in line with this parser's
 * fail-fast convention.
 */
static cyaml_node_t *finalize_scalar_node(parse_ctx_t *ctx, char *text,
                                          const char *resolved_tag,
                                          bool implicit_ok) {
  cyaml_node_t *n = NULL;

  if (resolved_tag && strcmp(resolved_tag, _CYAML_TAG_NON_SPECIFIC) == 0) {
    /* The non-specific "!": a scalar under it is a string whatever its text,
     * and the node carries no tag (YAML 1.2 section 6.9.1). */
    n = string_node_adopting_text(ctx, &text);
    _ccol_mem_free(ctx->mp, text); /* Does nothing when the node owns it. */
    return n;
  }
  if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_NULL) == 0) {
    /* The YAML spec says an empty tag value defaults to null, so we force a
     * null node whatever the text says. */
    n = node_alloc(CYAML_NULL, ctx->mp);
  } else if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_BOOL) == 0) {
    bool bval;
    if (!try_parse_bool_scalar_explicit(text, &bval)) {
      parse_err(ctx, "'%s' is not a valid !!bool value", _cyaml_echo(text));
      _ccol_mem_free(ctx->mp, text);
      return NULL;
    }
    n = node_alloc(CYAML_BOOL, ctx->mp);
    if (n) n->value.boolean = bval;
  } else if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_INT) == 0) {
    trim_trailing_ws_for_numeric_tag(text);
    long long ival;
    if (!try_parse_int_scalar(text, &ival)) {
      parse_err(ctx, "'%s' is not a valid !!int value", _cyaml_echo(text));
      _ccol_mem_free(ctx->mp, text);
      return NULL;
    }
    n = node_alloc(CYAML_INTEGER, ctx->mp);
    if (n) n->value.integer = ival;
  } else if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_FLOAT) == 0) {
    trim_trailing_ws_for_numeric_tag(text);
    double dval;
    if (!try_parse_float_scalar(text, &dval)) {
      parse_err(ctx, "'%s' is not a valid !!float value", _cyaml_echo(text));
      _ccol_mem_free(ctx->mp, text);
      return NULL;
    }
    n = node_alloc(CYAML_FLOAT, ctx->mp);
    if (n) n->value.number = dval;
  } else if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_STR) == 0) {
    n = string_node_adopting_text(ctx, &text);
  } else if (resolved_tag && (strcmp(resolved_tag, CYAML_TAG_SEQ) == 0 ||
                              strcmp(resolved_tag, CYAML_TAG_MAP) == 0)) {
    /* A !!seq or !!map tag on a scalar, the reverse of the structural kind
     * check that finalize_collection_node does for a scalar-only core-schema
     * tag on real collection syntax. A scalar can never satisfy either tag,
     * because the block or flow syntax of a collection fully decides its
     * type and the library never infers it. */
    parse_err(ctx, "tag '%s' does not match the node it decorates",
              _cyaml_echo(resolved_tag));
    _ccol_mem_free(ctx->mp, text);
    return NULL;
  } else if (implicit_ok) {
    /* No tag forces a type (no tag, a custom tag, or !!binary), so fall
     * through to ordinary implicit resolution. */
    n = make_typed_scalar(ctx, text);
  } else {
    /* A quoted or block scalar without a type-forcing tag is always a string,
     * with no implicit typing. */
    n = string_node_adopting_text(ctx, &text);
  }

  _ccol_mem_free(ctx->mp, text); /* A no-op when the node owns it. */

  if (!n) return NULL;

  if (resolved_tag) {
    char *tag_copy = strdup_charged(ctx->mp, resolved_tag);
    if (!tag_copy) {
      __cyaml_destroy((cyaml)n);
      return NULL;
    }
    n->tag = tag_copy;
  }

  return n;
}

/*
 * Attaches an explicit tag to a fully built collection node (a list or a
 * dictionary) after checking the tag against the structure. A tag can force
 * the type of a scalar through finalize_scalar_node, but a collection's type
 * is fully decided by its own block or flow syntax. So a tag here is either
 * a structural tag that matches (!!seq, !!map, or a custom tag that expects
 * no structure), or a real disagreement (any of the 5 scalar core-schema
 * tags, or a !!seq or !!map on the wrong kind of collection), which is a hard
 * parse failure.
 *
 * Each dispatch branch in parse_node that builds a collection calls this at
 * the exact point where it builds its result, following the same "as early as
 * possible" rule finalize_scalar_node follows for scalars, instead of waiting
 * until the recursive call of the enclosing tag returns. A tag can pass
 * through one or more '&' anchor layers, for example in
 * "!!seq &x\n  - 1\n  - 2\n", and the node carries its tag from the moment it
 * is built, so the node the anchor branch registers is already complete and
 * every later alias copies the tag along with it.
 *
 * `resolved_tag` is borrowed, exactly like the same parameter of
 * finalize_scalar_node: it is never owned here and always copied with
 * strdup. Taking ownership isn't allowed, because the same pointer may still
 * need to reach other collection-construction sites deeper in the same
 * recursion, through more '&' layers, after this call returns.
 *
 * Returns NULL on a structural disagreement and on an OOM, destroying `coll`
 * in both cases. On success it returns `coll` unchanged, with a strdup copy
 * of the tag in its ->tag field. A NULL `resolved_tag` is always a successful
 * no-op.
 */
static cyaml_node_t *finalize_collection_node(parse_ctx_t *ctx,
                                              cyaml_node_t *coll,
                                              const char *resolved_tag) {
  if (!resolved_tag || !coll) return coll;
  /* The non-specific "!" leaves a collection as its syntax made it and
   * attaches nothing. */
  if (strcmp(resolved_tag, _CYAML_TAG_NON_SPECIFIC) == 0) return coll;

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
              _cyaml_echo(resolved_tag), ctx->pos);
    __cyaml_destroy((cyaml)coll);
    return NULL;
  }

  char *tag_copy = strdup_charged(ctx->mp, resolved_tag);
  if (!tag_copy) {
    __cyaml_destroy((cyaml)coll);
    return NULL;
  }
  coll->tag = tag_copy;
  return coll;
}

/* Double-quoted scalar */

/* Encodes a code point as UTF-8 and appends it to b. */
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

/* Reads exactly 4 hex digits and decodes them into a Unicode code point,
 * moving ctx->pos forward by 4 on success. Returns false when the input is
 * too short or a digit isn't a hex digit. */
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
      parse_err(ctx, "invalid hex digit '%s' in \\uXXXX at position %zu",
                _cyaml_echo_c(c), ctx->pos + i);
      return false;
    }
    v = (v << 4) | d;
  }
  ctx->pos += 4;
  *out = v;
  return true;
}

/* Reads exactly 8 hex digits for a YAML \UXXXXXXXX escape, which names a code
 * point in a Unicode supplementary plane, moving ctx->pos forward by 8 on
 * success. */
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
      parse_err(ctx, "invalid hex digit '%s' in \\UXXXXXXXX at position %zu",
                _cyaml_echo_c(c), ctx->pos + i);
      return false;
    }
    v = (v << 4) | d;
  }
  ctx->pos += 8;
  *out = v;
  return true;
}

/*
 * The newline fold step that both quoted scalar styles share, as YAML 1.2
 * section 8.1.2 defines it. The caller only calls it when cur(ctx) is '\r' or
 * '\n'. First it removes the trailing inline whitespace of b, down to
 * strip_floor bytes but never past that. The double-quoted style passes its
 * own escape_boundary, because the output of an escape sequence is real
 * content and not source whitespace, while the single-quoted style has no
 * escapes at all and always passes 0. Then it folds the line break the same
 * way for both styles: into a literal newline when one or more blank
 * continuation lines follow (as at_blank_line() reports), and into one space
 * for an ordinary line break.
 *
 * On success it updates b, leaves ctx->pos just past the folded whitespace
 * and returns true. It returns false and sets ctx->error, naming style_name,
 * when it finds a document marker in the middle of a fold: that's an
 * unclosed quote running into a '---' or '...' boundary, which is never
 * literal content to fold across. On that path the caller cleans up
 * and free b itself, and the two callers do it differently, one with a goto
 * to a fail label and the other by freeing b and returning in place.
 */
static bool fold_quoted_newline(parse_ctx_t *ctx, ybuf_t *b, size_t strip_floor,
                                const char *style_name) {
  yb_trim_trailing_inline_ws(b, strip_floor);
  skip_newline(ctx);
  if (at_doc_marker(ctx)) {
    parse_err(ctx,
              "document marker inside unterminated %s scalar at position %zu",
              style_name, ctx->pos);
    return false;
  }
  /* A blank line may hold inline whitespace only, a lone tab for example.
   * at_blank_line(), unlike a plain cur(ctx) check, is what makes such a line
   * fold to a newline and not a space, as YAML 1.2 section 6.5 wants. */
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
 * Parses a double-quoted YAML scalar starting at the opening '"', handling
 * the YAML escape sequences, which are a superset of the JSON escapes. On
 * success it puts a heap string into *out and returns true.
 */
static bool parse_double_quoted(parse_ctx_t *ctx, char **out) {
  if (at_end(ctx) || cur(ctx) != '"') {
    parse_err(ctx, "expected '\"' at position %zu", ctx->pos);
    return false;
  }
  ctx->pos++;

  ybuf_t b;
  yb_init(&b, ctx->mp);

  /* How many bytes at the START of the buffer the code below must not remove
   * when it strips trailing whitespace before a fold. It is b.len at the end
   * of the most recent escape sequence. An escaped space or tab (a "\t"
   * escape, or a backslash right before a literal tab byte) is real content
   * the author asked for explicitly, not source whitespace sitting before a
   * line break. The "trailing white space is stripped" rule of YAML 1.2
   * section 8.1.2 only applies to literal, unescaped whitespace copied
   * straight from the source, never to a byte an escape sequence produced.
   * Without this value the next fold would silently remove a trailing
   * escaped tab. */
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
          /* A "\0" escape resolves to the code point U+0000. The shared comment
           * on the \\x, \\u and \\U cases below explains why this parser
           * can't append it as a real byte. */
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
          /* The \xXX escape, with 2 hex digits. */
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
          /* The scalar value of a node is a plain char* that ends with a NUL
           * byte and has no separate length (see the contract of
           * cyaml_str_val). If an escape decoded into a real NUL inside the
           * value, any later strlen(), printf() or serialize call would
           * silently truncate it, which is exactly the kind of data
           * corruption this library's no-NUL-in-a-value policy prevents.
           * percent_decode_tag_inplace applies the same rule to tag text. So
           * we refuse the escape instead of silently truncating the value. */
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
          /* A \u escape names a UTF-16 code unit. A high surrogate
           * (0xD800-0xDBFF) only means something as the first half of a pair:
           * a second \uXXXX escape with a low surrogate (0xDC00-0xDFFF) has to
           * follow right away, and the two name one code point in a
           * supplementary plane, as in JSON. A high surrogate without that
           * low surrogate, and a low surrogate on its own, name no Unicode
           * character, so the document is refused. */
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t next = 0;
            if (ctx->pos + 1 < ctx->len && ctx->src[ctx->pos] == '\\' &&
                ctx->src[ctx->pos + 1] == 'u') {
              ctx->pos += 2;
              if (!parse_hex4(ctx, &next)) goto fail;
            }
            if (next < 0xDC00 || next > 0xDFFF) {
              parse_err(ctx,
                        "\\u escape at position %zu names the high "
                        "surrogate U+%04X without a low surrogate escape "
                        "after it; a lone surrogate is not a Unicode "
                        "character",
                        esc_pos, (unsigned)cp);
              goto fail;
            }
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (next - 0xDC00u);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            parse_err(ctx,
                      "\\u escape at position %zu names the low surrogate "
                      "U+%04X without a high surrogate before it; a lone "
                      "surrogate is not a Unicode character",
                      esc_pos, (unsigned)cp);
            goto fail;
          }
          /* The \\x case above explains why a result that is a null byte must
           * be refused instead of silently appended. */
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
          /* A \U escape can name any 32-bit hex value, but only a value from 0
           * to 0x10FFFF outside the UTF-16 surrogate range 0xD800-0xDFFF is a
           * Unicode scalar value. Any other value names no character (and
           * encode_utf8() couldn't encode it either), so the document is
           * refused. */
          if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            parse_err(ctx,
                      "\\U escape at position %zu names 0x%08X, which is "
                      "not a Unicode scalar value (a surrogate, or above "
                      "U+10FFFF)",
                      esc_pos, (unsigned)cp);
            goto fail;
          }
          /* The \\x case above explains why a result that is a null byte has to
           * be refused instead of silently appended. */
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
          /* An escaped newline, which YAML 1.2 section 8.1.2 defines with the
           * s-double-escaped production, "\" b-non-content
           * l-empty(n,flow-in)* s-flow-line-prefix(n). Only the escaped break
           * ITSELF is non-content and adds nothing. An ordinary unescaped
           * break is s-flow-folded, which fold_quoted_newline() below
           * handles, and always folds to a space or to literal newlines. Each
           * l-empty blank line after the escaped break ends in a real
           * b-as-line-feed, just as in the at_blank_line() loop of
           * fold_quoted_newline(), so we append one literal '\n' per blank
           * line and skip only the very first break, the escaped one,
           * appending nothing for it. */
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
          parse_err(ctx, "unknown escape '\\%s' at position %zu",
                    _cyaml_echo_c(esc), ctx->pos - 1);
          goto fail;
      }
      /* The trailing-whitespace strip below must not remove whatever this
       * escape appended; see the doc comment of escape_boundary above. */
      escape_boundary = b.len;
      continue;
    }

    /* A newline inside a double-quoted scalar folds to a space, or to
     * newlines for one or more blank continuation lines. The doc comment of
     * escape_boundary above explains why the trailing-whitespace strip stops
     * at that boundary and not at 0. */
    if (c == '\r' || c == '\n') {
      if (!fold_quoted_newline(ctx, &b, escape_boundary, "double-quoted"))
        goto fail;
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
  _ccol_mem_free(b.m_procs, b.buf);
  return false;
}

/* Single-quoted scalar */

/*
 * Parses a single-quoted YAML scalar. This style has only one escape, two
 * single quotes in a row ('') for one literal single quote, and a newline
 * inside the scalar folds with the same rules as in the double-quoted style.
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

    /* The same fold_quoted_newline() step as in parse_double_quoted, with
     * strip_floor = 0: the single-quoted style has no escapes at all, so
     * there's no protected span like escape_boundary to stop at. */
    if (c == '\r' || c == '\n') {
      if (!fold_quoted_newline(ctx, &b, 0, "single-quoted")) {
        _ccol_mem_free(b.m_procs, b.buf);
        return false;
      }
      continue;
    }

    yb_append_c(&b, c);
    ctx->pos++;
  }

  parse_err(ctx, "unterminated single-quoted scalar");
  _ccol_mem_free(b.m_procs, b.buf);
  return false;

done:
  if (b.oom) {
    _ccol_mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

/* Block scalar (literal | and folded >) */

typedef enum { CHOMP_CLIP, CHOMP_STRIP, CHOMP_KEEP } chomp_t;

/*
 * Parses the header of a block scalar, after the caller has read the style
 * indicator (| or >). The header sits on the same line and may hold an
 * optional chomp indicator (- or +), an optional explicit indent indicator
 * (one digit from 1 to 9), or both.
 *
 * Sets *chomp and *explicit_indent; an *explicit_indent of 0 means the header
 * gave no indent indicator.
 */
static bool parse_block_scalar_header(parse_ctx_t *ctx, chomp_t *chomp,
                                      int *explicit_indent) {
  *chomp = CHOMP_CLIP;
  *explicit_indent = 0;

  /* The header may hold a chomp indicator and an indent indicator in either
   * order, but c-b-block-header allows at most ONE of each kind. had_chomp
   * and had_indent refuse a second one of either kind, which is a duplicate
   * or a contradiction ("|--", "|+-" and "|24", for example). Without them
   * the later indicator would silently overwrite the earlier one. */
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

  /* A trailing comment needs whitespace before the '#', as YAML 1.2's
   * s-b-comment production says: the comment text can only be reached
   * through the optional s-separate-in-line group in front of it. It's the
   * same general "a comment has to follow whitespace" rule that
   * skip_ws_comments applies elsewhere. '>#comment' has nothing between the
   * header and the '#', so it's no comment but an invalid character at the
   * end of the header. */
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

/* The result of scan_block_scalar_line, telling the caller what to do with
 * the line the scan has just classified. parse_block_scalar_content (for the
 * literal | style) and parse_folded_scalar_content (for the folded > style)
 * share it. */
typedef enum {
  SCAN_BLOCK_LINE_ERROR,  /* ctx->error is already set; the caller
                              cleans up and returns false. */
  SCAN_BLOCK_LINE_END,    /* The scalar ends before this line,
                              either a first line that isn't blank and
                              isn't indented past parent_indent, or a later
                              line less indented than block_indent. The scan
                              rewinds ctx->pos to the start of this line, so
                              the caller's caller sees it as input nobody
                              has read yet. */
  SCAN_BLOCK_LINE_BLANK,  /* An empty line, which before
                              indent_determined is also a line the scan
                              can't classify yet. *spaces_out means nothing
                              here, because both callers already know that a
                              blank line's extra indentation is zero, or
                              don't need it. ctx->pos sits exactly at the end
                              of the line (the end of the input, a '\r', or a
                              '\n'), and the caller moves past it in
                              its own way. */
  SCAN_BLOCK_LINE_CONTENT /* A real content line. *spaces_out gets
                              the number of leading spaces the scan has
                              already read, which is >= block_indent, and
                              ctx->pos sits just past them, ready for the
                              caller to read the line's text. */
} scan_block_line_t;

/*
 * Measures the leading spaces of one block scalar line and classifies the
 * line, updating indent_determined, block_indent and max_leading_blank_spaces
 * exactly as parse_block_scalar_content and parse_folded_scalar_content both
 * need. Three rules are enforced here once, so no hand-written copy in each
 * caller needs to be kept in step: YAML 1.2 section 6.1's rule that a tab as
 * indentation is ambiguous while the indent is undetermined, section 8.1.1's
 * rule that no leading empty line may be more indented than the first
 * non-empty line, and the auto-detection of the indent from the first
 * content line.
 *
 * On purpose this does not also unify what happens AFTER the classification:
 * committing buffered blank lines, reading the text of a content line, and
 * moving past the line's trailing newline. The two callers do that work
 * differently enough (one streams into a single ybuf_t, the other collects
 * into a line_t array and folds it afterwards) that a shared implementation
 * would have to bring that difference back as internal branches, and those
 * would hide the tab and indentation rules this helper exists to state
 * exactly once, for little gain.
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
  /* Clamp it, since without the clamp a line with more than INT_MAX leading
   * spaces would silently overflow a signed int. current_col() clamps the
   * same way, and its doc comment explains why. */
  int spaces = space_count > (size_t)INT_MAX ? INT_MAX : (int)space_count;

  /* The doc comment of parse_block_scalar_content explains the YAML 1.2
   * section 6.1 reason in more detail. A tab as the very first character of
   * this line while block_indent is undetermined is truly ambiguous and a
   * hard error. Once the scan has read even one real space, or once
   * block_indent is fixed, a tab is ordinary content, which the caller's
   * content-reading step handles. */
  if (!*indent_determined && spaces == 0 && !at_end(ctx) && cur(ctx) == '\t') {
    parse_err(ctx,
              "tab cannot be used as block scalar indentation at "
              "position %zu",
              ctx->pos);
    return SCAN_BLOCK_LINE_ERROR;
  }

  /* A document-start marker ('---') or document-end marker ('...') at column 0
   * always ends the block scalar, because YAML 1.2 section 6.9 makes a
   * document marker c-forbidden, so it's never valid scalar content. This
   * check must come before the indent determination and the content-or-end
   * decision below: the block indent can be auto-detected as exactly 0, for a
   * scalar at the document root whose content sits at column 0, and a marker
   * line then has spaces == 0 too. Without this check that code would take
   * the marker line as one more content line and silently pull the marker
   * into the scalar text instead of ending the scalar and the document. Every
   * other block content parser in this file (parse_block_dictionary,
   * parse_one_document, parse_plain_scalar_multiline, and the two
   * quoted-scalar folders) guards against this with at_doc_marker(), and this
   * one must as well. at_doc_marker() already requires column 0, so the
   * check does nothing whenever spaces > 0, since a more indented line can
   * never be a marker. */
  if (spaces == 0 && at_doc_marker(ctx)) {
    ctx->pos = line_start;
    return SCAN_BLOCK_LINE_END;
  }

  /* Once block_indent is known, a line with MORE spaces than block_indent is
   * never "blank" for chomping and folding, even when it has no other
   * content. A trailing blank line may be chomped or folded, but YAML 1.2's
   * l-strip-empty production only matches such a line when its indentation
   * is AT MOST block_indent; a more indented line is ordinary literal content
   * even if it holds only whitespace. While the indent is undetermined a
   * leading blank line is always a candidate; the error check below runs
   * once the indent IS determined and guarantees that every leading blank
   * line's indentation was already <= the final block_indent. */
  bool is_blank =
      at_eol(ctx) && (!*indent_determined || spaces <= *block_indent);

  if (is_blank) {
    if (!*indent_determined && spaces > *max_leading_blank_spaces)
      *max_leading_blank_spaces = spaces;
    return SCAN_BLOCK_LINE_BLANK;
  }

  if (!*indent_determined) {
    if (spaces <= parent_indent) {
      /* The first non-blank line isn't more indented than the parent, so the
       * scalar is empty. */
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
    /* This line is less indented than the block, so the scalar ends here. */
    ctx->pos = line_start;
    return SCAN_BLOCK_LINE_END;
  }
  *spaces_out = spaces;
  return SCAN_BLOCK_LINE_CONTENT;
}

/*
 * Reads the content of a block scalar.
 *
 * parent_indent: the indentation level of the block container holding this
 *   scalar, used as the base for the block indent when the header gives no
 *   explicit indent.
 *
 * On success *out gets a heap string, which the caller must free with
 * ctx->mp, and the function returns true.
 */
static bool parse_block_scalar_content(parse_ctx_t *ctx, int parent_indent,
                                       chomp_t chomp, int explicit_indent,
                                       char **out) {
  /*
   * Determine the block indent: relative to the parent when the header gave
   * an explicit indicator, or otherwise auto-detected from the first
   * non-empty content line.
   */
  int block_indent = 0;
  bool indent_determined = (explicit_indent > 0);
  if (indent_determined) {
    /* The explicit indent indicator is always relative to an effective parent
     * indentation of at least 0. The value -1 is only a sentinel for "no
     * enclosing block context" (the document root), which lets an
     * auto-detected indentation be 0 in the code below; the indicator
     * arithmetic has no such negative-base case, as a reference parser
     * confirms. */
    int effective_parent = parent_indent < 0 ? 0 : parent_indent;
    block_indent = effective_parent + explicit_indent;
  }

  ybuf_t b;
  yb_init(&b, ctx->mp);

  /* Holds every trailing line not committed yet: for each blank line its
   * extra indentation past block_indent followed by a '\n', and also the
   * single '\n' that ends the most recent content line. Those bytes wait here
   * until either more content arrives, and then the buffer is copied into b
   * byte for byte, extra spaces of each line included, or the scalar ends and
   * the chomp code below deals with the buffer. Delaying them is what lets
   * the chomp mode decide what happens to these trailing lines only after
   * the whole scalar is read: CHOMP_KEEP keeps them all, CHOMP_CLIP reduces
   * them to at most one newline, and CHOMP_STRIP drops them all. */
  ybuf_t pending;
  yb_init(&pending, ctx->mp);
  bool have_content = false;

  /* True when a REAL line break character follows the most recent line
   * (blank or content) in the source, and false when the input ends right
   * there. YAML 1.2's b-chomped-last production, "b-as-line-feed | <end of
   * file>", governs the very last line of the scalar under every chomp mode,
   * CLIP and KEEP included. When the source has no line break at all after
   * the scalar's last line, nothing is added for that line, not even a
   * newline a chomp mode would otherwise want: CLIP and KEEP must not invent
   * a newline the input never had. */
  bool had_trailing_newline = false;

  /* Like had_trailing_newline, but only updated for a real CONTENT line and
   * never for a trailing blank line, because YAML 1.2's b-chomped-last
   * production governs the break after the very last TEXT line of the
   * scalar, not whatever trailing blank line got handled last.
   * had_trailing_newline alone can't tell two cases apart, since every line,
   * blank or not, overwrites it: in one, the last content line had a real
   * break and is followed by a blank line with no break of its own that ends
   * at the end of the input; in the other, the last content line had no
   * break. CLIP is the only mode that needs that difference, and it reads
   * this variable instead. */
  bool content_had_trailing_newline = false;

  /* The largest indent of any LEADING empty line so far, while the block's
   * indent is being auto-detected (explicit_indent <= 0). YAML 1.2 section
   * 8.1.1 says: "It is an error for any of the leading empty lines to
   * contain more spaces than the first non-empty line." It only means
   * something before indent_determined; once real content or an explicit
   * indicator fixes block_indent, a blank line's indentation has no limit. */
  int max_leading_blank_spaces = -1;

  while (!at_end(ctx)) {
    size_t line_start = ctx->pos;
    int spaces = 0;
    scan_block_line_t kind = scan_block_scalar_line(
        ctx, parent_indent, &indent_determined, &block_indent,
        &max_leading_blank_spaces, &spaces);

    if (kind == SCAN_BLOCK_LINE_ERROR) {
      _ccol_mem_free(b.m_procs, b.buf);
      _ccol_mem_free(pending.m_procs, pending.buf);
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

    /* SCAN_BLOCK_LINE_CONTENT. Commit every buffered blank line and newline
     * before this one, keeping each one's extra indentation. */
    yb_append_buf(&b, &pending);
    pending.len = 0;
    have_content = true;

    /* Skip exactly block_indent spaces. The scan above has already read them,
     * but it may have read more than block_indent. */
    int extra = spaces - block_indent;

    /* Append the extra leading spaces of a more indented line. */
    for (int i = 0; i < extra; i++) yb_append_c(&b, ' ');

    /* Read this line's content. */
    while (!at_end(ctx) && !at_eol(ctx)) {
      yb_append_c(&b, cur(ctx));
      ctx->pos++;
    }

    /* This line's trailing newline is delayed exactly like a blank line's,
     * because this may be the scalar's very last line and only the chomp mode
     * decides what happens to it (see the doc comment of
     * had_trailing_newline above). When the source has no real line break
     * here at all because the input ends, nothing is delayed for this
     * line. */
    had_trailing_newline = !at_end(ctx);
    content_had_trailing_newline = had_trailing_newline;
    if (had_trailing_newline) yb_append_c(&pending, '\n');
    skip_newline(ctx);
  }

  /* Apply the chomp mode. */
  switch (chomp) {
    case CHOMP_STRIP:
      /* Keep no trailing newline, so all of pending is dropped. */
      break;
    case CHOMP_CLIP:
      /* Keep one trailing newline, but only when the scalar had content AND the
       * LAST CONTENT line really had a line break to keep (see the doc
       * comment of content_had_trailing_newline). The last text line can also
       * be the very last byte of the input with no line break at all, and
       * then the content gets no newline, even under CLIP. A trailing blank
       * line without a break of its own must not stop this newline, since the
       * content line before it did have one. Either way the contents of
       * pending, which may hold the extra indentation of several blank lines,
       * are dropped. */
      if (have_content && content_had_trailing_newline) yb_append_c(&b, '\n');
      break;
    case CHOMP_KEEP:
      /* Keep every trailing line, including the lines before the first
       * content line and the extra indentation of each: YAML 1.2 section
       * 8.1.1.2 says trailing empty lines are part of the scalar's content
       * whether or not any non-empty line appears. pending already leaves out
       * a final '\n' with no real line break behind it (see the doc comment
       * of had_trailing_newline), so no separate check is needed here. */
      yb_append_buf(&b, &pending);
      break;
  }

  bool pending_oom = pending.oom;
  _ccol_mem_free(pending.m_procs, pending.buf);

  if (b.oom || pending_oom) {
    _ccol_mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

/*
 * A simple parser for the content of a folded block scalar.
 *
 * The folded style folds a single newline between two content lines into a
 * space, keeps a blank line as a literal newline, and also keeps the newline
 * before and after a more indented line.
 *
 * It works in two steps over the literal result: first it collects the
 * lines, then it folds them by YAML 1.2's folding rules.
 */
static bool parse_folded_scalar_content(parse_ctx_t *ctx, int parent_indent,
                                        chomp_t chomp, int explicit_indent,
                                        char **out) {
  int block_indent = 0;
  bool indent_determined = (explicit_indent > 0);
  if (indent_determined) {
    /* Same as the comment in parse_block_scalar_content: -1 is only a
     * sentinel for auto-detection at the document root, not a real base for
     * the indicator arithmetic. */
    int effective_parent = parent_indent < 0 ? 0 : parent_indent;
    block_indent = effective_parent + explicit_indent;
  }

  /* Collect the lines into a dynamic array of {content, extra_indent,
   * is_blank}. text and text_len are a borrowed span straight into ctx->src,
   * not an owned copy. By the time the scan below reaches a content line,
   * ctx->pos has already moved past that line's leading spaces: the mandatory
   * block_indent part and also any "extra" part of a more indented line,
   * which YAML 1.2 section 8.1.3 makes real scalar content. So the whole
   * span, extra spaces included, already sits in ctx->src as one unbroken,
   * unchanged run of bytes, and nothing changes or frees ctx->src before this
   * function returns. A borrowed span saves one allocate, copy and free cycle
   * per line, and a wrapped scalar can have many thousands of lines.
   * fold_quoted_newline uses the same reasoning for quoted-scalar folding:
   * don't copy while the source stays stable. */
  typedef struct {
    const char *text;
    size_t text_len;
    int extra;
    bool blank;
    bool spaced;
  } line_t;

  size_t lines_cap = 16, lines_len = 0;
  line_t *lines = _ccol_mem_alloc(ctx->mp, lines_cap * sizeof(line_t));
  if (!lines) return false;

  /* The same counter as in parse_block_scalar_content: YAML 1.2 section
   * 8.1.1 forbids a leading empty line that is more indented than the first
   * non-empty line, a rule that applies while the block's indent is being
   * auto-detected. */
  int max_leading_blank_spaces = -1;

  /* See the doc comment of had_trailing_newline in
   * parse_block_scalar_content. This describes the line the loop collected
   * most recently, so once the loop below ends it describes the scalar's
   * true final line. */
  bool had_trailing_newline = false;

  while (!at_end(ctx)) {
    size_t line_start = ctx->pos;
    int spaces = 0;
    scan_block_line_t kind = scan_block_scalar_line(
        ctx, parent_indent, &indent_determined, &block_indent,
        &max_leading_blank_spaces, &spaces);

    if (kind == SCAN_BLOCK_LINE_ERROR) {
      _ccol_mem_free(ctx->mp, lines);
      return false;
    }
    if (kind == SCAN_BLOCK_LINE_END) break;

    bool is_blank = (kind == SCAN_BLOCK_LINE_BLANK);

    /* Grow the lines array when it's full. */
    if (lines_len == lines_cap) {
      lines_cap *= 2;
      line_t *tmp =
          _ccol_mem_realloc(ctx->mp, lines, lines_cap * sizeof(line_t));
      if (!tmp) {
        _ccol_mem_free(ctx->mp, lines);
        return false;
      }
      lines = tmp;
    }

    line_t *ln = &lines[lines_len++];
    ln->blank = is_blank;
    /* A truly blank line never has an extra indent of its own, because the
     * definition of is_blank above already excludes every line more indented
     * than block_indent and treats it as ordinary content. So a blank line is
     * either a leading blank line while the indent is still undetermined,
     * whose indentation the error check above guarantees was already <= the
     * final block_indent, or a blank line after the content or between two
     * content lines, whose indentation the definition of is_blank already
     * makes <= block_indent. */
    ln->extra = is_blank ? 0 : spaces - block_indent;
    /* YAML 1.2 section 8.1.3 defines a "more-indented" line with the
     * s-nb-spaced-text production, s-indent(n) s-white nb-char*, where
     * s-white is a space OR a tab. So the first byte past the mandatory
     * block_indent spaces can be a tab, and such a line is also "spaced" for
     * the fold-or-newline decision. It adds no extra literal SPACE of its
     * own, so ln->extra stays 0, since the leading-space counter above only
     * counts ' '. The tab itself becomes ordinary line content below whatever
     * this flag says; only the fold decision further down reads the flag. */
    ln->spaced =
        !is_blank && (ln->extra > 0 || (!at_end(ctx) && cur(ctx) == '\t'));
    ln->text = NULL;
    ln->text_len = 0;

    if (!is_blank) {
      /* As the contract of scan_block_scalar_line says, ctx->pos already sits
       * ln->extra bytes past where this line's "extra" leading spaces (those
       * past block_indent) begin. So those spaces and the real content right
       * after them are already one unbroken, unchanged run of bytes in
       * ctx->src (see the doc comment of line_t above), span_start is
       * ln->extra bytes behind ctx->pos, and no copy is needed. */
      const char *span_start = ctx->src + (ctx->pos - (size_t)ln->extra);
      while (!at_end(ctx) && !at_eol(ctx)) ctx->pos++;
      ln->text = span_start;
      ln->text_len = (size_t)((ctx->src + ctx->pos) - span_start);
    }
    /* See the doc comment of had_trailing_newline in
     * parse_block_scalar_content. The question is whether a real line break
     * follows THIS line in the source or the input ends right there, which
     * matters for whichever line turns out to be the scalar's very last. */
    had_trailing_newline = !at_end(ctx);
    ctx->pos = line_start;
    skip_to_eol(ctx);
  }

  /* Now fold the lines into the output buffer. */
  ybuf_t b;
  yb_init(&b, ctx->mp);

  size_t trailing_blanks = 0;
  bool have_content = false;
  /* True when the previous non-blank line was more indented. */
  bool prev_extra = false;

  for (size_t i = 0; i < lines_len; i++) {
    line_t *ln = &lines[i];
    if (ln->blank) {
      trailing_blanks++;
      continue;
    }

    if (trailing_blanks > 0) {
      /* Blank lines come before this line, leading ones or ones between two
       * content lines. Write each one's extra indentation, if it has any, as
       * literal spaces right before that line's newline. */
      for (size_t j = i - trailing_blanks; j < i; j++) {
        for (int k = 0; k < lines[j].extra; k++) yb_append_c(&b, ' ');
        yb_append_c(&b, '\n');
      }
      /* YAML 1.2 section 8.1.3 says that moving into or out of a "spaced" run
       * of more indented lines always costs one literal newline ON TOP OF the
       * blank lines separating the two chunks: it adds to the blank lines'
       * newlines written above instead of replacing them, as a reference
       * parser confirms. The rule only applies when a content line comes
       * before this break so the break can sit next to it, and the leading
       * blank lines before the scalar's very first content line have no such
       * line before them, so have_content guards this check, just as it
       * guards the same check in the other branch below. */
      if (have_content && (ln->spaced || prev_extra)) yb_append_c(&b, '\n');
    } else if (have_content) {
      /* As YAML 1.2 section 8.1.1.2 says, a line break next to a more indented
       * line (the current one, or the previous non-blank one) stays a
       * newline, and any other break folds to a space. */
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

  /* Apply the chomp mode. lines[], with each line's extra indentation, must
   * stay alive for CHOMP_KEEP's use of it below, so it's freed after this
   * switch and not right after the fold. */
  switch (chomp) {
    case CHOMP_STRIP:
      break;
    case CHOMP_CLIP:
      /* trailing_blanks > 0 means at least one blank line follows the last real
       * content line, which is only possible when a real line break already
       * separated them in the source. So that content line's break must be
       * kept even when the LAST trailing blank line has none of its own. Every
       * line, trailing blank lines included, overwrites had_trailing_newline,
       * so that value alone can't tell the two cases apart; the KEEP mode
       * below has the same check. See the doc comment of had_trailing_newline
       * in parse_block_scalar_content for the case honored here: no newline
       * at all when the source had none. */
      if (have_content && (trailing_blanks > 0 || had_trailing_newline))
        yb_append_c(&b, '\n');
      break;
    case CHOMP_KEEP:
      /* First write the newline that ends the last real content line, which is
       * always there when a trailing blank line follows it: only the scalar's
       * true final line can lack a real line break in the source, and when a
       * trailing blank line exists, that blank line is the final line and not
       * this content line. */
      if (have_content && (trailing_blanks > 0 || had_trailing_newline))
        yb_append_c(&b, '\n');
      /* Then write each trailing blank line, i.e. each lines[] entry after the
       * last content line, which the main fold loop above never wrote because
       * no further content line came along to trigger it. Each one gets its
       * extra indentation and then its newline; only the very last one
       * depends on had_trailing_newline, for the same reason as above. */
      for (size_t j = lines_len - trailing_blanks; j < lines_len; j++) {
        for (int k = 0; k < lines[j].extra; k++) yb_append_c(&b, ' ');
        if (j + 1 < lines_len || had_trailing_newline) yb_append_c(&b, '\n');
      }
      break;
  }

  /* Free the lines[] array itself. Each entry's text and text_len are a
   * borrowed span into ctx->src (see the doc comment of line_t above) and not
   * an owned allocation, so there's nothing to free per entry. */
  _ccol_mem_free(ctx->mp, lines);

  if (b.oom) {
    _ccol_mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

/* Plain scalar (block and flow context) */

/*
 * Scans the plain-scalar content of one physical line from the current
 * position and appends the characters to *b, which the caller has already
 * initialized. parse_plain_scalar (for the first line) and
 * parse_plain_scalar_multiline (for every continuation line) share it, so the
 * same rules end a token at each character on a continuation line as on the
 * first line.
 *
 * line_start is where the content scan of this line began. It's needed so
 * that a ':' at the very first scanned character isn't taken as a character
 * with content before it. Only the '#' comment check below uses it, and that
 * check doesn't apply to ':'.
 *
 * Returns true, leaving ctx->pos exactly at that character without reading
 * it, when the scan hits a real end of the token: an inline comment, a ':',
 * or a flow character that indicates a value or a collection. Returns false,
 * leaving ctx->pos there, when the line ended at the end of the line or the
 * end of the input.
 */
_CYAML_PARSE_HOT bool scan_plain_scalar_line(parse_ctx_t *ctx, bool in_flow,
                                             size_t line_start, ybuf_t *b) {
  while (!at_end(ctx)) {
    char c = cur(ctx);

    /* The end of the line ends this line, not the whole scalar. */
    if (c == '\n' || c == '\r') return false;

    /* An inline comment (a space followed by a '#') ends the token. */
    if (c == '#' && ctx->pos > line_start) {
      char prev = ctx->src[ctx->pos - 1];
      if (prev == ' ' || prev == '\t') return true;
    }

    /* A colon followed by a space, or a colon at the end of the line, ends
     * the token, since the colon is a dictionary's value indicator. */
    if (c == ':') {
      if (ctx->pos + 1 >= ctx->len) return true; /* colon at very end */
      char next = ctx->src[ctx->pos + 1];
      if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
        return true;
      /* In a flow context a ':' only belongs to the scalar when an
       * ns-plain-safe(c) character follows it, and every flow indicator is
       * outside that set; the check above already handles whitespace. */
      if (in_flow) {
        char safe = ctx->src[ctx->pos + 1]; /* pos+1 < len guaranteed above */
        if (safe == ',' || safe == ']' || safe == '}' || safe == '[' ||
            safe == '{')
          return true;
      }
    }

    /* In a flow context every flow indicator ends the scalar, because YAML
     * 1.2 section 7.3.3's ns-plain-safe(c) excludes all five of them. A ',',
     * ']' or '}' is the collection's next token, while a '[' or '{' is a
     * token that no flow entry may hold at that point, so the collection
     * frame refuses it as a missing ','. */
    if (in_flow && (c == ',' || c == ']' || c == '}' || c == '[' || c == '{'))
      return true;

    yb_append_c(b, c);
    ctx->pos++;
  }
  return false; /* reached end of input, not a real terminator */
}

/*
 * Parses a plain scalar on one line. The scalar ends at the end of the line,
 * at an inline comment (' #'), or at a value indicator (': ' or ':\n').
 *
 * In a flow context (in_flow=true) the scalar also ends at ',', ']' and '}',
 * and at a ':' followed by whitespace or a flow terminator. As YAML 1.2
 * section 7.3.3 says, a bare ':' with none of those after it is part of the
 * scalar, as in a URL like "http://example.com".
 *
 * When hit_real_terminator isn't NULL it reports how the scalar stopped: true
 * for a real terminator, false for the end of the line or of the input.
 * Callers that try a multi-line continuation (see
 * parse_plain_scalar_multiline) only try it in the false case. An implicit
 * dictionary key always stays on one line and must never try a continuation,
 * whatever follows it.
 */
_CYAML_PARSE_HOT bool parse_plain_scalar(parse_ctx_t *ctx, bool in_flow,
                                         char **out,
                                         bool *hit_real_terminator) {
  ybuf_t b;
  yb_init(&b, ctx->mp);

  size_t start = ctx->pos;
  bool terminated = scan_plain_scalar_line(ctx, in_flow, start, &b);
  if (hit_real_terminator) *hit_real_terminator = terminated;

  /* Trim trailing spaces. */
  yb_trim_trailing_inline_ws(&b, 0);

  if (b.oom) {
    _ccol_mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

/*
 * Extends the already-read first line of a plain scalar with more
 * continuation lines, as the ns-plain-multi-line production of YAML 1.2
 * section 7.3.3 defines. It is only called from a value position or a
 * standalone-node position, and only after the parser has made sure the
 * first line does NOT introduce a dictionary key. An implicit key always
 * stays on one line (see the doc comment of parse_plain_scalar), so the
 * parser needs to look at the first line for a following ':' before it can
 * decide whether to try a continuation at all.
 *
 * It takes ownership of first_owned, the first line's text that
 * parse_plain_scalar has already read. first_terminated says whether that
 * call hit a real terminator or just the end of the line or input, and a
 * continuation is only tried in the second case. On success *out gets the
 * final text, which is first_owned itself, unchanged, when no continuation
 * applied.
 *
 * When the first line ended at the end of the line or the input, every
 * following blank line is read, and so is every line indented more than
 * `indent`, the enclosing block's indent that parse_node and
 * parse_block_map_node already pass through. So a value continuation is
 * never mistaken for an unrelated sibling entry at or below that indent.
 *
 * The fold rules differ from a '>' folded block scalar: a plain scalar's
 * line-fold grammar is s-flow-folded, which has no exception that lets a
 * more indented line keep its own newline. Every step from one line to the
 * next folds to a single space, and a run of blank lines folds to one
 * newline per blank line, whatever the extra indent of a continuation line
 * is, so a continuation line's leading whitespace past `indent` isn't kept.
 *
 * The continuation stops and reads nothing more in three cases: a document
 * marker ('---' or '...') at column 0, a line indented at or below `indent`,
 * and a continuation line that hits a real terminator itself, the same rules
 * that end the first line.
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
  _ccol_mem_free(ctx->mp, first_owned);

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
    /* Clamp it: without the clamp a continuation line with more than INT_MAX
     * leading spaces would silently overflow a signed int. current_col()
     * clamps the same way, and so does scan_block_scalar_line for its own
     * separate leading-space counter. */
    int spaces =
        line_space_count > (size_t)INT_MAX ? INT_MAX : (int)line_space_count;

    if (at_end(ctx)) {
      ctx->pos = save;
      break;
    }
    if (at_blank_line(ctx)) {
      /* A blank line counts toward the fold, so the scan goes on. The rest of
       * such a line's whitespace can be a tab instead of more spaces, and
       * at_blank_line() still sees the line as blank then, while a bare
       * at_eol() check on the byte right after the leading spaces can't see
       * past the tab to the real line break behind it. Double-quoted and
       * single-quoted folding handle it the same way (see the doc comment of
       * at_blank_line). Below we move past the remaining whitespace, up to
       * but not past the line terminator, so the skip_newline() at the top of
       * the next iteration reads exactly one line break. Otherwise it would
       * read this line's terminator and then the leading bytes of the next
       * line. */
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
    /* The block needs its own indentation, which is spaces only, and more
     * separator whitespace (spaces, tabs, or both) can follow it: YAML 1.2's
     * s-separate-in-line is s-white+, and s-white is a space or a tab. That
     * whitespace isn't scalar content, so we skip it; the code above has
     * already read every indentation space, however far past indent they
     * went. Without this skip scan_plain_scalar_line() below would get a tab
     * here and take it as a literal content byte. */
    while (!at_end(ctx) && (cur(ctx) == ' ' || cur(ctx) == '\t')) ctx->pos++;
    if (in_flow && (cur(ctx) == ']' || cur(ctx) == '}' || cur(ctx) == ',' ||
                    cur(ctx) == '[' || cur(ctx) == '{')) {
      /* A flow indicator can never be content of a flow plain scalar, so a line
       * whose first non-whitespace character is one of them is the end of the
       * enclosing flow collection or entry and not a continuation of this
       * scalar. "baz\n]", for example, gives the value "baz" and not
       * "baz " with a trailing fold space taken from a ']' that was never
       * content, and "[a\n[b]]" leaves the '[' for the collection frame to
       * refuse. */
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
        /* This ':' value indicator sits on a new line and nothing else was read
         * from that line first, so it isn't a continuation of this scalar: an
         * implicit key's ':' has to stay on the SAME line as the key's last
         * real content. Two independent reference parsers agree, rejecting
         * "{k\n: v}" and accepting "{k: \nv}", where the colon follows the key
         * right away on its own line and only the VALUE folds onto the next
         * line. We leave the ':' to the caller's key-detection logic, which
         * must see the input as if this multi-line attempt had never looked
         * past the previous line. */
        ctx->pos = save;
        break;
      }
    }
    if (cur(ctx) == '#') {
      /* A comment line is one whose first non-whitespace character is a '#',
       * and that can never be plain scalar content on its own, here or
       * anywhere else, so a comment line isn't a continuation of this scalar.
       * We leave it for the caller's ordinary skip_ws_comments to read
       * normally, and that code then sees the input as if this multi-line
       * attempt had never looked past the previous line. */
      ctx->pos = save;
      break;
    }

    /* A real continuation line. */
    if (blank_count > 0) {
      for (size_t i = 0; i < blank_count; i++) yb_append_c(&b, '\n');
    } else {
      yb_append_c(&b, ' ');
    }
    blank_count = 0;

    /* Scan this line's content straight into the shared buffer b instead of a
     * throwaway ybuf_t per line, with the same strip_floor trick that
     * fold_quoted_newline uses for double-quoted and single-quoted folding,
     * so a long wrapped scalar needs no allocate, copy and free cycle per
     * continuation line. line_floor marks where this line's content starts
     * inside b, right after the fold separators appended above, so the
     * trailing-whitespace trim below can never reach back into an earlier
     * line's already folded content. */
    size_t line_floor = b.len;
    bool cont_terminated = scan_plain_scalar_line(ctx, in_flow, line_start, &b);
    yb_trim_trailing_inline_ws(&b, line_floor);

    if (cont_terminated) break;
  }

  if (b.oom) {
    _ccol_mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

/* A forward declaration. The function is defined below next to
 * parse_flow_dictionary, one of its users, but try_parse_scalar_dict_key,
 * which handles an alias as a key and a flow collection as a key, needs it
 * here too. */
static char *node_to_dict_key_string(parse_ctx_t *ctx, cyaml_node_t *key_node,
                                     const char *context_label);
static char *node_to_dict_key_string_ex(parse_ctx_t *ctx,
                                        cyaml_node_t *key_node,
                                        const char *context_label,
                                        bool consume);

/* A forward declaration of the canonical-mode variant of the flow
 * serializer, which is defined further down next to that serializer. We need
 * it here because node_to_dict_key_string, its only caller in this file,
 * comes well before that section. */
static char *serialize_flow_canonical(cyaml_node_t *n, bool *too_long);

/* Forward declarations that try_parse_scalar_dict_key needs for handling a
 * flow collection as a key. A flow list or flow dictionary is unambiguous on
 * its own and, unlike a bare plain scalar, needs no "was this a key after
 * all" rewind. Calling these two directly here instead of through the
 * general parse_node dispatch is on purpose: it skips parse_node's
 * key-versus-value ambiguity handling entirely, which doesn't apply to a
 * flow collection at all. */
static cyaml_node_t *parse_flow_list(parse_ctx_t *ctx, int indent);
static cyaml_node_t *parse_flow_dictionary(parse_ctx_t *ctx, int indent);

/*
 * Whether the current position is a real value continuation for a block
 * construct governed by `indent`, or a position that belongs to a sibling
 * entry, in which case this construct's value or content is absent, i.e.
 * null. The caller must already have skipped the leading whitespace, at the
 * start of a non-blank line. The answer is true whenever the position is
 * more indented than `indent`.
 *
 * The flag seq_ok_at_indent also allows exactly `indent` itself, but only
 * when a block sequence entry ('- ') starts there. YAML 1.2 section 8.2.2
 * gives this one exception to "a value must be more indented than its key",
 * and only for the value of a MAPPING key. The caller must pass true only
 * when `indent` is a mapping's indent (parse_block_map_node's map_indent, or
 * that value passed on through an anchor or a tag decorating the value), and
 * never for a sequence's indent (parse_block_list's seq_indent): at exactly
 * a sequence's indent a further '-' is a sibling element of that same
 * sequence and not nested content, whatever anchor or tag sits between
 * them.
 */
static bool at_block_value_col(parse_ctx_t *ctx, int indent,
                               bool seq_ok_at_indent) {
  /* A tab in the leading whitespace is separation, so only the spaces
   * before it count; see line_space_indent(). */
  int col = line_space_indent(ctx);
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
 * True when the current position holds a bare '-' block sequence indicator,
 * immediately followed by whitespace or the end of the input, as opposed to
 * a '-' that just starts ordinary plain scalar content such as "-1" or
 * "-foo".
 */
static bool at_bare_seq_indicator(parse_ctx_t *ctx) {
  if (at_end(ctx) || cur(ctx) != '-') return false;
  char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
  return nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0';
}

/*
 * True when the current character is a valid first character of a plain
 * scalar in a flow context, as YAML 1.2 section 6.6's ns-plain-first(c)
 * defines it. Every character is valid except '-' and '?', which also need an
 * ns-plain-safe(c) character right after them, i.e. one that may itself
 * appear INSIDE a flow plain scalar. Whitespace, the end of a line or of the
 * input, and any flow indicator (',', '[', ']', '{' and '}') fail that rule,
 * so a bare "-" or "?" directly followed by one of them has no valid plain
 * scalar reading in a flow context, like the lone "-" in "[-, -]" or "[-]". A
 * reference parser confirms this.
 *
 * A block context has no matching gap, because there a '-' or '?' followed by
 * whitespace or the end of the line is taken earlier by the block sequence
 * dispatch or the explicit-key dispatch, before anything calls this.
 *
 * ':' is left out on purpose, even though YAML 1.2 gives it the same grammar
 * restriction, because scan_plain_scalar_line has its own earlier special
 * case for a ':' reached with nothing scanned yet, which returns an empty
 * scalar and leaves the ':' unread. parse_flow_list's bare-colon-key
 * shorthand (for example "[: value]", which holds an empty implicit key)
 * depends on exactly that, and rejecting ':' here would block it before it
 * ever runs.
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
 * Skips a tag token starting at a '!' that the caller has already confirmed
 * at the current position. It handles both the shorthand forms ("!", "!foo"
 * and "!!foo") and the verbatim form ("!<...>"), whose content runs through
 * the closing '>' and may validly hold characters that the shorthand
 * character set excludes, a literal ',' among them, since the verbatim
 * syntax exists to allow exactly such characters. So the two forms can't
 * share one rule for where the token ends.
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
  /* A shorthand tag token never holds a flow indicator, as YAML 1.2 section
   * 5.5 says, because ns-tag-char excludes c-flow-indicator. Without this
   * check a tag directly followed by ',', '[', ']', '{' or '}' in a flow
   * context ("!!str,", for example) would swallow that indicator into the tag
   * name, and the caller's flow-terminator scan would lose its place. */
  while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
         cur(ctx) != '\n' && cur(ctx) != '\r' && cur(ctx) != ',' &&
         cur(ctx) != '[' && cur(ctx) != ']' && cur(ctx) != '{' &&
         cur(ctx) != '}')
    ctx->pos++;
}

/*
 * True when c may appear unescaped in tag text. With tag_char false the set
 * is ns-uri-char of YAML 1.2 section 5.6, used by verbatim tag content and a
 * %TAG prefix: ASCII letters and digits, '-', the punctuation
 * "#;/?:@&=+$,_.!~*'()[]", and the '%' that starts an escape
 * (percent_decode_tag_inplace() checks the two hex digits after it). With
 * tag_char true the set is ns-tag-char, used by a shorthand tag's suffix: the
 * same set minus '!', ',', '[' and ']'. Every other byte, each byte of a
 * non-ASCII character included, has to be written as a percent escape.
 */
static inline bool tag_text_char_ok(unsigned char c, bool tag_char) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9'))
    return true;
  switch (c) {
    case '-':
    case '%':
    case '#':
    case ';':
    case '/':
    case '?':
    case ':':
    case '@':
    case '&':
    case '=':
    case '+':
    case '$':
    case '_':
    case '.':
    case '~':
    case '*':
    case '\'':
    case '(':
    case ')':
      return true;
    case '!':
    case ',':
    case '[':
    case ']':
      return !tag_char;
    default:
      return false;
  }
}

/* Refuses the first byte of text[0..len) that tag_text_char_ok() refuses, as
 * a parse error naming the byte and its position (start is text's offset in
 * the input). Returns true when every byte is allowed. */
static bool tag_text_check(parse_ctx_t *ctx, const char *text, size_t len,
                           size_t start, bool tag_char) {
  for (size_t i = 0; i < len; i++) {
    if (!tag_text_char_ok((unsigned char)text[i], tag_char)) {
      parse_err(ctx,
                "character '%s' at position %zu is not allowed in a tag; "
                "write it as a percent escape",
                _cyaml_echo_c(text[i]), start + i);
      return false;
    }
  }
  return true;
}

/*
 * Percent-decodes the content of a verbatim tag in place. YAML 1.2 sections
 * 5.6 and 5.7's ns-uri-char allows a "%" followed by two hex digits as an
 * escape for characters that the URI grammar otherwise excludes. A %00
 * escape is refused outright: a node's tag is a plain char* ending with a NUL
 * and has no separate length, so decoding into a real NUL inside the tag
 * would silently truncate it, which is exactly the kind of data corruption
 * this library's no-NUL-in-a-value policy prevents.
 * Returns false, with a parse error set on every one of those paths, on a
 * malformed escape, on an escape that produces a NUL, and on escapes that
 * decode to bytes that aren't well-formed UTF-8. Otherwise returns true,
 * shortening text in place as needed. */
static bool percent_decode_tag_inplace(parse_ctx_t *ctx, char *text,
                                       size_t pos_for_err) {
  char *w = text;
  bool decoded_non_ascii = false;
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
    if (byte >= 0x80) decoded_non_ascii = true;
    *w++ = (char)byte;
    r += 2;
  }
  *w = '\0';
  /* Percent-escapes name the bytes of a character's UTF-8 encoding, so escapes
   * that decode to a byte sequence that isn't well-formed UTF-8 name no
   * character, and a tag holding them couldn't be stored as text. Only a
   * decoded byte at 0x80 or above can break the encoding, since the raw bytes
   * around it are already well-formed. */
  if (decoded_non_ascii && !ccol_utf8_is_valid(text, (size_t)(w - text))) {
    parse_err(ctx,
              "tag percent-escapes at position %zu decode to bytes that are "
              "not well-formed UTF-8",
              pos_for_err);
    return false;
  }
  return true;
}

/*
 * Parses a tag token starting at a '!' that the caller has already confirmed
 * at the current position, and resolves it to one fully normalized tag
 * string. It handles the shorthand forms ("!", "!foo", "!!foo" and
 * "!name!foo") and the verbatim form ("!<...>"). It splits the token into
 * exactly the same pieces as skip_tag_token, but keeps and resolves the
 * content where skip_tag_token only skips it.
 *
 * On success *resolved_out gets an owned, fully resolved tag string that the
 * caller must free, or NULL for a bare "!" non-specific tag;
 * parse_tag_token() below turns that NULL into _CYAML_TAG_NON_SPECIFIC.
 *
 * Returns false on five kinds of failure: a malformed token, a shorthand
 * handle with no suffix, an undefined named handle, an allocation failure,
 * and a malformed percent-escape, which also covers an escape producing a
 * NUL and escapes decoding to bytes that aren't well-formed UTF-8. The raw
 * spelling of those bytes never gets here, because parse_common() refuses it
 * for the whole stream. Every failure path, the allocation failure included,
 * sets a parse error with parse_err().
 *
 * That always-set error matters beyond the direct callers:
 * try_parse_scalar_dict_key() calls this speculatively and reads
 * ctx->error[0] alone to tell a real error from "not a key after all",
 * because this path doesn't restore ctx->pos.
 */
static bool parse_tag_token_raw(parse_ctx_t *ctx, char **resolved_out) {
  size_t tag_start = ctx->pos;
  ctx->pos++; /* consume the leading '!' */

  if (!at_end(ctx) && cur(ctx) == '<') {
    /* The verbatim form, !<content>. */
    ctx->pos++;
    size_t content_start = ctx->pos;
    while (!at_end(ctx) && cur(ctx) != '>' && cur(ctx) != '\n' &&
           cur(ctx) != '\r') {
      ctx->pos++;
    }
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
    if (!tag_text_check(ctx, ctx->src + content_start, content_len,
                        content_start, false))
      return false;
    char *content = _ccol_mem_alloc(ctx->mp, content_len + 1);
    if (!content) {
      parse_err(ctx, "out of memory parsing verbatim tag at position %zu",
                tag_start);
      return false;
    }
    memcpy(content, ctx->src + content_start, content_len);
    content[content_len] = '\0';
    if (!percent_decode_tag_inplace(ctx, content, content_start)) {
      _ccol_mem_free(ctx->mp, content);
      return false;
    }
    /* The emptiness test above reads the RAW span, and this one reads the
     * string the node will really carry, after the copy and the decode. They
     * could only differ if some byte made the stored form shorter, and that
     * can't happen: parse_common() refuses every raw control byte of the
     * input before parsing starts, and percent_decode_tag_inplace() refuses a
     * percent-escaped null byte. So no current path reaches this test; it's a
     * standing guard that states the invariant that matters, namely that a
     * stored tag is never the empty string.
     *
     * cyaml_node_set_tag refuses an empty tag for exactly this invariant,
     * because the serializer writes a custom tag in its verbatim "!<...>"
     * form and would write "!<>", which this very function then refuses to
     * parse again. The guard belongs on the parse path too, so the invariant
     * holds however a tag arrives and not only through the public setter. */
    if (content[0] == '\0') {
      parse_err(ctx, "empty verbatim tag at position %zu", tag_start);
      _ccol_mem_free(ctx->mp, content);
      return false;
    }
    *resolved_out = content;
    return true;
  }

  /* The shorthand form. Here we decide which handle introduces the token:
   * the primary "!", the secondary "!!", or a named "!name!". The secondary
   * handle is checked first because that check is unambiguous: a bare second
   * '!' can never start the word-char name of a named handle, since such a
   * name is never empty. Otherwise a speculative look-ahead scans
   * ns-word-char+ (alphanumerics and '-') for a closing '!' that confirms a
   * named handle. If there is none we fall back to the primary handle, and
   * the look-ahead leaves ctx->pos unchanged. */
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

  char *handle = _ccol_mem_alloc(ctx->mp, handle_len + 1);
  if (!handle) {
    parse_err(ctx, "out of memory parsing tag handle at position %zu",
              tag_start);
    return false;
  }
  memcpy(handle, ctx->src + tag_start, handle_len);
  handle[handle_len] = '\0';

  /* A shorthand tag token never holds a flow indicator, as YAML 1.2 section
   * 5.5 says, because ns-tag-char excludes c-flow-indicator. The same comment
   * in skip_tag_token gives the full reason. */
  size_t suffix_start = ctx->pos;
  while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
         cur(ctx) != '\n' && cur(ctx) != '\r' && cur(ctx) != ',' &&
         cur(ctx) != '[' && cur(ctx) != ']' && cur(ctx) != '{' &&
         cur(ctx) != '}') {
    ctx->pos++;
  }
  size_t suffix_len = ctx->pos - suffix_start;

  if (suffix_len == 0 && handle_len == 1) {
    /* A bare "!" with nothing at all after it is the non-specific tag (the
     * c-non-specific-tag production), a separate production from a shorthand
     * tag, which needs ns-tag-char+ after its handle. */
    _ccol_mem_free(ctx->mp, handle);
    *resolved_out = NULL;
    return true;
  }
  if (suffix_len == 0) {
    /* A "!!" or "!name!" with nothing after it isn't a valid shorthand tag,
     * because c-ns-shorthand-tag needs at least one ns-tag-char. */
    parse_err(ctx, "tag handle '%s' with no suffix at position %zu",
              _cyaml_echo(handle), tag_start);
    _ccol_mem_free(ctx->mp, handle);
    return false;
  }
  /* Every byte of the suffix must be an ns-tag-char, so a '!' that doesn't
   * close a named handle ("!a.b!c") and a character like '"' or '<' are
   * refused instead of becoming part of the tag. */
  if (!tag_text_check(ctx, ctx->src + suffix_start, suffix_len, suffix_start,
                      true)) {
    _ccol_mem_free(ctx->mp, handle);
    return false;
  }

  const char *prefix = tag_handles_lookup(ctx, handle);
  if (!prefix) {
    parse_err(ctx, "undefined tag handle '%s' at position %zu",
              _cyaml_echo(handle), tag_start);
    _ccol_mem_free(ctx->mp, handle);
    return false;
  }

  /* The shorthand suffix's grammar is ns-tag-char from YAML 1.2 section 5.5,
   * which derives from ns-uri-char exactly like the verbatim content does, so
   * a suffix may hold the same "%" ns-hex-digit ns-hex-digit escapes. We
   * decode the suffix into its own buffer first and only then join it to the
   * prefix; percent_decode_tag_inplace shortens in place, so the decoded
   * length can only be <= suffix_len. The verbatim branch decodes before use
   * the same way, so neither branch copies the raw source bytes unchanged. */
  char *suffix = _ccol_mem_alloc(ctx->mp, suffix_len + 1);
  if (!suffix) {
    parse_err(ctx, "out of memory parsing tag suffix at position %zu",
              tag_start);
    _ccol_mem_free(ctx->mp, handle);
    return false;
  }
  memcpy(suffix, ctx->src + suffix_start, suffix_len);
  suffix[suffix_len] = '\0';
  if (!percent_decode_tag_inplace(ctx, suffix, suffix_start)) {
    _ccol_mem_free(ctx->mp, handle);
    _ccol_mem_free(ctx->mp, suffix);
    return false;
  }
  size_t decoded_suffix_len = strlen(suffix);

  size_t prefix_len = strlen(prefix);
  char *resolved =
      _ccol_mem_alloc(ctx->mp, prefix_len + decoded_suffix_len + 1);
  if (!resolved) {
    parse_err(ctx, "out of memory resolving tag at position %zu", tag_start);
    _ccol_mem_free(ctx->mp, handle);
    _ccol_mem_free(ctx->mp, suffix);
    return false;
  }
  memcpy(resolved, prefix, prefix_len);
  memcpy(resolved + prefix_len, suffix, decoded_suffix_len);
  resolved[prefix_len + decoded_suffix_len] = '\0';
  _ccol_mem_free(ctx->mp, handle);
  _ccol_mem_free(ctx->mp, suffix);
  *resolved_out = resolved;
  return true;
}

/*
 * Parses a tag token like parse_tag_token_raw() does, but gives the bare
 * non-specific "!" as an owned _CYAML_TAG_NON_SPECIFIC string so it reaches
 * node construction (YAML 1.2 section 6.9.1: a plain scalar under it is a
 * string, and a collection keeps its kind). A written tag that resolves to
 * the same string, such as the verbatim "!<!>" that YAML 1.2 example 6.25
 * declares invalid, is a parse error, because a verbatim tag isn't resolved
 * and no node may carry "!".
 */
static bool parse_tag_token(parse_ctx_t *ctx, char **resolved_out) {
  size_t tag_start = ctx->pos;
  char *tag = NULL;
  if (!parse_tag_token_raw(ctx, &tag)) return false;
  if (tag) {
    if (strcmp(tag, _CYAML_TAG_NON_SPECIFIC) == 0) {
      _ccol_mem_free(ctx->mp, tag);
      parse_err(ctx,
                "tag at position %zu resolves to '!', which is reserved for "
                "the non-specific tag",
                tag_start);
      return false;
    }
    *resolved_out = tag;
    return true;
  }
  tag = strdup_charged(ctx->mp, _CYAML_TAG_NON_SPECIFIC);
  if (!tag) {
    parse_err(ctx, "out of memory parsing tag at position %zu", tag_start);
    return false;
  }
  *resolved_out = tag;
  return true;
}

/*
 * Registers anchor_name so it points at a value. When anchor_name is NULL it
 * does nothing and returns true. When anchor_value isn't NULL the anchor
 * points at it and this function takes ownership of it; otherwise the anchor
 * points at a fresh CYAML_STRING node holding a copy of key_str. Either node
 * goes into the parse's kept list, because no tree holds it.
 *
 * This is the shared "a later '*name' alias resolves to this key text" step
 * that every place where a dictionary key can carry its own anchor needs:
 * the '&' and '!' dispatch branches in parse_node, and the matching branch
 * for a later entry in parse_one_dict_entry_key. All of them get anchor_name,
 * key_str and anchor_value from try_parse_scalar_dict_key.
 *
 * A non-NULL anchor_value is the key's real, fully typed node: the actual
 * list or mapping of a flow-collection key, or the implicitly typed scalar of
 * a plain-scalar key. The caller builds it so that an alias to this anchor
 * resolves to the same kind of value as an anchor in an ordinary value
 * position; without it the alias would always collapse to the plain string
 * the dictionary keeps as its key. A quoted-scalar key has no richer form,
 * since a quoted scalar is always a string matching key_str exactly, so its
 * caller passes NULL and we build the CYAML_STRING wrapper straight from
 * key_str.
 *
 * When anchor_name is NULL any anchor_value the caller passed is destroyed
 * instead of leaked. That's a standing defence, since no current caller does
 * it.
 *
 * anchor_name is always freed, and when it isn't NULL anchor_value is always
 * consumed too, by a store or a destroy. Returns false only on an OOM, and a
 * parse error is already set on that path, either here or by
 * anchors_store().
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
      anchor_val->value.string = strdup_charged(ctx->mp, key_str);
      if (!anchor_val->value.string) {
        node_free(anchor_val);
        anchor_val = NULL;
      }
    }
  }
  if (!anchor_val) {
    parse_err(ctx, "out of memory registering key anchor '%s' at position %zu",
              _cyaml_echo(anchor_name), ctx->pos);
    _ccol_mem_free(ctx->mp, anchor_name);
    return false;
  }
  /* No tree holds a key's node, since the dictionary keeps the key as text,
   * so the kept list owns it once the anchor points at it. */
  bool stored = anchors_store(ctx, anchor_name, anchor_val);
  if (stored)
    parse_keep(ctx, anchor_val);
  else
    __cyaml_destroy((cyaml)anchor_val);
  _ccol_mem_free(ctx->mp, anchor_name);
  return stored;
}

/* True when ctx->pos, after any inline whitespace, is a block mapping's ':'
 * value indicator, i.e. a ':' followed by a space, a tab, a line break or the
 * end of the input. On true ctx->pos is left on that ':', and on false past
 * the whitespace. */
static inline __attribute__((always_inline)) bool at_block_value_indicator(
    parse_ctx_t *ctx) {
  skip_inline_ws(ctx);
  if (at_end(ctx) || cur(ctx) != ':') return false;
  char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
  return after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
         after == '\0';
}

/*
 * Tries to parse a block dictionary key up to and including its own ':'
 * indicator. The key is a plain scalar, a double-quoted scalar, a
 * single-quoted scalar, a flow collection, or a resolved alias, optionally
 * decorated by an anchor, a tag, or both. Two places share this: the '&'
 * dispatch of parse_node for the very first entry of a dictionary, which
 * uses it when the anchored node introduces a key instead of a standalone
 * value, and parse_one_dict_entry_key for every later entry.
 *
 * A tag on the key applies exactly as on a value, and as on a flow
 * dictionary key or an explicit "? key": the key node is built through
 * finalize_scalar_node() or finalize_collection_node() with the resolved
 * tag, and its canonical text becomes the dictionary key. So "!!str 010" and
 * "!!str 10" are two keys, and "!!int abc" is a parse error.
 *
 * The ':' check comes before any work that only a real key needs (cloning
 * an alias, typing a scalar, the canonical text of a flow collection),
 * because the same text is a value ("- *a", "- [..]") far more often than a
 * key. A canonical text also has a length limit
 * (CYAML_MAX_CANONICAL_KEY_LEN) that a value doesn't have, so it must never
 * be computed for a value. Keep this order when you change the function:
 * moving key-only work in front of the check makes the parser refuse values
 * that are perfectly valid.
 *
 * On a real match it returns true, leaves ctx->pos just past the ':' it
 * read, and fills the out parameters as follows.
 *
 * *anchor_name_out gets an owned anchor name when there was one, and NULL for
 * an alias key and for an undecorated key. The caller calls
 * anchors_store() itself once it knows the key's final node.
 *
 * *key_out gets the owned key text.
 *
 * *is_merge_candidate_out, when not NULL, gets whether this key is a real
 * merge-key trigger; see below.
 *
 * *anchor_value_out, when not NULL, gets the key's real, fully typed node,
 * which the caller then owns, but only when anchor_name_out came back
 * non-NULL AND a richer form than the plain key string exists: the actual
 * list or mapping of a flow-collection key, the implicitly typed scalar of a
 * plain-scalar key, or the node of a tagged key. Otherwise it gets NULL and
 * the caller has to register the anchor as a plain string of the key text
 * (see register_key_anchor).
 *
 * On no match it returns false, restores ctx->pos to exactly where it was on
 * entry, and changes no out parameter. ctx->error[0] tells a real parse error
 * (a malformed quoted scalar, a malformed tag, a tag that disagrees with the
 * key, or an alias to an unknown anchor) apart from an ordinary "this wasn't
 * a key after all".
 *
 * *is_merge_candidate_out is true only when the key text is exactly "<<" and
 * one of two things holds. Either the key comes from the plain-scalar branch
 * with no tag at all, mirroring the "only an untagged plain scalar" rule of
 * implicit core-schema resolution that resolves such a "<<" to the merge
 * type, or the key's tag is the core merge tag (!!merge or its verbatim
 * form), from the plain or a quoted branch, so the explicit tag names the
 * merge type directly. An untagged quoted "<<", and a "<<" with any other
 * tag, is an ordinary literal key, and PyYAML reads all of these cases the
 * same way. The alias-as-key and flow-collection-as-key branches never give a
 * merge key. An anchor on a "<<" does NOT disqualify it, because an anchor is
 * a separate c-ns-properties matter unrelated to typing, so "&x <<: *y" is a
 * real merge-key trigger as well.
 */
static bool _try_parse_scalar_dict_key(parse_ctx_t *ctx, char **anchor_name_out,
                                       char **key_out,
                                       bool *is_merge_candidate_out,
                                       cyaml_node_t **anchor_value_out) {
  size_t saved_pos = ctx->pos;
  char *anchor_name = NULL;

  /* An alias can be a key itself, for example "*b : *a". We resolve it here
   * and turn its value into a canonical string key, just like any other typed
   * scalar, with no anchor name to register. The clone only happens after the
   * ':' check, because "- *a" is far more common than "*a : v". */
  if (!at_end(ctx) && cur(ctx) == '*') {
    /* An unknown alias is reported at its '*', where the token starts. */
    const size_t alias_start = ctx->pos;
    ctx->pos++;
    char *alias_name = NULL;
    if (!parse_anchor_name(ctx, &alias_name)) return false;
    cyaml_node_t *aliased = anchors_lookup(ctx, alias_name);
    if (!aliased) {
      parse_err(ctx, "unknown alias '*%s' at position %zu",
                _cyaml_echo(alias_name), alias_start);
      _ccol_mem_free(ctx->mp, alias_name);
      return false;
    }
    _ccol_mem_free(ctx->mp, alias_name);

    if (!at_block_value_indicator(ctx)) {
      ctx->pos = saved_pos;
      return false;
    }
    ctx->pos++; /* consume ':' */

    /* The copy is a key, so it sits below the mapping that holds it, one
     * level below ctx->depth (see parse_key_fits_below_mapping()). */
    bool too_deep = false;
    size_t height = 0;
    size_t key_levels = ctx->depth + 1 < CYAML_MAX_NESTING_DEPTH
                            ? CYAML_MAX_NESTING_DEPTH - (ctx->depth + 1)
                            : 0;
    cyaml_node_t *clone =
        parse_clone_levels(aliased, key_levels, &too_deep, &height);
    if (clone && ctx->depth + 1 + height > ctx->peak_depth)
      ctx->peak_depth = ctx->depth + 1 + height;
    if (!clone && too_deep) {
      parse_depth_err(ctx);
      return false;
    }
    if (!clone) {
      /* cyaml_clone() never calls parse_err() on an OOM, while each caller does
       * its own "ctx->error[0] means a real error, no error means not a key
       * after all" check (see the doc comment of this function). Without an
       * explicit error here that check would classify this OOM as "not a key"
       * and carry on from ctx->pos, which is already past the ':', so the
       * parser would lose its place instead of failing the whole document
       * cleanly. */
      parse_err(ctx,
                "out of memory resolving alias as dictionary key at "
                "position %zu",
                saved_pos);
      return false;
    }

    char *key_str = node_to_dict_key_string(ctx, clone, "block dictionary");
    if (!key_str) return false;

    *anchor_name_out = NULL;
    *key_out = key_str;
    if (is_merge_candidate_out) *is_merge_candidate_out = false;
    if (anchor_value_out) *anchor_value_out = NULL;
    return true;
  }

  /* c-ns-properties(n,c) allows an anchor and a tag in either order
   * (c-tag-property s-separate c-ns-anchor-property, or the reverse), each at
   * most once, so two passes are enough whatever the order. The !had_tag
   * guard protects the tag branch just as !anchor_name protects the anchor
   * branch; without it a second tag would silently be read as part of the
   * property list.
   *
   * key_tag is the resolved tag, owned here until the key node is built.
   * Under the non-specific "!" it is _CYAML_TAG_NON_SPECIFIC, which makes the
   * key a string, just as on a value. */
  bool had_tag = false;
  char *key_tag = NULL;
  for (int prop_pass = 0; prop_pass < 2; prop_pass++) {
    if (!at_end(ctx) && cur(ctx) == '&' && !anchor_name) {
      ctx->pos++;
      if (!parse_anchor_name(ctx, &anchor_name)) {
        _ccol_mem_free(ctx->mp, key_tag);
        return false;
      }
      skip_inline_ws(ctx);
    } else if (!at_end(ctx) && cur(ctx) == '!' && !had_tag) {
      /* parse_tag_token() sets a parse error on every failure, so an undefined
       * %TAG handle, a malformed verbatim tag and a percent-escaped null byte
       * are real errors here, exactly as anywhere else a tag appears. */
      if (!parse_tag_token(ctx, &key_tag)) {
        _ccol_mem_free(ctx->mp, anchor_name);
        return false;
      }
      had_tag = true;
      skip_inline_ws(ctx);
    } else {
      break;
    }
  }
  /* The core merge tag makes a "<<" key a merge key whatever its style, as
   * the tag:yaml.org,2002:merge type defines; any other tag keeps it an
   * ordinary key. */
  const bool key_has_merge_tag =
      key_tag && strcmp(key_tag, _CYAML_TAG_MERGE) == 0;
  if (!at_end(ctx) && cur(ctx) == '!' && had_tag) {
    /* A second tag on the same key. c-ns-properties allows at most one tag per
     * node, so we report it explicitly as an unrecoverable error, just as
     * parse_node_inner does for a value position. Without this check the
     * input would fall through to the plain-scalar dispatch below, which,
     * unlike the '&' and '*' excluded just below, has no guard of its own
     * against a leading '!', and it would silently pull the unread second
     * tag's text into the key string. */
    parse_err(ctx, "a node cannot carry two tags at position %zu", ctx->pos);
    _ccol_mem_free(ctx->mp, key_tag);
    _ccol_mem_free(ctx->mp, anchor_name);
    return false;
  }
  if (at_end(ctx) || cur(ctx) == '\n' || cur(ctx) == '\r') {
    ctx->pos = saved_pos;
    _ccol_mem_free(ctx->mp, key_tag);
    _ccol_mem_free(ctx->mp, anchor_name);
    return false;
  }
  char c = cur(ctx);
  if (c == '|' || c == '>' || c == '&' || c == '*' || c == '#' || c == '%' ||
      c == '@' || c == '`' || c == ']' || c == '}' || c == ',') {
    /* '%', '@', '`', ']', '}' and ',' are c-indicator characters, like the
     * '#' just above, which YAML 1.2 section 7.3.3's ns-plain-first excludes,
     * and they have no valid reading as a plain scalar key at all. '|', '>',
     * '&' and '*' are different: each is a real indicator for ANOTHER kind of
     * node (a block scalar, an anchor, an alias) that a caller may want to
     * read this position as, while the other six have no such reading. This
     * branch calls no parse_err() and still ends up with a clear, specific
     * error, because the caller reads this position as an ordinary value, and
     * that path goes back through the same check for these characters in
     * parse_node_inner. */
    ctx->pos = saved_pos;
    _ccol_mem_free(ctx->mp, key_tag);
    _ccol_mem_free(ctx->mp, anchor_name);
    return false;
  }

  char *key_str = NULL;
  cyaml_node_t *coll = NULL;
  _anchor_speculation_t spec;
  bool parsed;
  if (c == '"' || c == '\'') {
    size_t quote_start = ctx->pos;
    parsed = (c == '"') ? parse_double_quoted(ctx, &key_str)
                        : parse_single_quoted(ctx, &key_str);
    if (parsed && span_crosses_newline(ctx, quote_start)) {
      /* The same one-line rule applies to a flow-collection key just below: an
       * implicit key always stays on one line, as ns-s-implicit-yaml-key
       * says, and a quoted scalar is no exception. Two independent reference
       * parsers confirm this. */
      _ccol_mem_free(ctx->mp, key_str);
      ctx->pos = saved_pos;
      _ccol_mem_free(ctx->mp, key_tag);
      _ccol_mem_free(ctx->mp, anchor_name);
      return false;
    }
  } else if (c == '[' || c == '{') {
    /* A flow collection is a valid implicit block mapping key too, even though
     * it isn't a scalar: YAML 1.2 section 8.2.2 allows ns-flow-node in a
     * block-key context, not only a scalar. A flow collection is unambiguous
     * on its own, because its '[' or '{' and ']' or '}' boundaries need no
     * look-ahead and rewind the way a bare plain scalar does, so we parse it
     * directly instead of through parse_node. */
    size_t coll_start = ctx->pos;
    /* This function only serves a block-context implicit key, and the check
     * below always holds such a key to one physical line whatever indent
     * this call uses, so any newline inside the collection fails that check
     * anyway and flow_skip_ws's indent-based validation would add nothing. We
     * pass -1, the most permissive value, because no more meaningful
     * enclosing indent is available here.
     *
     * The parse runs as a speculation. The collection is only a key when a
     * ':' follows it; otherwise the caller reads the same text again as a
     * value, and then the anchors the collection defines and the budget it
     * charges must be exactly as if this parse never ran. See
     * anchor_speculation_begin(). */
    anchor_speculation_begin(ctx, &spec);
    coll =
        (c == '[') ? parse_flow_list(ctx, -1) : parse_flow_dictionary(ctx, -1);
    if (!coll) {
      /* The whole parse fails here, so the speculation ends by keeping its
       * changes: the budget flags still name the limit that stopped it, and
       * the anchor table goes away when the parse unwinds.
       *
       * A real syntax error inside the flow collection has already set
       * ctx->error with parse_err(), but an OOM at the very top of
       * parse_flow_list() or parse_flow_dictionary() (cyaml_create_list_mp()
       * itself failing, for example) returns NULL without calling
       * parse_err(). Without an error here the caller would read that OOM as
       * "not a key after all" and carry on from a position this function
       * didn't restore. */
      anchor_speculation_accept(ctx, &spec);
      if (!ctx->error[0]) {
        if (_parse_node_budget_exhausted)
          parse_err(ctx,
                    "document exceeded the %zu node-allocation limit at "
                    "position %zu",
                    _parse_node_limit, coll_start);
        else if (_parse_byte_budget_exhausted)
          parse_err(ctx,
                    "document exceeded the %zu-byte memory limit at "
                    "position %zu",
                    _parse_byte_limit, coll_start);
        else
          parse_err(ctx,
                    "out of memory parsing flow collection dictionary key "
                    "at position %zu",
                    coll_start);
      }
      _ccol_mem_free(ctx->mp, key_tag);
      _ccol_mem_free(ctx->mp, anchor_name);
      return false;
    }
    /* Every implicit key must fit on one line, scalar or not, as
     * ns-s-implicit-yaml-key says. An explicit '? key' is another story, but
     * this function only serves implicit-style keys. */
    parsed = !span_crosses_newline(ctx, coll_start);
  } else
    /* An implicit key always stays on one line, as ns-plain-one-line says,
     * so this call needs no hit_real_terminator out parameter: a key never
     * gets a continuation attempt, whatever follows it. */
    parsed = parse_plain_scalar(ctx, false, &key_str, NULL);
  bool from_plain_branch = (c != '"' && c != '\'' && c != '[' && c != '{');
  if (!parsed && !coll) {
    /* Usually a real parse error, for which parse_double_quoted(),
     * parse_single_quoted() or parse_plain_scalar() has already set
     * ctx->error. But each of them can also fail on an allocator OOM alone
     * without calling parse_err(), and without this fallback every caller
     * would silently classify such a failure as "not a key after all" (see
     * the doc comment of this function) when it's a real failure. */
    if (!ctx->error[0])
      parse_err(ctx, "out of memory parsing dictionary key at position %zu",
                saved_pos);
    _ccol_mem_free(ctx->mp, key_tag);
    _ccol_mem_free(ctx->mp, anchor_name);
    return false;
  }

  if (!parsed || !at_block_value_indicator(ctx)) {
    /* Not a key: a flow collection spanning lines, or any text with no ':'
     * after it. Nothing that only a key needs has been computed. A flow
     * collection's tree goes before its speculation is rejected, and the
     * rejection restores the budget that tree charged. */
    ctx->pos = saved_pos;
    _ccol_mem_free(ctx->mp, key_str);
    if (coll) {
      __cyaml_destroy((cyaml)coll);
      anchor_speculation_reject(ctx, &spec);
    }
    _ccol_mem_free(ctx->mp, key_tag);
    _ccol_mem_free(ctx->mp, anchor_name);
    return false;
  }
  if (coll) {
    anchor_speculation_accept(ctx, &spec);
    if (!parse_key_fits_below_mapping(ctx, coll)) {
      __cyaml_destroy((cyaml)coll);
      _ccol_mem_free(ctx->mp, key_tag);
      _ccol_mem_free(ctx->mp, anchor_name);
      return false;
    }
  }
  ctx->pos++; /* consume ':' */

  /* The key's node. A flow collection already is one. A plain scalar always
   * gets one, for its core-schema typing: without it "~: v" would store the
   * literal text "~" while "? ~\n: v" stores "null" for the identical value,
   * and YAML 1.2 treats the two notations as the same entry. A tagged quoted
   * scalar gets one for its tag, while an untagged quoted scalar needs none,
   * since its value already is the string in key_str. Each finalize call
   * takes ownership of its input, on failure too. */
  cyaml_node_t *key_node = NULL;
  if (!key_tag && from_plain_branch) {
    /* The common case, an untagged plain key: exactly what
     * finalize_scalar_node() does for it, minus the tag dispatch. */
    key_node = make_typed_scalar(ctx, key_str);
    _ccol_mem_free(ctx->mp, key_str);
    key_str = NULL;
  } else if (coll) {
    key_node = finalize_collection_node(ctx, coll, key_tag);
  } else if (key_tag || from_plain_branch) {
    key_node = finalize_scalar_node(ctx, key_str, key_tag, from_plain_branch);
    key_str = NULL;
  }
  if (key_tag) _ccol_mem_free(ctx->mp, key_tag);
  if (!key_node && !key_str) {
    /* finalize_scalar_node() and finalize_collection_node() set a parse error
     * for a tag that disagrees with the key, and return NULL without an error
     * on an OOM. */
    if (!ctx->error[0])
      parse_err(ctx, "out of memory typing dictionary key at position %zu",
                saved_pos);
    _ccol_mem_free(ctx->mp, anchor_name);
    return false;
  }

  cyaml_node_t *anchor_value = NULL;
  if (key_node) {
    if (anchor_name) {
      /* The key's real node becomes the anchor's target, so the key text comes
       * from it without consuming it. An alias to this key's anchor has to
       * resolve to the typed scalar, the tagged node, or the real list or
       * mapping, just like an anchor in a value position, and not to the
       * string that only the dictionary's key storage uses. */
      key_str =
          node_to_dict_key_string_ex(ctx, key_node, "block dictionary", false);
      if (!key_str) {
        __cyaml_destroy((cyaml)key_node);
        _ccol_mem_free(ctx->mp, anchor_name);
        return false;
      }
      anchor_value = key_node;
    } else {
      key_str = node_to_dict_key_string(ctx, key_node, "block dictionary");
      if (!key_str) return false;
    }
  }

  *anchor_name_out = anchor_name;
  *key_out = key_str;
  if (is_merge_candidate_out)
    *is_merge_candidate_out =
        ((from_plain_branch && !had_tag) || key_has_merge_tag) &&
        strcmp(key_str, "<<") == 0;
  if (anchor_value_out)
    *anchor_value_out = anchor_value;
  else if (anchor_value)
    __cyaml_destroy((cyaml)anchor_value);
  return true;
}

/*
 * A speculative parse of a dictionary key reads content that may turn out
 * not to be a key; for a '[' or '{' key that's a complete, independent parse
 * of a flow collection. When the speculation is declined, the caller goes on
 * from ctx->pos and keeps using the nesting budget in ctx->depth, so both
 * must be exactly what they were before the attempt: the position because
 * the declined content gets read again as something else, and the depth
 * because the nested walk shares that budget instead of having its own. The
 * code above provides both, but the type system enforces neither and the
 * whole construct rests on that rewind, so a test build checks it instead of
 * assuming it.
 */
static bool try_parse_scalar_dict_key(parse_ctx_t *ctx, char **anchor_name_out,
                                      char **key_out,
                                      bool *is_merge_candidate_out,
                                      cyaml_node_t **anchor_value_out) {
#ifdef RUNNING_UNIT_TESTS
  const size_t saved_pos = ctx->pos;
  const size_t saved_depth = ctx->depth;
#endif
  bool accepted = _try_parse_scalar_dict_key(
      ctx, anchor_name_out, key_out, is_merge_candidate_out, anchor_value_out);
#ifdef RUNNING_UNIT_TESTS
  _ccol_assert(ctx->depth == saved_depth);
  if (!accepted && !ctx->error[0]) _ccol_assert(ctx->pos == saved_pos);
#endif
  return accepted;
}

/* ========================================================================== */
/*                         FORWARD DECLARATIONS                               */
/* ========================================================================== */

/*
 * The parse of a node, and of every collection parser it descends into, is
 * driven by an explicit stack of frames instead of the native call stack. So
 * the stack one parse needs is a constant that doesn't grow with the
 * document's nesting depth, and CYAML_MAX_NESTING_DEPTH bounds the work a
 * parse may be asked to do instead of the stack a caller must provide.
 *
 * There is one frame kind per construct that can hold another node. A frame
 * runs until it either finishes and hands its node to the frame below, or
 * asks the driver to push a child frame and then carries on from the state
 * it recorded once that child finishes. A frame only holds values that are
 * really live across a descent; everything else stays an ordinary local of
 * the step function.
 */
typedef enum {
  _PFK_NODE,       /* one node dispatch: _pni_step */
  _PFK_FLOW_LIST,  /* '[' elem (',' elem)* ']' */
  _PFK_FLOW_DICT,  /* '{' key ':' value (',' ...)* '}' */
  _PFK_BLOCK_LIST, /* '-' entries at one indent */
  _PFK_BLOCK_DICT  /* 'key:' / '? key' entries at one indent */
} _pframe_kind_t;

/*
 * What a frame asks the driver to push next. The fields one kind reads are
 * exactly the arguments of the matching call, so a descent request is an
 * exact record of the call it stands for.
 */
typedef struct {
  _pframe_kind_t kind;
  int indent;            /* every kind: node indent, seq_indent or map_indent */
  bool in_flow;          /* _PFK_NODE */
  bool seq_ok_at_indent; /* _PFK_NODE */
  bool allow_inline_map; /* _PFK_NODE */
  bool had_anchor;       /* _PFK_NODE */
  bool had_tag;          /* _PFK_NODE */
  const char *resolved_tag;          /* _PFK_NODE, borrowed */
  const char *first_key;             /* _PFK_BLOCK_DICT, borrowed */
  bool first_key_colon_consumed;     /* _PFK_BLOCK_DICT */
  bool first_key_is_merge_candidate; /* _PFK_BLOCK_DICT */
} _preq_t;

/* Resume points. Each one names the descent where the frame was suspended,
 * so a frame's state is always "which child am I waiting for". */
enum {
  _NS_ENTRY = 0,
  _NS_AFTER_ANCHOR_VALUE, /* the node an '&' anchor decorates */
  _NS_AFTER_TAG_VALUE,    /* the node a '!' tag decorates */
  _NS_AFTER_COLLECTION,   /* a block collection this node turned out to be */
  _NS_AFTER_FLOW_COLL     /* a flow collection, which may still become a key */
};
enum {
  _FLS_ENTRY = 0,
  _FLS_AFTER_PAIR_KEY,       /* "[? key: value]" key */
  _FLS_AFTER_PAIR_VALUE,     /* "[? key: value]" value */
  _FLS_AFTER_ELEM,           /* a bare element */
  _FLS_AFTER_SHORTHAND_VALUE /* "[key: value]" value */
};
enum { _FDS_ENTRY = 0, _FDS_AFTER_KEY, _FDS_AFTER_VALUE };
enum { _BLS_ENTRY = 0, _BLS_AFTER_ELEM };
enum {
  _BDS_ENTRY = 0,
  _BDS_AFTER_FIRST_KEY_NODE, /* an explicit '?' key opening the mapping */
  _BDS_AFTER_KEY_NODE,       /* an explicit '?' key of a later entry */
  _BDS_AFTER_VALUE
};

/*
 * One merge entry of a mapping being read: the value of a "<<" merge key,
 * which the frame owns until the mapping ends, and how many members the
 * mapping held when the entry appeared, which places the merged members in
 * the member order. A merge entry never goes into the map itself, so it
 * never collides with another merge entry or with an ordinary "<<" key.
 */
typedef struct {
  cyaml_node_t *val;
  size_t pos;
} _cyaml_merge_entry_t;

/*
 * One suspended parser. `child` holds the node the frame above has just
 * finished, which this frame owns until its resume point consumes it.
 * `result` holds this frame's finished node once the frame is done.
 *
 * _pframe_cleanup() frees every pointer a frame owns. When a parse fails the
 * driver runs it for EVERY frame on the stack, not just the innermost one: a
 * container under construction isn't reachable from any root until its frame
 * hands it to the frame below, so each frame owns its container outright,
 * and without that sweep an abandoned stack would leak one container per
 * open level.
 */
typedef struct {
  _pframe_kind_t kind;
  int state;
  cyaml_node_t *child;
  cyaml_node_t *result;
  union {
    struct {
      int indent;
      bool in_flow, seq_ok_at_indent, allow_inline_map, had_anchor, had_tag;
      const char *resolved_tag;
      char *name;        /* owned anchor name */
      char *tag;         /* owned tag text */
      char *pending_key; /* owned key text the collection resume releases */
      int col;
      size_t coll_start;
    } n;
    struct {
      int indent;
      cyaml_node_t *seq; /* owned */
      char *key_str;     /* owned */
      size_t elem_start;
      bool is_merge_candidate;
    } fl;
    struct {
      int indent;
      cyaml_node_t *map;            /* owned */
      char *key_str;                /* owned */
      _cyaml_merge_entry_t *merges; /* owned, with every value */
      size_t merge_count, merge_cap;
      bool is_explicit_key_indicator;
      bool explicit_key_is_merge_candidate;
      bool implicit_key_is_merge_candidate;
      bool is_merge_candidate;
    } fd;
    struct {
      int seq_indent;
      cyaml_node_t *seq; /* owned */
    } bl;
    struct {
      int map_indent;
      cyaml_node_t *map; /* owned */
      const char *key;   /* borrowed: the caller's first_key, or key_owned */
      char *key_owned;   /* owned */
      _cyaml_merge_entry_t *merges; /* owned, with every value */
      size_t merge_count, merge_cap;
      bool have_colon, is_explicit;
      bool entry_is_merge_candidate;
    } bd;
  } u;
} _pframe_t;

/* What _pbmn_begin() decided about the node in a block dictionary's key or
 * value position. */
typedef enum {
  _PBMN_NODE,    /* *node_out holds it (NULL means an allocation failure) */
  _PBMN_DESCEND, /* the request is filled in; resume with the child */
  _PBMN_ERROR    /* ctx->error is set */
} _pbmn_t;

/* What one step of a frame's own state machine decided. */
typedef enum {
  _PSTEP_DONE,    /* finished; f->result holds the node */
  _PSTEP_DESCEND, /* the driver should push the request just filled in */
  _PSTEP_FAIL     /* failed; ctx->error is set unless this was a bare OOM */
} _pstep_t;

/*
 * How many frames of the parser's walk stack the driver keeps in its locals;
 * a document nested deeper than that moves the stack onto the heap. Each
 * nesting level opens two frames (the node dispatch and the collection the
 * node turns out to be), and each anchor or tag decorating that node opens
 * one more, so this covers a document of ordinary shape without touching the
 * allocator at all.
 */
#define _CYAML_PARSE_INLINE_FRAMES 24

static cyaml_node_t *_parse_drive(parse_ctx_t *ctx, const _preq_t *root_req);

/*
 * Parses one node. The depth guard lives in the collection frames that the
 * walk opens and not here (see _pframe_open), so CYAML_MAX_NESTING_DEPTH
 * bounds every container whatever YAML construct drove it (nested mappings,
 * sequences, explicit keys and flow pairs), without a matching check at each
 * of the many separate descent sites in this file. A document past the limit
 * is a hard parse error, in line with this parser's fail-fast convention
 * everywhere else. Every exit path, the depth-exceeded error path included,
 * releases ctx->depth, so a caller further up that then reports some other
 * error always sees a consistent depth count.
 */
static cyaml_node_t *parse_node(parse_ctx_t *ctx, int indent, bool in_flow,
                                bool seq_ok_at_indent, bool allow_inline_map,
                                bool had_anchor, bool had_tag,
                                const char *resolved_tag) {
  _preq_t rq = {.kind = _PFK_NODE,
                .indent = indent,
                .in_flow = in_flow,
                .seq_ok_at_indent = seq_ok_at_indent,
                .allow_inline_map = allow_inline_map,
                .had_anchor = had_anchor,
                .had_tag = had_tag,
                .resolved_tag = resolved_tag,
                .first_key = NULL,
                .first_key_colon_consumed = false,
                .first_key_is_merge_candidate = false};
  return _parse_drive(ctx, &rq);
}

/* The definitions of these functions, near parse_block_dictionary, carry the
 * full doc comments. They are declared here because the flow parsers,
 * defined earlier in this file, need them too. */
static bool merge_apply(parse_ctx_t *ctx, cyaml_node_t *map,
                        const _cyaml_merge_entry_t *entries, size_t n);
static bool merge_entry_push(parse_ctx_t *ctx, _cyaml_merge_entry_t **arr,
                             size_t *count, size_t *cap, cyaml_node_t *val,
                             size_t pos);
static void merge_entries_free(parse_ctx_t *ctx, _cyaml_merge_entry_t **arr,
                               size_t *count, size_t *cap);
static bool merge_entries_finish(parse_ctx_t *ctx, cyaml_node_t *map,
                                 _cyaml_merge_entry_t **arr, size_t *count,
                                 size_t *cap);

/*
 * Stores the single entry of the one-pair mapping that a "[key: value]" or
 * "[? key: value]" flow sequence entry makes. A merge entry (a "<<" the
 * look-ahead confirmed as a merge key) is expanded into the empty mapping
 * instead of stored. It takes ownership of key_str and val in every case and
 * destroys pair on a failure.
 */
static bool flow_pair_store(parse_ctx_t *ctx, cyaml_node_t *pair, char *key_str,
                            cyaml_node_t *val, bool is_merge_candidate) {
  bool ok;
  if (is_merge_candidate && strcmp(key_str, "<<") == 0) {
    _cyaml_merge_entry_t entry = {.val = val, .pos = 0};
    ok = merge_apply(ctx, pair, &entry, 1);
    parse_discard(val);
  } else {
    /* pair is a dictionary the caller has just created, key_str isn't NULL,
     * and val is a fresh node that nothing attached and that isn't pair
     * itself, so the only failures this call can hit are the ones that take
     * ownership of val and destroy it. */
    ok = dictionary_set_impl((cyaml)pair, key_str, (cyaml)val) == ccol_success;
  }
  _ccol_mem_free(ctx->mp, key_str);
  if (!ok) __cyaml_destroy((cyaml)pair);
  return ok;
}

/* The definition, near parse_one_dict_entry_key, carries the full doc
 * comment. It is declared here because parse_flow_dictionary, defined
 * earlier in this file, needs it too for its own "{? key: value}"
 * explicit-key shorthand. */
static bool explicit_key_peek_is_merge_candidate(parse_ctx_t *ctx, int indent,
                                                 bool in_flow);
/*
 * Fills in a request for one node dispatch. The argument list reads exactly
 * like the parse_node() call each descent site stands for.
 *
 * Each builder here only writes the fields that _pframe_open() reads for
 * that kind, so a request never carries anything a frame of its kind could
 * act on. The two sides together are the interface, and they sit a few
 * lines apart.
 */
static void _preq_node(_preq_t *rq, int indent, bool in_flow,
                       bool seq_ok_at_indent, bool allow_inline_map,
                       bool had_anchor, bool had_tag,
                       const char *resolved_tag) {
  rq->kind = _PFK_NODE;
  rq->indent = indent;
  rq->in_flow = in_flow;
  rq->seq_ok_at_indent = seq_ok_at_indent;
  rq->allow_inline_map = allow_inline_map;
  rq->had_anchor = had_anchor;
  rq->had_tag = had_tag;
  rq->resolved_tag = resolved_tag;
}

/* Fills in a request for a block dictionary. The request borrows first_key,
 * while the frame issuing the request keeps ownership of it and stays below
 * the new frame on the stack for as long as the new frame can read it. */
static void _preq_block_dict(_preq_t *rq, int map_indent, const char *first_key,
                             bool first_key_colon_consumed,
                             bool first_key_is_merge_candidate) {
  rq->kind = _PFK_BLOCK_DICT;
  rq->indent = map_indent;
  rq->first_key = first_key;
  rq->first_key_colon_consumed = first_key_colon_consumed;
  rq->first_key_is_merge_candidate = first_key_is_merge_candidate;
}

/* Fills in a request for a block sequence at seq_indent. */
static void _preq_block_list(_preq_t *rq, int seq_indent) {
  rq->kind = _PFK_BLOCK_LIST;
  rq->indent = seq_indent;
}

/* Fills in a request for a flow collection: a list for a '[', a dictionary
 * for a '{'. */
static void _preq_flow(_preq_t *rq, bool is_list, int indent) {
  rq->kind = is_list ? _PFK_FLOW_LIST : _PFK_FLOW_DICT;
  rq->indent = indent;
}

/* Records a node frame's finished node. Every producer in this file reports
 * a failure as a NULL node, so it stays a failure here. */
static _pstep_t _pni_finish(_pframe_t *f, cyaml_node_t *n) {
  f->result = n;
  return n ? _PSTEP_DONE : _PSTEP_FAIL;
}

/* Frees the anchor name a node frame owns across the descent into the value
 * the anchor decorates, and clears the field, which is the only thing that
 * makes the frame's cleanup idempotent. */
static void _pni_release_name(parse_ctx_t *ctx, _pframe_t *f) {
  _ccol_mem_free(ctx->mp, f->u.n.name);
  f->u.n.name = NULL;
}

/* Frees the tag text a node frame owns across the descent into the node the
 * tag decorates. See _pni_release_name. */
static void _pni_release_tag(parse_ctx_t *ctx, _pframe_t *f) {
  _ccol_mem_free(ctx->mp, f->u.n.tag);
  f->u.n.tag = NULL;
}

/* ========================================================================== */
/*                         FLOW COLLECTION PARSERS                            */
/* ========================================================================== */

/* The result of flow_list_expect_comma_or_close, telling the switch on the
 * caller side in parse_flow_list what to do next. */
typedef enum {
  FLOW_LIST_TAIL_CLOSE,    /* Read a ']'. The caller gives the list as it
                              is. */
  FLOW_LIST_TAIL_CONTINUE, /* Read a ','. The caller loops for one more
                              entry. */
  FLOW_LIST_TAIL_ERROR     /* ctx->error is already set. The caller goes to
                              its fail label. */
} flow_list_tail_t;

/* The shared tail step after one element of a flow list: it skips whitespace
 * and then needs a ']' or a ','. Both entry shapes of parse_flow_list (the
 * "[? key: value]" explicit-pair branch and the bare-element branch below
 * it) reach it the same way. It reads the ']' or ',' itself, so the caller
 * only has to act on the result. */
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

/* Parses a flow list, '[' elem (',' elem)* ']', into a CYAML_LIST node whose
 * backing cvec holds cyaml_node_t * child pointers. `f->u.fl.indent` is the
 * indent of the enclosing block value, or -1 at the document root, and also
 * -1 when this flow list sits inside another flow collection that already
 * fixed the indent (see the doc comment of flow_skip_ws).
 *
 * Each of the four points where an element, or the key or value of a
 * "[? key: value]" pair, gets parsed is a descent, everything between them is
 * ordinary straight-line code, and the frame only keeps the values that live
 * across a descent. */
static _pstep_t _pfl_step(parse_ctx_t *ctx, _pframe_t *f, _preq_t *rq) {
  const int indent = f->u.fl.indent;
  cyaml_node_t *key_node;
  cyaml_node_t *val;
  cyaml_node_t *elem;
  char *key_str;

  switch (f->state) {
    case _FLS_AFTER_PAIR_KEY:
      goto after_pair_key;
    case _FLS_AFTER_PAIR_VALUE:
      goto after_pair_value;
    case _FLS_AFTER_ELEM:
      goto after_elem;
    case _FLS_AFTER_SHORTHAND_VALUE:
      goto after_shorthand_value;
    default:
      break;
  }

  /* consume '[' */
  ctx->pos++;
  if (!flow_skip_ws(ctx, indent)) return _PSTEP_FAIL;

  f->u.fl.seq = (cyaml_node_t *)cyaml_create_list_interned(ctx->mp);
  if (!f->u.fl.seq) return _PSTEP_FAIL;

  if (!at_end(ctx) && cur(ctx) == ']') {
    ctx->pos++;
    goto done;
  }

  while (1) {
    if (!flow_skip_ws(ctx, indent)) return _PSTEP_FAIL;
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow list");
      return _PSTEP_FAIL;
    }
    if (cur(ctx) == ']') {
      ctx->pos++;
      goto done;
    }
    if (cur(ctx) == ',') {
      /* A ',' with no content since the '[' or since the previous ',' is an
       * empty entry, which means nothing in a sequence, since every element
       * must really be there (a flow mapping's bare-key-with-no-value
       * shorthand is a different thing). Without this check parse_node and
       * parse_plain_scalar would silently give a zero-length string "" here. */
      parse_err(ctx, "empty flow list entry at position %zu", ctx->pos);
      return _PSTEP_FAIL;
    }

    /* "[? key: value]" is a single-pair entry in the explicit style, which YAML
     * 1.2 section 7.4.1 defines in Spec Example 7.20. It works like the bare
     * "[key: value]" shorthand below with two differences: a '?' introduces
     * the key, which allows multi-line and non-scalar key content that the
     * bare form's implicit-key restriction forbids, and a separate ':'
     * introduces the value, when there is one. */
    bool is_explicit_pair_indicator = false;
    if (cur(ctx) == '?') {
      char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
      if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
        is_explicit_pair_indicator = true;
    }
    if (is_explicit_pair_indicator) {
      ctx->pos++;
      if (!flow_skip_ws(ctx, indent)) return _PSTEP_FAIL;
      /* Same as the "{? key: value}" handling in parse_flow_dictionary: an
       * untagged plain "<<" key here is just as real a merge trigger as the
       * bare "[<<: value]" shorthand below, and the flow-dictionary and
       * block-dictionary forms of both behave the same. We need to look ahead
       * for it before reading the key. */
      f->u.fl.is_merge_candidate =
          explicit_key_peek_is_merge_candidate(ctx, indent, true);
      _preq_node(rq, indent, true, false, false, false, false, NULL);
      f->state = _FLS_AFTER_PAIR_KEY;
      return _PSTEP_DESCEND;
    after_pair_key:
      key_node = f->child;
      f->child = NULL;
      /* A failed child unwinds the whole stack and never resumes this frame,
       * so key_node is never NULL here. The guard is there in case a future
       * frame kind reports a failure by finishing with no node; without it
       * such a frame kind would turn this into a null dereference. */
      if (!key_node) return _PSTEP_FAIL;
      if (!parse_key_fits_below_mapping(ctx, key_node)) {
        __cyaml_destroy((cyaml)key_node);
        return _PSTEP_FAIL;
      }
      key_str = node_to_dict_key_string(ctx, key_node, "flow sequence entry");
      if (!key_str) return _PSTEP_FAIL;

      if (!flow_skip_ws(ctx, indent)) {
        _ccol_mem_free(ctx->mp, key_str);
        return _PSTEP_FAIL;
      }
      if (!at_end(ctx) && cur(ctx) == ':') {
        ctx->pos++;
        if (!flow_skip_ws(ctx, indent) || !flow_pair_level_take(ctx)) {
          _ccol_mem_free(ctx->mp, key_str);
          return _PSTEP_FAIL;
        }
        f->u.fl.key_str = key_str;
        _preq_node(rq, indent, true, false, false, false, false, NULL);
        f->state = _FLS_AFTER_PAIR_VALUE;
        return _PSTEP_DESCEND;
      after_pair_value:
        key_str = f->u.fl.key_str;
        f->u.fl.key_str = NULL;
        ctx->depth--; /* the level of the pair, see flow_pair_level_take */
        val = f->child;
        f->child = NULL;
        if (!val) {
          _ccol_mem_free(ctx->mp, key_str);
          return _PSTEP_FAIL;
        }
      } else {
        /* The pair has no value, but its mapping still sits one level below
         * the sequence. */
        if (ctx->depth >= CYAML_MAX_NESTING_DEPTH) {
          parse_depth_err(ctx);
          _ccol_mem_free(ctx->mp, key_str);
          return _PSTEP_FAIL;
        }
        if (ctx->depth + 1 > ctx->peak_depth) ctx->peak_depth = ctx->depth + 1;
        val = node_alloc(CYAML_NULL, ctx->mp);
        if (!val) {
          _ccol_mem_free(ctx->mp, key_str);
          return _PSTEP_FAIL;
        }
      }

      {
        cyaml_node_t *pair =
            (cyaml_node_t *)cyaml_create_dictionary_interned(ctx->mp);
        if (!pair) {
          _ccol_mem_free(ctx->mp, key_str);
          __cyaml_destroy((cyaml)val);
          return _PSTEP_FAIL;
        }
        if (!flow_pair_store(ctx, pair, key_str, val,
                             f->u.fl.is_merge_candidate))
          return _PSTEP_FAIL;

        cyaml_node_t *pep = pair;
        if (cvector_push_back(f->u.fl.seq->value.list, &pep) != ccol_success) {
          __cyaml_destroy((cyaml)pair);
          return _PSTEP_FAIL;
        }
        /* The list owns the node from here on and this flag records that, so a
         * later attach of the node through a borrowed cyaml_list_get()
         * reference is refused. Without the flag that attach would give the
         * node a second owner. */
        pep->attached = true;
      }

      switch (flow_list_expect_comma_or_close(ctx, indent)) {
        case FLOW_LIST_TAIL_CLOSE:
          goto done;
        case FLOW_LIST_TAIL_ERROR:
          return _PSTEP_FAIL;
        case FLOW_LIST_TAIL_CONTINUE:
          continue;
      }
    }

    f->u.fl.elem_start = ctx->pos;
    /* This look-ahead runs before the element is read, just like the
     * implicit-key look-ahead of parse_flow_dictionary. Only an untagged,
     * plain, unquoted scalar whose text is exactly "<<" is a real merge-key
     * trigger. An anchor doesn't disqualify it, and the anchor handling of
     * explicit_key_peek_is_merge_candidate covers that case as well; the doc
     * comment of try_parse_scalar_dict_key gives the general rule. We need a
     * look-ahead over the whole property chain here and not a check of one
     * character, because a tag after an anchor, as in "&y !!str <<: *x", must
     * be seen and must disqualify the key, which looking at the very
     * first character can't do. */
    f->u.fl.is_merge_candidate =
        explicit_key_peek_is_merge_candidate(ctx, indent, true);
    _preq_node(rq, indent, true, false, false, false, false, NULL);
    f->state = _FLS_AFTER_ELEM;
    return _PSTEP_DESCEND;
  after_elem:
    elem = f->child;
    f->child = NULL;
    if (!elem) return _PSTEP_FAIL;

    /* "[foo: bar]" is shorthand for "[{foo: bar}]": a bare "key: value" pair
     * inside a flow sequence, without '{' and '}' around it, means a
     * one-entry mapping element, as YAML 1.2 section 7.4.1's ns-flow-pair
     * defines. Like every implicit key it has to fit on one line, as
     * ns-s-implicit-yaml-key says, so a ':' that's only reached after
     * crossing a newline isn't this shorthand but the next real error, a
     * missing ',' between this element and whatever follows. That holds for
     * both ways of crossing the newline: the element's own parse can cross
     * it, for example when a plain scalar folds over several lines right up
     * to the ':', or the whitespace skip after the element can cross it
     * before the ':' is found. */
    if (!flow_skip_ws(ctx, indent)) {
      __cyaml_destroy((cyaml)elem);
      return _PSTEP_FAIL;
    }
    if (!span_crosses_newline(ctx, f->u.fl.elem_start) && !at_end(ctx) &&
        cur(ctx) == ':') {
      if (!parse_key_fits_below_mapping(ctx, elem)) {
        __cyaml_destroy((cyaml)elem);
        return _PSTEP_FAIL;
      }
      key_str = node_to_dict_key_string(ctx, elem, "flow sequence entry");
      if (!key_str) return _PSTEP_FAIL;
      ctx->pos++; /* consume ':' */
      if (!flow_skip_ws(ctx, indent)) {
        _ccol_mem_free(ctx->mp, key_str);
        return _PSTEP_FAIL;
      }

      if (!flow_pair_level_take(ctx)) {
        _ccol_mem_free(ctx->mp, key_str);
        return _PSTEP_FAIL;
      }
      f->u.fl.key_str = key_str;
      _preq_node(rq, indent, true, false, false, false, false, NULL);
      f->state = _FLS_AFTER_SHORTHAND_VALUE;
      return _PSTEP_DESCEND;
    after_shorthand_value:
      key_str = f->u.fl.key_str;
      f->u.fl.key_str = NULL;
      ctx->depth--; /* the level of the pair, see flow_pair_level_take */
      val = f->child;
      f->child = NULL;
      if (!val) {
        _ccol_mem_free(ctx->mp, key_str);
        return _PSTEP_FAIL;
      }

      {
        cyaml_node_t *pair =
            (cyaml_node_t *)cyaml_create_dictionary_interned(ctx->mp);
        if (!pair) {
          _ccol_mem_free(ctx->mp, key_str);
          __cyaml_destroy((cyaml)val);
          return _PSTEP_FAIL;
        }
        if (!flow_pair_store(ctx, pair, key_str, val,
                             f->u.fl.is_merge_candidate))
          return _PSTEP_FAIL;
        elem = pair;
      }
    }

    {
      cyaml_node_t *ep = elem;
      if (cvector_push_back(f->u.fl.seq->value.list, &ep) != ccol_success) {
        __cyaml_destroy((cyaml)elem);
        return _PSTEP_FAIL;
      }
      /* The list owns the node from here on and this flag records that, so a
       * later attach of the node through a borrowed cyaml_list_get()
       * reference is refused. Without the flag that attach would give the
       * node a second owner. */
      ep->attached = true;
    }

    switch (flow_list_expect_comma_or_close(ctx, indent)) {
      case FLOW_LIST_TAIL_CLOSE:
        goto done;
      case FLOW_LIST_TAIL_ERROR:
        return _PSTEP_FAIL;
      case FLOW_LIST_TAIL_CONTINUE:
        continue;
    }
  }

done:
  f->result = f->u.fl.seq;
  f->u.fl.seq = NULL;
  return _PSTEP_DONE;
}

/* Parses a flow list as a standalone walk. One place parses a flow
 * collection outside any node dispatch: try_parse_scalar_dict_key's
 * speculative read of a flow-collection block-mapping key, which drops the
 * result and rewinds ctx->pos when the collection turns out not to be a key.
 * The speculation is a complete, independent parse with its own frame stack,
 * which the driver drains or sweeps before this returns, so nothing of the
 * outer walk needs to be saved across it. Both parses share ctx, and with it
 * ctx->depth, so the same remaining depth budget that reached this list
 * bounds the elements inside it. */
static cyaml_node_t *parse_flow_list(parse_ctx_t *ctx, int indent) {
  _preq_t rq;
  _preq_flow(&rq, true, indent);
  return _parse_drive(ctx, &rq);
}

/*
 * Writes the canonical dictionary-key text of the float v into buf: the
 * shortest decimal that strtod() reads back as exactly v (see
 * format_shortest_double()). yb_append_double() uses the same digits for a
 * float value and adds the ".0" that a value needs and a key doesn't. A NaN
 * gives ".nan", an infinity ".inf" or "-.inf", and both zeros give "0",
 * since -0.0 and 0.0 are one number.
 * The text of an integral value has no ".0", so the float key 1.0 and the
 * integer key 1 are the same key, just as the two numbers are equal.
 *
 * Every such text parses back through try_parse_int_scalar() or
 * try_parse_float_scalar() to a value whose own canonical text is the same
 * string; key_is_canonical_number() depends on that, and it lets the
 * serializer write such a key without quotes.
 *
 * Like try_parse_float_scalar(), this only runs inside the "C" locale scope
 * of a public parse or serialize call, so the text always uses '.'.
 */
#define CYAML_FLOAT_KEY_BUF_LEN 32

/* Writes into buf the shortest "%.Ng" text that strtod() reads back as
 * exactly the finite double v. Every double round trips at 17 significant
 * digits, and when a shorter text of that form round trips we write that:
 * 1.0 / 3.0 gives "0.3333333333333333" and not the 17-digit
 * "0.33333333333333331". For a normal double, fewer than 15 digits never
 * needs its own try, because "%.15g" already drops the trailing zeros of a
 * value that a shorter text describes. A subnormal double (below DBL_MIN in
 * magnitude) has fewer significant bits, so a text with fewer than 15 digits
 * can name it even though the 15-digit text of the same value has no
 * trailing zeros: 5e-324 is "5e-324" and not "4.94065645841247e-324". Only
 * such a value tries 1 to 14 digits first. */
static void format_shortest_double(double v,
                                   char buf[static CYAML_FLOAT_KEY_BUF_LEN]) {
  if (v > -DBL_MIN && v < DBL_MIN && v != 0.0) {
    for (int prec = 1; prec < 15; prec++) {
      snprintf(buf, CYAML_FLOAT_KEY_BUF_LEN, "%.*g", prec, v);
      if (strtod(buf, NULL) == v) return;
    }
  }
  snprintf(buf, CYAML_FLOAT_KEY_BUF_LEN, "%.15g", v);
  if (strtod(buf, NULL) == v) return;
  snprintf(buf, CYAML_FLOAT_KEY_BUF_LEN, "%.16g", v);
  if (strtod(buf, NULL) == v) return;
  snprintf(buf, CYAML_FLOAT_KEY_BUF_LEN, "%.17g", v);
}

static void format_float_key(double v,
                             char buf[static CYAML_FLOAT_KEY_BUF_LEN]) {
  if (__builtin_isnan(v)) {
    memcpy(buf, ".nan", sizeof(".nan"));
    return;
  }
  if (__builtin_isinf(v)) {
    if (v > 0)
      memcpy(buf, ".inf", sizeof(".inf"));
    else
      memcpy(buf, "-.inf", sizeof("-.inf"));
    return;
  }
  if (v == 0.0) {
    memcpy(buf, "0", sizeof("0"));
    return;
  }
  format_shortest_double(v, buf);
}

/*
 * Turns a parsed node into its canonical string form so a dictionary can
 * store it as a key. The dictionaries of this DOM are always char* -> node,
 * backed by a chmap, so in the end every key is a string. With consume true
 * this always disposes of key_node, through parse_discard(); with consume
 * false key_node is left exactly as it was, for a caller that registers the
 * node as an anchor's target.
 *
 * A non-scalar key_node is a list or another dictionary, which YAML 1.2
 * sections 7.4.1 and 8.2.2 both allow. Its canonical form comes from
 * serialize_flow_canonical, an internal-only variant of cyaml_serialize_flow
 * written for exactly this (see its own doc comment). The result is the
 * node's compact flow-YAML text, for example "[a, b]" or "{x: 1}", and that
 * becomes the key string.
 *
 * So two non-scalar keys with the same structure collide, because their flow
 * output matches, just as this DOM treats the numeric key 1 and the string
 * key "1" as the same string key. The order in which either dictionary got
 * its own keys doesn't matter. The canonical form doesn't depend on
 * context_label, so three places are treated alike: an explicit key of a
 * block dictionary, a key of a flow dictionary, and the "key: value"
 * shorthand of a flow sequence.
 */
static char *node_to_dict_key_string_ex(parse_ctx_t *ctx,
                                        cyaml_node_t *key_node,
                                        const char *context_label,
                                        bool consume) {
  char *key_str = NULL;
  switch (key_node->type) {
    case CYAML_STRING:
      /* Move the string out of the node instead of copying it. With consume
         true the node is disposed of before we return, so a copy here would
         be paid for twice: an allocation and a copy to make it, a free of the
         original a few lines below, and then the caller copies the string
         again into the dictionary's storage. Instead we take the pointer and
         clear the node's reference to it, so the node's teardown has nothing
         to free for this member; node_clear_value already handles that,
         since a cleared node is in exactly this state.

         An anchored node keeps its string, because the anchor table points at
         it and a later alias copies it whole. The move only happens when the
         two allocators are the same object,
         because the caller frees this string through ctx->mp while the node
         would have freed it through its own. They're the same for every node
         this parser builds, and the copy keeps the code correct for any node
         that might ever come from somewhere else. */
      if (consume && !key_node->anchored && key_node->m_procs == ctx->mp &&
          key_node->value.string) {
        key_str = key_node->value.string;
        key_node->value.string = NULL;
      } else {
        key_str = ccol_strdup(ctx->mp, key_node->value.string);
      }
      break;
    case CYAML_INTEGER: {
      char tmp[32];
      snprintf(tmp, sizeof(tmp), "%lld", key_node->value.integer);
      key_str = ccol_strdup(ctx->mp, tmp);
      break;
    }
    case CYAML_FLOAT: {
      char tmp[CYAML_FLOAT_KEY_BUF_LEN];
      format_float_key(key_node->value.number, tmp);
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
    default: {
      bool too_long = false;
      key_str = serialize_flow_canonical(key_node, &too_long);
      if (too_long) {
        /* See the doc comment of CYAML_MAX_CANONICAL_KEY_LEN. A non-scalar key
         * can nest inside another such key many levels deep, and then its
         * canonical text grows exponentially with the nesting depth, since
         * each level quotes the text of the level below again. This has
         * nothing to do with CYAML_MAX_NESTING_DEPTH and is far cheaper to
         * reach, and many aliases of one long scalar reach the same length
         * without any nesting at all. serialize_flow_canonical() stops at the
         * first byte past the limit, so this refusal costs at most one
         * limit's worth of text. */
        parse_err(ctx,
                  "canonical form of a non-scalar key in %s exceeds %d "
                  "bytes at position %zu",
                  context_label, CYAML_MAX_CANONICAL_KEY_LEN, ctx->pos);
      } else if (!key_str) {
        parse_err(ctx,
                  "out of memory canonicalizing non-scalar key in %s "
                  "at position %zu",
                  context_label, ctx->pos);
      } else {
        /* A collection key's canonical text can be far longer than the text
         * that declared it, because each nested level quotes the level below
         * again. That makes it amplification, so it's charged to the fixed
         * amplification budget as well as to the ordinary one. */
        parse_amp_enter();
        bool charged =
            parse_bytes_charge(strlen(key_str) + 1 + _CYAML_ALLOC_OVERHEAD);
        parse_amp_leave();
        if (!charged) {
          parse_err(ctx,
                    "canonical form of a non-scalar key in %s exceeded the "
                    "%s memory limit at position %zu",
                    context_label,
                    _parse_amp_limit_hit ? "alias expansion" : "document",
                    ctx->pos);
          _ccol_mem_free(ctx->mp, key_str);
          key_str = NULL;
        }
      }
      break;
    }
  }
  /* The LIST and DICTIONARY branch above always reports its own failure (an
   * OOM, or a canonical form that is too long) with parse_err() before
   * falling through to here. The five scalar branches only have one failure
   * mode of their own, a bare ccol_strdup OOM, and none of them reports it.
   * An unreported OOM here doesn't crash or lose the parser's place, since
   * parse_common's top-level fallback reports a general out-of-memory
   * message when ctx->error is empty, but a real OOM here deserves this call
   * site's specific diagnostic, in line with the convention elsewhere in
   * this file, try_parse_scalar_dict_key for example. */
  if (!key_str && !ctx->error[0]) {
    parse_err(ctx,
              "out of memory converting a scalar key to a dictionary "
              "key string in %s at position %zu",
              context_label, ctx->pos);
  }
  if (consume) parse_discard(key_node);
  return key_str;
}

static char *node_to_dict_key_string(parse_ctx_t *ctx, cyaml_node_t *key_node,
                                     const char *context_label) {
  return node_to_dict_key_string_ex(ctx, key_node, context_label, true);
}

/*
 * Parses a flow dictionary, '{' key ':' value (',' ...)* '}', into a
 * CYAML_DICTIONARY node. A key may be a node of any type: a scalar (a
 * string, an integer, a null or a bool), a sequence, a mapping or a flow
 * collection. node_to_dict_key_string() makes the canonical form of every
 * key so the chmap stores each key as a char *; see that function's doc
 * comment.
 *
 * The key and the value are each a descent, everything else is
 * straight-line code, and the frame only keeps what lives across a descent.
 */
static _pstep_t _pfd_step(parse_ctx_t *ctx, _pframe_t *f, _preq_t *rq) {
  const int indent = f->u.fd.indent;
  cyaml_node_t *key_node = NULL;
  cyaml_node_t *val = NULL;
  char *key_str = NULL;

  switch (f->state) {
    case _FDS_AFTER_KEY:
      goto after_key;
    case _FDS_AFTER_VALUE:
      goto after_value;
    default:
      break;
  }

  /* consume '{' */
  ctx->pos++;
  if (!flow_skip_ws(ctx, indent)) return _PSTEP_FAIL;

  f->u.fd.map = (cyaml_node_t *)cyaml_create_dictionary_interned(ctx->mp);
  if (!f->u.fd.map) return _PSTEP_FAIL;

  if (!at_end(ctx) && cur(ctx) == '}') {
    ctx->pos++;
    goto done;
  }

  /* Every merge entry (a "<<" the look-ahead confirmed as a merge key) goes
   * aside into f->u.fd.merges instead of into the map, and the merges expand
   * once the mapping ends, so two merge entries, or a merge entry and an
   * ordinary "<<" key, never replace each other. */
  while (1) {
    if (!flow_skip_ws(ctx, indent)) return _PSTEP_FAIL;
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow dictionary");
      return _PSTEP_FAIL;
    }
    if (cur(ctx) == '}') {
      ctx->pos++;
      if (f->u.fd.merge_count &&
          !merge_entries_finish(ctx, f->u.fd.map, &f->u.fd.merges,
                                &f->u.fd.merge_count, &f->u.fd.merge_cap))
        return _PSTEP_FAIL;
      goto done;
    }

    /* "{? key: value}" comes from YAML 1.2 section 7.4.2's
     * ns-flow-map-explicit-entry: an explicit '?' may introduce the key of a
     * flow dictionary entry, just like the block-style "? key\n: value" form.
     * It lets the key be a flow collection itself, or be left out entirely (a
     * bare "?" with nothing else, meaning a null key). Without this check a
     * '?' here would reach the general node dispatch below, whose explicit-key
     * handling is block-only because !in_flow gates it, so the '?' would be
     * scanned as ordinary plain scalar TEXT and we'd silently end up with a
     * key starting with a literal "? " instead of the real key after it. */
    f->u.fd.is_explicit_key_indicator = false;
    if (cur(ctx) == '?') {
      char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
      if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
        f->u.fd.is_explicit_key_indicator = true;
    }

    /* Parse the key. Both forms, the implicit one without '?' and the explicit
     * '?' one, use the same look-ahead: before the general dispatch reads the
     * key, explicit_key_peek_is_merge_candidate looks at the key's leading
     * anchor and tag property chain, in either order, to confirm a real
     * merge-key candidate.
     * The doc comment of try_parse_scalar_dict_key gives the same rule for
     * the block-dictionary side, and the doc comment of
     * explicit_key_peek_is_merge_candidate explains why this needs a
     * look-ahead over the whole property chain and not a one-character check:
     * an anchor does NOT disqualify the key, but a tag after an anchor, as in
     * "&y !!str <<: *x", must be seen and must disqualify it, which
     * looking only at the very first character can't do. */
    f->u.fd.implicit_key_is_merge_candidate = false;
    f->u.fd.explicit_key_is_merge_candidate = false;
    if (f->u.fd.is_explicit_key_indicator) {
      ctx->pos++;
      if (!flow_skip_ws(ctx, indent)) return _PSTEP_FAIL;
      if (!at_end(ctx) &&
          (cur(ctx) == ':' || cur(ctx) == ',' || cur(ctx) == '}')) {
        key_node = node_alloc(CYAML_NULL, ctx->mp);
        goto have_key_node;
      }
      f->u.fd.explicit_key_is_merge_candidate =
          explicit_key_peek_is_merge_candidate(ctx, indent, true);
    } else {
      f->u.fd.implicit_key_is_merge_candidate =
          explicit_key_peek_is_merge_candidate(ctx, indent, true);
    }
    _preq_node(rq, indent, true, false, false, false, false, NULL);
    f->state = _FDS_AFTER_KEY;
    return _PSTEP_DESCEND;
  after_key:
    key_node = f->child;
    f->child = NULL;
  have_key_node:
    if (!key_node) return _PSTEP_FAIL;

    /* The dictionary needs the key as a string. */
    key_str = node_to_dict_key_string(ctx, key_node, "flow dictionary");
    if (!key_str) return _PSTEP_FAIL;
    f->u.fd.is_merge_candidate = f->u.fd.is_explicit_key_indicator
                                     ? f->u.fd.explicit_key_is_merge_candidate
                                     : f->u.fd.implicit_key_is_merge_candidate;

    /* "{key}", a bare key with no ':' at all, is shorthand for "{key: null}",
     * and "{key:}", a ':' with nothing before the next ',' or '}', has a null
     * value too: YAML 1.2 section 7.4.2's ns-flow-map-entry lets a pair leave
     * out its key half, its value half, or both. */
    if (!flow_skip_ws(ctx, indent)) {
      _ccol_mem_free(ctx->mp, key_str);
      return _PSTEP_FAIL;
    }
    if (!at_end(ctx) && cur(ctx) == ':') {
      ctx->pos++;
      if (!flow_skip_ws(ctx, indent)) {
        _ccol_mem_free(ctx->mp, key_str);
        return _PSTEP_FAIL;
      }
      if (!at_end(ctx) && (cur(ctx) == ',' || cur(ctx) == '}')) {
        val = node_alloc(CYAML_NULL, ctx->mp);
      } else {
        f->u.fd.key_str = key_str;
        _preq_node(rq, indent, true, false, false, false, false, NULL);
        f->state = _FDS_AFTER_VALUE;
        return _PSTEP_DESCEND;
      after_value:
        key_str = f->u.fd.key_str;
        f->u.fd.key_str = NULL;
        val = f->child;
        f->child = NULL;
      }
    } else if (!at_end(ctx) && (cur(ctx) == ',' || cur(ctx) == '}')) {
      val = node_alloc(CYAML_NULL, ctx->mp);
    } else {
      parse_err(ctx, "expected ':' after flow dictionary key at position %zu",
                ctx->pos);
      _ccol_mem_free(ctx->mp, key_str);
      return _PSTEP_FAIL;
    }
    if (!val) {
      _ccol_mem_free(ctx->mp, key_str);
      return _PSTEP_FAIL;
    }

    if (f->u.fd.is_merge_candidate && strcmp(key_str, "<<") == 0) {
      _ccol_mem_free(ctx->mp, key_str);
      if (!merge_entry_push(ctx, &f->u.fd.merges, &f->u.fd.merge_count,
                            &f->u.fd.merge_cap, val,
                            chmap_elem_count(f->u.fd.map->value.dictionary)))
        return _PSTEP_FAIL;
    } else {
      ccol_retval_t r =
          dictionary_set_impl((cyaml)f->u.fd.map, key_str, (cyaml)val);
      _ccol_mem_free(ctx->mp, key_str);
      if (r != ccol_success) return _PSTEP_FAIL;
    }

    if (!flow_skip_ws(ctx, indent)) return _PSTEP_FAIL;
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow dictionary");
      return _PSTEP_FAIL;
    }
    if (cur(ctx) == '}') {
      ctx->pos++;
      if (f->u.fd.merge_count &&
          !merge_entries_finish(ctx, f->u.fd.map, &f->u.fd.merges,
                                &f->u.fd.merge_count, &f->u.fd.merge_cap))
        return _PSTEP_FAIL;
      goto done;
    }
    if (cur(ctx) != ',') {
      parse_err(ctx, "expected ',' or '}' in flow dictionary at position %zu",
                ctx->pos);
      return _PSTEP_FAIL;
    }
    ctx->pos++;
  }

done:
  f->result = f->u.fd.map;
  f->u.fd.map = NULL;
  return _PSTEP_DONE;
}

/* Parses a flow dictionary as a standalone walk. The doc comment of
 * parse_flow_list explains when that's needed and why the speculation it
 * serves needs nothing saved across it. */
static cyaml_node_t *parse_flow_dictionary(parse_ctx_t *ctx, int indent) {
  _preq_t rq;
  _preq_flow(&rq, false, indent);
  return _parse_drive(ctx, &rq);
}

/* ========================================================================== */
/*                         BLOCK COLLECTION PARSERS                           */
/* ========================================================================== */

/*
 * The fields of a _PFK_BLOCK_DICT request.
 *
 * indent is the mapping's map_indent: the column where the dictionary's keys
 * and '?' indicators appear.
 *
 * A non-NULL first_key is the key string of the first entry's implicit
 * ("key: value") key, which the requesting frame already read before it knew
 * it had a dictionary and not a standalone scalar. That frame keeps
 * ownership of the string and stays below this one on the stack, so the
 * borrow stays valid as long as anything reads it.
 *
 * first_key_colon_consumed says whether the ':' after that key has been read
 * too. It's true for a key with an anchor or a tag, which
 * try_parse_scalar_dict_key parses and which always reads the ':', and false
 * for the plain and quoted scalar dispatch branches, which leave the ':' for
 * the block dictionary to find and read itself.
 *
 * When first_key is NULL, ctx->pos is at map_indent on a line starting with
 * a '?': the explicit key style, "? key" followed by ": value", which is the
 * only way the node dispatch reaches a block dictionary with no key read in
 * advance, and which doesn't use first_key_colon_consumed. As YAML 1.2
 * section 8.2.2 allows, every later entry may freely mix the explicit and the
 * implicit style within one dictionary.
 *
 * first_key_is_merge_candidate says whether first_key is a real merge-key
 * trigger (an untagged plain "<<"), as confirmed by whichever site produced
 * the key; see the doc comment of try_parse_scalar_dict_key for the exact
 * rule. It's always false when first_key is NULL, because then the mapping's
 * first entry uses the '?' explicit-key form and the block dictionary decides
 * that entry's merge candidacy itself (see
 * explicit_key_peek_is_merge_candidate).
 */

/*
 * The core node parser, which dispatches to the right sub-parser based on the
 * first meaningful characters.
 *
 * indent: the block collection indent level of the current container, where
 *   -1 means the top level, a flow context, or an indent nothing has fixed
 *   yet.
 * in_flow: true inside a flow collection ({} or []).
 * seq_ok_at_indent: whether `indent` is a mapping's indent. True means a
 *   mapping, so a '-' sequence value at exactly that column is YAML 1.2
 *   section 8.2.2's valid compact-sequence-under-a-key exception. False means
 *   a sequence's indent, so a '-' at exactly that column is always a sibling
 *   element and never nested content (see the doc comment of
 *   at_block_value_col). The flag means nothing when in_flow is true or
 *   indent is -1, but the '&' and '!' branches below pass it on regardless,
 *   because otherwise a decorated value would lose its own indent context.
 * allow_inline_map: whether a plain or quoted scalar found here may open a
 *   NEW block mapping right here when a ':' turns out to follow it directly,
 *   making this call the first key of that mapping. It is true wherever the
 *   parser really expects a fresh key: the document root, a sequence element
 *   ("- key: value"), the key or value content of the explicit '?' and ':'
 *   style, and a value that starts on its own, more indented line. A
 *   reference parser confirms the explicit-style case, giving that content
 *   the same "compact" privilege for a nested mapping as for a nested
 *   sequence. It is false only for the SAME-LINE content of an ordinary
 *   implicit value ("key: value"): the recursive parse_node call for the
 *   "value" half gets false, so a second ':' chained there ("a: b: c"), which
 *   has no valid reading, is a hard error instead of silent, unbounded
 *   nesting. A reference parser confirms this and rejects the chain the same
 *   way wherever it sits: the document root, inside a sequence element's
 *   compact mapping, at any nesting depth. The flag means nothing once
 *   in_flow is true, since flow mappings have their own '{' and '}'
 *   dispatch, but the '&', '!' and '*' branches below pass it on regardless,
 *   for the same reason as seq_ok_at_indent: otherwise a decorated value
 *   would lose its own inline-mapping privilege, or the lack of one.
 * had_anchor: whether the "ordinary content" recursion of an ENCLOSING
 *   anchor has already applied that anchor to the exact node position this
 *   call is about to resolve, so this call's result would become that
 *   anchor's direct content unless this call gives a key (see below).
 *   c-ns-properties allows at most one anchor per node, so if THIS call needs
 *   an anchor while had_anchor is already true, that's two anchors on one
 *   node, which has no valid reading. Two independent reference parsers
 *   confirm it: both reject "&a &b val", two bare anchors around one scalar
 *   with nothing else between them, whether on one line or two. The flag is
 *   only read at the very top of the '&' branch, and only after that branch's
 *   "is this a key after all" fast path fails or is skipped, so a property
 *   that DOES resolve to a key is never affected, whatever had_anchor says.
 *   One example is "&node1\n  &k1 key1: val1", where &k1 decorates the key
 *   scalar "key1", a different node from &node1's mapping value, and that
 *   path returns before it ever reads the flag. The flag is false at every
 *   other call site, including the '&' and '!' branches' "is this a key"
 *   fast path and every call site outside those two branches, because each
 *   of them starts a genuinely fresh node position with no pending property
 *   to clash with.
 * had_tag: whether an ENCLOSING '!' branch's recursion has already applied
 *   a tag to the exact node position this call is about to resolve; it plays
 *   for tags exactly the role had_anchor plays for anchors. Only two guards
 *   read it, the '!' branch's two-tags guard and the '*' branch's
 *   cannot-carry-a-tag guard. "! !!str x" and "! *alias" are rejected just
 *   like "!!str !!str x" and "!!str *alias".
 * resolved_tag: the fully resolved tag string that an ENCLOSING '!' branch's
 *   recursion passes to this call. It's NULL for every ordinary untagged call
 *   and _CYAML_TAG_NON_SPECIFIC under a bare "!". This is the real value this
 *   call's scalar-construction dispatch reads (see finalize_scalar_node), and
 *   it decides which type gets built in the first place, rather than being
 *   metadata attached to an already finished node afterwards. The '&'
 *   branch's recursive calls pass it on exactly as received, because an
 *   anchor never introduces or consumes a tag of its own. That's what carries
 *   a tag all the way down to where the real content is read, however many
 *   anchor and tag layers sit in between. c-ns-properties allows either
 *   order, so "!!str &a foo" and "&a !!str foo" both work.
 */
/*
 * Dispatches one node. This is the whole decision tree the parser runs per
 * node, which the driver suspends and resumes at the four points where it
 * descends into another node or a collection (see the _NS_* resume points).
 *
 * The compiler inlines it into the driver next to the four collection steps.
 * It's by far the largest of the five, so the inlining isn't free in code
 * size; measure it again with instruction counts if the balance between the
 * steps ever changes. As things stand, half of all the steps a parse runs
 * are node dispatches, and a call for each of them costs more.
 */
static _pstep_t _pni_step(parse_ctx_t *ctx, _pframe_t *f, _preq_t *rq) {
  const int indent = f->u.n.indent;
  const bool in_flow = f->u.n.in_flow;
  const bool seq_ok_at_indent = f->u.n.seq_ok_at_indent;
  const bool allow_inline_map = f->u.n.allow_inline_map;
  const bool had_anchor = f->u.n.had_anchor;
  const bool had_tag = f->u.n.had_tag;
  const char *const resolved_tag = f->u.n.resolved_tag;

  switch (f->state) {
    case _NS_AFTER_ANCHOR_VALUE:
      goto after_anchor_value;
    case _NS_AFTER_TAG_VALUE:
      goto after_tag_value;
    case _NS_AFTER_COLLECTION:
      goto after_collection;
    case _NS_AFTER_FLOW_COLL:
      goto after_flow_coll;
    default:
      break;
  }

  {
    if (!in_flow)
      skip_ws_comments(ctx);
    else
      skip_inline_ws(ctx);

    /* A tab in the whitespace just skipped is separation whitespace in front
     * of a flow node, which YAML 1.2 allows after the indentation spaces. A
     * block collection can't follow it, because the start of every block
     * collection refuses the tab itself; see line_space_indent(). */

    if (at_end(ctx)) {
      if (!in_flow) return _pni_finish(f, node_alloc(CYAML_NULL, ctx->mp));
      parse_err(ctx, "unexpected end of input in flow context at position %zu",
                ctx->pos);
      return _PSTEP_FAIL;
    }

    /* A node never starts with a byte order mark at the start of a line,
     * because such a mark isn't content (see at_col0_bom()). Reading it as the
     * first character of a plain scalar would turn "\xEF\xBB\xBFb: 2" into a
     * key no other reader produces. */
    if (at_col0_bom(ctx)) {
      parse_err(ctx,
                "byte order mark inside a document at position %zu; a byte "
                "order mark may only precede a document",
                ctx->pos);
      return _PSTEP_FAIL;
    }

    if (in_flow && at_doc_marker(ctx)) {
      /* A '---' or '...' document marker at the start of a line is c-forbidden
       * (YAML 1.2 section 6.9), so it can never be plain scalar content in any
       * context. A block context handles it easily, since the enclosing loop
       * treats it as the end of the current collection, an ordinary document
       * boundary. An open flow collection has no valid way to end here at
       * all, because it needs its own closing ']' or '}' and not a
       * stream-level document boundary. Two independent reference parsers
       * confirm this. */
      parse_err(ctx,
                "a document marker cannot appear inside an unclosed "
                "flow collection at position %zu",
                ctx->pos);
      return _PSTEP_FAIL;
    }

    char c = cur(ctx);

    /* Anchor */
    if (c == '&') {
      /* In a block context "&anchor key: value" anchors only the key scalar,
       * not the whole dictionary the key turns out to introduce, so we check
       * for that shape first. Without the check the input would fall through
       * to the general anchor-then-recurse handling below, whose recursive
       * parse_node call reads every sibling entry too and anchors the whole
       * resulting dictionary. parse_one_dict_entry_key handles a later entry
       * the same way. allow_inline_map gates this fast path, because the
       * path always concludes "this is a valid key", so it must not run when
       * this call has no licence to open a fresh mapping here, as with the
       * chained implicit value "a: &x b: c". That input then falls through to
       * the ordinary anchor handling below, which sends it through the same
       * allow_inline_map rejection the plain-scalar dispatch already has. */
      if (!in_flow && allow_inline_map) {
        int col = current_col(ctx);
        char *anchor_name = NULL;
        char *key_str = NULL;
        bool is_merge_candidate = false;
        cyaml_node_t *anchor_value = NULL;
        if (try_parse_scalar_dict_key(ctx, &anchor_name, &key_str,
                                      &is_merge_candidate, &anchor_value)) {
          if (!register_key_anchor(ctx, anchor_name, key_str, anchor_value)) {
            _ccol_mem_free(ctx->mp, key_str);
            return _PSTEP_FAIL;
          }
          f->u.n.pending_key = key_str;
          _preq_block_dict(rq, col, key_str, /*colon_consumed=*/true,
                           is_merge_candidate);
          f->state = _NS_AFTER_COLLECTION;
          return _PSTEP_DESCEND;
        }
        if (ctx->error[0]) return _PSTEP_FAIL;
        /* Not a key after all. try_parse_scalar_dict_key has already restored
         * ctx->pos, so we fall through to the ordinary anchor handling
         * below. */
      }

      if (had_anchor) {
        /* This anchor doesn't decorate a key (the fast path above has already
         * ruled that out, or doesn't apply here), so it's ordinary content of
         * an ENCLOSING anchor, which has already used up the one anchor a
         * single node may carry. See the doc comment of had_anchor. */
        parse_err(ctx, "a node cannot carry two anchors at position %zu",
                  ctx->pos);
        return _PSTEP_FAIL;
      }

      ctx->pos++;
      if (!parse_anchor_name(ctx, &f->u.n.name)) return _PSTEP_FAIL;
      char *name = f->u.n.name;
      size_t after_name_pos = ctx->pos;
      skip_inline_ws(ctx);
      if (ctx->pos == after_name_pos && !at_end(ctx) &&
          (cur(ctx) == '[' || cur(ctx) == '{' ||
           (!in_flow &&
            (cur(ctx) == ',' || cur(ctx) == ']' || cur(ctx) == '}')))) {
        /* ns-anchor-char excludes every c-flow-indicator character in every
         * case, so parse_anchor_name always stops right before one, and what
         * comes next is only valid content when s-separate (real whitespace)
         * really separates it from the anchor. A flow-collection OPENER ('['
         * or '{') with no separator before it has no valid reading in ANY
         * context, because nothing in the grammar lets a brand-new nested
         * collection follow an anchor name without a separator, so we always
         * reject it, flow context included. ',', ']' and '}' are different:
         * each is valid, meaningful flow-collection structure right after an
         * anchor INSIDE a flow collection ("[&a, b]" and "[&a]", for
         * example), so we only reject those three in a block context, where
         * they mean nothing at all. A reference parser confirms this: it
         * rejects "[&a{x: 1}, *a]" and "[&a[1,2], *a]", which put an anchor
         * directly against a nested collection's opener, while it accepts an
         * anchor directly against a flow terminator. */
        parse_err(ctx,
                  "unexpected '%s' directly after anchor name at "
                  "position %zu",
                  _cyaml_echo_c(cur(ctx)), ctx->pos);
        _pni_release_name(ctx, f);
        return _PSTEP_FAIL;
      }

      cyaml_node_t *n;
      if (!in_flow && rest_of_line_is_blank(ctx)) {
        /* Nothing else is on this line apart from maybe a comment, so we make
         * the same check as parse_block_map_node before descending: a value
         * on a later line only counts when it really is more indented, or,
         * for a sequence, exactly as indented. Without the check a sibling
         * entry at or below `indent` (for example "b: 2" after "a: &anchor" at
         * the same column) would be read as this anchor's value: an unchecked
         * descent walks straight past the newline and pulls that sibling, and
         * everything after it, into the anchor. */
        skip_ws_comments(ctx);
        bool is_value_col =
            !at_end(ctx) && at_block_value_col(ctx, indent, seq_ok_at_indent);
        /* A document marker at the value column ends the document, so nothing
         * follows the property here either. */
        if (at_end(ctx) || !is_value_col || at_doc_marker(ctx)) {
          /* Nothing at all follows this anchor. We send this case through
           * finalize_scalar_node with empty text and implicit_ok=true instead
           * of building a bare CYAML_NULL node directly, because resolved_tag
           * can be an ENCLOSING tag handing work down to this anchor, as in
           * "!!str &a" with nothing after the anchor either. That tag has to
           * get the same chance to override the type of this empty content
           * that it gets at any other empty scalar position; a direct
           * CYAML_NULL node would silently drop the tag just because no anchor
           * content follows. */
          char *empty = ccol_strdup(ctx->mp, "");
          n = empty ? finalize_scalar_node(ctx, empty, resolved_tag, true)
                    : NULL;
        } else {
          /* A value confirmed to start on its own fresh line always gets the
           * "may open a fresh mapping here" privilege, whatever
           * allow_inline_map this call itself got; parse_block_map_node's
           * later-line branch writes true for the same reason, and only
           * SAME-LINE chaining after an already open implicit value is ever
           * restricted. We pass had_anchor=true because this call's anchor is
           * used up, and pass had_tag and resolved_tag on unchanged; the doc
           * comment of parse_node explains why. */
          _preq_node(rq, indent, in_flow, seq_ok_at_indent, true, true, had_tag,
                     resolved_tag);
          f->state = _NS_AFTER_ANCHOR_VALUE;
          return _PSTEP_DESCEND;
        }
      } else if (in_flow && rest_of_line_is_blank(ctx)) {
        /* A flow context with nothing else on this line apart from maybe a
         * comment, which ends the line here just as after an anchor in a block
         * context. A flow collection
         * may wrap over several lines, so the value can follow on a later
         * line, and there's no sibling-entry ambiguity to guard against the
         * way a block context has, because a flow collection's entries are
         * separated by its ',', ']' and '}' terminators and not by
         * indentation. We need to cross the newline with flow_skip_ws and not
         * a plain skip_ws_comments, which holds this continuation line to the
         * same s-separate(n,c) indentation and tab rules as every other
         * flow-collection continuation line in this file. skip_ws_comments
         * alone would silently accept a continuation line here indented with
         * a tab, or indented too little, which flow_skip_ws rejects
         * everywhere else. */
        if (!flow_skip_ws(ctx, indent)) {
          _pni_release_name(ctx, f);
          return _PSTEP_FAIL;
        }
        _preq_node(rq, indent, in_flow, seq_ok_at_indent, allow_inline_map,
                   true, had_tag, resolved_tag);
        f->state = _NS_AFTER_ANCHOR_VALUE;
        return _PSTEP_DESCEND;
      } else if (!in_flow && at_bare_seq_indicator(ctx)) {
        /* A block sequence can't start on the same line right after an anchor:
         * only a mapping gets that "compact" privilege, and only when a '-'
         * itself introduces it, not when a node property before it does.
         * try_parse_scalar_dict_key has already ruled out "this is an
         * anchored key" above, so nothing valid is left for a bare '-' here. */
        parse_err(ctx,
                  "a block sequence cannot start on the same line as an "
                  "anchor at position %zu",
                  ctx->pos);
        _pni_release_name(ctx, f);
        return _PSTEP_FAIL;
      } else if (!in_flow && cur(ctx) == '&') {
        /* This anchor's same-line content is another anchor, while
         * c-ns-properties allows at most one anchor per node. The had_anchor
         * check below can't catch this on its own, because the "is this an
         * anchored key" fast path above comes first, and that path silently
         * accepts the input whenever what follows the second anchor looks
         * like a valid key, as in "&a &b foo: bar". The same path correctly
         * accepts the valid two-line case, as in "top: &a\n  &b key: val",
         * which the rest_of_line_is_blank branch above handles, and this
         * branch isn't that one. So we catch the error here, before any
         * descent, instead of relying on the callee's had_anchor check, which
         * only runs once the callee's own fast path fails, for example when
         * nothing after the second anchor looks like a key. */
        parse_err(ctx, "a node cannot carry two anchors at position %zu",
                  ctx->pos);
        _pni_release_name(ctx, f);
        return _PSTEP_FAIL;
      } else if (!in_flow && cur(ctx) == '*') {
        /* An alias never carries a property, as a value or as a key, and the
         * descent's alias-as-key fast path would accept "&y *x : v" at the
         * first entry of a mapping and silently drop the anchor. */
        parse_err(ctx, "an alias cannot carry an anchor or tag at position %zu",
                  ctx->pos);
        _pni_release_name(ctx, f);
        return _PSTEP_FAIL;
      } else {
        _preq_node(rq, indent, in_flow, seq_ok_at_indent, allow_inline_map,
                   true, had_tag, resolved_tag);
        f->state = _NS_AFTER_ANCHOR_VALUE;
        return _PSTEP_DESCEND;
      }
      goto anchor_have_value;
    after_anchor_value:
      n = f->child;
      f->child = NULL;
      name = f->u.n.name;
    anchor_have_value:
      if (!n) {
        _pni_release_name(ctx, f);
        return _PSTEP_FAIL;
      }
      /* The anchor table points at n itself, which stays in the tree, so
       * registering copies nothing; an alias pays for its own copy. */
      if (!anchors_store(ctx, name, n)) {
        /* anchors_store() has already reported the specific error. n is a fully
         * valid parsed node, but the document as a whole must fail here;
         * otherwise a later '*name' alias reference to this anchor would
         * resolve against nothing, or worse against a stale entry from an
         * outer scope, and either way it would never get the real "out of
         * memory" failure. */
        __cyaml_destroy((cyaml)n);
        _pni_release_name(ctx, f);
        return _PSTEP_FAIL;
      }
      _pni_release_name(ctx, f);
      return _pni_finish(f, n);
    }

    /* Alias */
    if (c == '*') {
      /* "*alias : value" uses the alias directly as an implicit key, which
       * try_parse_scalar_dict_key handles the same way. This top-level
       * dispatch needs it too, because the parser also gets here the general
       * way, for example by descending through an anchor that's alone on its
       * line, as in "top: &node\n  *alias : value\n". allow_inline_map gates
       * this fast path for the same reason as the matching fast path of the
       * '&' branch above. */
      if (!in_flow && allow_inline_map) {
        int col = current_col(ctx);
        char *anchor_name = NULL;
        char *key_str = NULL;
        if (try_parse_scalar_dict_key(ctx, &anchor_name, &key_str, NULL,
                                      NULL)) {
          /* We only get here when the dispatch character itself is '*', so this
           * can only be the alias-as-key branch inside
           * try_parse_scalar_dict_key, which by construction is never a merge
           * candidate (see that function's doc comment). So this call doesn't
           * even ask for the out parameter. */
          _ccol_mem_free(ctx->mp,
                         anchor_name); /* an alias key has no name of its own */
          f->u.n.pending_key = key_str;
          _preq_block_dict(rq, col, key_str, /*colon_consumed=*/true, false);
          f->state = _NS_AFTER_COLLECTION;
          return _PSTEP_DESCEND;
        }
        if (ctx->error[0]) return _PSTEP_FAIL;
        /* Not a key after all. try_parse_scalar_dict_key has already restored
         * ctx->pos, so we fall through to the ordinary alias resolution
         * below. */
      }

      if (had_anchor || had_tag) {
        /* c-ns-alias-node is its own top-level alternative in ns-flow-node's
         * grammar, fully separate from the c-ns-properties branch that an
         * anchor or a tag decorates, so an alias can never carry a property
         * at all. Two independent reference parsers confirm it: both reject
         * "&anchor *alias" and "!!tag *alias" the same way. This rule is
         * different from, and stricter than, the stacked-properties check of
         * the '&' and '!' branches above, since even a SINGLE anchor or tag
         * on a bare alias is invalid, not only a second one. An alias-as-key
         * is another matter, and the fast path above already handles it. The
         * check reads had_tag, which every enclosing tag sets, a bare
         * non-specific "!" included. */
        parse_err(ctx, "an alias cannot carry an anchor or tag at position %zu",
                  ctx->pos);
        return _PSTEP_FAIL;
      }

      /* An unknown alias is reported at its '*', where the token starts. */
      const size_t alias_start = ctx->pos;
      ctx->pos++;
      char *name = NULL;
      if (!parse_anchor_name(ctx, &name)) return _PSTEP_FAIL;
      cyaml_node_t *anchored = anchors_lookup(ctx, name);
      if (!anchored) {
        parse_err(ctx, "unknown alias '*%s' at position %zu", _cyaml_echo(name),
                  alias_start);
        _ccol_mem_free(ctx->mp, name);
        return _PSTEP_FAIL;
      }
      /* The copy lands below the ctx->depth containers around this node, so it
       * may only use the levels that are left. clone_node() measures the
       * anchored subtree's height in the walk that copies it, so the check
       * costs nothing on top of that walk. The value of a merge key counts
       * here too, at the place where it stands before the merge moves its
       * members up one level. */
      bool too_deep = false;
      size_t height = 0;
      cyaml_node_t *clone = parse_clone_levels(
          anchored, CYAML_MAX_NESTING_DEPTH - ctx->depth, &too_deep, &height);
      if (clone && ctx->depth + height > ctx->peak_depth)
        ctx->peak_depth = ctx->depth + height;
      if (!clone && too_deep)
        parse_err(ctx,
                  "alias '*%s' expansion exceeds the maximum nesting depth "
                  "(%d) at position %zu",
                  _cyaml_echo(name), CYAML_MAX_NESTING_DEPTH, ctx->pos);
      else if (!clone && _parse_amp_limit_hit)
        parse_err(ctx,
                  "alias '*%s' expansion exceeded the %zu-node, %zu-byte "
                  "alias expansion memory limit at position %zu; check for "
                  "exponential anchor/alias nesting",
                  _cyaml_echo(name), (size_t)_CYAML_PARSE_NODE_FLOOR,
                  (size_t)_CYAML_PARSE_BYTE_FLOOR, ctx->pos);
      else if (!clone && _parse_node_budget_exhausted)
        parse_err(ctx,
                  "alias '*%s' expansion exceeded the %zu node-allocation "
                  "limit at position %zu; check for exponential anchor/alias "
                  "nesting",
                  _cyaml_echo(name), _parse_node_limit, ctx->pos);
      else if (!clone && _parse_byte_budget_exhausted)
        parse_err(ctx,
                  "alias '*%s' expansion exceeded the %zu-byte document memory "
                  "limit at position %zu; check for exponential anchor/alias "
                  "nesting",
                  _cyaml_echo(name), _parse_byte_limit, ctx->pos);
      _ccol_mem_free(ctx->mp, name);
      return _pni_finish(f, clone);
    }

    /* Tag */
    if (c == '!') {
      /* Same check as in the '&' branch just above: "!!tag key: value" tags
       * only the key scalar, not the whole dictionary the key turns out to
       * introduce. try_parse_scalar_dict_key already handles a bare tag prefix
       * without an anchor before it, but c-ns-properties allows the anchor and
       * the tag in EITHER order, so this key may also carry an anchor that the
       * same call found, as in "!!str &a1 key:". We must register that
       * anchor exactly as the '&' branch does; otherwise it would be silently
       * dropped just because the parser came into THIS branch through the tag
       * instead of the anchor. allow_inline_map gates this fast path for the
       * same reason as the matching fast path of the '&' branch above. */
      if (!in_flow && allow_inline_map) {
        int col = current_col(ctx);
        char *anchor_name = NULL;
        char *key_str = NULL;
        cyaml_node_t *anchor_value = NULL;
        bool is_merge_candidate = false;
        if (try_parse_scalar_dict_key(ctx, &anchor_name, &key_str,
                                      &is_merge_candidate, &anchor_value)) {
          /* We only get here when the dispatch character itself is '!', so the
           * key carries a tag, and it's a merge key only when that tag is the
           * core merge tag. See the doc comment of
           * try_parse_scalar_dict_key. */
          if (!register_key_anchor(ctx, anchor_name, key_str, anchor_value)) {
            _ccol_mem_free(ctx->mp, key_str);
            return _PSTEP_FAIL;
          }
          f->u.n.pending_key = key_str;
          _preq_block_dict(rq, col, key_str, /*colon_consumed=*/true,
                           is_merge_candidate);
          f->state = _NS_AFTER_COLLECTION;
          return _PSTEP_DESCEND;
        }
        if (ctx->error[0]) return _PSTEP_FAIL;
        /* Not a key after all. try_parse_scalar_dict_key has already restored
         * ctx->pos, so we fall through to the ordinary tag handling below. */
      }

      if (had_tag) {
        /* Same as the had_anchor check of the '&' branch above: this tag is
         * ordinary content of an ENCLOSING tag, which has already used up the
         * one tag a single node may carry. The check reads had_tag, the one
         * signal every enclosing tag sets, the bare non-specific "!"
         * included. */
        parse_err(ctx, "a node cannot carry two tags at position %zu",
                  ctx->pos);
        return _PSTEP_FAIL;
      }

      if (!parse_tag_token(ctx, &f->u.n.tag)) return _PSTEP_FAIL;
      char *tag = f->u.n.tag;
      size_t after_tag_pos = ctx->pos;
      skip_inline_ws(ctx);
      if (ctx->pos == after_tag_pos && !at_end(ctx) &&
          (cur(ctx) == '[' || cur(ctx) == '{' ||
           (!in_flow &&
            (cur(ctx) == ',' || cur(ctx) == ']' || cur(ctx) == '}')))) {
        /* Same check as in the '&' branch above, whose comment says more. A
         * flow-collection OPENER ('[' or '{') right against a tag, as in
         * "!!seq[1,2]" inside "[!!seq[1,2]]", has no valid reading in any
         * context, so we reject it even in a flow context. ',', ']' and '}'
         * are valid right after a tag INSIDE a flow collection, as in
         * "[!!str, x]", so those three are only rejected in a block context,
         * where they mean nothing at all, as with "!!str,xxx", which a
         * reference parser confirms. */
        parse_err(ctx, "unexpected '%s' directly after tag at position %zu",
                  _cyaml_echo_c(cur(ctx)), ctx->pos);
        _pni_release_tag(ctx, f);
        return _PSTEP_FAIL;
      }
      if (ctx->pos == after_tag_pos && !at_end(ctx) && cur(ctx) != '\n' &&
          cur(ctx) != '\r' && cur(ctx) != '#' && cur(ctx) != '!' &&
          cur(ctx) != '&' && cur(ctx) != '*' &&
          !(in_flow &&
            (cur(ctx) == ',' || cur(ctx) == ']' || cur(ctx) == '}'))) {
        /* c-ns-properties needs an s-separate(n,c) before any following
         * ns-flow-content, so a verbatim or shorthand tag with no separating
         * whitespace at all before ordinary content ("!<a>b", a quote, a block
         * scalar indicator, or plain scalar text) has no valid grammar path. A
         * reference parser confirms this and rejects both "!<a>b" and
         * "!<a>|\n  x\n". The flow-opener case just above already covers '['
         * and '{', and a following '#', '!', '&' or '*' is left to the more
         * specific checks elsewhere in this branch, which deal with a comment,
         * a stacked tag, and an anchor or alias after the tag. */
        parse_err(ctx,
                  "tag must be separated from its content by whitespace at "
                  "position %zu",
                  ctx->pos);
        _pni_release_tag(ctx, f);
        return _PSTEP_FAIL;
      }

      cyaml_node_t *n;
      if (!in_flow && rest_of_line_is_blank(ctx)) {
        /* Nothing else is on this line apart from maybe a comment, so we make
         * the same guard as the '&' branch, whose comment is above. Without
         * it a sibling entry at or below `indent` (for example a list entry at
         * the same column as the "- !!str" itself) would be read as this tag's
         * value: an unchecked descent, whenever the rest of the line is blank,
         * walks straight past the newline and pulls that sibling, and
         * everything after it, into this tag's value, just as the '&' branch's
         * comment warns. */
        skip_ws_comments(ctx);
        bool is_value_col =
            !at_end(ctx) && at_block_value_col(ctx, indent, seq_ok_at_indent);
        /* A document marker at the value column ends the document, so nothing
         * follows the property here either. */
        if (at_end(ctx) || !is_value_col || at_doc_marker(ctx)) {
          /* Nothing at all follows this tag. We send this case through
           * finalize_scalar_node with empty text instead of building a bare
           * CYAML_NULL directly, so a "!!str" with nothing after it resolves
           * to an empty string and not to null. */
          char *empty = ccol_strdup(ctx->mp, "");
          n = empty ? finalize_scalar_node(ctx, empty, tag, true) : NULL;
        } else {
          _preq_node(rq, indent, in_flow, seq_ok_at_indent, true, had_anchor,
                     true, tag);
          f->state = _NS_AFTER_TAG_VALUE;
          return _PSTEP_DESCEND;
        }
      } else if (in_flow && rest_of_line_is_blank(ctx)) {
        /* A flow context with nothing else on this line apart from maybe a
         * comment, where the tagged value may wrap onto a following line, as
         * in the same case of the '&' branch above. We
         * have to cross the newline with flow_skip_ws and not a plain
         * skip_ws_comments, which holds this continuation line to the same
         * s-separate(n,c) indentation and tab rules as every other
         * flow-collection continuation line in this file. The skip at the top
         * of parse_node only covers inline whitespace when in_flow is true, so
         * we cross the newline explicitly before the descent;
         * otherwise the dispatch would see a '\n' next and read it as an empty
         * plain scalar. There's no sibling-entry ambiguity to guard against
         * here the way the block-context branch above has, because a flow
         * collection's entries are separated by its ',', ']' and '}'
         * terminators and not by indentation. */
        if (!flow_skip_ws(ctx, indent)) {
          _pni_release_tag(ctx, f);
          return _PSTEP_FAIL;
        }
        _preq_node(rq, indent, in_flow, seq_ok_at_indent, allow_inline_map,
                   had_anchor, true, tag);
        f->state = _NS_AFTER_TAG_VALUE;
        return _PSTEP_DESCEND;
      } else if (!in_flow && at_bare_seq_indicator(ctx)) {
        /* Same check as in the '&' branch above: a block sequence can't start
         * on the same line right after a tag either, because only a mapping
         * gets that "compact" privilege, and only when a '-' itself introduces
         * it, not when a node property before it does. */
        parse_err(ctx,
                  "a block sequence cannot start on the same line as a "
                  "tag at position %zu",
                  ctx->pos);
        _pni_release_tag(ctx, f);
        return _PSTEP_FAIL;
      } else if (!in_flow && cur(ctx) == '!') {
        /* This tag's same-line content is another tag, while c-ns-properties
         * allows at most one tag per node. It mirrors the same-line
         * double-anchor check of the '&' branch above, whose comment explains
         * why the had_tag check further up can't catch this on its own. */
        parse_err(ctx, "a node cannot carry two tags at position %zu",
                  ctx->pos);
        _pni_release_tag(ctx, f);
        return _PSTEP_FAIL;
      } else if (!in_flow && cur(ctx) == '*') {
        /* As in the '&' branch above: an alias never carries a tag, and the
         * descent's alias-as-key fast path would accept "! *x : v" at the
         * first entry of a mapping and silently drop the tag. */
        parse_err(ctx, "an alias cannot carry an anchor or tag at position %zu",
                  ctx->pos);
        _pni_release_tag(ctx, f);
        return _PSTEP_FAIL;
      } else {
        _preq_node(rq, indent, in_flow, seq_ok_at_indent, allow_inline_map,
                   had_anchor, true, tag);
        f->state = _NS_AFTER_TAG_VALUE;
        return _PSTEP_DESCEND;
      }
      goto tag_have_value;
    after_tag_value:
      n = f->child;
      f->child = NULL;
    tag_have_value:
      if (!n) {
        _pni_release_tag(ctx, f);
        return _PSTEP_FAIL;
      }

      /* By now `tag` has already been consumed at the exact place of
       * construction, which can be many descent layers deep:
       * finalize_scalar_node for a scalar, or finalize_collection_node for a
       * collection. Both get `tag` unchanged through any '&' anchor layers
       * in between, and the doc comment of either function explains why the
       * attach can't wait until here, after the descent returns: an anchor
       * can sit between this tag and the real content, and such an anchor
       * registers the node before this point ever runs. That whole chain
       * only borrowed this
       * local copy and nothing ever took it over, so it's freed here in every
       * case, whatever path the parse took. */
      _pni_release_tag(ctx, f);
      return _pni_finish(f, n);
    }

    /* Flow list */
    /* Flow dictionary */
    if (c == '[' || c == '{') {
      f->u.n.col = current_col(ctx);
      f->u.n.coll_start = ctx->pos;
      _preq_flow(rq, c == '[', indent);
      f->state = _NS_AFTER_FLOW_COLL;
      return _PSTEP_DESCEND;
    after_flow_coll:;
      cyaml_node_t *coll = f->child;
      f->child = NULL;
      if (!coll) return _PSTEP_FAIL;

      /* In a block context a flow collection directly followed by a ':' is an
       * implicit block mapping key itself, even though it isn't a scalar:
       * YAML 1.2 section 8.2.2's "compact mapping" form
       * (ns-l-compact-mapping) allows any ns-flow-node as a key here, not
       * only a scalar, and the plain and quoted scalar dispatch branches
       * already make the same check for a scalar key. But like every implicit
       * key, scalar or not, it must fit on one line, as
       * ns-s-implicit-yaml-key says, so a flow collection spanning several
       * lines is a valid standalone value and never an implicit key,
       * whatever follows it. */
      bool spans_multiple_lines = span_crosses_newline(ctx, f->u.n.coll_start);
      if (!in_flow && !spans_multiple_lines) {
        skip_inline_ws(ctx);
        if (!at_end(ctx) && cur(ctx) == ':') {
          char after =
              (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
          if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
              ctx->pos + 1 >= ctx->len) {
            if (!allow_inline_map) {
              /* The same-line chain restriction as in the scalar-key cases
               * above: "a: [1,2]: c" has no valid reading, just as "a: b: c"
               * has none. See the doc comment of allow_inline_map. */
              parse_err(ctx,
                        "mapping values are not allowed here "
                        "(position %zu)",
                        ctx->pos);
              __cyaml_destroy((cyaml)coll);
              return _PSTEP_FAIL;
            }
            if (!parse_key_fits_below_mapping(ctx, coll)) {
              __cyaml_destroy((cyaml)coll);
              return _PSTEP_FAIL;
            }
            char *key_str =
                node_to_dict_key_string(ctx, coll, "block dictionary");
            if (!key_str) return _PSTEP_FAIL;
            /* A flow collection can never be a merge-key candidate, since "<<"
             * is always a plain scalar. */
            /* The matching comment in the double-quoted scalar branch explains
             * why resolved_tag can safely be attached to the mapping that
             * results here, rather than to the flow collection that turned
             * into this mapping's key. */
            f->u.n.pending_key = key_str;
            _preq_block_dict(rq, f->u.n.col, key_str, false, false);
            f->state = _NS_AFTER_COLLECTION;
            return _PSTEP_DESCEND;
          }
        }
      }
      return _pni_finish(f, finalize_collection_node(ctx, coll, resolved_tag));
    }

    /* Literal block scalar */
    if (c == '|' && !in_flow) {
      ctx->pos++;
      chomp_t chomp;
      int exp_indent;
      if (!parse_block_scalar_header(ctx, &chomp, &exp_indent))
        return _PSTEP_FAIL;
      char *s = NULL;
      if (!parse_block_scalar_content(ctx, indent, chomp, exp_indent, &s))
        return _PSTEP_FAIL;
      return _pni_finish(f, finalize_scalar_node(ctx, s, resolved_tag, false));
    }

    /* Folded block scalar */
    if (c == '>' && !in_flow) {
      ctx->pos++;
      chomp_t chomp;
      int exp_indent;
      if (!parse_block_scalar_header(ctx, &chomp, &exp_indent))
        return _PSTEP_FAIL;
      char *s = NULL;
      if (!parse_folded_scalar_content(ctx, indent, chomp, exp_indent, &s))
        return _PSTEP_FAIL;
      return _pni_finish(f, finalize_scalar_node(ctx, s, resolved_tag, false));
    }

    /* Block list (- item) */
    if (c == '-' && !in_flow) {
      if (ctx->pos + 1 < ctx->len) {
        char next = ctx->src[ctx->pos + 1];
        /* A tab after the '-' is separation whitespace (s-separate-in-line,
         * YAML 1.2 section 6.2) just like a space, and a scalar or a flow
         * collection may follow it. A block collection may not follow a tab,
         * because a tab has no defined width, and the start of every block
         * collection refuses a tab in the whitespace before it on its line;
         * see line_indent_has_tab(). */
        if (next == ' ' || next == '\t' || next == '\n' || next == '\r') {
          _preq_block_list(rq, current_col(ctx));
          f->state = _NS_AFTER_COLLECTION;
          return _PSTEP_DESCEND;
        }
      } else {
        /* A '-' at the very end of the input is a one-element list whose value
         * is null. */
        _preq_block_list(rq, current_col(ctx));
        f->state = _NS_AFTER_COLLECTION;
        return _PSTEP_DESCEND;
      }
    }

    /* Block mapping (explicit "? key" / ": value") */
    if (c == '?' && !in_flow) {
      bool is_explicit_key = false;
      if (ctx->pos + 1 < ctx->len) {
        char next = ctx->src[ctx->pos + 1];
        /* A tab is separation whitespace here too; see the '-' branch above. */
        if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
          is_explicit_key = true;
      } else {
        is_explicit_key = true; /* a '?' at the very end of the input */
      }
      if (is_explicit_key) {
        if (!allow_inline_map) {
          /* The same-line chain restriction as in every other "this could open
           * a fresh mapping here" case above (a plain or quoted scalar key, and
           * a flow-collection key): ns-l-block-map-implicit-value has no
           * compact-mapping alternative, so a NEW explicit-key mapping starting
           * from the same-line content of an ordinary implicit value has no
           * valid reading either, as in "a: ? b\n : c". Otherwise the explicit
           * form is fully legal once the parser is inside explicit-style
           * content, where allow_inline_map is already true. */
          parse_err(ctx,
                    "mapping values are not allowed here "
                    "(position %zu)",
                    ctx->pos);
          return _PSTEP_FAIL;
        }
        _preq_block_dict(rq, current_col(ctx), NULL, false, false);
        f->state = _NS_AFTER_COLLECTION;
        return _PSTEP_DESCEND;
      }
    }

    /* Double-quoted scalar */
    if (c == '"') {
      int col = current_col(ctx);
      size_t quote_start = ctx->pos;
      char *s = NULL;
      if (!parse_double_quoted(ctx, &s)) return _PSTEP_FAIL;
      /* In block context a quoted scalar may be a dictionary key. */
      if (!in_flow) {
        skip_inline_ws(ctx);
        if (!at_end(ctx) && cur(ctx) == ':') {
          char after =
              (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
          if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
              ctx->pos + 1 >= ctx->len) {
            if (span_crosses_newline(ctx, quote_start)) {
              /* An implicit key always stays on one line, as
               * ns-s-implicit-yaml-key says, for a quoted key just as for a
               * plain scalar key, so a quoted scalar spanning several lines
               * has no valid reading as a key. Two independent reference
               * parsers confirm this. */
              parse_err(ctx,
                        "implicit keys cannot span multiple lines at "
                        "position %zu",
                        ctx->pos);
              _ccol_mem_free(ctx->mp, s);
              return _PSTEP_FAIL;
            }
            if (!allow_inline_map) {
              /* The same-line chain restriction as in the plain-scalar case
               * above: "a: 'b': c" has no valid reading, just as "a: b: c"
               * has none. See the doc comment of allow_inline_map. */
              parse_err(ctx,
                        "mapping values are not allowed here "
                        "(position %zu)",
                        ctx->pos);
              _ccol_mem_free(ctx->mp, s);
              return _PSTEP_FAIL;
            }
            /* A quoted scalar can never be a merge-key candidate, since only
             * the plain scalar production is ever eligible; see the doc comment
             * of try_parse_scalar_dict_key. */
            f->u.n.pending_key = s;
            _preq_block_dict(rq, col, s, false, false);
            f->state = _NS_AFTER_COLLECTION;
            /* A resolved_tag can only reach this "the value turns out to be a
             * key" shape in one way, through one or more '&' anchor layers
             * passing the tag along, because a tag sitting right next to a key
             * is always caught earlier, by try_parse_scalar_dict_key's preamble
             * in the '&' and '!' branches. So there's no doubt here about what
             * the tag belongs to: it decorates the whole resulting mapping,
             * just as in the explicit '?' case and the flow-collection-as-key
             * case elsewhere in this function. */
            return _PSTEP_DESCEND;
          }
        }
      }
      /* A quoted scalar is always a string with no implicit typing, although
       * an explicit tag can force another type. */
      return _pni_finish(f, finalize_scalar_node(ctx, s, resolved_tag, false));
    }

    /* Single-quoted scalar */
    if (c == '\'') {
      int col = current_col(ctx);
      size_t quote_start = ctx->pos;
      char *s = NULL;
      if (!parse_single_quoted(ctx, &s)) return _PSTEP_FAIL;
      /* In block context a quoted scalar may be a dictionary key. */
      if (!in_flow) {
        skip_inline_ws(ctx);
        if (!at_end(ctx) && cur(ctx) == ':') {
          char after =
              (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
          if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
              ctx->pos + 1 >= ctx->len) {
            if (span_crosses_newline(ctx, quote_start)) {
              /* See the matching check in the double-quoted branch above. */
              parse_err(ctx,
                        "implicit keys cannot span multiple lines at "
                        "position %zu",
                        ctx->pos);
              _ccol_mem_free(ctx->mp, s);
              return _PSTEP_FAIL;
            }
            if (!allow_inline_map) {
              /* The same-line chain restriction as in the plain-scalar case
               * above: "a: 'b': c" has no valid reading, just as "a: b: c"
               * has none. See the doc comment of allow_inline_map. */
              parse_err(ctx,
                        "mapping values are not allowed here "
                        "(position %zu)",
                        ctx->pos);
              _ccol_mem_free(ctx->mp, s);
              return _PSTEP_FAIL;
            }
            /* A quoted scalar can never be a merge-key candidate. See the
             * matching comment in the double-quoted branch. */
            f->u.n.pending_key = s;
            _preq_block_dict(rq, col, s, false, false);
            f->state = _NS_AFTER_COLLECTION;
            /* See the matching comment in the double-quoted branch above. */
            return _PSTEP_DESCEND;
          }
        }
      }
      return _pni_finish(f, finalize_scalar_node(ctx, s, resolved_tag, false));
    }

    /* A '#' can never start a plain scalar in any context, because YAML 1.2
     * section 6.6's ns-plain-first excludes it outright. A '#' that gets here
     * means skip_ws_comments or skip_inline_ws already tried to treat it as a
     * comment and couldn't, because no whitespace, no start of the input and
     * no newline came before it, so there's no valid reading left. */
    if (c == '#') {
      parse_err(ctx,
                "unexpected '#' at position %zu (a comment must be "
                "preceded by whitespace)",
                ctx->pos);
      return _PSTEP_FAIL;
    }

    /* '%', '@' and '`' can never start a plain scalar in any context either:
     * YAML 1.2 section 6.6's ns-plain-first excludes all three c-indicator
     * characters outright, and section 5.5 also reserves '@' and '`' for
     * future use. One of them getting here means every earlier, more
     * specific dispatch (directives, tags, anchors, aliases, block and flow
     * indicators, and quoted scalars) has declined to claim it, so there's no
     * valid reading left, just as for the '#' above. The serializer's
     * needs_quoting() already quotes all three on output; without this check
     * the parser would silently accept unquoted input that this library's
     * serializer would never produce. */
    if (c == '%' || c == '@' || c == '`') {
      parse_err(ctx,
                "unexpected '%s' at position %zu (not a valid plain scalar "
                "start character)",
                _cyaml_echo_c(c), ctx->pos);
      return _PSTEP_FAIL;
    }

    /* The flow indicators ']', '}' and ',' are c-indicator characters too, so
     * the same production keeps them out of the first character of a plain
     * scalar in a block context ("a: ]", "]: x", "- ,a"). In a flow context
     * one of them getting here ends an empty node, as in "[&a, b]" or
     * "[!!str]", and the plain-scalar scan below gives that empty scalar. */
    if (!in_flow && (c == ']' || c == '}' || c == ',')) {
      parse_err(ctx,
                "unexpected '%s' at position %zu (a flow indicator cannot "
                "start a plain scalar)",
                _cyaml_echo_c(c), ctx->pos);
      return _PSTEP_FAIL;
    }

    /* In a flow context a '-', '?' or ':' followed by a flow indicator,
     * whitespace, the end of the line or the end of the input has no valid
     * reading at all. See the doc comment of
     * at_valid_flow_plain_scalar_start. */
    if (in_flow && !at_valid_flow_plain_scalar_start(ctx)) {
      parse_err(ctx,
                "unexpected '%s' at position %zu (not a valid plain "
                "scalar in flow context)",
                _cyaml_echo_c(c), ctx->pos);
      return _PSTEP_FAIL;
    }

    /* Plain scalar (or block dictionary key) */
    {
      int col = current_col(ctx);
      char *s = NULL;
      bool hit_real_terminator = false;
      if (!parse_plain_scalar(ctx, in_flow, &s, &hit_real_terminator))
        return _PSTEP_FAIL;

      /* Check whether a ':' value indicator follows, which would make this
       * scalar a dictionary key. An implicit key always stays on one line, so
       * this check must run before any multi-line continuation attempt
       * below. */
      if (!in_flow) {
        skip_inline_ws(ctx);
        if (!at_end(ctx) && cur(ctx) == ':') {
          char after =
              (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
          if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
              ctx->pos + 1 >= ctx->len) {
            if (!allow_inline_map) {
              /* This scalar is itself the same-line content of an ordinary
               * implicit value, so it has no licence to open yet another
               * nested mapping here, as in "a: b: c". The doc comment of
               * allow_inline_map, above parse_node, gives the reasoning and
               * the reference-parser check. */
              parse_err(ctx,
                        "mapping values are not allowed here "
                        "(position %zu)",
                        ctx->pos);
              _ccol_mem_free(ctx->mp, s);
              return _PSTEP_FAIL;
            }
            /* A block dictionary key. The comment in the double-quoted branch
             * above explains why resolved_tag can safely be attached to the
             * resulting mapping here.
             *
             * This is also the one "becomes a key" site that a real untagged
             * plain "<<" merge-key trigger can reach, since an
             * anchor-decorated "<<" goes to the '&' branch's preamble instead.
             * The doc comment of try_parse_scalar_dict_key gives the exact
             * rule it must satisfy too.
             *
             * resolved_tag here is whatever tag an ENCLOSING '!' layer passes
             * down to the whole resulting mapping, as in
             * "m: !!map\n  <<: *b\n", and it says nothing about whether the
             * KEY "<<" itself has a tag: the '!' branch's fast path above
             * already catches a tag placed directly on the key, so such a tag
             * never reaches this plain-scalar dispatch at all. A rule that
             * disqualified the merge whenever the enclosing collection
             * happens to carry a tag would silently leave "<<" as a literal
             * key with no expansion. */
            bool is_merge_candidate = strcmp(s, "<<") == 0;
            /* This plain-scalar key must reach its canonical form through the
             * same core-schema typing that node_to_dict_key_string() already
             * applies everywhere ELSE a key is captured: three branches of
             * try_parse_scalar_dict_key (a flow collection, an alias as a key,
             * and a plain scalar with an anchor or a tag), a key of a flow
             * dictionary or flow sequence, and an explicit "? key" block key.
             * Without it a plain first entry such as "~: v" would store the
             * literal text "~" while "? ~\n: v" stores "null" for the
             * identical value, although YAML 1.2 treats the two notations as
             * exactly the same entry. is_merge_candidate is computed above
             * from the raw text s on purpose, like every other merge-key check
             * in this file, because only a literal "<<" with no type
             * resolution is ever a trigger. */
            cyaml_node_t *typed = make_typed_scalar(ctx, s);
            if (!typed) {
              parse_err(ctx,
                        "out of memory typing dictionary key at position %zu",
                        ctx->pos);
              _ccol_mem_free(ctx->mp, s);
              return _PSTEP_FAIL;
            }
            char *canon_key =
                node_to_dict_key_string(ctx, typed, "block dictionary");
            _ccol_mem_free(ctx->mp, s);
            if (!canon_key) return _PSTEP_FAIL;
            f->u.n.pending_key = canon_key;
            _preq_block_dict(rq, col, canon_key, false, is_merge_candidate);
            f->state = _NS_AFTER_COLLECTION;
            return _PSTEP_DESCEND;
          }
        }
      }

      /* Not a dictionary key, so, as YAML 1.2 section 7.3.3 says, a plain
       * scalar's value may span several lines. */
      char *full = NULL;
      if (!parse_plain_scalar_multiline(ctx, in_flow, indent, s,
                                        hit_real_terminator, &full))
        return _PSTEP_FAIL;

      return _pni_finish(f,
                         finalize_scalar_node(ctx, full, resolved_tag, true));
    }
  }

after_collection: {
  /* Every block collection this node turned out to be arrives here: a key
   * that opens a mapping, with an anchor, an alias or a tag decorating that
   * key; a flow collection that turned out to be a mapping key; a '-'
   * sequence; an explicit '?' mapping; and a quoted or plain scalar key that
   * opens a mapping.
   * Each first frees the key text it owns, if it owns one, and then
   * decorates the finished collection with whatever tag an enclosing layer
   * passed down to it, in that order. */
  cyaml_node_t *coll = f->child;
  f->child = NULL;
  if (f->u.n.pending_key) {
    _ccol_mem_free(ctx->mp, f->u.n.pending_key);
    f->u.n.pending_key = NULL;
  }
  return _pni_finish(f, finalize_collection_node(ctx, coll, resolved_tag));
}
}

/* Block list */

/*
 * Parses a block sequence, '-' entries at column f->u.bl.seq_indent. Each
 * entry's value is a descent, and everything else is straight-line code.
 */
static _pstep_t _pbl_step(parse_ctx_t *ctx, _pframe_t *f, _preq_t *rq) {
  const int seq_indent = f->u.bl.seq_indent;
  cyaml_node_t *elem = NULL;

  if (f->state == _BLS_AFTER_ELEM) goto after_elem;

  f->u.bl.seq = (cyaml_node_t *)cyaml_create_list_interned(ctx->mp);
  if (!f->u.bl.seq) return _PSTEP_FAIL;

  while (!at_end(ctx)) {
    /* Make sure we're at the right indent and at the '-' indicator. */
    skip_ws_comments(ctx);
    if (at_end(ctx)) break;

    if (line_indent_has_tab(ctx)) {
      parse_err(ctx,
                "tab cannot be used as block sequence indentation at "
                "position %zu",
                ctx->pos);
      return _PSTEP_FAIL;
    }
    if (current_col(ctx) != seq_indent) break;
    if (cur(ctx) != '-') break;

    /* Is the '-' a list indicator? */
    {
      bool is_seq_entry = false;
      if (ctx->pos + 1 < ctx->len) {
        char next = ctx->src[ctx->pos + 1];
        /* See the '-' branch of the node dispatch: a tab is separation
         * whitespace, just like a space. */
        if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
          is_seq_entry = true;
      } else {
        is_seq_entry = true; /* a '-' at the end of the input */
      }

      if (!is_seq_entry) break;
    }

    ctx->pos++; /* consume '-' */

    /* Skip the optional space after '-'. Any further spaces and tabs are
     * separation whitespace, which the node dispatch skips. "- \t-" and
     * "-\t-" still fail, because the nested block sequence refuses the tab in
     * the whitespace before it on its line. */
    if (!at_end(ctx) && cur(ctx) == ' ') ctx->pos++;

    /* Decide where the element's value is. We use rest_of_line_is_blank()
     * instead of a bare at_eol() or at_end() check, which is what makes
     * "- # Empty" (a dash followed only by a comment) behave like a bare "-".
     * cur(ctx) is a '#' here and not a newline, so at_eol() alone would
     * conclude that real content starts here and hand the comment text itself
     * to the node dispatch as this entry's value, which would then pull in
     * every following sibling entry as nested content. The right result is a
     * null entry followed by its own separate siblings. */
    if (rest_of_line_is_blank(ctx) || at_end(ctx)) {
      /* Nothing follows the '-' on this line, so look ahead at the next
       * non-empty line. That line is the element's value when it is more
       * indented than seq_indent, and in every other case (same indent, less
       * indent, or end of input) the element is null. */
      skip_ws_comments(ctx);
      /* A tab in the leading whitespace is separation, so only the spaces
       * before it count; see line_space_indent(). */
      if (at_end(ctx) || line_space_indent(ctx) <= seq_indent) {
        elem = node_alloc(CYAML_NULL, ctx->mp);
        goto have_elem;
      }
    }
    /* The element starts on the same line or on a more indented later line.
     * We pass seq_indent so that a block scalar nested inside the element
     * uses the list's indent as its parent_indent, as the YAML spec needs.
     * seq_ok_at_indent is false because seq_indent is a sequence's indent and
     * not a mapping's, so a '-' at exactly that column is always a sibling
     * element of this same list and never nested content; the parser only
     * reaches such a '-' through an anchor or a tag decorating this very
     * element. allow_inline_map is true because "- key: value", a compact
     * mapping right after the dash, is always a valid, ordinary pattern: the
     * "a: b: c" chain restriction only applies to the same-line content of an
     * ordinary implicit mapping VALUE, not to a sequence element. */
    _preq_node(rq, seq_indent, false, false, true, false, false, NULL);
    f->state = _BLS_AFTER_ELEM;
    return _PSTEP_DESCEND;
  after_elem:
    elem = f->child;
    f->child = NULL;
  have_elem:
    if (!elem) return _PSTEP_FAIL;

    {
      cyaml_node_t *ep = elem;
      if (cvector_push_back(f->u.bl.seq->value.list, &ep) != ccol_success) {
        __cyaml_destroy((cyaml)elem);
        return _PSTEP_FAIL;
      }
      /* The list owns the node from here on and this flag records that, so a
       * later attach of the node through a borrowed cyaml_list_get()
       * reference is refused. Without the flag that attach would give the
       * node a second owner. */
      ep->attached = true;
    }
  }

  f->result = f->u.bl.seq;
  f->u.bl.seq = NULL;
  return _PSTEP_DONE;
}

/* Block dictionary */

/*
 * Parses the node in a block dictionary's key or value position: on the
 * same line as the '?' or ':' indicator before it, or, when nothing follows
 * before the end of the line, on a more indented later line. Otherwise the
 * position holds an implicit null. Three call sites share it (the
 * explicit-key and explicit-value sides of parse_block_dictionary, and the
 * value of an implicit entry), and all three share YAML 1.2 section 8.2.2's
 * placement grammar for content on a following line.
 *
 * allow_inline_seq marks the one point where explicit and implicit entries
 * really differ for a SEQUENCE. Explicit-style content, both the '?' key's
 * content and the ':' value after it, is s-l+block-indented in the grammar,
 * and that production's "compact" alternative lets a bare '-' sequence start
 * on the very same line; a reference parser confirms this rather than us
 * assuming it. The value of an ordinary implicit entry ("key: value") is
 * ns-l-block-map-implicit-value instead, which has no compact alternative at
 * all, so a sequence value there must always start on its own line.
 *
 * The same asymmetry holds for a nested MAPPING chained on the very same
 * line: "a: b: c" has no valid reading and is a hard error, while
 * "? k\n: a: b" is accepted. Both restrictions apply together and lift
 * together at exactly the same call site, so allow_inline_seq is passed
 * straight through as parse_node's allow_inline_map argument below, since a
 * second parameter that always held the same value would add nothing.
 * Callers only pass true for explicit-style key or value content.
 *
 * is_merge_candidate_out, when not NULL, gets whether the node about to be
 * parsed is a real "<<" merge-key candidate (see the doc comment of
 * explicit_key_peek_is_merge_candidate). That's decided here and not in the
 * caller because this is the one point that has already worked out whether
 * the content starts on this line or a later one, and so the only position
 * where the look-ahead's documented precondition ("already at the start of
 * the key's real content") holds in both cases. Pass NULL for a value
 * position, which can never be a merge key.
 */
_CYAML_PARSE_HOT _pbmn_t _pbmn_begin(parse_ctx_t *ctx, int map_indent,
                                     bool allow_inline_seq,
                                     bool *is_merge_candidate_out,
                                     cyaml_node_t **node_out, _preq_t *rq) {
  if (is_merge_candidate_out) *is_merge_candidate_out = false;
  if (rest_of_line_is_blank(ctx)) {
    skip_ws_comments(ctx);
    bool is_value_col =
        !at_end(ctx) && at_block_value_col(ctx, map_indent, true);
    if (at_end(ctx) || !is_value_col || at_doc_marker(ctx)) {
      *node_out = node_alloc(CYAML_NULL, ctx->mp);
      return _PBMN_NODE;
    }
    /* ctx sits exactly at the start of the node's real content here, which can
     * begin on the line of this same call or, as here, on a fresh line. This
     * is the one position where explicit_key_peek_is_merge_candidate's
     * documented precondition ("already confirmed to be the start of a
     * dictionary key's content") holds in BOTH cases, so the look-ahead lives
     * here and not in our caller. A look-ahead in the caller would run too
     * early, before anything knows whether the key's content starts on this
     * line or a later one: it would only see whatever follows the '?' on the
     * '?' line itself, which is usually nothing but the newline this branch
     * exists to skip, so it would conclude "not a merge candidate" for every
     * multi-line explicit key, even a real "? \n  <<\n: ..." merge. Only the
     * key call site asks for this out parameter; the value call site passes
     * NULL, since a value is never a merge key. */
    if (is_merge_candidate_out)
      *is_merge_candidate_out =
          explicit_key_peek_is_merge_candidate(ctx, map_indent, false);
    /* A value that starts on its own fresh line is never an ambiguous
     * same-line chain, whatever the entry style, so allow_inline_map is always
     * true here, unlike in the same-line case below. */
    _preq_node(rq, map_indent, false, true, true, false, false, NULL);
    return _PBMN_DESCEND;
  }
  if (!allow_inline_seq && at_bare_seq_indicator(ctx)) {
    parse_err(ctx,
              "a block sequence cannot start on the same line as its "
              "mapping key at position %zu",
              ctx->pos);
    return _PBMN_ERROR;
  }
  if (is_merge_candidate_out)
    *is_merge_candidate_out =
        explicit_key_peek_is_merge_candidate(ctx, map_indent, false);
  _preq_node(rq, map_indent, false, true, allow_inline_seq, false, false, NULL);
  return _PBMN_DESCEND;
}

/*
 * Looks ahead at a dictionary entry's key text and answers whether that key
 * is a real merge-key trigger. It reads nothing, since ctx->pos is restored
 * before returning. An untagged plain scalar whose text is exactly "<<"
 * counts, and so does a plain or quoted "<<" whose tag is the core merge
 * tag. An anchor doesn't disqualify it, while a quote without that tag, any
 * other tag, or a non-scalar key does.
 *
 * The name says "explicit key", but the parser uses it at the start of the
 * content of EVERY dictionary key: the implicit forms ("key:", "{key:...}"
 * and "[key:...]") as well as the explicit forms ("? key" and
 * "{? key:...}"). A one-character check that only asks whether the very
 * first byte is a quote or a tag sigil can't rule out a tag, because
 * c-ns-properties allows an anchor and a tag in either order: "&y !!str <<"
 * has a tag although its first character is a '&'. Only a walk over the
 * whole anchor and tag property chain sees a tag that follows an anchor, and
 * that's the walk we make here. Two independent reference parsers confirm
 * the rule: the explicit '? <<' form is a real merge trigger under the same
 * conditions as the implicit '<<:' shorthand, in block and in flow context,
 * and the explicit form gets no exemption from merging whatever quotes it
 * carries.
 *
 * in_flow picks the whitespace skip: true picks flow_skip_ws, which crosses
 * lines, matching the real parse of the flow dictionary key that this
 * look-ahead precedes, and false picks skip_inline_ws, because a block
 * context keeps its own "a key stays on one physical line" convention. Only
 * flow_skip_ws reads indent.
 *
 * The boundary check after "<<" is coarser than scan_plain_scalar_line's
 * termination rules on purpose: it OVER-approximates them. scan_plain_
 * scalar_line doesn't stop at ordinary whitespace inside a scalar, only at
 * four things: the end of the line, a ': ' or a ':' at the end of the input,
 * an inline ' #' comment, and a flow terminator. So a key like "<< foo"
 * really parses as the single scalar "<< foo" and not "<<", while this
 * function treats any whitespace right after "<<" as enough for a candidate,
 * and so it can flag a non-merge key as a false positive.
 *
 * That's safe for one of two reasons at each call site. Most callers check
 * the real, fully parsed key text against the literal string "<<" before
 * they trust the flag (see the strcmp gate of try_parse_scalar_dict_key and
 * of parse_one_dict_entry_key). Two callers in parse_flow_list, the
 * "[? key: value]" form and the bare "[key: value]" compact-mapping
 * shorthand, hand the flag to flow_pair_store, which makes the same check of
 * the key text before it expands anything.
 *
 * The ':' handling, on the other hand, matches scan_plain_scalar_line's rule
 * exactly: a ':' followed by whitespace or the end of the input, and in a
 * flow context also a ':' followed by a flow terminator. A flow key of
 * "<<:", with the colon right against the "<<", is one different plain
 * scalar in a block context, but in a flow context it really resolves to
 * just "<<". This one case must be exact, unlike the whitespace
 * over-approximation above, because that distinction depends on it.
 *
 * It is only called at a position already confirmed to be the start of a
 * dictionary key's content: just past the '?' and its required separator
 * for the explicit form, and the key position itself for the implicit form.
 *
 * A malformed anchor or tag, and a flow_skip_ws failure (an unterminated flow
 * collection), give "not a merge candidate", because the real parse, which
 * runs right after this from our caller, finds and reports such an error
 * properly. ctx->error is cleared before returning on either failure path,
 * because parse_anchor_name() or flow_skip_ws() may already have called
 * parse_err(). Without that, this throwaway look-ahead's diagnostic would
 * stay behind and hide a later, unrelated error message from another part of
 * the same parse, one that is correctly guarded with "only set it if nothing
 * set it yet".
 *
 * That clear is also what makes using skip_tag_token() here, in place of
 * parse_tag_token(), safe. We only need the tag to be valid to recognise the
 * merge tag on a key that can still be "<<", and otherwise we just skip past
 * the tag so we can set had_tag; the real parse validates the tag when it
 * runs.
 */
static bool explicit_key_peek_is_merge_candidate(parse_ctx_t *ctx, int indent,
                                                 bool in_flow) {
  size_t saved_pos = ctx->pos;
  bool had_tag = false;
  size_t tag_pos = 0;
  bool had_anchor = false;
  for (int prop_pass = 0; prop_pass < 2; prop_pass++) {
    if (!at_end(ctx) && cur(ctx) == '&' && !had_anchor) {
      ctx->pos++;
      char *tmp_anchor = NULL;
      if (!parse_anchor_name(ctx, &tmp_anchor)) {
        /* This is only a throwaway look-ahead (see the doc comment of this
         * function), so a failure here is never the real, final error for this
         * position, and the real parse right after this call reports it.
         * parse_anchor_name() may have set ctx->error with parse_err(), so we
         * clear it. Otherwise the look-ahead's diagnostic, stale by then,
         * would stay behind and hide a later, unrelated error message
         * elsewhere in the same parse, one that is correctly guarded with
         * "only set it if nothing set it yet". */
        ctx->error[0] = '\0';
        ctx->pos = saved_pos;
        return false;
      }
      _ccol_mem_free(ctx->mp, tmp_anchor);
      had_anchor = true;
    } else if (!at_end(ctx) && cur(ctx) == '!' && !had_tag) {
      tag_pos = ctx->pos;
      skip_tag_token(ctx);
      had_tag = true;
    } else {
      break;
    }
    bool ws_ok =
        in_flow ? flow_skip_ws(ctx, indent) : (skip_inline_ws(ctx), true);
    if (!ws_ok) {
      /* Same reasoning as for the parse_anchor_name() failure above: the
       * look-ahead's error must not outlive the look-ahead. */
      ctx->error[0] = '\0';
      ctx->pos = saved_pos;
      return false;
    }
  }
  /* A tag disqualifies the key unless it is the core merge tag, behind any
   * %TAG handle or in verbatim form. The tag is only resolved for a key that
   * can still be "<<", so an ordinary tagged key pays nothing beyond the
   * skip. A tag that doesn't resolve gives "not a merge candidate", and the
   * real parse reports it. */
  bool merge_tag = false;
  if (had_tag && !at_end(ctx) &&
      (cur(ctx) == '<' || cur(ctx) == '"' || cur(ctx) == '\'')) {
    size_t key_pos = ctx->pos;
    char *tag = NULL;
    ctx->pos = tag_pos;
    if (parse_tag_token(ctx, &tag)) {
      merge_tag = tag && strcmp(tag, _CYAML_TAG_MERGE) == 0;
      _ccol_mem_free(ctx->mp, tag);
    } else {
      ctx->error[0] = '\0';
    }
    ctx->pos = key_pos;
  }
  bool is_candidate = false;
  size_t after = SIZE_MAX;
  if ((!had_tag || merge_tag) && !at_end(ctx) && ctx->pos + 1 < ctx->len &&
      ctx->src[ctx->pos] == '<' && ctx->src[ctx->pos + 1] == '<') {
    after = ctx->pos + 2;
  } else if (merge_tag && !at_end(ctx) &&
             (cur(ctx) == '"' || cur(ctx) == '\'')) {
    /* A quoted "<<" with the merge tag is a merge key too. The scalar is
     * decoded so that every spelling of "<<" counts. */
    char *text = NULL;
    bool ok = cur(ctx) == '"' ? parse_double_quoted(ctx, &text)
                              : parse_single_quoted(ctx, &text);
    /* The closing quote ends the scalar, so no boundary check follows. */
    is_candidate = ok && strcmp(text, "<<") == 0;
    if (ok) _ccol_mem_free(ctx->mp, text);
    ctx->error[0] = '\0';
  }
  if (after != SIZE_MAX) {
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
 * Finishes the key of an explicit ("? key") entry once its node is in hand:
 * builds the canonical key string the dictionary stores, and then looks for
 * the ':' value indicator that may follow on a later line. It's split from
 * _pode_begin() below because the key's node is a descent, so the two halves
 * run on either side of it. Returns 1 on success and -1 on an error.
 */
static int _pode_finish(parse_ctx_t *ctx, int map_indent,
                        cyaml_node_t *key_node, bool is_merge_candidate,
                        char **key_out, bool *have_colon_out,
                        bool *is_explicit_out, bool *is_merge_candidate_out) {
  if (!key_node) return -1;
  char *key_str = node_to_dict_key_string(ctx, key_node, "block dictionary");
  if (!key_str) return -1;

  /* The ':' value indicator, when there is one, is always on its own line at
   * exactly map_indent, never on the same line after the key, as YAML 1.2
   * section 8.2.2 says: l-block-map-explicit-value starts with
   * s-indent(n) ":". We compare current_col() against an indentation level
   * here, so we need the same line_indent_has_tab() rejection that every
   * other such comparison in this file carries (parse_block_dictionary's
   * next-sibling-entry lookup a bit further down is one example).
   * current_col() counts a tab as one byte of width like a space, so without
   * this check a ':' indented with a tab would line up with map_indent by
   * byte offset alone and be silently accepted. */
  skip_ws_comments(ctx);
  bool have_colon = false;
  if (!at_end(ctx) && !at_doc_marker(ctx)) {
    if (line_indent_has_tab(ctx)) {
      parse_err(ctx,
                "tab cannot be used as block mapping indentation at "
                "position %zu",
                ctx->pos);
      _ccol_mem_free(ctx->mp, key_str);
      return -1;
    }
    /* A ':' only acts as the value indicator when a space, a tab, an end of
     * line or the end of the input follows it, the same rule every other such
     * decision in this file uses (scan_plain_scalar_line's rule, for
     * example). ns-plain-first(c) allows a ':' as the first character of a
     * plain scalar when something other than a space follows, so ":adapter:
     * pg" at this mapping's indent is an ordinary entry with the key
     * ":adapter", not this explicit key's value indicator. Accepting a ':'
     * without a separator here would swallow that entry whole and invent a
     * submap for a key the document gives as null. */
    if (current_col(ctx) == map_indent && cur(ctx) == ':') {
      if (ctx->pos + 1 >= ctx->len) {
        have_colon = true; /* ':' at the very end of the input */
      } else {
        char after = ctx->src[ctx->pos + 1];
        have_colon =
            (after == ' ' || after == '\t' || after == '\n' || after == '\r');
      }
    }
  }
  if (have_colon) ctx->pos++;

  *key_out = key_str;
  *have_colon_out = have_colon;
  *is_explicit_out = true;
  *is_merge_candidate_out = is_merge_candidate;
  return 1;
}

/*
 * Reads the key of one block dictionary entry. Returns 1 when it made a key,
 * 0 when this position isn't a key at all, meaning the mapping ends here, and
 * -1 on a real error. Returns 2 when the key is an explicit one whose own
 * node has to be read first; the request is filled in then, and the caller
 * resumes through _pode_finish() with the resulting node.
 *
 * On the path that returns 2, *is_merge_candidate_out is already final,
 * because _pbmn_begin() decides it before the descent and _pode_finish() then
 * writes the same value again. So a -1 from _pode_finish() leaves that out
 * parameter holding the look-ahead's value rather than false, but no caller
 * reads it there, since every caller abandons the mapping on a -1.
 */
static int _pode_begin(parse_ctx_t *ctx, int map_indent, char **key_out,
                       bool *have_colon_out, bool *is_explicit_out,
                       bool *is_merge_candidate_out, _preq_t *rq) {
  char c = cur(ctx);
  *is_merge_candidate_out = false;

  /* A '?' is only an explicit-key indicator when whitespace, the end of the
   * line or the end of the input follows it, the same rule as the node
   * dispatch's '?' check. Otherwise the '?' validly starts a plain scalar
   * key, as in "?foo: value". */
  bool is_explicit_key_indicator = false;
  if (c == '?') {
    if (ctx->pos + 1 < ctx->len) {
      char next = ctx->src[ctx->pos + 1];
      /* See the '?' branch of the node dispatch: a tab is separation
       * whitespace, just like a space. */
      if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
        is_explicit_key_indicator = true;
    } else {
      is_explicit_key_indicator = true;
    }
  }

  if (is_explicit_key_indicator) {
    ctx->pos++;

    /* Skip at most one optional separator space. Further spaces and tabs are
     * separation whitespace, and a block collection after a tab fails, just
     * as after a '-'. */
    if (!at_end(ctx) && cur(ctx) == ' ') ctx->pos++;

    /* _pbmn_begin decides this itself, once it knows whether the key's
     * content starts on this line or a later one: YAML 1.2 section 8.2.2 lets
     * an explicit key move on to a fresh line, so a merge-candidate
     * look-ahead taken any earlier would only see the newline right after
     * the '?' and never the real key. */
    cyaml_node_t *key_node = NULL;
    switch (_pbmn_begin(ctx, map_indent, true, is_merge_candidate_out,
                        &key_node, rq)) {
      case _PBMN_ERROR:
        return -1;
      case _PBMN_DESCEND:
        return 2;
      case _PBMN_NODE:
        break;
    }
    return _pode_finish(ctx, map_indent, key_node, *is_merge_candidate_out,
                        key_out, have_colon_out, is_explicit_out,
                        is_merge_candidate_out);
  }

  /* Indicator characters that can't start any kind of implicit key. A '-' is
   * only a list indicator when whitespace or the end of the input follows
   * it; otherwise it can validly start a plain scalar key, as in "-key:". We
   * don't exclude '*', because try_parse_scalar_dict_key handles an alias
   * used directly as a key, nor '[' or '{', because a flow collection is a
   * valid non-scalar implicit key: YAML 1.2 section 8.2.2 allows ns-flow-node
   * in a block-key context, not only a scalar, and the '[' and '{' branch of
   * try_parse_scalar_dict_key parses exactly that. Excluding them here would
   * only break the entries after the mapping's first one, because the node
   * dispatch finds the first entry directly without any such filter, so
   * whether the construct is accepted would depend on nothing but the
   * entry's position. */
  if (c == '-') {
    char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
    if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
      return 0;
  } else if (c == '|' || c == '>' || c == '#') {
    return 0;
  }

  /* An implicit key can carry an anchor, a tag, or both, as in
   * "&anchor key: value". try_parse_scalar_dict_key handles both and restores
   * ctx->pos itself when this turns out not to be a key. */
  {
    char *anchor_name = NULL;
    char *key_str = NULL;
    cyaml_node_t *anchor_value = NULL;
    if (!try_parse_scalar_dict_key(ctx, &anchor_name, &key_str,
                                   is_merge_candidate_out, &anchor_value))
      return ctx->error[0] ? -1 : 0;

    if (!register_key_anchor(ctx, anchor_name, key_str, anchor_value)) {
      _ccol_mem_free(ctx->mp, key_str);
      return -1;
    }

    *key_out = key_str;
    *have_colon_out = true;
    *is_explicit_out = false;
  }
  return 1;
}

/*
 * Inserts a copy of val into target under key, which target doesn't hold
 * yet. The copy and its new dictionary entry are both amplification, since
 * the same source may be merged into any number of targets, so both count
 * against the alias expansion budget. Returns false, with ctx->error set, on
 * a failure.
 */
static bool merge_insert_copy(parse_ctx_t *ctx, cyaml_node_t *target,
                              const char *key, cyaml_node_t *val) {
  parse_amp_enter();
  cyaml clone = cyaml_clone((cyaml)val);
  bool ok =
      clone && dictionary_set_impl((cyaml)target, key, clone) == ccol_success;
  parse_amp_leave();
  if (ok) return true;
  if (_parse_amp_limit_hit)
    parse_err(ctx,
              "merge key expansion exceeded the %zu-node, %zu-byte alias "
              "expansion memory limit at position %zu",
              (size_t)_CYAML_PARSE_NODE_FLOOR, (size_t)_CYAML_PARSE_BYTE_FLOOR,
              ctx->pos);
  else if (_parse_node_budget_exhausted)
    parse_err(ctx,
              "merge key expansion exceeded the %zu node-allocation "
              "limit at position %zu",
              _parse_node_limit, ctx->pos);
  else if (_parse_byte_budget_exhausted)
    parse_err(ctx,
              "merge key expansion exceeded the %zu-byte document memory "
              "limit at position %zu",
              _parse_byte_limit, ctx->pos);
  else
    parse_err(ctx, "out of memory expanding merge key at position %zu",
              ctx->pos);
  return false;
}

/*
 * Merges the entries of one source mapping into target, skipping any key
 * target already holds: an explicit member of target, or a member that an
 * earlier source of the same merge entry brought in. The caller has already
 * checked that source is a mapping.
 *
 * Each value is copied before it is inserted, because cyaml_dictionary_set
 * always takes ownership of its child, while a value from a merge source
 * belongs to that source's tree.
 */
static bool merge_one_source_into(parse_ctx_t *ctx, cyaml_node_t *target,
                                  cyaml_node_t *source) {
  /* Walk the source in its own insertion order, so one source's merged
   * members arrive in the order the source declares them. */
  for (const ccol_chmap_entry_ref *e = dict_first_entry(source); e;) {
    const cmap_pair *kp, *vp;
    e = ccol_chmap_entry_read(e, &kp, &vp);
    const char *key = (const char *)kp->ptr;
    if (cyaml_dictionary_get((cyaml)target, key)) continue;
    if (!merge_insert_copy(ctx, target, key, _cyaml_read_child(vp->ptr)))
      return false;
  }
  return true;
}

/*
 * Moves count members of map, starting at the entry first and following the
 * insertion order, to the end of that order, keeping their relative order.
 * Each member is taken out of the map and inserted again with the same key
 * and child, and an insert places it after every other member. The child is
 * never freed or copied. A member the insert refuses for lack of memory is
 * destroyed and the failure is reported; the parse then fails as a whole.
 *
 * The key bytes live in the entry the delete frees, so the insert reads a
 * copy of them, a stack buffer for an ordinary key and one allocation for a
 * longer one. Only a mapping with a "<<" entry followed by explicit members
 * pays for this.
 */
static bool merge_move_to_end(parse_ctx_t *ctx, cyaml_node_t *map,
                              const ccol_chmap_entry_ref *first, size_t count) {
  const ccol_chmap_entry_ref *e = first;
  for (size_t i = 0; i < count && e; i++) {
    const cmap_pair *kp, *vp;
    const ccol_chmap_entry_ref *next = ccol_chmap_entry_read(e, &kp, &vp);
    cyaml_node_t *child = _cyaml_read_child(vp->ptr);
    char stack_key[256];
    char *key = stack_key;
    size_t key_size = kp->size;
    if (key_size > sizeof(stack_key)) {
      key = _ccol_mem_alloc(ctx->mp, key_size);
      if (!key) {
        parse_err(ctx, "out of memory expanding merge key at position %zu",
                  ctx->pos);
        return false;
      }
    }
    memcpy(key, kp->ptr, key_size);
    cmap_pair key_pair = {.ptr = key, .size = key_size};
    cmap_pair val_pair = {.ptr = &child, .size = sizeof(child)};
    ccol_retval_t r = chmap_delete_elem(map->value.dictionary, &key_pair);
    if (r == ccol_success)
      r = chmap_insert_elem(map->value.dictionary, &key_pair, &val_pair);
    if (key != stack_key) _ccol_mem_free(ctx->mp, key);
    if (r != ccol_success) {
      __cyaml_destroy((cyaml)child);
      parse_err(ctx, "out of memory expanding merge key at position %zu",
                ctx->pos);
      return false;
    }
    e = next;
  }
  return true;
}

/* How many sources one merge entry has: the length of its sequence, or 1 for
 * a single mapping. */
static size_t merge_source_count(const cyaml_node_t *val) {
  return val->type == CYAML_LIST ? cvector_elem_count(val->value.list) : 1;
}

/* The source at index i of one merge entry. */
static cyaml_node_t *merge_source_at(cyaml_node_t *val, size_t i) {
  if (val->type != CYAML_LIST) return val;
  return *(cyaml_node_t **)cvector_at(val->value.list, i);
}

/*
 * One segment of the member order that merge_reorder() moves: the first
 * entry of the explicit members after a merge entry, and the first entry and
 * count of the members the merge entry itself placed.
 */
typedef struct {
  const ccol_chmap_entry_ref *explicit_first;
  const ccol_chmap_entry_ref *merged_first;
  size_t merged_count;
} _cyaml_merge_seg_t;

/*
 * Puts the members of map in their final order. On entry the map holds its
 * explicit_count explicit members first, then the members each merge entry
 * placed, entry by entry (seg[i].merged_count of them for entry i). On
 * return the members entry i placed come after the explicit members before
 * that entry (entries[i].pos of them) and before the explicit members after
 * it.
 *
 * Only out-of-place members move. Every segment up to the first merge entry
 * with explicit members after it is already in place; from there on, those
 * explicit members, and then each later entry's merged members and explicit
 * members, move to the end in turn.
 */
static bool merge_reorder(parse_ctx_t *ctx, cyaml_node_t *map,
                          const _cyaml_merge_entry_t *entries,
                          _cyaml_merge_seg_t *seg, size_t n,
                          size_t explicit_count) {
  size_t k = 0;
  while (k < n) {
    size_t end = k + 1 < n ? entries[k + 1].pos : explicit_count;
    if (end > entries[k].pos) break;
    k++;
  }
  if (k == n) return true;

  /* Find the first entry of every segment that moves, in one walk. A
   * segment's entries keep their addresses until that segment moves, since
   * deleting or inserting some other member never moves an entry. */
  {
    size_t idx = 0, ei = k, mi = k + 1, merged_at = explicit_count;
    for (size_t i = 0; i <= k && i < n; i++) merged_at += seg[i].merged_count;
    for (const ccol_chmap_entry_ref *e = dict_first_entry(map); e; idx++) {
      const cmap_pair *ekp, *evp;
      const ccol_chmap_entry_ref *cur_e = e;
      e = ccol_chmap_entry_read(e, &ekp, &evp);
      while (ei < n && entries[ei].pos == idx) seg[ei++].explicit_first = cur_e;
      while (mi < n && merged_at == idx) {
        seg[mi].merged_first = cur_e;
        merged_at += seg[mi++].merged_count;
      }
    }
  }

  for (size_t i = k; i < n; i++) {
    size_t end = i + 1 < n ? entries[i + 1].pos : explicit_count;
    if (i > k && seg[i].merged_count &&
        !merge_move_to_end(ctx, map, seg[i].merged_first, seg[i].merged_count))
      return false;
    if (end > entries[i].pos &&
        !merge_move_to_end(ctx, map, seg[i].explicit_first,
                           end - entries[i].pos))
      return false;
  }
  return true;
}

/*
 * Expands the merge entries of a mapping once the whole mapping has been
 * read. map holds exactly the explicit members, since the frame keeps every
 * merge entry aside (see _cyaml_merge_entry_t). entries lists the merge
 * entries in document order, and the caller keeps ownership of their values.
 *
 * Precedence follows PyYAML: an explicit member always wins, of two merge
 * entries the later one wins, and within one merge entry whose value is a
 * sequence an earlier source wins over a later one. A merged member takes
 * its place from the first merge entry that brings its key, so the members
 * each entry places come after the explicit members before that entry and
 * before the explicit members after it, source by source, each source in its
 * own member order.
 *
 * A mapping that a merge brings in already had its own merge entries fully
 * expanded when the parser finished THAT mapping, because a nested mapping is
 * always complete before anything registers an anchor naming it (see the
 * call sites of anchors_store). So expansion through several levels needs no
 * special case here.
 *
 * Returns false, with ctx->error set, on a merge source that isn't a mapping
 * and on an OOM.
 */
static bool merge_apply(parse_ctx_t *ctx, cyaml_node_t *map,
                        const _cyaml_merge_entry_t *entries, size_t n) {
  for (size_t i = 0; i < n; i++) {
    cyaml_node_t *val = entries[i].val;
    size_t cnt = merge_source_count(val);
    for (size_t j = 0; j < cnt; j++) {
      cyaml_node_t *src = merge_source_at(val, j);
      if (!src || src->type != CYAML_DICTIONARY) {
        parse_err(ctx, "merge key source is not a mapping at position %zu",
                  ctx->pos);
        return false;
      }
    }
  }
  size_t explicit_count = chmap_elem_count(map->value.dictionary);

  if (n == 1) {
    /* One merge entry: an earlier source wins, which is exactly the order in
     * which members get placed, so a member is only placed when the map
     * doesn't hold its key yet. */
    size_t cnt = merge_source_count(entries[0].val);
    for (size_t j = 0; j < cnt; j++)
      if (!merge_one_source_into(ctx, map, merge_source_at(entries[0].val, j)))
        return false;
    _cyaml_merge_seg_t seg = {.merged_count =
                                  chmap_elem_count(map->value.dictionary) -
                                  explicit_count};
    return merge_reorder(ctx, map, entries, &seg, 1, explicit_count);
  }

  /* Several merge entries: the winning value and the place a member takes
   * come from different entries, so a first pass picks the value of every
   * merged key in order of precedence, and a second pass places the members
   * in document order. The map winners holds a borrowed pointer to the
   * winning value of each key, and NULL once the member is placed or when the
   * key is an explicit member. */
  char *err = NULL;
  chmap winners =
      ccol_chmap_create_compact(ccol_string, ccol_pointer, ctx->mp, &err);
  _cyaml_merge_seg_t *seg = _ccol_mem_alloc(ctx->mp, n * sizeof(*seg));
  bool ok = winners && seg;
  if (!ok)
    parse_err(ctx, "out of memory expanding merge key at position %zu",
              ctx->pos);
  for (size_t i = n; ok && i-- > 0;) {
    cyaml_node_t *val = entries[i].val;
    size_t cnt = merge_source_count(val);
    for (size_t j = 0; ok && j < cnt; j++) {
      cyaml_node_t *src = merge_source_at(val, j);
      for (const ccol_chmap_entry_ref *e = dict_first_entry(src); e;) {
        const cmap_pair *kp, *vp;
        cmap_pair slot;
        e = ccol_chmap_entry_read(e, &kp, &vp);
        if (chmap_get_elem_ref(map->value.dictionary, kp, &slot) ==
            ccol_success)
          continue;
        cyaml_node_t *child = _cyaml_read_child(vp->ptr);
        cmap_pair val_pair = {.ptr = &child, .size = sizeof(child)};
        ccol_retval_t r =
            ccol_chmap_insert_or_get_elem(winners, kp, &val_pair, &slot);
        if (r != ccol_success && r != ccol_key_already_present) {
          parse_err(ctx, "out of memory expanding merge key at position %zu",
                    ctx->pos);
          ok = false;
          break;
        }
      }
    }
  }
  for (size_t i = 0; ok && i < n; i++) {
    cyaml_node_t *val = entries[i].val;
    size_t cnt = merge_source_count(val);
    size_t before = chmap_elem_count(map->value.dictionary);
    for (size_t j = 0; ok && j < cnt; j++) {
      cyaml_node_t *src = merge_source_at(val, j);
      for (const ccol_chmap_entry_ref *e = dict_first_entry(src); e;) {
        const cmap_pair *kp, *vp;
        cmap_pair slot;
        e = ccol_chmap_entry_read(e, &kp, &vp);
        cyaml_node_t *none = NULL;
        cmap_pair none_pair = {.ptr = &none, .size = sizeof(none)};
        ccol_retval_t r =
            ccol_chmap_insert_or_get_elem(winners, kp, &none_pair, &slot);
        if (r == ccol_success) continue; /* an explicit member */
        if (r != ccol_key_already_present) {
          parse_err(ctx, "out of memory expanding merge key at position %zu",
                    ctx->pos);
          ok = false;
          break;
        }
        cyaml_node_t *win = _cyaml_read_child(slot.ptr);
        if (!win) continue; /* placed by an earlier entry or source */
        memcpy(slot.ptr, &none, sizeof(none));
        if (!merge_insert_copy(ctx, map, (const char *)kp->ptr, win)) {
          ok = false;
          break;
        }
      }
    }
    if (ok)
      seg[i].merged_count = chmap_elem_count(map->value.dictionary) - before;
  }
  if (ok) ok = merge_reorder(ctx, map, entries, seg, n, explicit_count);
  if (winners) __chmap_destroy(winners);
  _ccol_mem_free(ctx->mp, seg);
  return ok;
}

/*
 * Keeps one merge entry of a mapping aside until the mapping ends. The frame
 * takes ownership of val in every case; a failure destroys it and sets
 * ctx->error.
 */
static bool merge_entry_push(parse_ctx_t *ctx, _cyaml_merge_entry_t **arr,
                             size_t *count, size_t *cap, cyaml_node_t *val,
                             size_t pos) {
  if (*count == *cap) {
    size_t new_cap = *cap ? *cap * 2 : 2;
    _cyaml_merge_entry_t *grown =
        _ccol_mem_realloc(ctx->mp, *arr, new_cap * sizeof(*grown));
    if (!grown) {
      __cyaml_destroy((cyaml)val);
      parse_err(ctx, "out of memory parsing merge key at position %zu",
                ctx->pos);
      return false;
    }
    *arr = grown;
    *cap = new_cap;
  }
  (*arr)[*count].val = val;
  (*arr)[*count].pos = pos;
  (*count)++;
  return true;
}

/* Destroys every merge entry a frame keeps aside, and the array. A value may
 * hold anchored nodes, as in "<<: &b {x: 1}", so it goes through
 * parse_discard(). */
static void merge_entries_free(parse_ctx_t *ctx, _cyaml_merge_entry_t **arr,
                               size_t *count, size_t *cap) {
  for (size_t i = 0; i < *count; i++) parse_discard((*arr)[i].val);
  _ccol_mem_free(ctx->mp, *arr);
  *arr = NULL;
  *count = 0;
  *cap = 0;
}

/* Expands the merge entries a frame kept aside into map and then destroys
 * them. The entries are gone on return, whatever the result. */
static bool merge_entries_finish(parse_ctx_t *ctx, cyaml_node_t *map,
                                 _cyaml_merge_entry_t **arr, size_t *count,
                                 size_t *cap) {
  bool ok = merge_apply(ctx, map, *arr, *count);
  merge_entries_free(ctx, arr, count, cap);
  return ok;
}

/*
 * Parses a block dictionary. The doc comment of the forward declaration
 * covers the two modes of first_key (a key the caller already read, or NULL
 * for a first entry led by a '?'), what first_key_colon_consumed means, and
 * what first_key_is_merge_candidate means; the frame carries all three.
 *
 * Each entry's key, when it's explicit, and each entry's value are descents,
 * and everything else is straight-line code.
 */
static _pstep_t _pbd_step(parse_ctx_t *ctx, _pframe_t *f, _preq_t *rq) {
  const int map_indent = f->u.bd.map_indent;
  cyaml_node_t *val = NULL;
  int r = 0;

  switch (f->state) {
    case _BDS_AFTER_FIRST_KEY_NODE:
      goto after_first_key_node;
    case _BDS_AFTER_KEY_NODE:
      goto after_key_node;
    case _BDS_AFTER_VALUE:
      goto after_value;
    default:
      break;
  }

  /* A block mapping's first key sits on the current line at column
   * map_indent. A tab in front of it on that line leaves the mapping's column
   * undefined, since a tab has no width, so "\tb: c", "- \tb: c" and
   * "key:\tb: c" are all refused, while the same key after spaces only is a
   * mapping at a well-defined column. The loop's line_indent_has_tab() test
   * below refuses every later entry the same way. */
  {
    size_t ls = line_start_pos(ctx);
    size_t span = ctx->pos - ls;
    if (map_indent >= 0 && (size_t)map_indent < span) span = (size_t)map_indent;
    if (memchr(ctx->src + ls, '\t', span) != NULL) {
      parse_err(ctx,
                "tab cannot be used as block mapping indentation at "
                "position %zu",
                ls + span);
      return _PSTEP_FAIL;
    }
  }

  f->u.bd.map = (cyaml_node_t *)cyaml_create_dictionary_interned(ctx->mp);
  if (!f->u.bd.map) return _PSTEP_FAIL;

  /* f->u.bd.entry_is_merge_candidate says whether the current entry's key is
   * a merge key. Every merge entry goes aside into f->u.bd.merges instead of
   * into the map, and the merges expand once the mapping ends, so two merge
   * entries, or a merge entry and an ordinary "<<" key, never replace each
   * other. */
  if (f->u.bd.key != NULL) {
    /* A first key the caller has already read is always implicit-style,
     * because the node dispatch's scalar, flow-collection, anchor, tag and
     * alias branches never get here for an entry led by a '?'. See the doc
     * comment of the forward declaration. */
    if (f->u.bd.have_colon) {
      /* the caller already consumed the ':' */
    } else if (at_end(ctx) || cur(ctx) != ':') {
      parse_err(ctx, "expected ':' after dictionary key at position %zu",
                ctx->pos);
      return _PSTEP_FAIL;
    } else {
      ctx->pos++;
      f->u.bd.have_colon = true;
    }
  } else {
    r = _pode_begin(ctx, map_indent, &f->u.bd.key_owned, &f->u.bd.have_colon,
                    &f->u.bd.is_explicit, &f->u.bd.entry_is_merge_candidate,
                    rq);
    if (r == 2) {
      f->state = _BDS_AFTER_FIRST_KEY_NODE;
      return _PSTEP_DESCEND;
    }
    goto first_key_done;
  after_first_key_node:
    r = _pode_finish(ctx, map_indent, f->child,
                     f->u.bd.entry_is_merge_candidate, &f->u.bd.key_owned,
                     &f->u.bd.have_colon, &f->u.bd.is_explicit,
                     &f->u.bd.entry_is_merge_candidate);
    f->child = NULL;
  first_key_done:
    if (r <= 0) {
      /* No real input gets here: this mode is only entered after a '?' has
       * been confirmed at the current position, and _pode_begin can never
       * turn that down. The branch is a standing defence. */
      if (r == 0)
        parse_err(ctx, "expected block dictionary key at position %zu",
                  ctx->pos);
      return _PSTEP_FAIL;
    }
    /* This mode is only reached for the '?' explicit-key form (see the doc
     * comment of the forward declaration), whose merge candidacy
     * _pode_begin() or _pode_finish() wrote into entry_is_merge_candidate
     * once the key was really read. */
    f->u.bd.key = f->u.bd.key_owned;
  }

  while (1) {
    /* Skip the optional space after ':'. Further spaces and tabs are
     * separation whitespace ("key:\tvalue"), and a block collection after a
     * tab fails, just as after a '-'. */
    if (f->u.bd.have_colon && !at_end(ctx) && cur(ctx) == ' ') ctx->pos++;

    /* Parse the value. An explicit key with no ':' at all has have_colon
     * false and a null value, by YAML 1.2 section 8.2.2's "| e-node"
     * alternative, so nothing more is read for it. allow_inline_seq is only
     * true for an explicit entry's value; the doc comment of _pbmn_begin
     * explains why implicit and explicit values really differ here. */
    if (f->u.bd.have_colon) {
      switch (
          _pbmn_begin(ctx, map_indent, f->u.bd.is_explicit, NULL, &val, rq)) {
        case _PBMN_ERROR:
          val = NULL;
          goto have_value;
        case _PBMN_NODE:
          goto have_value;
        case _PBMN_DESCEND:
          f->state = _BDS_AFTER_VALUE;
          return _PSTEP_DESCEND;
      }
    } else {
      val = node_alloc(CYAML_NULL, ctx->mp);
      goto have_value;
    }
  after_value:
    val = f->child;
    f->child = NULL;
  have_value:
    if (!val) {
      /* val is NULL either because the descent already set ctx->error or
       * because node_alloc returned NULL on an OOM, so we set a diagnostic
       * when there's no other message and the caller always gets a useful
       * string. */
      if (!ctx->error[0])
        parse_err(ctx, "out of memory parsing dictionary value at position %zu",
                  ctx->pos);
      return _PSTEP_FAIL;
    }

    if (f->u.bd.entry_is_merge_candidate && strcmp(f->u.bd.key, "<<") == 0) {
      if (!merge_entry_push(ctx, &f->u.bd.merges, &f->u.bd.merge_count,
                            &f->u.bd.merge_cap, val,
                            chmap_elem_count(f->u.bd.map->value.dictionary)))
        return _PSTEP_FAIL;
    } else if (dictionary_set_impl((cyaml)f->u.bd.map, f->u.bd.key,
                                   (cyaml)val) != ccol_success) {
      return _PSTEP_FAIL;
    }
    _ccol_mem_free(ctx->mp, f->u.bd.key_owned);
    f->u.bd.key_owned = NULL;
    f->u.bd.key = NULL;

    /* Look for the next entry at the same indent. */
    skip_ws_comments(ctx);
    if (at_end(ctx)) break;
    if (at_doc_marker(ctx)) break;
    if (at_col0_bom(ctx)) break;
    if (line_indent_has_tab(ctx)) {
      parse_err(ctx,
                "tab cannot be used as block mapping indentation at "
                "position %zu",
                ctx->pos);
      return _PSTEP_FAIL;
    }
    if (current_col(ctx) != map_indent) break;

    r = _pode_begin(ctx, map_indent, &f->u.bd.key_owned, &f->u.bd.have_colon,
                    &f->u.bd.is_explicit, &f->u.bd.entry_is_merge_candidate,
                    rq);
    if (r == 2) {
      f->state = _BDS_AFTER_KEY_NODE;
      return _PSTEP_DESCEND;
    }
    goto next_key_done;
  after_key_node:
    r = _pode_finish(ctx, map_indent, f->child,
                     f->u.bd.entry_is_merge_candidate, &f->u.bd.key_owned,
                     &f->u.bd.have_colon, &f->u.bd.is_explicit,
                     &f->u.bd.entry_is_merge_candidate);
    f->child = NULL;
  next_key_done:
    if (r < 0) return _PSTEP_FAIL;
    if (r == 0) break;
    f->u.bd.key = f->u.bd.key_owned;
  }

  if (f->u.bd.merge_count &&
      !merge_entries_finish(ctx, f->u.bd.map, &f->u.bd.merges,
                            &f->u.bd.merge_count, &f->u.bd.merge_cap))
    return _PSTEP_FAIL;

  f->result = f->u.bd.map;
  f->u.bd.map = NULL;
  return _PSTEP_DONE;
}

/* ========================================================================== */
/*                         PARSER DRIVER                                      */
/* ========================================================================== */

/*
 * Initializes one frame from the request that asked for it. Returns false
 * when the frame can't open at all, which for a node frame means the parse
 * has hit the nesting cap. Nothing is pushed then, and ctx->pos is exactly
 * where the requesting frame left it, so the error names the position where
 * something asked for the descent.
 *
 * A collection frame holds one ctx->depth level for as long as it is on the
 * stack, which covers the whole parse of the container it builds, and
 * _pframe_release() and _pframe_cleanup() give that level back on every exit
 * path. A node frame holds no level, because a scalar, a tag and an anchor
 * aren't containers.
 */
static bool _pframe_open(parse_ctx_t *ctx, _pframe_t *f, const _preq_t *rq) {
  f->kind = rq->kind;
  f->state = 0;
  f->child = NULL;
  f->result = NULL;
  /* Over three quarters of the frames a parse opens are node frames, so we
   * answer that kind before the depth guard and the jump table the other four
   * kinds share. */
  if (rq->kind == _PFK_NODE) {
    f->u.n.indent = rq->indent;
    f->u.n.in_flow = rq->in_flow;
    f->u.n.seq_ok_at_indent = rq->seq_ok_at_indent;
    f->u.n.allow_inline_map = rq->allow_inline_map;
    f->u.n.had_anchor = rq->had_anchor;
    f->u.n.had_tag = rq->had_tag;
    f->u.n.resolved_tag = rq->resolved_tag;
    f->u.n.name = NULL;
    f->u.n.tag = NULL;
    f->u.n.pending_key = NULL;
    return true;
  }
  /* We only clear the fields a kind reads before it writes them: every
   * pointer the frame can come to own, so the cleanup sweep always sees a
   * definite value, and every flag the frame carries across one turn of a
   * loop. Every other field is written before it is read. */
  if (ctx->depth >= CYAML_MAX_NESTING_DEPTH) {
    parse_depth_err(ctx);
    return false;
  }
  if (++ctx->depth > ctx->peak_depth) ctx->peak_depth = ctx->depth;
  switch (rq->kind) {
    case _PFK_NODE:
      break; /* already handled above */
    case _PFK_FLOW_LIST:
      f->u.fl.indent = rq->indent;
      f->u.fl.seq = NULL;
      f->u.fl.key_str = NULL;
      break;
    case _PFK_FLOW_DICT:
      f->u.fd.indent = rq->indent;
      f->u.fd.map = NULL;
      f->u.fd.key_str = NULL;
      f->u.fd.merges = NULL;
      f->u.fd.merge_count = 0;
      f->u.fd.merge_cap = 0;
      break;
    case _PFK_BLOCK_LIST:
      f->u.bl.seq_indent = rq->indent;
      f->u.bl.seq = NULL;
      break;
    case _PFK_BLOCK_DICT:
      f->u.bd.map_indent = rq->indent;
      f->u.bd.map = NULL;
      f->u.bd.key = rq->first_key;
      f->u.bd.key_owned = NULL;
      f->u.bd.have_colon = rq->first_key_colon_consumed;
      f->u.bd.is_explicit = false;
      f->u.bd.merges = NULL;
      f->u.bd.merge_count = 0;
      f->u.bd.merge_cap = 0;
      f->u.bd.entry_is_merge_candidate = rq->first_key_is_merge_candidate;
      break;
  }
  return true;
}

/*
 * Frees everything a frame still owns. When a parse fails the driver runs
 * this for every frame left on the stack, innermost first, because each
 * frame owns its container outright: a collection under construction isn't
 * reachable from any root until the frame below takes it, so without this
 * sweep an abandoned stack would leak one container per open level.
 *
 * A finished frame goes through _pframe_release() instead, which asserts that
 * the frame holds nothing and only gives back a collection frame's depth
 * level. This sweep is for the failure path only.
 */
static void _pframe_cleanup(parse_ctx_t *ctx, _pframe_t *f) {
  if (f->child) {
    __cyaml_destroy((cyaml)f->child);
    f->child = NULL;
  }
  switch (f->kind) {
    case _PFK_NODE:
      _ccol_mem_free(ctx->mp, f->u.n.name);
      _ccol_mem_free(ctx->mp, f->u.n.tag);
      _ccol_mem_free(ctx->mp, f->u.n.pending_key);
      f->u.n.name = NULL;
      f->u.n.tag = NULL;
      f->u.n.pending_key = NULL;
      return;
    case _PFK_FLOW_LIST:
      __cyaml_destroy((cyaml)f->u.fl.seq);
      f->u.fl.seq = NULL;
      /* Key text still held by the frame means it was reading the value of a
       * "key: value" pair, which holds a level of its own (see
       * flow_pair_level_take). */
      if (f->u.fl.key_str) {
        _ccol_mem_free(ctx->mp, f->u.fl.key_str);
        f->u.fl.key_str = NULL;
        ctx->depth--;
      }
      break;
    case _PFK_FLOW_DICT:
      __cyaml_destroy((cyaml)f->u.fd.map);
      f->u.fd.map = NULL;
      _ccol_mem_free(ctx->mp, f->u.fd.key_str);
      f->u.fd.key_str = NULL;
      merge_entries_free(ctx, &f->u.fd.merges, &f->u.fd.merge_count,
                         &f->u.fd.merge_cap);
      break;
    case _PFK_BLOCK_LIST:
      __cyaml_destroy((cyaml)f->u.bl.seq);
      f->u.bl.seq = NULL;
      break;
    case _PFK_BLOCK_DICT:
      __cyaml_destroy((cyaml)f->u.bd.map);
      f->u.bd.map = NULL;
      _ccol_mem_free(ctx->mp, f->u.bd.key_owned);
      f->u.bd.key_owned = NULL;
      f->u.bd.key = NULL;
      merge_entries_free(ctx, &f->u.bd.merges, &f->u.bd.merge_count,
                         &f->u.bd.merge_cap);
      break;
  }
  ctx->depth--;
}

/*
 * Releases a finished frame. A frame that reports itself done has already
 * handed its node to the driver and let go of everything else it owned, so
 * all that's left is the nesting level a collection frame holds. A
 * test build checks that instead of assuming it; otherwise only the full
 * sweep above would catch anything the frame still holds.
 */
static void _pframe_release(parse_ctx_t *ctx, _pframe_t *f) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_assert(f->child == NULL);
  switch (f->kind) {
    case _PFK_NODE:
      _ccol_assert(f->u.n.name == NULL && f->u.n.tag == NULL &&
                   f->u.n.pending_key == NULL);
      break;
    case _PFK_FLOW_LIST:
      _ccol_assert(f->u.fl.seq == NULL && f->u.fl.key_str == NULL);
      break;
    case _PFK_FLOW_DICT:
      _ccol_assert(f->u.fd.map == NULL && f->u.fd.key_str == NULL &&
                   f->u.fd.merges == NULL);
      break;
    case _PFK_BLOCK_LIST:
      _ccol_assert(f->u.bl.seq == NULL);
      break;
    case _PFK_BLOCK_DICT:
      _ccol_assert(f->u.bd.map == NULL && f->u.bd.key_owned == NULL &&
                   f->u.bd.merges == NULL);
      break;
  }
#endif
  /* Every collection frame holds a nesting level from the moment it opens,
   * because _pframe_open() refuses the frame outright once the parse is at
   * the cap, so nothing ever pushes such a frame without a level. A node
   * frame holds none. */
  if (f->kind != _PFK_NODE) ctx->depth--;
}

/* Runs one step of whichever state machine this frame belongs to. */
static _pstep_t _pframe_step(parse_ctx_t *ctx, _pframe_t *f, _preq_t *rq) {
  /* Half of all the steps a parse runs are node dispatches, so we answer
   * that kind here, before the jump table the four collection kinds share. */
  if (f->kind == _PFK_NODE) return _pni_step(ctx, f, rq);
  switch (f->kind) {
    case _PFK_NODE:
      break; /* already handled above */
    case _PFK_FLOW_LIST:
      return _pfl_step(ctx, f, rq);
    case _PFK_FLOW_DICT:
      return _pfd_step(ctx, f, rq);
    case _PFK_BLOCK_LIST:
      return _pbl_step(ctx, f, rq);
    case _PFK_BLOCK_DICT:
      return _pbd_step(ctx, f, rq);
  }
  return _PSTEP_FAIL;
}

/*
 * Runs one complete walk from root_req and returns the node it made, or NULL
 * on any failure. ctx->error is set on such a failure unless it was a bare
 * allocation failure, and every caller already treats both the same way.
 *
 * The frame stack lives in this function's locals and moves onto the heap
 * when a document nests deeper than _CYAML_PARSE_INLINE_FRAMES. Only two
 * things ever pass between frames: the request a frame fills in to ask for a
 * child, and the finished node a child hands back. The driver puts that node
 * into the parent's `child` slot, and the parent's cleanup owns it from then
 * until the resume point consumes it.
 */
static cyaml_node_t *_parse_drive(parse_ctx_t *ctx, const _preq_t *root_req) {
  _pframe_t inline_frames[_CYAML_PARSE_INLINE_FRAMES];
  _pframe_t *stack = inline_frames;
  size_t cap = _CYAML_PARSE_INLINE_FRAMES;
  size_t sp = 0;
  cyaml_node_t *result = NULL;
  bool failed = true;
  _preq_t req = *root_req;
#ifdef RUNNING_UNIT_TESTS
  const size_t entry_depth = ctx->depth;
#endif

  if (_pframe_open(ctx, &stack[0], &req)) {
    sp = 1;
    failed = false;
    /* The frame this loop steps is carried in a pointer instead of being
     * indexed from sp on each pass: a frame is neither 8 nor 16 bytes wide, so
     * an index costs a multiply, and this loop would pay two per step, one to
     * find the frame to step and one to hand the finished node to the frame
     * below. Only a growth moves the stack, and that's the one place that
     * rebuilds the pointer from sp. */
    _pframe_t *f = stack;
    for (;;) {
      _pstep_t r = _pframe_step(ctx, f, &req);
      if (r == _PSTEP_DESCEND) {
        if (sp == cap) {
          /* See _CYAML_PARSE_MAX_FRAMES. The collection frames' depth guard
           * refuses a document long before it gets here; this test keeps the
           * stack bounded even for a construct that only pushes node frames,
           * and it costs nothing on the common path because it only runs when
           * the stack grows. A growth never goes past the bound, so the stack
           * holds at most _CYAML_PARSE_MAX_FRAMES frames. */
          if (cap >= _CYAML_PARSE_MAX_FRAMES) {
            parse_depth_err(ctx);
            failed = true;
            break;
          }
          size_t want = cap * 2 < _CYAML_PARSE_MAX_FRAMES
                            ? cap * 2
                            : (size_t)_CYAML_PARSE_MAX_FRAMES;
          _pframe_t *grown = walk_stack_grow_to(
              stack, &cap, want, inline_frames, sizeof(*stack), ctx->mp);
          if (!grown) {
            failed = true;
            break;
          }
          stack = grown;
          f = stack + (sp - 1);
        }
        if (!_pframe_open(ctx, f + 1, &req)) {
          failed = true;
          break;
        }
        sp++;
        f++;
        continue;
      }
      if (r == _PSTEP_FAIL) {
        failed = true;
        break;
      }
      /* The result is _PSTEP_DONE, so hand the finished node to the frame
       * below, or out of this function. */
      {
        cyaml_node_t *done_node = f->result;
        f->result = NULL;
        _pframe_release(ctx, f);
        sp--;
        if (sp == 0) {
          result = done_node;
          break;
        }
        f--;
        f->child = done_node;
      }
    }
  }

  if (failed) {
    /* Nothing is ever in flight outside a frame, because a finished node
     * goes straight into the frame below, which owns it from then on, so the
     * unwind only sweeps the frames. */
    while (sp > 0) {
      sp--;
      _pframe_cleanup(ctx, &stack[sp]);
    }
    result = NULL;
  }
  if (stack != inline_frames) _ccol_mem_free(ctx->mp, stack);

#ifdef RUNNING_UNIT_TESTS
  if (cap > cyaml_test_peak_parse_frame_cap)
    cyaml_test_peak_parse_frame_cap = cap;
  /* Every collection frame gives back the nesting level it took, on the
   * finished and the abandoned path alike, so a walk always leaves the depth
   * count exactly as it found it. That's what lets a speculative key parse
   * (try_parse_scalar_dict_key, which drops the result and rewinds ctx->pos)
   * run a walk of its own without drifting the depth budget it shares with
   * the surrounding walk. */
  _ccol_assert(ctx->depth == entry_depth);
#endif
  return result;
}

/*
 * Reads one directive line, with ctx->pos at the leading '%'. Every directive
 * name is accepted, because YAML 1.2 section 6.8.2's ns-reserved-directive
 * allows a name the parser doesn't know, with any number of parameters,
 * which are ignored.
 *
 * The name "YAML" gets its own strict grammar, fixed by the spec:
 * l-yaml-directive ::= "YAML" s-separate-in-line ns-yaml-version. It allows
 * exactly one <major>.<minor> version token, and nothing else may be on the
 * line apart from optional trailing whitespace and a comment. Like any
 * comment, whitespace must separate it from the version, so a comment right
 * against the version with no space in between isn't a valid comment at all
 * but malformed trailing content.
 *
 * Returns false and sets ctx->error on a malformed "%YAML" line, and true
 * otherwise, which covers a valid "%YAML" line and any other directive name,
 * accepted as it is.
 *
 * *is_yaml_directive_out, when not NULL, says whether this line's directive
 * name was exactly "YAML". The caller needs that to apply YAML 1.2 section
 * 6.8.1's "it is an error to define more than one YAML directive for the
 * same document", which this function can't apply itself because it keeps
 * no per-document state.
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
  if (name_len == 0) {
    /* ns-directive-name is ns-char+, so a '%' with no name after it isn't a
     * directive at all. */
    parse_err(ctx, "directive with no name at position %zu", start);
    return false;
  }
  bool is_yaml = name_len == 4 && memcmp(ctx->src + name_start, "YAML", 4) == 0;
  bool is_tag = name_len == 3 && memcmp(ctx->src + name_start, "TAG", 3) == 0;
  if (is_yaml_directive_out) *is_yaml_directive_out = is_yaml;

  if (is_tag) {
    /* A directive's separators are s-separate-in-line (YAML 1.2 section
     * 6.8), i.e. any run of spaces and tabs. */
    if (at_end(ctx) || (cur(ctx) != ' ' && cur(ctx) != '\t')) {
      parse_err(ctx, "malformed %%TAG directive at position %zu", start);
      return false;
    }
    skip_inline_ws(ctx);

    size_t handle_start = ctx->pos;
    while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
           cur(ctx) != '\n' && cur(ctx) != '\r')
      ctx->pos++;
    size_t handle_len = ctx->pos - handle_start;
    if (handle_len == 0) {
      parse_err(ctx, "malformed %%TAG directive at position %zu", start);
      return false;
    }
    char *handle = _ccol_mem_alloc(ctx->mp, handle_len + 1);
    if (!handle) return false;
    memcpy(handle, ctx->src + handle_start, handle_len);
    handle[handle_len] = '\0';

    /* A %TAG handle has exactly three valid forms: "!", the primary handle;
     * "!!", the secondary handle; or "!" followed by one or more word
     * characters (alphanumerics or '-') and a "!", a named handle. Unlike a
     * shorthand tag token's handle, which takes some work to resolve, this
     * one doesn't, because a directive's handle is a whole word on its own
     * with whitespace on both sides. */
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
      parse_err(ctx, "malformed %%TAG handle '%s' at position %zu",
                _cyaml_echo(handle), start);
      _ccol_mem_free(ctx->mp, handle);
      return false;
    }

    /* Same separator rule as between the name and the handle. */
    if (at_end(ctx) || (cur(ctx) != ' ' && cur(ctx) != '\t')) {
      parse_err(ctx, "malformed %%TAG directive at position %zu", start);
      _ccol_mem_free(ctx->mp, handle);
      return false;
    }
    skip_inline_ws(ctx);

    size_t prefix_start = ctx->pos;
    /* A '#' does NOT stop this scan, unlike the "extra content after the
     * prefix" scan further down. A tag prefix's grammar is ns-uri-char (YAML
     * 1.2 sections 5.6 and 5.7), which allows a '#' as an ordinary URI
     * character, and by s-b-comment a comment can only start after real
     * separating whitespace, never right against the prefix. Stopping at
     * every '#' here would silently truncate any prefix that holds one, a
     * fragment identifier for example, and treat the rest as a trailing
     * comment with no separator at all. The suffix scan of parse_tag_token
     * and skip_tag_token treats a '#' as an ordinary tag character the same
     * way. */
    while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
           cur(ctx) != '\n' && cur(ctx) != '\r') {
      ctx->pos++;
    }
    size_t prefix_len = ctx->pos - prefix_start;
    if (prefix_len == 0) {
      parse_err(ctx, "malformed %%TAG prefix at position %zu", start);
      _ccol_mem_free(ctx->mp, handle);
      return false;
    }
    char *prefix = _ccol_mem_alloc(ctx->mp, prefix_len + 1);
    if (!prefix) {
      _ccol_mem_free(ctx->mp, handle);
      return false;
    }
    memcpy(prefix, ctx->src + prefix_start, prefix_len);
    prefix[prefix_len] = '\0';
    /* A prefix is YAML 1.2 section 6.8.2.2's ns-tag-prefix: a local prefix
     * "!" ns-uri-char*, or a global prefix ns-tag-char ns-uri-char*, so its
     * first byte is never ',', '[' or ']' and every byte is an ns-uri-char.
     * Its percent escapes are decoded here exactly like those of a verbatim
     * tag and a shorthand suffix, so "!e!a" under the prefix "tag:x%21,"
     * resolves to the same tag as "!<tag:x%21,a>", which is "tag:x!,a". */
    if (prefix[0] == ',' || prefix[0] == '[' || prefix[0] == ']') {
      parse_err(ctx,
                "character '%s' at position %zu cannot start a %%TAG prefix",
                _cyaml_echo_c(prefix[0]), prefix_start);
      _ccol_mem_free(ctx->mp, handle);
      _ccol_mem_free(ctx->mp, prefix);
      return false;
    }
    if (!tag_text_check(ctx, prefix, prefix_len, prefix_start, false) ||
        !percent_decode_tag_inplace(ctx, prefix, prefix_start)) {
      _ccol_mem_free(ctx->mp, handle);
      _ccol_mem_free(ctx->mp, prefix);
      return false;
    }

    /* Only whitespace may follow the prefix, plus a comment once that
     * whitespace separates it from the prefix; the %YAML branch below makes
     * the same "extra content after the version" check. YAML 1.2 section
     * 6.8.2's l-tag-directive ends in the same s-l-comments production as
     * every other directive line, not in the free-form ns-reserved-directive
     * parameter list an unknown directive name gets. Without this check a
     * line like "%TAG !e! tag:example.com,2000:app/ garbage-text\n" would
     * silently drop "garbage-text" instead of being rejected. */
    if (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
        cur(ctx) != '\n' && cur(ctx) != '\r' && cur(ctx) != '#') {
      parse_err(ctx, "extra content after %%TAG prefix at position %zu",
                ctx->pos);
      _ccol_mem_free(ctx->mp, handle);
      _ccol_mem_free(ctx->mp, prefix);
      return false;
    }
    skip_inline_ws(ctx);
    if (!at_end(ctx) && cur(ctx) != '\n' && cur(ctx) != '\r' &&
        cur(ctx) != '#') {
      parse_err(ctx, "extra content after %%TAG prefix at position %zu",
                ctx->pos);
      _ccol_mem_free(ctx->mp, handle);
      _ccol_mem_free(ctx->mp, prefix);
      return false;
    }

    /* YAML 1.2 section 6.8.2 says "it is an error to define the same handle
     * more than once", and section 6.8.1 has the same rule for a duplicate
     * %YAML directive. Two independent reference parsers confirm it, and
     * both reject a duplicate even when the second directive repeats the
     * exact same prefix. */
    ccol_retval_t set_rv = tag_handles_set(ctx, handle, prefix);
    _ccol_mem_free(ctx->mp, handle);
    _ccol_mem_free(ctx->mp, prefix);
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
    /* The separator is s-separate-in-line (spaces and tabs); see the %TAG
     * branch above. */
    if (at_end(ctx) || (cur(ctx) != ' ' && cur(ctx) != '\t')) {
      parse_err(ctx, "malformed %%YAML directive at position %zu", start);
      return false;
    }
    skip_inline_ws(ctx);
    size_t ver_start = ctx->pos;
    while (!at_end(ctx) && cur(ctx) >= '0' && cur(ctx) <= '9') ctx->pos++;
    bool has_major = ctx->pos > ver_start;
    /* The major version, without leading zeros. YAML 1.2 section 6.8.1 lets a
     * parser process a document of a higher minor version and requires it to
     * refuse a document of any other major version. */
    bool major_is_one = false;
    if (has_major) {
      size_t m = ver_start;
      while (m < ctx->pos - 1 && ctx->src[m] == '0') m++;
      major_is_one = ctx->pos - m == 1 && ctx->src[m] == '1';
    }
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
    if (!major_is_one) {
      parse_err(ctx,
                "unsupported %%YAML version at position %zu (only major "
                "version 1 is supported)",
                start);
      return false;
    }
    /* Only whitespace may follow, plus a comment once that whitespace
     * separates it from the version. */
    if (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
        cur(ctx) != '\n' && cur(ctx) != '\r') {
      parse_err(ctx, "extra content after %%YAML version at position %zu",
                ctx->pos);
      return false;
    }
    skip_inline_ws(ctx);
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

/* Reads a '---' document-start marker, whose three dashes the caller has
 * already confirmed at ctx->pos with at_doc_marker() and a check for the
 * literal "---". It moves past the marker and the separator whitespace and
 * newline after it, and reports through *same_line_content_out whether real
 * content follows on the same physical line. The separator may hold spaces
 * and tabs ("---\tscalar").
 */
static void consume_doc_start_marker(parse_ctx_t *ctx,
                                     bool *same_line_content_out) {
  ctx->pos += 3;
  skip_inline_ws(ctx);
  *same_line_content_out = !rest_of_line_is_blank(ctx);
  if (!at_end(ctx) && at_eol(ctx)) skip_newline(ctx);
  skip_ws_comments(ctx);
}

/* ========================================================================== */
/*                         PUBLIC PARSE ENTRY                                 */
/* ========================================================================== */

/*
 * Skips a UTF-8 byte order mark (EF BB BF) at ctx->pos when ctx->pos is at
 * the start of a line and no document is open. YAML 1.2 section 9.1.1's
 * l-document-prefix allows a byte order mark before every document of a
 * stream, not only before the first, which is what you get from
 * concatenating files that each start with one. libyaml skips such a mark at
 * column 0 the same way.
 *
 * The mark isn't content, so for column tracking the line holding it starts
 * right after it. That's why the line bookkeeping restarts at the new
 * position: ctx->first_line_start, the line cache and line_start_pos()'s
 * memo all move there. Without that every column on the line would be 3 too
 * high, and at_doc_marker()'s column-0 test would refuse a "---" after the
 * mark. The parser never asks about a position before a completed document,
 * so dropping the earlier line starts loses nothing. The '#' tests of
 * skip_ws_comments() and rest_of_line_is_blank() treat ctx->first_line_start
 * as the start of the stream, so a comment may follow the mark directly.
 *
 * Returns true when it skipped a mark.
 */
static bool skip_document_prefix_bom(parse_ctx_t *ctx) {
  size_t p = ctx->pos;
  if (ctx->len - p < 3 || (unsigned char)ctx->src[p] != 0xEF ||
      (unsigned char)ctx->src[p + 1] != 0xBB ||
      (unsigned char)ctx->src[p + 2] != 0xBF)
    return false;
  if (p > ctx->first_line_start && ctx->src[p - 1] != '\n' &&
      ctx->src[p - 1] != '\r')
    return false;
  p += 3;
  ctx->pos = p;
  ctx->first_line_start = p;
  if (!ctx->line_cache_disabled) {
    ctx->line_starts[0] = p;
    ctx->line_starts_len = 1;
    ctx->line_scan_pos = p;
  }
  ctx->memo_pos = SIZE_MAX;
  return true;
}

/*
 * Parses one YAML document from the context's current position.
 *
 * It reads an optional leading '---' document-start marker, along with any
 * '%YAML' and '%TAG' directives before that marker, which it ignores. Then
 * it parses one root node, resets the document's anchor table, and finally
 * reads an optional trailing '...' document-end marker.
 *
 * On success it returns the root node, with ctx->pos past the document it
 * read, any trailing '...' included, and just before any leading '---' of
 * the NEXT document, so the caller can loop.
 *
 * *consumed_end_marker says whether a trailing '...' was found and read.
 * YAML 1.2 section 6.9's l-yaml-stream only lets the next document leave out
 * its own '---' when one or more '...' end markers come directly before it,
 * so the caller needs this to decide whether the next document requires a
 * '---'.
 *
 * Returns NULL, with ctx->error set, on a parse error. The anchor table is
 * always destroyed before it returns.
 */
static cyaml_node_t *parse_one_document(parse_ctx_t *ctx,
                                        bool *consumed_end_marker) {
  /* Whether real root content follows an explicit '---' marker directly on
   * that SAME physical line, as opposed to a lone '---' with content starting
   * on a later line. YAML 1.2's grammar only gives root content reached this
   * way the "flow-in-block" alternative of s-l+block-node (a plain scalar, a
   * quoted scalar, or a flow collection), never the "block-in-block"
   * alternative a block mapping or block sequence needs, because that one
   * requires s-l-comments (only whitespace, a comment, or a newline) right
   * after the '---', and after any node properties decorating the '---' too.
   * Two independent reference parsers confirm it: both reject "--- a: b" and
   * "--- - a", and both accept "---\na: b" and a bare "a: b" with no marker at
   * all. */
  bool doc_marker_same_line_content = false;

  /* Whether this document has already read its own leading '---'. As YAML
   * 1.2 section 6.8.1 says, a directive can only come before the '---' it
   * configures, so once a document's start marker has been read, a '%' found
   * after it can never belong to THIS document; it can only be the directive
   * prefix of the next document, and this document is then empty and ends
   * right here. Without this flag, when a bare '---' is followed on a later
   * line by a document carrying a directive, that next document's whole
   * "%directive\n---\ncontent" would silently be taken as content of this
   * document, and the empty document this '---' really introduces would be
   * lost. */
  bool consumed_leading_dashes = false;

  /* Accept an optional leading document-start marker. By YAML 1.2 the '---'
   * token is only a document-start indicator when whitespace, a comment or
   * the end of the input follows its three dashes, so '---X', with X any
   * other character, is a plain scalar and not a marker. */
  if (at_doc_marker(ctx) && memcmp(ctx->src + ctx->pos, "---", 3) == 0) {
    consume_doc_start_marker(ctx, &doc_marker_same_line_content);
    consumed_leading_dashes = true;
  }

  /* Accept a %YAML or %TAG directive without reporting anything about it. A
   * directive always needs an explicit '---' document-start marker after it,
   * because YAML 1.2 section 6.8.1 ties a directive to one specific document,
   * never to "no document" or an implicit empty one. So the end of the input,
   * or a plain '...' end marker, with no such '---' in between is a syntax
   * error and not an empty document, and we have to track and enforce that
   * explicitly because it doesn't fall out of the loop naturally. The
   * !consumed_leading_dashes test gates this loop, because a document that has
   * already read its own '---' can never also own a directive (see the doc
   * comment of consumed_leading_dashes above), so a '%' found there must be
   * left untouched for the NEXT parse_one_document call to take as the
   * following document's prefix. */
  bool saw_directive = false;
  bool saw_yaml_directive = false;
  bool consumed_doc_start_after_directive = false;
  while (!consumed_leading_dashes && !at_end(ctx) && cur(ctx) == '%') {
    saw_directive = true;
    bool is_yaml_directive = false;
    if (!parse_directive_line(ctx, &is_yaml_directive)) {
      /* A %TAG directive may already have filled ctx->tag_handles before this
       * directive line failed (a malformed %TAG after an earlier valid one,
       * for example), and this path never reaches the function's normal
       * exit, so every early return in this directive section must clean
       * up that table, not only the exit at the bottom. Each of them cleans up
       * the anchor table too, which keeps the paths alike and protects a
       * future change, even though nothing can fill the anchor table this
       * early. */
      anchors_destroy(ctx);
      tag_handles_destroy(ctx);
      return NULL;
    }
    if (is_yaml_directive) {
      /* YAML 1.2 section 6.8.1 states this rule: "It is an error to define
       * more than one YAML directive for the same document, even if both
       * occurrences give the same version." */
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
      consume_doc_start_marker(ctx, &doc_marker_same_line_content);
      consumed_doc_start_after_directive = true;
      /* This store keeps consumed_leading_dashes true to its name, since the
       * marker has just been read, although no current path reads the flag
       * again: the break below leaves the only loop that tests it and nothing
       * after that loop looks at it, so a static analyzer reports the store as
       * dead. It stays, because the alternative is a variable saying no
       * dashes were read right after they were, which becomes wrong the moment
       * the break changes or a later reader appears. */
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

  /* An empty document: we are at the end of the input, at another document
   * boundary, or at a fresh '%' directive line at column 0. at_doc_marker()
   * doesn't cover the last case because it only recognizes a literal '---'
   * or '...', but '%' is a c-indicator character, so a plain scalar can never
   * validly start with it, and a bare '%' reached here can only be the next
   * document's directive prefix, never this document's content. Without this
   * check two directive-only documents in a row, as in
   * "%YAML 1.2\n---\n%YAML 1.2\n---\n", would lose the second one's directive
   * line, which would silently be read as scalar content of the first
   * document, since parse_node() knows nothing about directive syntax. */
  cyaml_node_t *root;
  if (at_end(ctx) || at_doc_marker(ctx) ||
      (current_col(ctx) == 0 && cur(ctx) == '%')) {
    root = node_alloc(CYAML_NULL, ctx->mp);
  } else if (doc_marker_same_line_content && at_bare_seq_indicator(ctx)) {
    /* A block sequence has no "flow-in-block" alternative either, so it can
     * no more start on the same line as a '---' than a block mapping can. See
     * the doc comment of doc_marker_same_line_content. */
    parse_err(ctx,
              "a block sequence cannot start on the same line as "
              "its document start marker at position %zu",
              ctx->pos);
    root = NULL;
  } else {
    /* -1 is the document root's indentation sentinel (YAML 1.2 section
     * 8.1.1.1): the root node has no enclosing block context, so a block
     * scalar at column 0, right after "--- >" for example, is more indented
     * than its "parent" and must not be rejected as empty. allow_inline_map
     * is false only when real content sits right on the same line as the
     * '---', because an implicit "key: value" mapping there has no valid
     * grammar path either, by the same underlying rule as the bare sequence
     * check above, so it is reported as an ordinary chained-mapping
     * value error instead of being silently accepted. */
    root = parse_node(ctx, -1, false, false, !doc_marker_same_line_content,
                      false, false, NULL);
  }

  /* The anchor table and the %TAG handle table both belong to one document,
   * so reset them before returning. */
  anchors_destroy(ctx);
  tag_handles_destroy(ctx);

  if (!root) return NULL;

  /* Read any trailing '...' document-end markers. YAML 1.2 section 6.9's
   * l-yaml-stream grammar groups one or more consecutive '...' markers, with
   * its l-document-suffix+ production, into one unit of separator material
   * between documents, not one document boundary per marker. So a second
   * '...' found here, with nothing but whitespace and comments between it and
   * the one just read, belongs to the closing of THIS same document instead
   * of being a separate document of its own, which the empty-document check
   * above would then wrongly make empty. Without this loop "a: 1\n...\n...\n"
   * would leave the second '...' for the next parse_one_document call, which
   * would run the empty-document check above again and invent an extra
   * CYAML_NULL document out of what is only repeated separator noise. PyYAML
   * parses this input as one document.
   *
   * at_doc_marker() requires column 0, so an indented '...' is never silently
   * swallowed as a document boundary; the outer parse_common loop reports it
   * as content at the end of the stream. */
  skip_ws_comments(ctx);
  *consumed_end_marker = false;
  while (at_doc_marker(ctx) && memcmp(ctx->src + ctx->pos, "...", 3) == 0) {
    ctx->pos += 3;
    /* Spaces and tabs may separate the marker from a comment. */
    skip_inline_ws(ctx);
    /* Only whitespace and an optional comment may share the '...' marker's
     * line; real content there, such as "... invalid", has no valid reading.
     * Without this check that content would silently be left for the caller,
     * which would read it as the start of a bare next document. */
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

/* ========================================================================== */
/*                         PARSE ERROR REPORTING                              */
/* ========================================================================== */

/*
 * The buffer behind the err_str out-parameter of every parse entry point.
 *
 * The err out-parameter of EVERY module in this library has one rule: the
 * string belongs to the library and the caller never frees it. Ten other
 * modules keep that rule by handing back a _CCOL_ERR_STR() literal. This
 * parser can't, because its message names a position and a token, so the
 * text isn't known until the parse fails.
 *
 * Allocating the message on the heap looks like the natural answer, but it
 * would give the same-looking `char **err_str` parameter a second, opposite
 * ownership rule, and nothing in the type or at the call site would tell the
 * two apart. A caller who learned "never free it" from cvector or chashmap
 * would leak here, and a caller who learned "free it" here would call free()
 * on a string literal there and crash. It would also allocate on a path that
 * a failed allocation can reach.
 *
 * With this buffer the question goes away. The library owns the storage, the
 * caller never frees anything, and the rule is the same everywhere. The
 * lifetime follows the strerror(3) and dlerror(3) convention that the header
 * documents: the text stays valid until the NEXT failing parse on the SAME
 * thread. It is per-thread, so two threads parsing at the same time never
 * overwrite each other's message.
 *
 * The buffer itself lives on the heap and the thread holds a pointer to it;
 * the thread's first failing parse allocates it and the thread-exit drain of
 * the node pool frees it. A thread-local array of this size would sit in the
 * library's thread-local block, which the loader has to fit into its small
 * reserve when a process loads the library with dlopen(). When the
 * allocation fails, or the thread can't arm the key whose destructor would
 * free the buffer, *err_str points at a fixed message instead, which the
 * caller doesn't free either.
 *
 * 512 bytes is the size of parse_ctx_t.error, where every message of this
 * parser is built, so nothing longer than that can arrive here.
 */
#define CYAML_ERR_BUF_LEN 512
static const char cyaml_err_unstored[] =
    "parse failed; the error message could not be stored";

/* The calling thread's message buffer, allocated on first use, or NULL when
 * the allocation fails. */
static char *cyaml_err_storage(void) {
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  _ccol_call_once(_pool_key_once, _do_pool_key_init);
  _cyaml_tls_t *t = _cyaml_tls_get();
  if (!t) return NULL;
  if (!t->err_buf) t->err_buf = malloc(CYAML_ERR_BUF_LEN);
  return t->err_buf;
#else
  if (!cyaml_err_buf) {
    char *buf = malloc(CYAML_ERR_BUF_LEN);
    if (!buf) return NULL;
    _ccol_call_once(_pool_key_once, _do_pool_key_init);
    _pool_key_arm();
    /* Only the thread-exit drain of an armed key frees the buffer, so a
     * thread whose key can't be armed keeps no buffer at all. */
    if (!_pool_key_armed) {
      free(buf);
      return NULL;
    }
    cyaml_err_buf = buf;
  }
  return cyaml_err_buf;
#endif /* _CCOL_EMULATE_DARWIN_TLS */
}

#ifdef RUNNING_UNIT_TESTS
/* Every message a parse reports is printable ASCII (see cyaml_err_echo()).
 * Under the unit tests and the fuzz targets a message that breaks that rule
 * ends the process right where it was produced. */
static void cyaml_err_assert_printable(const char *msg) {
  for (const unsigned char *p = (const unsigned char *)msg; *p; p++) {
    if (*p < 0x20 || *p > 0x7E) {
      fprintf(stderr, "cyaml error message holds byte 0x%02X: ", *p);
      for (const unsigned char *q = (const unsigned char *)msg; *q; q++)
        fprintf(stderr, (*q >= 0x20 && *q <= 0x7E) ? "%c" : "<0x%02X>", *q);
      fputc('\n', stderr);
      abort();
    }
  }
}
#endif

/* Copies msg into the per-thread buffer and points *err_str at it. Does
 * nothing when the caller passed no err_str. */
static void cyaml_report_err(char **err_str, const char *msg) {
#ifdef RUNNING_UNIT_TESTS
  cyaml_err_assert_printable(msg);
#endif
  if (!err_str) return;
  char *buf = cyaml_err_storage();
  if (!buf) {
    *err_str = (char *)cyaml_err_unstored;
    return;
  }
  snprintf(buf, CYAML_ERR_BUF_LEN, "%s", msg);
  *err_str = buf;
}

/*
 * The shared YAML parse entry point.
 *
 * It handles an optional UTF-8 BOM (EF BB BF) and then loops over every
 * document of the stream, separated by optional '---' and '...' markers,
 * resetting the anchor table between documents so no anchor crosses a
 * boundary.
 *
 * A stream with one document gives that document's root node directly.
 *
 * A stream with more than one document gives one CYAML_LIST node whose
 * elements are the individual document roots, in order. Every document
 * after the first must begin with a '---' marker, and bare content after
 * the end of a document is a parse error.
 *
 * On failure, when err_str isn't NULL, *err_str points at the per-thread
 * message buffer that cyaml_report_err() fills, which the library owns and
 * the caller never frees. Every document parsed successfully so far is
 * destroyed before NULL comes back.
 */
static cyaml parse_common(const char *src, size_t len, char **err_str,
                          ccol_memmgmt_procs_t *mp) {
  if (!src) {
    cyaml_report_err(err_str, "null input");
    return NULL;
  }
  /* The whole stream must be c-printable UTF-8 before any scanner reads
   * it; see cyaml_first_refused_char(). */
  size_t refused_at = cyaml_first_refused_char((const unsigned char *)src, len);
  if (refused_at != len) {
    if (err_str) {
      char *buf = cyaml_err_storage();
      if (!buf) {
        *err_str = (char *)cyaml_err_unstored;
        return NULL;
      }
      cyaml_describe_refused_char((const unsigned char *)src, len, refused_at,
                                  buf, CYAML_ERR_BUF_LEN);
#ifdef RUNNING_UNIT_TESTS
      cyaml_err_assert_printable(buf);
#endif
      *err_str = buf;
    }
    return NULL;
  }
  if (!cyaml_intern_procs(&mp)) {
    cyaml_report_err(err_str,
                     "too many distinct allocator procs in this process");
    return NULL;
  }
  parse_ctx_t ctx = {.src = src,
                     .pos = 0,
                     .len = len,
                     .error = "",
                     .mp = mp,
                     .anchors = NULL,
                     /* No document reaches this offset, so the first question
                        asked can't match a memo that was never filled in.
                        It's set explicitly instead of left at the zero this
                        field would otherwise get, because zero is a real
                        position, and the BOM branch below rewrites
                        line_starts[0] after this point, so a memo that
                        looked valid at position zero would answer with the
                        pre-BOM line start. */
                     .memo_pos = SIZE_MAX};
  parse_node_budget_arm(len);

  /* Allocate line_start_pos()'s position cache here, before any parse
   * function runs. That function's doc comment explains why failing this one
   * allocation is fatal while a later growth of the same cache degrades
   * gracefully. */
  ctx.line_starts = _ccol_mem_alloc(mp, 64 * sizeof(size_t));
  if (!ctx.line_starts) {
    cyaml_report_err(err_str, "out of memory");
    parse_node_budget_disarm();
    return NULL;
  }
  ctx.line_starts_cap = 64;
  ctx.line_starts[0] = 0;
  ctx.line_starts_len = 1;

  /* Skip the byte order marks of the first document prefix; see
   * skip_document_prefix_bom(). */
  while (skip_document_prefix_bom(&ctx)) {
  }

#ifdef RUNNING_UNIT_TESTS
  if (cyaml_test_force_line_cache_disabled) ctx.line_cache_disabled = true;
#endif

  /* The stream's leading whitespace may hold tabs, since they are separation
   * in front of a flow node, and a block collection after them fails at its
   * own start (see line_space_indent()). A comment line may sit between two
   * byte order marks of the prefix, as between documents. */
  skip_ws_comments(&ctx);
  while (skip_document_prefix_bom(&ctx)) skip_ws_comments(&ctx);

  /* A temporary array collecting the parsed document roots. */
  cyaml_node_t **docs = NULL;
  size_t ndocs = 0;
  size_t dcap = 0;
  bool prev_consumed_end_marker = false;
  size_t bom_pos = SIZE_MAX;

  size_t first_doc_peak = 0;

  while (!at_end(&ctx)) {
    /* After the first document the next one must start with a '---', with
     * one exception: when a '...' end marker ended the previous document, a
     * bare document without '---' may follow directly, as YAML 1.2 section
     * 6.9's l-yaml-stream says. A document with no '...' before it has to be
     * explicit, while one that does follow a '...' may be bare, may carry
     * directives, or may be explicit. Bare content with no '...' before it is
     * ambiguous trailing material at the end of the stream. */
    if (ndocs > 0 && !prev_consumed_end_marker) {
      if (!at_doc_marker(&ctx) || memcmp(ctx.src + ctx.pos, "---", 3) != 0) {
        char buf[128];
        if (bom_pos != SIZE_MAX)
          snprintf(buf, sizeof(buf),
                   "byte order mark inside a document at position %zu; a "
                   "byte order mark may only precede a document",
                   bom_pos);
        else
          snprintf(buf, sizeof(buf), "trailing content at position %zu",
                   ctx.pos);
        cyaml_report_err(err_str, buf);
        goto fail;
      }
    }

    /* A stream of several documents gives a list holding every document, so
     * each document sits one container deeper than it would alone. A document
     * after the first is known to be wrapped, so its depth count starts at 1.
     * The first one was parsed before anyone knew, so it gets refused here
     * when it used up every level. */
    if (ndocs == 1 && first_doc_peak >= CYAML_MAX_NESTING_DEPTH) {
      char buf[192];
      snprintf(buf, sizeof(buf),
               "maximum nesting depth (%d) exceeded at position %zu: the "
               "first document nests %d containers, and the list of a "
               "stream of several documents adds one more",
               CYAML_MAX_NESTING_DEPTH, ctx.pos, CYAML_MAX_NESTING_DEPTH);
      cyaml_report_err(err_str, buf);
      goto fail;
    }
    ctx.depth = (ndocs > 0) ? 1 : 0;
    ctx.peak_depth = ctx.depth;
    bool consumed_end_marker = false;
    cyaml_node_t *root = parse_one_document(&ctx, &consumed_end_marker);
    if (ndocs == 0) first_doc_peak = ctx.peak_depth;
    if (!root) {
      if (ctx.error[0]) {
        cyaml_report_err(err_str, ctx.error);
      } else {
        /* Every real syntax rejection in this parser reports a specific
         * diagnostic with parse_err() before returning a failure, the
         * convention throughout this file, so only two things get here with
         * ctx.error empty.
         *
         * One is an allocation that failed deep in the call chain with no
         * diagnostic of its own: many low-level DOM-building helpers, such as
         * node_alloc() and cyaml_create_dictionary_mp(), are shared with the
         * public API, which does no parsing, so they have no parse_ctx_t to
         * report through at all.
         *
         * The other is one of this thread's two parse budgets,
         * CYAML_MAX_PARSE_NODES and CYAML_MAX_PARSE_BYTES (see the doc
         * comments of those macros), running out at such a construction
         * site; node_alloc() can't report through ctx.error either, for the
         * same reason.
         *
         * We tell the cases apart instead of reporting all of them as a
         * general allocation failure, because neither budget case is an OOM
         * at all: each fires with plenty of memory free, and a caller who saw
         * "out of memory" for an ordinary large document would have no way to
         * tell the two apart. */
        char buf[128];
        if (_parse_node_budget_exhausted)
          snprintf(buf, sizeof(buf),
                   "document exceeded the %zu node-allocation limit at "
                   "position %zu",
                   _parse_node_limit, ctx.pos);
        else if (_parse_byte_budget_exhausted)
          snprintf(buf, sizeof(buf),
                   "document exceeded the %zu-byte memory limit at "
                   "position %zu",
                   _parse_byte_limit, ctx.pos);
        else
          snprintf(buf, sizeof(buf),
                   "out of memory (unreported allocation failure) at "
                   "position %zu",
                   ctx.pos);
        cyaml_report_err(err_str, buf);
      }
      goto fail;
    }

    /* Grow the docs array when needed. */
    if (ndocs == dcap) {
      size_t new_cap = dcap ? dcap * 2 : 4;
      cyaml_node_t **nd =
          _ccol_mem_realloc(mp, docs, new_cap * sizeof(cyaml_node_t *));
      if (!nd) {
        __cyaml_destroy((cyaml)root);
        cyaml_report_err(err_str, "out of memory");
        goto fail;
      }
      docs = nd;
      dcap = new_cap;
    }
    docs[ndocs++] = root;
    prev_consumed_end_marker = consumed_end_marker;

    skip_ws_comments(&ctx);
    /* A byte order mark may open the next document's prefix (see
     * skip_document_prefix_bom()), and the position of the first one is what
     * the error names when no document start follows. */
    bom_pos = ctx.pos;
    if (skip_document_prefix_bom(&ctx)) {
      skip_ws_comments(&ctx);
      while (skip_document_prefix_bom(&ctx)) skip_ws_comments(&ctx);
    } else {
      bom_pos = SIZE_MAX;
    }
  }

  /* The input is empty, so we give back a null node, the same as parsing an
   * empty plain scalar, since the YAML 1.2 core schema resolves an empty
   * value to null. */
  if (ndocs == 0) {
    _ccol_mem_free(mp, docs);
    cyaml_node_t *empty = node_alloc(CYAML_NULL, mp);
    if (!empty) {
      /* *err_str must not be set to NULL before success is confirmed. Every
       * other OOM path in this function sets a real message before returning
       * NULL, and the documented contract says *err_str is NULL only on
       * success, so a caller following that contract must never see a NULL
       * result next to a NULL message here. */
      cyaml_report_err(err_str, "out of memory");
      parse_node_budget_disarm();
      _ccol_mem_free(mp, ctx.line_starts);
      return NULL;
    }
    if (err_str) *err_str = NULL;
    parse_node_budget_disarm();
    _ccol_mem_free(mp, ctx.line_starts);
    return (cyaml)empty;
  }

  /* A single document: return its root node directly. */
  if (ndocs == 1) {
    cyaml_node_t *result = docs[0];
    _ccol_mem_free(mp, docs);
    if (err_str) *err_str = NULL;
    parse_node_budget_disarm();
    _ccol_mem_free(mp, ctx.line_starts);
    return (cyaml)result;
  }

  /* More than one document, so wrap them in a CYAML_LIST. */
  cyaml_node_t *list = (cyaml_node_t *)cyaml_create_list_interned(mp);
  if (!list) {
    cyaml_report_err(err_str, "out of memory");
    goto fail;
  }
  for (size_t i = 0; i < ndocs; i++) {
    cyaml_node_t *elem = docs[i];
    if (cvector_push_back(list->value.list, &elem) != ccol_success) {
      /* Destroy every document the list doesn't own yet. */
      for (size_t j = i; j < ndocs; j++) __cyaml_destroy((cyaml)docs[j]);
      _ccol_mem_free(mp, docs);
      __cyaml_destroy((cyaml)list);
      cyaml_report_err(err_str, "out of memory");
      parse_node_budget_disarm();
      _ccol_mem_free(mp, ctx.line_starts);
      return NULL;
    }
    /* The stream list owns this node from here on; see the guard of
     * cyaml_list_push(). */
    elem->attached = true;
  }
  _ccol_mem_free(mp, docs);
  if (err_str) *err_str = NULL;
  parse_node_budget_disarm();
  _ccol_mem_free(mp, ctx.line_starts);
  return (cyaml)list;

fail:
  parse_node_budget_disarm();
  for (size_t i = 0; i < ndocs; i++) __cyaml_destroy((cyaml)docs[i]);
  _ccol_mem_free(mp, docs);
  _ccol_mem_free(mp, ctx.line_starts);
  return NULL;
}

/*
 * parse_common() inside one "C" locale scope. The core schema writes a
 * float's decimal point as '.' whatever LC_NUMERIC says, while the strtod()
 * of try_parse_float_scalar() and the "%g" of format_float_key() follow
 * LC_NUMERIC. One scope around the whole parse costs two uselocale() calls
 * per document and nothing per scalar. Without it a thread whose locale
 * writes ',' as its decimal point would read "1.5" as a string.
 */
static cyaml parse_in_c_locale(const char *src, size_t len, char **err_str,
                               ccol_memmgmt_procs_t *mp) {
  ccol_c_locale_scope_t scope = ccol_c_locale_enter();
  cyaml root = parse_common(src, len, err_str, mp);
  ccol_c_locale_leave(scope);
  return root;
}

/* Parses a NUL-terminated YAML string. mp may be NULL for the default
 * allocator. On a failure, when err_str isn't NULL, *err_str points at a
 * per-thread message owned by the library, which the caller never frees and
 * which stays valid until the next failing parse on the same thread, as
 * cyaml.h documents. */
cyaml cyaml_parse_mp(const char *yaml_str, char **err_str,
                     ccol_memmgmt_procs_t *mp) {
  if (!yaml_str) return parse_common(NULL, 0, err_str, mp);
  return parse_in_c_locale(yaml_str, strlen(yaml_str), err_str, mp);
}

/* Like cyaml_parse_mp, but with an explicit byte length, so the input
 * doesn't have to end with a NUL byte. */
cyaml cyaml_parse_n_mp(const char *yaml_str, size_t len, char **err_str,
                       ccol_memmgmt_procs_t *mp) {
  return parse_in_c_locale(yaml_str, len, err_str, mp);
}

/* ========================================================================== */
/*                         SERIALIZER HELPERS                                 */
/* ========================================================================== */

/*
 * True only when n's tag is one of the seven YAML 1.2 core-schema tag URIs
 * AND matches n's type, like CYAML_TAG_INT on a CYAML_INTEGER node or
 * CYAML_TAG_SEQ on a CYAML_LIST node. The serializer never writes such a
 * tag, because its own output discipline already makes the value round trip
 * through its implicit type with no help, so a tag that only restates the
 * type would add nothing. That discipline has three parts: needs_quoting()'s
 * full coverage against a misread as another type, yb_append_double()'s
 * float-versus-integer guard, and the fixed, unambiguous literal form of
 * every other type.
 *
 * No public entry point builds a node whose core-schema tag names another
 * type: cyaml_node_set_tag() refuses such a tag, and every call that changes
 * a node's type drops a core tag that stops matching. We compare the type
 * anyway instead of treating all seven URIs as omittable, so that a
 * mismatched core tag, if one ever reached the serializer, would go out in
 * verbatim !<...> form like a custom tag instead of silently vanishing.
 */
static bool tag_matches_node_type(const cyaml_node_t *n) {
  if (!n->tag) return false;
  cyaml_node_type_t expected;
  /* A tag that isn't a core-schema tag is never redundant. */
  if (!core_tag_expected_type(n->tag, &expected)) return false;
  return n->type == expected;
}

/* The bytes that needs_quoting() looks at more closely: NUL (the end),
 * every byte is_disallowed_control_byte() refuses and the tab, the line
 * breaks, the indicators '?', ',', '[', ']', '{', '}', ':' and '#', and the
 * lead bytes 0xC2, 0xE2 and 0xEF of the UTF-8 sequences it quotes. */
static const unsigned char _cyaml_nq_class[256] = {
    [0x00] = 1, [0x01] = 1, [0x02] = 1, [0x03] = 1, [0x04] = 1, [0x05] = 1,
    [0x06] = 1, [0x07] = 1, [0x08] = 1, [0x09] = 1, [0x0A] = 1, [0x0B] = 1,
    [0x0C] = 1, [0x0D] = 1, [0x0E] = 1, [0x0F] = 1, [0x10] = 1, [0x11] = 1,
    [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 1, [0x16] = 1, [0x17] = 1,
    [0x18] = 1, [0x19] = 1, [0x1A] = 1, [0x1B] = 1, [0x1C] = 1, [0x1D] = 1,
    [0x1E] = 1, [0x1F] = 1, [0x7F] = 1, ['?'] = 1,  [','] = 1,  ['['] = 1,
    [']'] = 1,  ['{'] = 1,  ['}'] = 1,  [':'] = 1,  ['#'] = 1,  [0xC2] = 1,
    [0xE2] = 1, [0xEF] = 1};

/*
 * True when the serializer must quote the string s, and false when s can
 * go out as an unquoted plain YAML scalar because it doesn't look like a
 * null, a bool or a number, holds no indicator character, and isn't empty.
 * Its strtod() runs inside the serialize call's "C" locale scope.
 */
static bool needs_quoting(const char *s) {
  if (!s || s[0] == '\0') return true;

  /* A U+FEFF in UTF-8 at the very start of a document is a byte order mark
   * and not content, and a parse removes it, so a scalar whose own first
   * character is U+FEFF needs quotes, which put the mark inside a
   * double-quoted scalar's content where nothing removes it. Only a scalar
   * written as the very first bytes of the output can get to offset 0, but we
   * apply the test to every scalar for the same reason as the "<<" case
   * below: one shared rule stays in force at any future emission site, where
   * a per-call-site special case could go missing.
   * Without this a dictionary whose first key is "\xEF\xBB\xBFz" would
   * serialize to "\xEF\xBB\xBFz: 7", which parses back under the key "z",
   * silently losing the original key and its value. */
  if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
      (unsigned char)s[2] == 0xBF)
    return true;

  /* The strings a parse would read back as another type. The list always
   * holds "<<", in key and value position alike, although the ambiguity "<<"
   * guards against is merge-key detection, which only a key position ever
   * reaches: one shared check here is safer than a special case at the
   * key-emission call sites alone, which could miss a future use in a value
   * position, and the cost is one harmless, unneeded quote on the rare
   * literal scalar value "<<".
   *
   * The list covers two schemas. The first group is what THIS parser
   * resolves as a null, a bool or a special float under the YAML 1.2 core
   * schema. The second group, starting at "y", is what YAML 1.1 resolves as a
   * bool and 1.2 leaves as a plain string. This module reads "yes" as the
   * string "yes", and rightly so, but an unquoted "yes" in its OUTPUT is read
   * as boolean true by every YAML 1.1 reader, which is most of the deployed
   * ones (PyYAML in its default mode, Ruby's Psych and Go's yaml.v2 among
   * them), so a document going out of this serializer and into one of those
   * tools would silently change meaning. That's the "Norway problem", where
   * the country code NO becomes false. Quoting these on the way out costs two
   * bytes and settles the question for every reader whatever schema it
   * follows, and it changes nothing about how this module PARSES them.
   *
   * The switch on the first byte is what keeps the list cheap: a scalar like
   * "record_5" settles every question below with a one-byte comparison
   * instead of entering the C library once per candidate. */
  /* len is the length of s counted only up to 6: no string of the list is
   * longer than 5 bytes, so a longer scalar matches nothing. Each case below
   * only compares the spellings starting with its byte, by length first and
   * then with a constant-size memcmp that the compiler inlines. */
  size_t len = 0;
  while (len < 6 && s[len] != '\0') len++;
#define _CYAML_IS(lit) \
  (len == sizeof(lit) - 1 && memcmp(s, (lit), sizeof(lit) - 1) == 0)
  bool ambiguous = false;
  switch (s[0]) {
    case '~':
      ambiguous = _CYAML_IS("~");
      break;
    case 'n': /* null; the YAML 1.1 booleans n and no */
      ambiguous = _CYAML_IS("null") || _CYAML_IS("n") || _CYAML_IS("no");
      break;
    case 'N':
      ambiguous = _CYAML_IS("Null") || _CYAML_IS("NULL") || _CYAML_IS("N") ||
                  _CYAML_IS("No") || _CYAML_IS("NO");
      break;
    case 't':
      ambiguous = _CYAML_IS("true");
      break;
    case 'T':
      ambiguous = _CYAML_IS("True") || _CYAML_IS("TRUE");
      break;
    case 'f':
      ambiguous = _CYAML_IS("false");
      break;
    case 'F':
      ambiguous = _CYAML_IS("False") || _CYAML_IS("FALSE");
      break;
    case '.':
      ambiguous = _CYAML_IS(".inf") || _CYAML_IS(".Inf") || _CYAML_IS(".INF") ||
                  _CYAML_IS(".nan") || _CYAML_IS(".NaN") || _CYAML_IS(".NAN");
      break;
    case '+':
      ambiguous =
          _CYAML_IS("+.inf") || _CYAML_IS("+.Inf") || _CYAML_IS("+.INF");
      break;
    case '-':
      ambiguous =
          _CYAML_IS("-.inf") || _CYAML_IS("-.Inf") || _CYAML_IS("-.INF");
      break;
    case 'y': /* YAML 1.1 booleans; see the comment above. */
      ambiguous = _CYAML_IS("y") || _CYAML_IS("yes");
      break;
    case 'Y':
      ambiguous = _CYAML_IS("Y") || _CYAML_IS("Yes") || _CYAML_IS("YES");
      break;
    case 'o':
      ambiguous = _CYAML_IS("on") || _CYAML_IS("off");
      break;
    case 'O':
      ambiguous = _CYAML_IS("On") || _CYAML_IS("ON") || _CYAML_IS("Off") ||
                  _CYAML_IS("OFF");
      break;
    case '<':
      ambiguous = _CYAML_IS("<<");
      break;
    case '=':
      /* A lone "=" is YAML 1.1's "value" key type. PyYAML resolves it to
       * tag:yaml.org,2002:value and then refuses to construct it. */
      ambiguous = len == 1;
      break;
    default:
      break;
  }
#undef _CYAML_IS
  if (ambiguous) return true;

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

  /* A first character that is a digit or a numeric sign can parse as a
   * number. A '+' only needs quotes when a digit follows, as in "+42" and
   * "+100"; the first-byte switch above already catches "+.inf", "+.Inf" and
   * "+.INF", and any other '+.' string, like "+.foo", is a valid plain
   * scalar. The indicator-character check above already catches '-'. */
  if (fc >= '0' && fc <= '9') return true;
  if (fc == '+' && (s[1] >= '0' && s[1] <= '9')) return true;
  /* strtod parses a "+.N" pattern such as "+.3", "+.5e-10" or "+.1e400" as a
   * float, and neither the digit check nor the "+.inf" check above catches
   * it. try_parse_float_scalar() reads every such text as a float, overflow
   * (+inf) and underflow (a subnormal value or 0.0) included, so a
   * CYAML_STRING with this text needs quotes whatever errno says, or it would
   * parse back as a CYAML_FLOAT. */
  if (fc == '+' && s[1] == '.') {
    char *endp = NULL;
    (void)strtod(s, &endp);
    if (endp != s && *endp == '\0') return true;
  }
  if (fc == '.') return true;

  /* Only the bytes _cyaml_nq_class marks can make a plain scalar unsafe, so
   * the loop skips every other byte with one table load; most scalars hold
   * none of them. */
  for (;; p++) {
    char c = *p;
    if (__builtin_expect(!_cyaml_nq_class[(unsigned char)c], 1)) continue;
    if (c == '\0') break;
    /* A line break needs the double-quoted style, which writes it as an
     * escape. */
    if (c == '\n' || c == '\r') return true;
    /* A tab inside a plain scalar is valid YAML 1.2, but PyYAML and libyaml
     * end a plain scalar at it and then refuse the rest of the line. */
    if (c == '\t') return true;
    /* PyYAML and libyaml treat '?' as an indicator inside a plain scalar in a
     * flow context, and '?' followed by a space as a mapping key in a block
     * context. Quoting every '?' keeps the output readable by both in every
     * context. */
    if (c == '?') return true;
    /* NEL (U+0085), LS (U+2028) and PS (U+2029) are line breaks in YAML 1.1.
     * A 1.1 reader folds or splits a plain scalar holding one raw, so the
     * string goes out double-quoted, where yb_append_yaml_dquoted() writes
     * each as its \N, \L or \P escape. */
    if ((unsigned char)c == 0xE2 && (unsigned char)p[1] == 0x80 &&
        ((unsigned char)p[2] == 0xA8 || (unsigned char)p[2] == 0xA9))
      return true;
    /* YAML 1.2 section 5.1's c-printable leaves out the C1 controls
     * U+0080..U+009F except NEL, and the noncharacters U+FFFE and U+FFFF.
     * libyaml, PyYAML and go-yaml refuse a whole document that holds one of
     * them raw in any scalar style. Such a string goes out double-quoted,
     * where yb_append_yaml_dquoted() writes each as an escape. NEL (C2 85)
     * is in the same byte range and needs quoting for the reason given
     * above. */
    if ((unsigned char)c == 0xC2 && (unsigned char)p[1] >= 0x80 &&
        (unsigned char)p[1] <= 0x9F)
      return true;
    if ((unsigned char)c == 0xEF && (unsigned char)p[1] == 0xBF &&
        ((unsigned char)p[2] == 0xBE || (unsigned char)p[2] == 0xBF))
      return true;
    /* A raw C0 control byte other than a tab has no valid unescaped form in
     * ANY scalar style (see the doc comment of is_disallowed_control_byte),
     * so writing one straight into a plain scalar would produce output no
     * conformant YAML 1.2 parser can read back, this one included, since
     * parse_common() refuses it. yb_append_yaml_dquoted() is what actually
     * escapes the byte, through its own "\xXX" fallback for c < 0x20. */
    if (is_disallowed_control_byte(c)) return true;
    /* YAML 1.2 section 7.4's c-flow-indicator holds five characters, ',', '[',
     * ']', '{' and '}', and each one ends a plain scalar in a flow context, so
     * a string holding one of them anywhere needs quotes to survive a flow
     * round trip. We apply the rule always and not only for real flow-style
     * output, in line with this function's standing rule of quoting for the
     * strictest context the string may end up in. Block-style output pays
     * one harmless, unneeded quote, since a block-context plain scalar
     * accepts all five characters with no restriction. */
    if (c == ',' || c == '[' || c == ']' || c == '{' || c == '}') return true;
    if (c == ':') {
      char nx = *(p + 1);
      if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0' ||
          nx == ',' || nx == ']' || nx == '}')
        return true;
    }
    /* A space or a tab followed by '#' starts a comment. The table stops at
     * the '#', so the test looks back; p is past s here, because a '#' at the
     * start was already refused with the other first-character indicators. */
    if (c == '#' && (*(p - 1) == ' ' || *(p - 1) == '\t')) return true;
  }
  /* The plain scalar parser drops trailing spaces and tabs. */
  if (p > s && (*(p - 1) == ' ' || *(p - 1) == '\t')) return true;
  return false;
}

/*
 * True when the dictionary key s is exactly the canonical text of an integer
 * or float key as node_to_dict_key_string() makes it, i.e. it came from a
 * plain numeric scalar or spells one exactly. The serializer writes such a
 * key without quotes so the output keeps the numeric type for every reader,
 * and parsing that output canonicalizes the plain scalar back to this same
 * key string.
 *
 * Any other spelling of a number ("010", "0x10", "+5", "3.10", "-0") isn't
 * canonical, because parsing it plain would store a different key, so
 * needs_quoting() quotes it and a parse keeps its exact text.
 */
static int _lltoa_y(char *buf, long long v);

static bool key_is_canonical_number(const char *s) {
  /* Every canonical numeric text starts with a digit, a '-' or a '.', so the
   * first-byte test keeps the cost off every other key. */
  if (!((s[0] >= '0' && s[0] <= '9') || s[0] == '-' || s[0] == '.'))
    return false;
  char canon[CYAML_FLOAT_KEY_BUF_LEN];
  long long ival;
  double dval;
  if (try_parse_int_scalar(s, &ival)) {
    _lltoa_y(canon, ival);
  } else if (try_parse_float_scalar(s, &dval)) {
    format_float_key(dval, canon);
  } else {
    return false;
  }
  return strcmp(s, canon) == 0;
}

/* Whether the serializer needs to quote the dictionary key s. A canonical
 * numeric key goes out plain, and every other key follows needs_quoting(). */
static bool key_needs_quoting(const char *s) {
  if (s && key_is_canonical_number(s)) return false;
  return needs_quoting(s);
}

/*
 * Appends n bytes of d to b, but never lets b's content grow past limit + 1
 * bytes. A limit of SIZE_MAX means no limit, and every caller that passes it
 * passes the constant, so the test folds away there. Once the content is
 * longer than limit, every later call adds nothing. The canonical key
 * builder relies on that: it stops at the first byte past
 * CYAML_MAX_CANONICAL_KEY_LEN, so no input can make it hold more.
 */
static inline __attribute__((always_inline)) void yb_put(ybuf_t *b,
                                                         const char *d,
                                                         size_t n,
                                                         size_t limit) {
  if (limit != SIZE_MAX) {
    if (b->oom || b->len > limit) return;
    size_t room = limit - b->len;
    if (n > room) n = room + 1;
  }
  yb_append(b, d, n);
}

/* True once a capped build has gone past its limit. */
static inline __attribute__((always_inline)) bool yb_over(const ybuf_t *b,
                                                          size_t limit) {
  return limit != SIZE_MAX && b->len > limit;
}

/* The lowercase hex digits for a double-quoted scalar's "\xNN" escape. */
static const char _cyaml_hex_lower[] = "0123456789abcdef";

/*
 * Nonzero for a byte that ends a run of bytes a double-quoted scalar holds as
 * they are: the bytes that always need an escape (every C0 control, '"',
 * '\\' and DEL) and the lead bytes of the multi-byte sequences that may need
 * one, 0xC2 (a C1 control or NEL), 0xE2 (LS and PS) and 0xEF (U+FFFE and
 * U+FFFF). The NUL that ends the string is a C0 byte, so scanning a run needs
 * no separate end test.
 */
static const unsigned char _cyaml_dq_stop[256] = {
    [0x00 ... 0x1F] = 1, ['"'] = 1,  ['\\'] = 1, [0x7F] = 1,
    [0xC2] = 1,          [0xE2] = 1, [0xEF] = 1};

/*
 * Writes a double-quoted YAML string with every escape it needs, never
 * letting b hold more than limit + 1 bytes (see yb_put()). Each run of bytes
 * that needs no escape goes out in one append and each escape is built from
 * a table, so a byte that needs nothing costs one table load.
 */
static inline __attribute__((always_inline)) void yb_dquoted_body(
    ybuf_t *b, const char *s, size_t limit) {
  yb_put(b, "\"", 1, limit);
  if (!s) {
    yb_put(b, "\"", 1, limit);
    return;
  }
  const unsigned char *p = (const unsigned char *)s;
  const unsigned char *run = p;
  for (;;) {
    while (!_cyaml_dq_stop[*p]) p++;
    unsigned char c = *p;
    if (c == 0xC2 || c == 0xE2 || c == 0xEF) {
      /* A lead byte that starts no escaped sequence stays part of the run. */
      bool special = c == 0xC2 ? (p[1] >= 0x80 && p[1] <= 0x9F)
                     : c == 0xE2
                         ? (p[1] == 0x80 && (p[2] == 0xA8 || p[2] == 0xA9))
                         : (p[1] == 0xBF && (p[2] == 0xBE || p[2] == 0xBF));
      if (!special) {
        p++;
        continue;
      }
    }
    if (p > run) yb_put(b, (const char *)run, (size_t)(p - run), limit);
    if (c == 0) break;
    char esc[6];
    const char *out = esc;
    size_t out_len = 2;
    esc[0] = '\\';
    switch (c) {
      case '"':
        esc[1] = '"';
        break;
      case '\\':
        esc[1] = '\\';
        break;
      case '\n':
        esc[1] = 'n';
        break;
      case '\r':
        esc[1] = 'r';
        break;
      case '\t':
        esc[1] = 't';
        break;
      case '\b':
        esc[1] = 'b';
        break;
      case '\f':
        esc[1] = 'f';
        break;
      case 0xC2:
        /* NEL, LS and PS are line breaks to a YAML 1.1 reader, which folds a
         * raw one inside a double-quoted scalar into a space. Their escapes
         * read back as the same code point under 1.1 and 1.2. A C1 control
         * other than NEL is outside c-printable, so every mainstream reader
         * refuses it raw. "\xNN" names the code point U+00NN, which a parse
         * encodes back as the same two bytes. */
        p++;
        if (*p == 0x85) {
          esc[1] = 'N';
        } else {
          esc[1] = 'x';
          esc[2] = _cyaml_hex_lower[*p >> 4];
          esc[3] = _cyaml_hex_lower[*p & 0x0F];
          out_len = 4;
        }
        break;
      case 0xE2:
        p += 2;
        esc[1] = *p == 0xA8 ? 'L' : 'P';
        break;
      case 0xEF:
        /* The noncharacters U+FFFE and U+FFFF are outside c-printable too. */
        p += 2;
        out = *p == 0xBE ? "\\uFFFE" : "\\uFFFF";
        out_len = 6;
        break;
      default:
        /* Every other C0 control byte, and DEL (0x7F), which has no valid
         * unescaped form either; see the doc comment of
         * is_disallowed_control_byte. */
        esc[1] = 'x';
        esc[2] = _cyaml_hex_lower[c >> 4];
        esc[3] = _cyaml_hex_lower[c & 0x0F];
        out_len = 4;
        break;
    }
    yb_put(b, out, out_len, limit);
    if (yb_over(b, limit)) return;
    run = ++p;
  }
  yb_put(b, "\"", 1, limit);
}

static void yb_append_yaml_dquoted(ybuf_t *b, const char *s) {
  yb_dquoted_body(b, s, SIZE_MAX);
}

static void yb_append_yaml_dquoted_capped(ybuf_t *b, const char *s,
                                          size_t limit) {
  yb_dquoted_body(b, s, limit);
}

/*
 * The longest rendered key, in bytes, that the serializer writes as an
 * implicit key ("key: value"). YAML 1.2's ns-s-implicit-yaml-key limits an
 * implicit key to 1024 Unicode characters, and PyYAML and libyaml both refuse
 * a longer one. A key whose rendered text is longer goes out in the explicit
 * "? key" form instead, which has no such limit and which the parser reads
 * back as the same entry. Counting bytes instead of characters can only pick
 * the explicit form for a key that didn't strictly need it.
 */
#define CYAML_MAX_IMPLICIT_KEY_LEN 1024

/* Writes the text of a dictionary key: plain when that reads back as the
 * same key, double-quoted otherwise. The quoted form escapes every line
 * break, so a key always stays on one line. */
static inline void yb_append_key_text(ybuf_t *b, const char *key) {
  if (key_needs_quoting(key))
    yb_append_yaml_dquoted(b, key);
  else
    yb_append_cstr(b, key);
}

/* yb_append_key_text() under yb_put()'s limit. */
static inline __attribute__((always_inline)) void yb_append_key_text_lim(
    ybuf_t *b, const char *key, size_t limit) {
  if (limit == SIZE_MAX) {
    yb_append_key_text(b, key);
  } else if (key_needs_quoting(key)) {
    yb_append_yaml_dquoted_capped(b, key, limit);
  } else {
    yb_put(b, key, strlen(key), limit);
  }
}

/* The indent helper, two spaces per level of depth. It appends from a
 * constant block of spaces, one block at a time, so a line costs one append
 * per 128 bytes of indentation instead of one per byte. */
static void yb_indent(ybuf_t *b, int depth) {
  static const char spaces[129] =
      "                                                                "
      "                                                                ";
  size_t width = depth > 0 ? (size_t)depth * 2 : 0;
  while (width > 0) {
    size_t chunk = width < 128 ? width : 128;
    yb_append(b, spaces, chunk);
    width -= chunk;
  }
}

/* Gives a double's text in YAML notation: ".nan", ".inf", or the shortest
 * decimal that round trips, written into buf. The serializer only calls it
 * inside the "C" locale scope of cyaml_serialize() or cyaml_serialize_flow(),
 * so the decimal point is always '.'.
 */
static const char *yaml_double_text(double v,
                                    char buf[CYAML_FLOAT_KEY_BUF_LEN]) {
  if (__builtin_isnan(v)) return ".nan";
  if (__builtin_isinf(v)) return v > 0 ? ".inf" : "-.inf";
  format_shortest_double(v, buf);
  /* The mantissa always has a '.'. Without one an integral value like "1"
   * would read back as an integer, and an exponent form like "1e+20" or
   * "1e-05" would read back as a string under YAML 1.1, whose float grammar
   * requires the '.'; PyYAML in its default mode and Ruby's Psych both load
   * it that way. "1.0", "1.0e+20" and "1.0e-05" read back as the same double
   * under both versions of the spec. The longest text "%.17g" writes is 24
   * bytes, so the two extra bytes always fit. */
  if (!strchr(buf, '.')) {
    char *e = strchr(buf, 'e');
    size_t len = strlen(buf);
    size_t at = e ? (size_t)(e - buf) : len;
    memmove(buf + at + 2, buf + at, len - at + 1);
    buf[at] = '.';
    buf[at + 1] = '0';
  }
  return buf;
}

static void yb_append_double(ybuf_t *b, double v) {
  char buf[CYAML_FLOAT_KEY_BUF_LEN];
  yb_append_cstr(b, yaml_double_text(v, buf));
}

/* Converts an integer to decimal without calling snprintf. */
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

static void serialize_block(ybuf_t *b, cyaml_node_t *n, int depth,
                            int max_depth);

/* Writes a scalar string in a block context: double-quoted when a parse
 * would read it as another YAML type or it holds an indicator character, and
 * as a plain scalar otherwise. */
static void serialize_block_scalar(ybuf_t *b, cyaml_node_t *n) {
  const char *s = n->value.string;
  if (needs_quoting(s)) {
    yb_append_yaml_dquoted(b, s);
  } else {
    yb_append_cstr(b, s);
  }
}

/*
 * Whether c may appear literally inside a verbatim tag. This is YAML 1.2
 * section 5.6's ns-uri-char without its "%" escape form and without '#': a
 * letter, a digit, or one of -;/?:@&=+$,_.!~*'()[]. '%' isn't here because
 * it starts an escape. '#' is valid in the production, but libyaml and
 * PyYAML refuse a verbatim tag holding one literally, and its escape "%23"
 * decodes to the same byte in every reader.
 */
static inline bool is_verbatim_tag_literal_char(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9'))
    return true;
  switch (c) {
    case '-':
    case ';':
    case '/':
    case '?':
    case ':':
    case '@':
    case '&':
    case '=':
    case '+':
    case '$':
    case ',':
    case '_':
    case '.':
    case '!':
    case '~':
    case '*':
    case '\'':
    case '(':
    case ')':
    case '[':
    case ']':
      return true;
    default:
      return false;
  }
}

/*
 * Writes a node's tag in its verbatim "!<...>" form, with every byte that
 * ns-uri-char doesn't allow literally percent-escaped, so parsing the written
 * document gives back the identical tag string and every conforming YAML
 * reader accepts the token.
 *
 * That covers the bytes that would break the token (a '>' and a line break
 * each end the verbatim token, and a '%' starts an escape), and every other
 * byte the parser refuses literally inside a tag: a space, a quote, '<',
 * '{', '}', '|', a backslash, '^', '`', every control byte, and every byte
 * of a UTF-8 sequence. The escaped form of such a byte decodes to the same
 * byte, and every core-schema tag and any ordinary URI goes out byte for
 * byte.
 *
 * The separator after the closing '>' is the caller's business: a
 * collection's tag sits on its own line, while a scalar's tag is a prefix on
 * the same line.
 */
static inline __attribute__((always_inline)) void yb_verbatim_tag_body(
    ybuf_t *b, const char *tag, size_t limit) {
  static const char hex_digits[] = "0123456789ABCDEF";
  yb_put(b, "!<", 2, limit);
  const unsigned char *p = (const unsigned char *)tag;
  for (;;) {
    /* Each run of literal bytes goes out in one append. */
    const unsigned char *run = p;
    while (*p && is_verbatim_tag_literal_char(*p)) p++;
    if (p > run) yb_put(b, (const char *)run, (size_t)(p - run), limit);
    if (!*p || yb_over(b, limit)) break;
    char esc[3] = {'%', hex_digits[*p >> 4], hex_digits[*p & 0x0F]};
    yb_put(b, esc, 3, limit);
    p++;
  }
  yb_put(b, ">", 1, limit);
}

static void yb_append_verbatim_tag(ybuf_t *b, const char *tag) {
  yb_verbatim_tag_body(b, tag, SIZE_MAX);
}

/* yb_append_verbatim_tag() under yb_put()'s limit. */
static void yb_append_verbatim_tag_capped(ybuf_t *b, const char *tag,
                                          size_t limit) {
  yb_verbatim_tag_body(b, tag, limit);
}

/*
 * What serialize_block_head() found at a node: either it wrote the whole
 * node itself, or the node is a non-empty container whose children the
 * caller then walks.
 */
typedef enum {
  _CYAML_EMIT_DONE, /* fully written. Nothing is left to walk. */
  _CYAML_EMIT_LIST, /* a list that is not empty. Open a frame for it. */
  _CYAML_EMIT_DICT  /* a dictionary that is not empty. Open a frame. */
} _cyaml_emit_head_t;

/*
 * One level of the block serializer's explicit walk stack. A list frame walks
 * n by index, and a dictionary frame walks it in insertion order through
 * `next`, which is NULL when no entry is left.
 */
typedef struct {
  cyaml_node_t *n;
  size_t idx;
  size_t count;
  const ccol_chmap_entry_ref *next;
  int depth;
} _sblock_frame_t;

#ifdef RUNNING_UNIT_TESTS
__thread size_t _cyaml_serialize_oom_visits_for_tests;
#define _CYAML_COUNT_OOM_VISIT() (_cyaml_serialize_oom_visits_for_tests++)
#else
#define _CYAML_COUNT_OOM_VISIT() ((void)0)
#endif /* RUNNING_UNIT_TESTS */

/*
 * Writes everything about n that comes before its children: the null literal
 * for a NULL pointer, a verbatim tag where the node's type doesn't already
 * imply that tag, and the whole value of a scalar or an empty container.
 * Returns which kind of container is left to walk, if one is.
 *
 * depth decides the indentation, 2 spaces per level. It's the number of
 * containers around a container n, and the number of containers around the
 * parent of a scalar n. A container with a depth of max_depth or more would
 * make the tree nest more than max_depth containers, so it goes through the
 * buffer's oom flag exactly like an allocation failure; a scalar takes no
 * level and is never refused. An oom flag that is already set does the
 * same, so a walk that has already failed writes nothing more.
 *
 * It's always inlined, because it is the body the walk below runs per node,
 * and keeping it out of line would put a call and its argument setup on
 * every node the walk touches.
 */
static inline __attribute__((always_inline)) _cyaml_emit_head_t
serialize_block_head(ybuf_t *b, cyaml_node_t *n, int depth, size_t *count_out,
                     int max_depth) {
  if (b->oom) {
    _CYAML_COUNT_OOM_VISIT();
    return _CYAML_EMIT_DONE;
  }
  if (!n) {
    yb_append_cstr(b, "~");
    return _CYAML_EMIT_DONE;
  }

  /* A custom tag (outside the core schema) always serializes in its fully
   * resolved verbatim form, because a %TAG shorthand only belongs to the
   * parse and the tree keeps no such state to expand a shorthand back from.
   * A collection's tag sits on its own line, indented like the entries that
   * follow, since this parser's '&' and '!' branches already accept a node
   * property whose content starts on a later, more indented line. A scalar's
   * tag is a prefix on the same line, right before the value. */
  if (n->tag && !tag_matches_node_type(n)) {
    if (n->type == CYAML_LIST || n->type == CYAML_DICTIONARY) {
      yb_indent(b, depth);
      yb_append_verbatim_tag(b, n->tag);
      yb_append_c(b, '\n');
    } else {
      yb_append_verbatim_tag(b, n->tag);
      yb_append_c(b, ' ');
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
    case CYAML_LIST:
      if (depth >= max_depth) {
        _CYAML_COUNT_OOM_VISIT();
        b->oom = true;
        break;
      }
      *count_out = cvector_elem_count(n->value.list);
      if (*count_out == 0) {
        yb_indent(b, depth);
        yb_append_cstr(b, "[]\n");
        break;
      }
      return _CYAML_EMIT_LIST;
    case CYAML_DICTIONARY:
      if (depth >= max_depth) {
        _CYAML_COUNT_OOM_VISIT();
        b->oom = true;
        break;
      }
      *count_out = chmap_elem_count(n->value.dictionary);
      if (*count_out == 0) {
        yb_indent(b, depth);
        yb_append_cstr(b, "{}\n");
        break;
      }
      return _CYAML_EMIT_DICT;
  }
  return _CYAML_EMIT_DONE;
}

/* Starts the walk of the children of the non-empty container n at depth.
 *
 * It's always inlined, because it runs once per container the walk enters
 * and does little more than fill in a struct the caller already holds. */
static inline __attribute__((always_inline)) void sblock_frame_open(
    _sblock_frame_t *f, cyaml_node_t *n, int depth, _cyaml_emit_head_t kind,
    size_t count) {
  f->n = n;
  f->depth = depth;
  f->idx = 0;
  f->count = count;
  f->next = (kind == _CYAML_EMIT_DICT) ? dict_first_entry(n) : NULL;
}

/*
 * Writes block-style YAML for the subtree rooted at n, with depth deciding
 * the indentation, 2 spaces per level. Each mapping child and each list child
 * goes on its own indented line, and a NULL node pointer is written as the
 * YAML null literal '~'. A subtree nesting more than max_depth containers
 * sets the buffer's oom flag.
 *
 * An explicit stack of container frames drives the walk instead of
 * recursion, so the native call stack stays at O(1) depth for a tree of any
 * depth, and CYAML_MAX_NESTING_DEPTH bounds the work and not the stack. Only
 * a container opens a frame; serialize_block_head() writes every other child
 * completely on the spot.
 *
 * The whole walk stops as soon as anything sets the buffer's oom flag,
 * because carrying on would write nothing: every yb_append_* call is a no-op
 * by then and the whole buffer gets thrown away, yet the walk would still pay
 * for every remaining branch, which is exactly the cost
 * CYAML_MAX_NESTING_DEPTH bounds against a tree of an extreme shape.
 */
static void serialize_block(ybuf_t *b, cyaml_node_t *n, int depth,
                            int max_depth) {
  size_t count = 0;
  _cyaml_emit_head_t kind =
      serialize_block_head(b, n, depth, &count, max_depth);
  if (kind == _CYAML_EMIT_DONE) return;

  _sblock_frame_t inline_frames[_CYAML_WALK_INLINE_FRAMES];
  _sblock_frame_t *stack = inline_frames;
  size_t cap = _CYAML_WALK_INLINE_FRAMES;
  size_t sp = 1;
  sblock_frame_open(&stack[0], n, depth, kind, count);

  while (sp > 0 && !b->oom) {
    _sblock_frame_t *f = &stack[sp - 1];
    cyaml_node_t *child;

    if (f->n->type == CYAML_LIST) {
      if (f->idx >= f->count) {
        sp--;
        continue;
      }
      child = *(cyaml_node_t **)cvector_at(f->n->value.list, f->idx);
      f->idx++;
      yb_indent(b, f->depth);
      /* A dictionary or list child goes on the next line, so we write only
       * "-" with no space after it, and no line ends up holding whitespace
       * alone. */
      if (child &&
          (child->type == CYAML_DICTIONARY || child->type == CYAML_LIST)) {
        yb_append_cstr(b, "-\n");
        kind = serialize_block_head(b, child, f->depth + 1, &count, max_depth);
      } else {
        yb_append_cstr(b, "- ");
        (void)serialize_block_head(b, child, f->depth, &count, max_depth);
        yb_append_c(b, '\n');
        continue;
      }
    } else {
      if (!f->next) {
        sp--;
        continue;
      }
      /* key addresses the storage the dictionary keeps for the entry, and a
       * serialization never changes the tree, so key stays valid. */
      const cmap_pair *kp, *vp;
      f->next = ccol_chmap_entry_read(f->next, &kp, &vp);
      const char *key = (const char *)kp->ptr;
      child = _cyaml_read_child(vp->ptr);

      yb_indent(b, f->depth);
      size_t key_start = b->len;
      yb_append_key_text(b, key);
      if (!b->oom && b->len - key_start > CYAML_MAX_IMPLICIT_KEY_LEN) {
        /* Too long for an implicit key, so write it again in the explicit
         * "? key" form, with the ':' on its own line at the same indent. */
        b->len = key_start;
        yb_append_cstr(b, "? ");
        yb_append_key_text(b, key);
        yb_append_c(b, '\n');
        yb_indent(b, f->depth);
      }
      yb_append_c(b, ':');

      if (child &&
          (child->type == CYAML_DICTIONARY || child->type == CYAML_LIST)) {
        yb_append_c(b, '\n');
        kind = serialize_block_head(b, child, f->depth + 1, &count, max_depth);
      } else {
        yb_append_c(b, ' ');
        (void)serialize_block_head(b, child, f->depth, &count, max_depth);
        yb_append_c(b, '\n');
        continue;
      }
    }

    if (kind != _CYAML_EMIT_DONE) {
      int child_depth = f->depth + 1;
      /* Don't use f after this point, since growing the stack can move it. */
      if (sp == cap) {
        _sblock_frame_t *grown = walk_stack_grow(stack, &cap, inline_frames,
                                                 sizeof(*stack), b->m_procs);
        if (!grown) {
          b->oom = true;
          break;
        }
        stack = grown;
      }
      sblock_frame_open(&stack[sp], child, child_depth, kind, count);
      sp++;
    }
  }

  if (stack != inline_frames) _ccol_mem_free(b->m_procs, stack);
}

/* Serializes node to human-readable block YAML. The returned string is on
 * the heap and always ends with a newline, and the caller must free it with
 * cyaml_serialize_free() or cyaml_serialize_free_mp(). Returns NULL on an
 * OOM. */
char *cyaml_serialize(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  ybuf_t b;
  yb_init(&b, mp);
  /* needs_quoting() and yb_append_double() convert numbers with strtod() and
   * "%g", which follow LC_NUMERIC, so one "C" locale scope around the whole
   * walk keeps the output in the '.' form the core schema reads. */
  ccol_c_locale_scope_t scope = ccol_c_locale_enter();
  serialize_block(&b, n, 0, CYAML_MAX_NESTING_DEPTH);
  ccol_c_locale_leave(scope);
  /* Make sure the output ends with a newline. */
  if (b.len > 0 && !yb_ends_with_char(&b, '\n')) yb_append_c(&b, '\n');
  if (b.oom) {
    _ccol_mem_free(b.m_procs, b.buf);
    return NULL;
  }
  return b.buf;
}

/* Writes one document of a stream: the "---" marker on its own line, then
 * the block serialization of root. A marker on a line of its own is valid in
 * front of every kind of root, a block collection included, and a root tag
 * then sits on the line after it, exactly as cyaml_serialize() writes it.
 * max_depth is the largest number of nested containers root may hold: an
 * element of a list of documents sits one container below that list, so it
 * gets one level less, exactly as when parsing a multi-document stream and
 * as in cyaml_serialize() of the same list. */
static void serialize_stream_document(ybuf_t *b, cyaml_node_t *root,
                                      int max_depth) {
  yb_append_cstr(b, "---\n");
  size_t start = b->len;
  serialize_block(b, root, 0, max_depth);
  if (b->len > start && !yb_ends_with_char(b, '\n')) yb_append_c(b, '\n');
}

char *cyaml_serialize_stream(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  ybuf_t b;
  yb_init(&b, mp);
  /* Same "C" locale scope as in cyaml_serialize(). */
  ccol_c_locale_scope_t scope = ccol_c_locale_enter();
  if (n && n->type == CYAML_LIST) {
    size_t count = cyaml_list_len(node);
    for (size_t i = 0; i < count && !b.oom; i++)
      serialize_stream_document(&b, (cyaml_node_t *)cyaml_list_get(node, i),
                                CYAML_MAX_NESTING_DEPTH - 1);
  } else {
    serialize_stream_document(&b, n, CYAML_MAX_NESTING_DEPTH);
  }
  ccol_c_locale_leave(scope);
  if (b.oom) {
    _ccol_mem_free(b.m_procs, b.buf);
    return NULL;
  }
  return b.buf;
}

/* ========================================================================== */
/*                         FLOW SERIALIZER                                    */
/* ========================================================================== */

/* One key and value pair taken out of a dictionary's chmap so canonical mode
 * can sort it (see serialize_flow's "canonical" parameter and the doc comment
 * of serialize_flow_canonical below). key is borrowed: it points into the
 * storage the dictionary keeps for each entry and stays valid as long as
 * nothing changes the dictionary itself. */
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
 * One level of the flow serializer's explicit walk stack. A list frame walks
 * n by index, while a dictionary frame walks it either through `entries`, a
 * sorted copy canonical mode builds, or in insertion order through `next`,
 * which is NULL when no entry is left.
 */
typedef struct {
  cyaml_node_t *n;
  _flow_dict_entry_t *entries;
  const ccol_chmap_entry_ref *next;
  size_t idx;
  size_t count;
  int depth;
} _sflow_frame_t;

/*
 * Writes everything about n that comes before its children, flow style: the
 * null literal for a NULL pointer, a verbatim tag where the node's type
 * doesn't already imply that tag, the whole value of a scalar, and the
 * opening bracket or brace of a container, where an empty container gets
 * both brackets at once. Returns which kind of container is left to walk, if
 * one is.
 *
 * A flow context has no indentation question, so a custom tag is always a
 * plain same-line prefix, for a scalar and a collection alike, and a
 * core-schema tag that doesn't match the node's type is treated the same
 * way. depth is the number of containers around n. A container whose depth
 * is CYAML_MAX_NESTING_DEPTH or more goes through the buffer's oom flag just
 * like an allocation failure, while a scalar takes no level and is never
 * refused. An oom flag that is already set does the same, so a walk that has
 * already failed writes nothing more.
 *
 * Every append goes through yb_put() under limit; see serialize_flow().
 *
 * It's always inlined, for the reason the doc comment of
 * serialize_block_head gives.
 */
static inline __attribute__((always_inline)) _cyaml_emit_head_t
serialize_flow_head(ybuf_t *b, cyaml_node_t *n, int depth, size_t *count_out,
                    size_t limit) {
  if (b->oom) {
    _CYAML_COUNT_OOM_VISIT();
    return _CYAML_EMIT_DONE;
  }
  if (!n) {
    yb_put(b, "~", 1, limit);
    return _CYAML_EMIT_DONE;
  }
  if (n->tag && !tag_matches_node_type(n)) {
    if (limit == SIZE_MAX)
      yb_append_verbatim_tag(b, n->tag);
    else
      yb_append_verbatim_tag_capped(b, n->tag, limit);
    yb_put(b, " ", 1, limit);
  }
  switch (n->type) {
    case CYAML_NULL:
      yb_put(b, "~", 1, limit);
      break;
    case CYAML_BOOL:
      if (n->value.boolean)
        yb_put(b, "true", 4, limit);
      else
        yb_put(b, "false", 5, limit);
      break;
    case CYAML_INTEGER: {
      char buf[24];
      int len = _lltoa_y(buf, n->value.integer);
      yb_put(b, buf, (size_t)len, limit);
      break;
    }
    case CYAML_FLOAT: {
      char buf[CYAML_FLOAT_KEY_BUF_LEN];
      const char *text = yaml_double_text(n->value.number, buf);
      yb_put(b, text, strlen(text), limit);
      break;
    }
    case CYAML_STRING:
      if (!needs_quoting(n->value.string))
        yb_put(b, n->value.string, strlen(n->value.string), limit);
      else if (limit == SIZE_MAX)
        yb_append_yaml_dquoted(b, n->value.string);
      else
        yb_append_yaml_dquoted_capped(b, n->value.string, limit);
      break;
    case CYAML_LIST:
      if (depth >= CYAML_MAX_NESTING_DEPTH) {
        _CYAML_COUNT_OOM_VISIT();
        b->oom = true;
        break;
      }
      *count_out = cvector_elem_count(n->value.list);
      if (*count_out == 0) {
        yb_put(b, "[]", 2, limit);
        break;
      }
      yb_put(b, "[", 1, limit);
      return _CYAML_EMIT_LIST;
    case CYAML_DICTIONARY:
      if (depth >= CYAML_MAX_NESTING_DEPTH) {
        _CYAML_COUNT_OOM_VISIT();
        b->oom = true;
        break;
      }
      *count_out = chmap_elem_count(n->value.dictionary);
      if (*count_out == 0) {
        yb_put(b, "{}", 2, limit);
        break;
      }
      yb_put(b, "{", 1, limit);
      return _CYAML_EMIT_DICT;
  }
  return _CYAML_EMIT_DONE;
}

/* Releases the copy of the entries canonical mode built for a flow frame,
 * if it has one. */
static void sflow_frame_close(ybuf_t *b, _sflow_frame_t *f) {
  f->next = NULL;
  if (f->entries) {
    _ccol_mem_free(b->m_procs, f->entries);
    f->entries = NULL;
  }
}

/*
 * Starts the walk of the children of the non-empty container n at depth.
 * Returns false, and sets the buffer's oom flag too, when a canonical
 * dictionary's entry array can't be built.
 *
 * It's always inlined, for the reason the doc comment of sblock_frame_open
 * gives.
 */
static inline __attribute__((always_inline)) bool sflow_frame_open(
    ybuf_t *b, _sflow_frame_t *f, cyaml_node_t *n, int depth, bool canonical,
    _cyaml_emit_head_t kind, size_t count) {
  f->n = n;
  f->depth = depth;
  f->idx = 0;
  f->count = count;
  f->next = NULL;
  f->entries = NULL;
  if (kind == _CYAML_EMIT_LIST) return true;
  if (!canonical || f->count <= 1) {
    /* Either the ordinary non-canonical public path, which writes members in
     * insertion order, or a canonical-mode dictionary with 0 or 1 entries,
     * where order doesn't matter at all. */
    f->next = dict_first_entry(n);
    return true;
  }

  /* Collect into a plain array first: canonical mode exists so the result
   * doesn't depend on insert order, so we need to hold every entry before
   * deciding any order. qsort then sorts them lexicographically by key, as
   * this codebase does elsewhere for a small array sort: clogger.c orders its
   * rotated file names with the identical qsort-with-strcmp shape. */
  _flow_dict_entry_t *entries =
      _ccol_mem_calloc(b->m_procs, f->count, sizeof(*entries));
  if (!entries) {
    b->oom = true;
    return false;
  }
  size_t idx = 0;
  for (const ccol_chmap_entry_ref *e = dict_first_entry(n); e; idx++) {
    const cmap_pair *kp, *vp;
    e = ccol_chmap_entry_read(e, &kp, &vp);
    entries[idx].key = (const char *)kp->ptr;
    entries[idx].val = _cyaml_read_child(vp->ptr);
  }
  qsort(entries, f->count, sizeof(*entries), _flow_dict_entry_cmp);
  f->entries = entries;
  return true;
}

/* Writes one flow dictionary key followed by its ": " separator. A key
 * whose text is longer than CYAML_MAX_IMPLICIT_KEY_LEN goes out as an
 * explicit "? key: value" flow mapping entry. It's always inlined, since it
 * runs once per dictionary entry the walk writes. */
static inline __attribute__((always_inline)) void sflow_emit_key(
    ybuf_t *b, const char *key, size_t limit) {
  size_t key_start = b->len;
  yb_append_key_text_lim(b, key, limit);
  if (!b->oom && b->len - key_start > CYAML_MAX_IMPLICIT_KEY_LEN) {
    b->len = key_start;
    yb_put(b, "? ", 2, limit);
    yb_append_key_text_lim(b, key, limit);
  }
  yb_put(b, ": ", 2, limit);
}

/*
 * Writes compact one-line flow YAML for the subtree rooted at n, with '[' and
 * ']' around a list, '{' and '}' around a dictionary, '~' for a null, and
 * every string quoted by the same needs_quoting() rules as the block output,
 * so the result is always a valid YAML value a parser can read.
 *
 * canonical only decides the order of dictionary entries, because a list's
 * order already means something and is never changed here. false is the
 * public cyaml_serialize_flow() path, which writes each dictionary's members
 * in insertion order. true is the path of serialize_flow_canonical (see its
 * doc comment), which writes every dictionary's entries sorted
 * lexicographically by key at every nesting level, so the result is a pure
 * function of the content and doesn't depend on insert order.
 *
 * An explicit stack of container frames drives the walk instead of
 * recursion, so the native call stack stays at O(1) depth for a tree of any
 * depth, and CYAML_MAX_NESTING_DEPTH bounds the work and not the stack.
 * Only a container opens a frame, and
 * serialize_flow_head() writes every other child completely on the spot. The
 * whole walk stops as soon as anything sets the buffer's oom flag, for the
 * reason the doc comment of serialize_block gives.
 *
 * limit bounds b's content at limit + 1 bytes (see yb_put()), and the walk
 * ends as soon as the content is longer than limit. SIZE_MAX means no limit.
 * It's always inlined into its two callers, each of which passes a constant,
 * so the public path carries none of the tests.
 */
static inline __attribute__((always_inline)) void serialize_flow(
    ybuf_t *b, cyaml_node_t *n, bool canonical, int depth, size_t limit) {
  size_t count = 0;
  _cyaml_emit_head_t kind = serialize_flow_head(b, n, depth, &count, limit);
  if (kind == _CYAML_EMIT_DONE) return;

  _sflow_frame_t inline_frames[_CYAML_WALK_INLINE_FRAMES];
  _sflow_frame_t *stack = inline_frames;
  size_t cap = _CYAML_WALK_INLINE_FRAMES;
  size_t sp =
      sflow_frame_open(b, &stack[0], n, depth, canonical, kind, count) ? 1 : 0;

  while (sp > 0 && !b->oom && !yb_over(b, limit)) {
    _sflow_frame_t *f = &stack[sp - 1];
    cyaml_node_t *child;

    if (f->n->type == CYAML_LIST) {
      if (f->idx >= f->count) {
        yb_put(b, "]", 1, limit);
        sflow_frame_close(b, f);
        sp--;
        continue;
      }
      if (f->idx > 0) yb_put(b, ", ", 2, limit);
      child = *(cyaml_node_t **)cvector_at(f->n->value.list, f->idx);
      f->idx++;
    } else if (f->entries) {
      if (f->idx >= f->count) {
        yb_put(b, "}", 1, limit);
        sflow_frame_close(b, f);
        sp--;
        continue;
      }
      if (f->idx > 0) yb_put(b, ", ", 2, limit);
      sflow_emit_key(b, f->entries[f->idx].key, limit);
      child = f->entries[f->idx].val;
      f->idx++;
    } else {
      if (!f->next) {
        yb_put(b, "}", 1, limit);
        sflow_frame_close(b, f);
        sp--;
        continue;
      }
      if (f->idx > 0) yb_put(b, ", ", 2, limit);
      /* key addresses the storage the dictionary keeps for the entry, and a
       * serialization never changes the tree, so key stays valid. */
      const cmap_pair *kp, *vp;
      f->next = ccol_chmap_entry_read(f->next, &kp, &vp);
      child = _cyaml_read_child(vp->ptr);
      f->idx++;
      sflow_emit_key(b, (const char *)kp->ptr, limit);
    }

    int child_depth = f->depth + 1;
    kind = serialize_flow_head(b, child, child_depth, &count, limit);
    if (kind != _CYAML_EMIT_DONE) {
      /* Don't use f after this point, since growing the stack can move it. */
      if (sp == cap) {
        _sflow_frame_t *grown = walk_stack_grow(stack, &cap, inline_frames,
                                                sizeof(*stack), b->m_procs);
        if (!grown) {
          b->oom = true;
          break;
        }
        stack = grown;
      }
      if (!sflow_frame_open(b, &stack[sp], child, child_depth, canonical, kind,
                            count))
        break;
      sp++;
    }
  }

  for (size_t i = 0; i < sp; i++) sflow_frame_close(b, &stack[i]);
  if (stack != inline_frames) _ccol_mem_free(b->m_procs, stack);
}

/* Serializes node to compact one-line flow YAML. Returns a heap string that
 * the caller has to free with cyaml_serialize_free() or
 * cyaml_serialize_free_mp(). Returns NULL on an OOM. */
char *cyaml_serialize_flow(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  ybuf_t b;
  yb_init(&b, mp);
  /* Same "C" locale scope as in cyaml_serialize(). */
  ccol_c_locale_scope_t scope = ccol_c_locale_enter();
  serialize_flow(&b, n, false, 0, SIZE_MAX);
  ccol_c_locale_leave(scope);
  if (b.oom) {
    _ccol_mem_free(b.m_procs, b.buf);
    return NULL;
  }
  return b.buf;
}

/* The internal counterpart of cyaml_serialize_flow(). Only
 * node_to_dict_key_string() uses it, to build the canonical form of a
 * non-scalar value used as a dictionary key.
 *
 * It differs from the public one in one way: it writes the entries of every
 * dictionary it meets, at any nesting level inside n, sorted
 * lexicographically by key instead of in each dictionary's insertion order.
 * So two non-scalar keys with the same structure always produce the
 * identical string, whatever order built them, and correctly collide as the
 * same dictionary key.
 *
 * The library never exposes it; every other caller gets the documented,
 * unsorted output order of cyaml_serialize_flow().
 *
 * The walk stops at the first byte past CYAML_MAX_CANONICAL_KEY_LEN, so the
 * text it builds never holds more than that limit plus one byte whatever the
 * size of n, and a key made of many aliases of one long scalar is refused
 * after one limit's worth of work instead of after the whole rendering.
 * Returns NULL and sets *too_long when the canonical form is longer than the
 * limit, and returns NULL with *too_long false on an OOM, in line with
 * cyaml_serialize_flow()'s contract. */
#ifdef RUNNING_UNIT_TESTS
/* The largest buffer, in bytes, serialize_flow_canonical() has allocated on
 * this thread. A test resets it to 0 before the parse it checks. */
__thread size_t _cyaml_canonical_key_peak_bytes_for_tests;
#endif
static char *serialize_flow_canonical(cyaml_node_t *n, bool *too_long) {
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  ybuf_t b;
  yb_init(&b, mp);
  serialize_flow(&b, n, true, 0, CYAML_MAX_CANONICAL_KEY_LEN);
#ifdef RUNNING_UNIT_TESTS
  if (b.cap > _cyaml_canonical_key_peak_bytes_for_tests)
    _cyaml_canonical_key_peak_bytes_for_tests = b.cap;
#endif
  *too_long = !b.oom && b.len > CYAML_MAX_CANONICAL_KEY_LEN;
  if (b.oom || *too_long) {
    _ccol_mem_free(b.m_procs, b.buf);
    return NULL;
  }
  return b.buf;
}

/* Releases a string returned by cyaml_serialize or cyaml_serialize_flow
 * through the matching allocator. Pass mp == NULL for the default allocator.
 * s may be NULL, which _ccol_mem_free() treats as a safe no-op. A failed
 * parse's *err_str is not such a string: the library owns it, and it must
 * never get here. */
void cyaml_serialize_free_mp(char *s, ccol_memmgmt_procs_t *mp) {
  _ccol_mem_free(mp, s);
}

/* ========================================================================== */
/*                         PATH NAVIGATION                                    */
/* ========================================================================== */

/*
 * The escape sequences of a path, used by every navigate and set helper
 * below.
 *
 *   \.   -> a literal '.' in the key, and not a path separator
 *   \\   -> a literal '\' in the key
 *
 * A '\' before any other character is left as it is and passed through. Each
 * component's escapes are resolved after the path has been split at the
 * separator.
 */

/* A pointer to the first unescaped '.' in s, or NULL when there is none. */
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

/* A pointer to the last unescaped '.' in s, or NULL when there is none. */
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

/* Resolves the escape sequences in s in place. The string gets shorter or
 * keeps its length, but never gets longer. */
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
 * Strictly parses the digit string of a "#N" path component (digits points
 * just past the '#'), so it matches exactly this module's documented list
 * index grammar, "a component beginning with '#' followed by digits": "#",
 * "#  3", "#+3" and "#3x" are malformed, not index 3.
 *
 * A well-formed index too large for a size_t saturates at SIZE_MAX, which no
 * list reaches, so every well-formed out-of-range index is absent
 * (ccol_key_not_found at the caller) on every target width, whatever the
 * width of long.
 *
 * Returns false, leaving idx_out alone, for an empty or malformed digit
 * string, and true with *idx_out set otherwise.
 */
static bool parse_list_index_component(const char *digits, size_t *idx_out) {
  if (digits[0] == '\0') return false;
  size_t idx = 0;
  for (const char *c = digits; *c; c++) {
    if (*c < '0' || *c > '9') return false;
    size_t d = (size_t)(*c - '0');
    idx = idx > (SIZE_MAX - d) / 10 ? SIZE_MAX : idx * 10 + d;
  }
  *idx_out = idx;
  return true;
}

/* Why navigate_y() didn't reach the end of a path; it only means something
 * when that function returns NULL. It lets a caller that returns a
 * ccol_retval_t (_cyaml_set_typed and _cyaml_delete) tell two classes of
 * failure apart: an error in the path the caller built, which is
 * ccol_invalid_args and covers a parent of the wrong type, a malformed "#N"
 * index syntax, an empty path component and a component that isn't
 * well-formed UTF-8; and a well-formed path component that is absent, which
 * is ccol_key_not_found. The split holds at ANY position in the path, not
 * only at the leaf. The UTF-8 test only runs on a component that doesn't
 * resolve and on the components after it, so a path that resolves pays
 * nothing for it, since a key the tree holds is always well-formed.
 *
 * CYAML_NAV_EMPTY_COMPONENT matches the empty-component check that
 * _cyaml_set_typed() and _cyaml_delete() already apply to their own leaf
 * component. A path with nothing between two dots, or with a leading dot,
 * has no valid reading, and the library never takes it as addressing an
 * empty-string dictionary key, so the same input is refused the same way
 * whether the empty component is in the middle or is the leaf. */
typedef enum {
  CYAML_NAV_NOT_FOUND = 0,
  CYAML_NAV_WRONG_TYPE,
  CYAML_NAV_MALFORMED_INDEX,
  CYAML_NAV_EMPTY_COMPONENT,
  CYAML_NAV_BAD_UTF8,
} cyaml_nav_fail_t;

/*
 * Walks a dot-separated path through a YAML tree and returns the node at its
 * end, or NULL when any component isn't there.
 *
 * path_copy must be writable, because each unescaped dot gets overwritten
 * with a '\0' to carve out each component in place, and then that
 * component's escapes are resolved before it's used.
 *
 * A '#' prefix addresses a list element, so "items.#0.name" reaches the
 * 'name' key of the first element of 'items'.
 *
 * fail_reason_out, when not NULL, gets why the walk stopped early, and it is
 * only written when we return NULL. A caller with no way to report a
 * distinct error code, such as _cyaml_get, doesn't use it.
 */
static cyaml navigate_y(cyaml root, char *path_copy,
                        cyaml_nav_fail_t *fail_reason_out) {
  cyaml node = root;
  char *p = path_copy;
  cyaml_nav_fail_t fail_reason = CYAML_NAV_NOT_FOUND;
  bool more = false;

  while (node) {
    char *dot = path_find_unescaped_dot(p);
    more = dot != NULL;
    if (dot) *dot = '\0';

    if (p[0] == '\0') {
      fail_reason = CYAML_NAV_EMPTY_COMPONENT;
      node = NULL;
      break;
    }

    path_unescape_component(p);

    cyaml_node_t *n = (cyaml_node_t *)node;

    if (n->type == CYAML_LIST && p[0] == '#') {
      size_t idx;
      if (!parse_list_index_component(p + 1, &idx)) {
        fail_reason = CYAML_NAV_MALFORMED_INDEX;
        node = NULL;
        break;
      }
      void *slot = cvector_at(n->value.list, idx);
      node = slot ? *(cyaml *)slot : NULL;
      if (!node) fail_reason = CYAML_NAV_NOT_FOUND;
    } else if (n->type == CYAML_DICTIONARY) {
      cmap_pair kp = {.ptr = p, .size = strlen(p) + 1};
      cmap_pair vp;
      if (chmap_get_elem_ref(n->value.dictionary, &kp, &vp) != ccol_success) {
        fail_reason = ccol_utf8_is_valid(p, kp.size - 1) ? CYAML_NAV_NOT_FOUND
                                                         : CYAML_NAV_BAD_UTF8;
        node = NULL;
      } else {
        node = _cyaml_read_child(vp.ptr);
      }
    } else {
      fail_reason = CYAML_NAV_WRONG_TYPE;
      node = NULL;
    }

    if (!dot) break;
    p = dot + 1;
  }
  /* An absent component ends the walk, but an empty or non-UTF-8 component
   * later in the path still makes the whole path malformed, and that's the
   * answer the caller has to give (ccol_invalid_args rather than
   * ccol_key_not_found), so we scan the rest of the path for one. An escape
   * only adds or removes ASCII bytes, so a component's raw text is UTF-8
   * exactly when its unescaped form is. */
  if (!node && more && fail_reason == CYAML_NAV_NOT_FOUND) {
    for (;;) {
      char *dot = path_find_unescaped_dot(p);
      if (p[0] == '\0' || dot == p) {
        fail_reason = CYAML_NAV_EMPTY_COMPONENT;
        break;
      }
      if (!ccol_utf8_is_valid(p, dot ? (size_t)(dot - p) : strlen(p))) {
        fail_reason = CYAML_NAV_BAD_UTF8;
        break;
      }
      if (!dot) break;
      p = dot + 1;
    }
  }
  if (!node && fail_reason_out) *fail_reason_out = fail_reason;
  return node;
}

/* Maps a navigate_y() failure reason to the ccol_retval_t that a caller
 * (_cyaml_set_typed or _cyaml_delete, both of which return a ccol_retval_t)
 * reports for a parent path it couldn't resolve: an error in the path the
 * caller built (a parent of the wrong type, a malformed "#N" syntax, an empty
 * component, or a component that isn't well-formed UTF-8) gives
 * ccol_invalid_args, and a well-formed absent component gives
 * ccol_key_not_found. The leaf isn't part of the walk, so with an absent
 * parent a leaf that isn't well-formed UTF-8 also answers
 * ccol_invalid_args. */
static ccol_retval_t nav_fail_to_retval(cyaml_nav_fail_t reason,
                                        const char *leaf) {
  if (reason == CYAML_NAV_NOT_FOUND && !ccol_utf8_is_valid(leaf, strlen(leaf)))
    return ccol_invalid_args;
  switch (reason) {
    case CYAML_NAV_WRONG_TYPE:
    case CYAML_NAV_MALFORMED_INDEX:
    case CYAML_NAV_EMPTY_COMPONENT:
    case CYAML_NAV_BAD_UTF8:
      return ccol_invalid_args;
    case CYAML_NAV_NOT_FOUND:
    default:
      return ccol_key_not_found;
  }
}

/* The implementation behind the public cyaml_get(root, path) macro. It
 * copies path before handing it to navigate_y(), so the caller's string is
 * never changed. */
/* A path shorter than CYAML_GET_STACK_PATH is copied into a stack buffer, so
 * a lookup allocates nothing and can't fail for lack of memory; only a
 * longer path is copied to the heap. */
#define CYAML_GET_STACK_PATH 256
cyaml _cyaml_get(cyaml root, const char *path) {
  if (!root) return NULL;
  if (!path || path[0] == '\0') return root;
  ccol_memmgmt_procs_t *mp = ((cyaml_node_t *)root)->m_procs;
  char stack_copy[CYAML_GET_STACK_PATH];
  size_t len = strlen(path);
  char *copy = stack_copy;
  if (len < sizeof(stack_copy)) {
    memcpy(stack_copy, path, len + 1);
  } else {
    copy = ccol_strdup(mp, path);
    if (!copy) return NULL;
  }
  cyaml result = navigate_y(root, copy, NULL);
  if (copy != stack_copy) _ccol_mem_free(mp, copy);
  return result;
}

/*
 * The implementation behind the public cyaml_set(root, path, value) macro.
 *
 * The path is split at its LAST dot to separate the parent path from the
 * leaf key, and the leaf is copied with strdup before the copy of the parent
 * path is freed; in the other order the leaf string is a suffix of the full
 * path and the code would read freed memory.
 *
 * For CYAML_STRING, raw always names a pointer to the string: the
 * cyaml_set() macro copies its argument into a local of the decayed,
 * unqualified type, so a string literal and a char array both arrive as a
 * pointer to their first character.
 *
 * node_reinit_scalar changes a scalar node the leaf already holds in place,
 * while here a new key is allocated and inserted. No intermediate container
 * is ever created automatically, so the parent node must be there
 * already.
 */
ccol_retval_t _cyaml_set_typed(cyaml root, const char *path,
                               cyaml_node_type_t type, void *raw,
                               size_t raw_size, bool is_signed) {
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
    _ccol_mem_free(mp, copy);
  } else {
    *last_dot = '\0';
    /* An empty leaf makes the path malformed whether or not its parent
     * exists, so it's answered before the walk. */
    if (last_dot[1] == '\0') {
      _ccol_mem_free(mp, copy);
      return ccol_invalid_args;
    }
    cyaml_nav_fail_t fail_reason = CYAML_NAV_NOT_FOUND;
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    parent = navigate_y(root, copy, &fail_reason);
    _ccol_mem_free(mp, copy);
    if (!leaf_copy) return ccol_not_enough_memory;
    if (!parent) {
      ccol_retval_t nav_rv = nav_fail_to_retval(fail_reason, leaf_copy);
      _ccol_mem_free(mp, leaf_copy);
      return nav_rv;
    }
  }

  if (!leaf_copy) return ccol_not_enough_memory;
  path_unescape_component(leaf_copy);
  const char *leaf_comp = leaf_copy;
  if (leaf_comp[0] == '\0') {
    _ccol_mem_free(mp, leaf_copy);
    return ccol_invalid_args;
  }

  cyaml_node_t *pn = (cyaml_node_t *)parent;
  ccol_retval_t ret;

  if (pn->type == CYAML_DICTIONARY) {
    cmap_pair kp = {.ptr = (void *)leaf_comp, .size = strlen(leaf_comp) + 1};
    /* Refuse a bad key or a bad payload before the map changes at all, so a
     * refusal never needs to undo an insert. A key must be well-formed UTF-8;
     * see cyaml_api_string_ok(). */
    ret = ccol_utf8_is_valid(leaf_comp, kp.size - 1)
              ? scalar_payload_check(type, raw, raw_size)
              : ccol_invalid_args;
    if (ret == ccol_success) {
      /* One hash and one probe either find the leaf or insert the key with a
       * NULL placeholder child. An existing leaf is updated in place, so a
       * borrowed handle to it and a custom tag on it both survive. For a new
       * key, the only thing that runs between the insert and writing the real
       * child is building that child, which reads no map of this tree. No
       * parse is running here, so the byte budget is unarmed and charges
       * nothing for the new entry. */
      cyaml_node_t *placeholder = NULL;
      cmap_pair vp = {.ptr = &placeholder, .size = sizeof(placeholder)};
      cmap_pair slot;
      ccol_retval_t r =
          ccol_chmap_insert_or_get_elem(pn->value.dictionary, &kp, &vp, &slot);
      if (r == ccol_key_already_present) {
        cyaml_node_t *existing = _cyaml_read_child(slot.ptr);
        ret = node_reinit_scalar_checked(existing, type, raw, raw_size,
                                         is_signed);
      } else if (r == ccol_success) {
        cyaml_node_t *new_node = NULL;
        ret = node_make_scalar(type, raw, raw_size, is_signed, pn->m_procs,
                               &new_node);
        if (ret == ccol_success) {
          memcpy(slot.ptr, &new_node, sizeof(new_node));
          /* The dictionary owns the node from here on; see the guard of
           * cyaml_dictionary_set(). */
          new_node->attached = true;
        } else {
          /* Building the child ran out of memory, so remove the placeholder and
           * the map never keeps a NULL child. Deleting a key the map holds
           * always succeeds: a bucket array shrink that can't get memory is
           * skipped and the entry is removed anyway. */
          chmap_delete_elem(pn->value.dictionary, &kp);
        }
      } else {
        ret = r;
      }
    }
  } else if (pn->type == CYAML_LIST) {
    if (leaf_comp[0] != '#') {
      ret = ccol_invalid_args;
    } else {
      size_t idx;
      if (!parse_list_index_component(leaf_comp + 1, &idx)) {
        ret = ccol_invalid_args;
      } else {
        void *slot = cvector_at(pn->value.list, idx);
        if (!slot) {
          /* A syntactically valid index that simply isn't there is an absent
           * path component, just like a missing dictionary key, and not an
           * error in the path. The CYAML_DICTIONARY branch above creates the
           * leaf instead of reporting an error, but this branch has no such
           * counterpart, because an out-of-range list slot can't sensibly be
           * "created": nothing says what should fill the gap before it.
           * _cyaml_delete uses the same reasoning and the same return value
           * for the same situation. */
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

  _ccol_mem_free(mp, leaf_copy);
  return ret;
}

/*
 * The implementation behind the public cyaml_delete(root, path) macro.
 *
 * The path is split at its LAST unescaped dot to find the parent node and
 * the leaf key; with no dot, root is the parent. The leaf key's escapes are
 * resolved before it is used, so '\.' and '\\' work exactly as in cyaml_get
 * and cyaml_set.
 *
 * The addressed node is removed and deep-freed. Returns:
 *   ccol_success          - the node was removed and freed.
 *   ccol_invalid_args     - a NULL root, a NULL path or an empty path, a
 *                           node of the wrong type (not a container) before
 *                           the leaf, or a malformed "#N" index syntax. Any
 *                           path component, the leaf or one in the middle,
 *                           can give this code.
 *   ccol_key_not_found    - a well-formed path component is absent: a
 *                           dictionary key or a "#N" index at any position,
 *                           including a syntactically valid list index whose
 *                           value is out of range.
 *   ccol_not_enough_memory - a strdup call failed.
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
    _ccol_mem_free(mp, copy);
  } else {
    *last_dot = '\0';
    /* An empty leaf makes the path malformed whether or not its parent
     * exists, so it's answered before the walk. */
    if (last_dot[1] == '\0') {
      _ccol_mem_free(mp, copy);
      return ccol_invalid_args;
    }
    cyaml_nav_fail_t fail_reason = CYAML_NAV_NOT_FOUND;
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    parent = navigate_y(root, copy, &fail_reason);
    _ccol_mem_free(mp, copy);
    if (!leaf_copy) return ccol_not_enough_memory;
    if (!parent) {
      ccol_retval_t nav_rv = nav_fail_to_retval(fail_reason, leaf_copy);
      _ccol_mem_free(mp, leaf_copy);
      return nav_rv;
    }
  }

  if (!leaf_copy) return ccol_not_enough_memory;
  path_unescape_component(leaf_copy);
  const char *leaf_comp = leaf_copy;

  if (leaf_comp[0] == '\0') {
    _ccol_mem_free(mp, leaf_copy);
    return ccol_invalid_args;
  }

  cyaml_node_t *pn = (cyaml_node_t *)parent;
  ccol_retval_t ret;

  if (pn->type == CYAML_DICTIONARY) {
    /* A key that isn't well-formed UTF-8 can never be in the tree, so it's a
     * malformed path rather than an absent key. */
    ret = cyaml_dictionary_remove(parent, leaf_comp);
    if (ret == ccol_key_not_found &&
        !ccol_utf8_is_valid(leaf_comp, strlen(leaf_comp)))
      ret = ccol_invalid_args;
  } else if (pn->type == CYAML_LIST) {
    if (leaf_comp[0] != '#') {
      ret = ccol_invalid_args;
    } else {
      size_t idx;
      if (!parse_list_index_component(leaf_comp + 1, &idx)) {
        ret = ccol_invalid_args;
      } else if (idx >= cvector_elem_count(pn->value.list)) {
        /* A syntactically valid index that simply isn't there is an absent path
         * component, just like a missing dictionary key, and not an error in
         * the path. See the doc comment of this function and the public doc
         * comment of _cyaml_delete in cyaml.h. */
        ret = ccol_key_not_found;
      } else {
        ret = cyaml_list_remove(parent, idx);
      }
    }
  } else {
    ret = ccol_invalid_args;
  }

  _ccol_mem_free(mp, leaf_copy);
  return ret;
}
