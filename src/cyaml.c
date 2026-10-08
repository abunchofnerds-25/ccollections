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
#include <internal/cgrowbuf.h>
#include <internal/chashinsert.h>
#include <internal/cnumlocale.h>
#include <internal/cprocsintern.h>
#include <internal/cstrutil.h>
#include <internal/ctlsmodel.h>
#include <internal/cutf8.h>
#include <stdarg.h>
#include <stdatomic.h>

/* ========================================================================== */
/*                         INTERNAL DOM NODE                                  */
/* ========================================================================== */

/*
 * The full definition of the DOM node. This struct is also the public handle
 * type (cyaml == cyaml_node_t *), so it is not fully opaque. This is
 * different from cjson's tagged_node_t. But the caller must never read or
 * write these fields directly. The caller must use the public API.
 *
 * Layout:
 *  type     : one of the seven cyaml_node_type_t values.
 *  m_procs  : the allocator for this node and for the strings that it owns.
 *             NULL means the default allocator. Any other value is an
 *             interned copy from ccol_procs_intern(), never the pointer that
 *             the caller gave, so the caller's struct can go out of scope
 *             while the tree is still in use. Every public entry point that
 *             takes a caller's procs interns it before any node exists.
 *  tag      : an owned, fully resolved YAML tag string (for example
 *             "tag:yaml.org,2002:str", or a custom verbatim tag, or a
 *             resolved shorthand tag). It is NULL when this node has no
 *             tag. Only __cyaml_destroy frees it. node_clear never frees
 *             it. The doc comment of node_clear tells you why the two
 *             operations must stay separate.
 *  value    : a union of the scalar and the composite payloads.
 *    list : cvec of cyaml_node_t *   (CYAML_LIST)
 *    dictionary  : chmap char*->cyaml_node_t* (CYAML_DICTIONARY)
 */
/* attached: true after the library stores this node in a CYAML_LIST parent or
 * in a CYAML_DICTIONARY parent. In a tree that the public API builds, exactly
 * one parent slot can reach each node. This flag lets cyaml_list_push() and
 * cyaml_dictionary_set() refuse a child that already has a parent. Without
 * the flag, they make a second owner of the node and report nothing. Then
 * each slot destroys the node on its own when the library tears down the two
 * parents, which is a double free. The realistic way in is a borrowed
 * reference from cyaml_list_get() or cyaml_dictionary_get(). Those two
 * functions are the way that the API names a node that already has a parent.
 *
 * A new node starts with attached == false. node_alloc() zeroes the struct on
 * both of its paths: the calloc path, and the memset in the pool fast path.
 * The library never clears the flag after it sets it, because a node leaves a
 * parent only when the library destroys it. Aliases are safe, because an
 * alias resolves to a deep clone. It does not resolve to a second reference
 * to the anchored node.
 *
 * The position of the flag matches the layout of cjson_node_t, which holds
 * the same flag in the same position. On LP64 the flag uses padding that
 * already exists after `type`. The struct stays at 32 bytes, and every other
 * offset stays the same. On ILP32 there is no such padding. The node grows
 * from 20 bytes to 24 bytes, and the later fields move. No code outside this
 * file can see that, because cyaml_node_t is opaque. No public macro computes
 * a size or an offset from it. */
typedef struct cyaml_node_t {
  cyaml_node_type_t type;
  bool attached;
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
 * tag, in the "!!merge" shorthand or in its verbatim form, is a merge key
 * exactly as an untagged plain "<<" is. */
#define _CYAML_TAG_MERGE "tag:yaml.org,2002:merge"

/* ========================================================================== */
/*                         SERIALIZATION BUFFER                               */
/* ========================================================================== */

/* A dynamic string buffer for YAML serialization. It is backed by the
 * ccol_growbuf_t type of cgrowbuf.h. cjson.c's own sbuf_t uses the same
 * growable byte buffer type. The yb_* names are thin wrappers that forward
 * each call. They give this file one short, local name for each buffer
 * operation. After the library sets oom, every yb_* operation becomes a
 * safe no-op. */
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

/* Every read of the backing store of a buffer goes through one of the three
 * helpers below. Each helper answers the question "can I read the content of
 * this buffer at all" for itself. An append that fails latches the OOM flag
 * of the buffer. It also leaves the backing store in a state that no caller
 * may index. Each helper makes that decision locally. This is what keeps
 * these call sites correct on their own terms. They do not depend on the
 * implementation of the buffer. */

/* Drop the spaces and tabs at the end, down to `floor` but never past it. */
static inline void yb_trim_trailing_inline_ws(ybuf_t *b, size_t floor) {
  if (b->oom) return;
  while (b->len > floor &&
         (b->buf[b->len - 1] == ' ' || b->buf[b->len - 1] == '\t'))
    b->buf[--b->len] = '\0';
}

/* Append every byte of `src` to `dst`. A src with no readable content adds
 * nothing. The OOM flag of dst already makes the append a no-op. The caller
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
/* Drives each of the three helpers above with a buffer in the state that a
 * failed append leaves behind. In that state the OOM flag is latched, there
 * is no backing store, and the content length still holds its old value.
 * Each helper must get its answer from the flag alone and must index
 * nothing. This is what makes each helper correct on its own terms.
 * Returns one bit for each helper. The value is 7 when all three answered
 * and read nothing. Bit 1 is the trim, bit 2 is the buffer to buffer
 * append, and bit 4 is the last byte test. Remove any one of those guards,
 * and the matching call below becomes a null dereference. */
unsigned cyaml_debug_growbuf_guard_bits(void) {
  /* Read through a volatile. The shape of the buffer is then a run-time
   * value. The compiler then runs the helpers and does not fold them
   * away. */
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
 * The parser descends through an explicit frame stack and not through the
 * call stack. All of the parser therefore compiles into one very large
 * driver function, and GCC spends the inlining budget of that function
 * elsewhere. A measurement of the instruction counts over a whole parse
 * chose every helper that carries this macro. Inspection did not choose
 * them: the direction of the effect is not predictable, and some plausible
 * candidates measured worse. Measure again before you add another helper,
 * and before you remove one.
 */
#define _CYAML_PARSE_HOT static inline __attribute__((always_inline))

/*
 * The SSO storage of chmap_entry has natural alignment. This memcpy is
 * therefore defense in depth and not a live alignment need. The library
 * keeps it because cjson uses the same pattern.
 */
static inline cyaml_node_t *_cyaml_read_child(const void *src) {
  cyaml_node_t *p;
  memcpy(&p, src, sizeof(p));
  return p;
}

/*
 * The members of a dictionary, in insertion order. The dictionary map keeps
 * every live entry on one list in that order: a new key goes after every
 * other key, a replacement of the value of a key keeps its place, and a
 * delete takes it out. Parsing, cyaml_dictionary_set(), cyaml_clone(), the
 * serializers and cyaml_dictionary_first() therefore all see the members in
 * the order in which their keys first arrived. A walk allocates nothing and
 * cannot fail. ccol_chmap_entry_read() reads an entry and gives the one
 * after it.
 */
static inline const ccol_chmap_entry_ref *dict_first_entry(cyaml_node_t *n) {
  return ccol_chmap_oldest_entry(n->value.dictionary);
}

/* ========================================================================== */
/*                         THREAD-LOCAL NODE POOL                             */
/* ========================================================================== */

/*
 * A thread-local free-list pool for cyaml_node_t. It matches the pool in
 * cjson.c. node_free() puts a node that came from the default allocator
 * (m_procs == NULL) back into this pool. It does not free the node at once.
 * This spreads the cost of calloc and free over many operations. It helps a
 * workload that parses and destroys many documents.
 *
 * The pool holds at most _CYAML_POOL_CAP nodes. The library frees each node
 * above that count at once. A node from a custom allocator never goes into
 * the pool.
 *
 * The library registers a pthread destructor with _pool_key. That destructor
 * drains the pool when a thread exits. Without it, a worker thread leaks the
 * nodes in its pool for the life of the process.
 */
#define _CYAML_POOL_CAP 512U
#if defined(_CCOL_EMULATE_DARWIN_TLS)
/* The pool of one thread, and its error buffer. The pointer to this block is
 * the value of the pool key, so _pool_drain receives it as its argument and
 * reads no __thread variable; see ctlsmodel.h. A thread has a block from its
 * first pooled node or its first stored error message, and a value on the
 * key exactly while it has a block. */
typedef struct {
  cyaml_node_t *head;
  unsigned sz;
  char *err_buf;
} _cyaml_tls_t;
#else
static __thread cyaml_node_t *_pool_head = NULL;
static __thread unsigned _pool_sz = 0;
/* True once this thread has set its value on the pool key. The value only
 * has to be set once for the destructor of the key to run at thread exit, so
 * the lock below is taken once per thread and not every time the pool of the
 * thread runs empty. Taking it on every refill writes the cache line of the
 * lock once per parse on every thread, and that line can hold data that every
 * node allocation reads, such as the once flag of this subsystem. */
static __thread bool _pool_key_armed = false;
#endif /* _CCOL_EMULATE_DARWIN_TLS */
#ifdef RUNNING_UNIT_TESTS
/* How many times any thread took the pool key lock to arm the key. */
atomic_ulong _cyaml_pool_key_lock_count_for_tests;
#endif /* RUNNING_UNIT_TESTS */

/* Guards the calls to pthread_setspecific. The library sets it to true after
 * it creates the key, and to false before it deletes the key. Without this
 * guard, a background thread that still runs during a dlclose can call into
 * a deleted key. */
static atomic_bool _pool_key_live = false;
static ccol_thread_ls_key_t _pool_key;

/* Serializes two operations. The first is "use the key while it is still
 * live". That is the read side in node_free, and it takes this lock as a
 * reader. The second is "set _pool_key_live to false and delete the key",
 * which is the DSO destructor below and which is the only writer. An
 * atomic_load of _pool_key_live and then an action on it is not enough on
 * its own. The destructor can run between the load in node_free and the
 * ccol_thread_ls_set call of node_free. It then deletes the key under a
 * pthread_setspecific() that is already in flight, which POSIX makes
 * undefined. This lock covers the load and the action together, so the two
 * operations exclude each other. ccol_thread_ls_set therefore either
 * completes fully before the key deletion, or it never runs. The library
 * initializes this lock lazily inside _do_pool_key_init(). The same
 * _pool_key_once below guards that function, and it also guards every other
 * part of the one-time setup of this subsystem. This obeys the standing rule
 * of this codebase against static or constant lock initializers. */
static ccol_rw_lock_t _pool_key_rwlock;

static void _pool_drain(void *);

/*
 * The thread-local budget of node allocations that remain for the top-level
 * parse_common() call that runs now on this thread. (size_t)-1 means "no
 * parse runs on this thread now". That is the default value, and node_alloc()
 * treats it as unlimited. The doc comment of CYAML_MAX_PARSE_NODES, further
 * below near parse_ctx_t, tells you why this budget exists. Only
 * parse_node_budget_arm() and _disarm() write it. Those two functions sit
 * beside that macro. parse_common() calls _arm() once at entry, and it calls
 * _disarm() on every one of its exit paths. A value that a parse decremented
 * therefore never reaches unrelated node creation after that parse returns.
 * Such unrelated creation is a plain cyaml_create_list_mp() call, or a later,
 * separate parse on the same thread.
 */
static __thread size_t _parse_node_budget = (size_t)-1;

/* node_alloc() sets this flag when it refuses an allocation because
 * _parse_node_budget reached zero. It does not set the flag for a real
 * allocator OOM. Two groups of call sites can then report a specific,
 * useful diagnostic. The first group registers an anchor. The second group
 * clones a node to resolve an alias. This budget exists mainly to bound
 * those call sites.
 * Without the flag, they report only the general "out of memory" message
 * that the NULL return of node_alloc gives them. */
static __thread bool _parse_node_budget_exhausted = false;

/*
 * The thread-local budget of DOM bytes that remain for the top-level
 * parse_common() call that runs now on this thread. The library maintains it
 * in the same way as _parse_node_budget above. (size_t)-1 means "no parse
 * runs now", and parse_bytes_charge() treats that value as unlimited. Only
 * parse_byte_budget_arm() and _disarm() write it. The doc comment of
 * CYAML_MAX_PARSE_BYTES, further below near parse_ctx_t, tells you what this
 * budget bounds that the node count alone cannot bound.
 */
static __thread size_t _parse_byte_budget = (size_t)-1;

/* parse_bytes_charge() sets this flag when it refuses a charge because
 * _parse_byte_budget ran out. It does not set the flag for a real allocator
 * OOM. The call sites that this budget mainly exists to bound can then report
 * a specific, useful diagnostic. Without the flag, they report only the
 * general "out of memory" message that a NULL return gives them. This flag
 * has the same role for the byte count as _parse_node_budget_exhausted has
 * for the node count. */
static __thread bool _parse_byte_budget_exhausted = false;

/*
 * The approximate heap cost, in bytes, that the byte budget charges for each
 * kind of DOM allocation. Each figure is at or above the real cost of the
 * matching allocation. The figures come from a measurement of the cvec and
 * the chmap of this library on LP64. The budget therefore bounds real memory
 * and does not state too small a value. The macro _CYAML_ALLOC_OVERHEAD
 * covers the per-block header of a typical allocator and its rounding of
 * the size. The two container base figures cover the backing allocations of
 * an empty cvec and of an empty chmap. The macro _CYAML_BYTES_PER_DICT_ENTRY
 * covers one chmap entry and its share of the amortized growth of the
 * bucket array.
 *
 * The library does not charge for the element slots of a list on their own.
 * One slot holds a single pointer to a child node. That child node already
 * costs at least _CYAML_BYTES_PER_NODE. The backing array therefore can never
 * be more than a small constant fraction of the cost of the elements that it
 * indexes.
 */
#define _CYAML_ALLOC_OVERHEAD ((size_t)16)
#define _CYAML_BYTES_PER_NODE (sizeof(cyaml_node_t) + _CYAML_ALLOC_OVERHEAD)
#define _CYAML_BYTES_PER_LIST_BASE ((size_t)192)
#define _CYAML_BYTES_PER_DICT_BASE ((size_t)320)
#define _CYAML_BYTES_PER_DICT_ENTRY ((size_t)192)

/*
 * Charge bytes against the parse byte budget of this thread. Returns true,
 * and deducts the bytes, in two cases. The first case is an unarmed budget,
 * which means that no parse runs now. That is the common case for each
 * direct caller of the public API. The second case is a budget that still has
 * room. Returns false, and latches _parse_byte_budget_exhausted, when the
 * charge would take the budget past its limit. Every caller reports that
 * result in the same way as it reports an allocation failure.
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

/* Tell whether parse_bytes_charge(bytes) would refuse, without charging and
 * without latching _parse_byte_budget_exhausted. */
static inline bool parse_bytes_would_refuse(size_t bytes) {
  return _parse_byte_budget != (size_t)-1 && bytes > _parse_byte_budget;
}

/*
 * Give bytes back to the parse byte budget of this thread. The caller does
 * this after the budget accepted a charge but the allocation then failed.
 * The charge before the allocation is deliberate. The budget bounds how much
 * memory one parse may hold at one time. The library must therefore refuse a
 * request that is already past the limit before its bytes reach the
 * allocator. The refund keeps that order exact. Bytes charged for an
 * allocation that never came into existence are then still available for the
 * rest of the parse. This makes the byte budget agree with the node COUNT
 * budget. node_alloc() decrements the node count only after a node exists, so
 * the node count needs no refund of its own.
 *
 * No current path depends on this refund. Every parse treats a failed DOM
 * allocation as fatal and unwinds to parse_common(), which disarms both
 * budgets. Each top-level parse arms them again from the start. The refund is
 * a standing defence. It keeps the accounting exact from the moment that a
 * speculative path learns to recover from a failed allocation.
 */
static inline void parse_bytes_refund(size_t bytes) {
  if (_parse_byte_budget != (size_t)-1) _parse_byte_budget += bytes;
}

/*
 * ccol_strdup(), but it charges the bytes of the copy against the parse byte
 * budget first. The same budget that bounds the node structs then also bounds
 * a string that becomes part of the DOM. Such a string is the text of a
 * scalar, or the tag of a node. Returns NULL when the budget refuses the
 * charge, in the same way as for an allocation failure.
 */
_CYAML_PARSE_HOT char *strdup_charged(ccol_memmgmt_procs_t *mp, const char *s) {
  size_t n = strlen(s) + 1 + _CYAML_ALLOC_OVERHEAD;
  if (!parse_bytes_charge(n)) return NULL;
  char *copy = ccol_strdup(mp, s);
  if (!copy) parse_bytes_refund(n);
  return copy;
}

#if defined(_CCOL_EMULATE_DARWIN_TLS)
/* The block of the calling thread, or NULL when the thread has none. */
static _cyaml_tls_t *_cyaml_tls_peek(void) {
  if (!atomic_load_explicit(&_pool_key_live, memory_order_acquire)) return NULL;
  return (_cyaml_tls_t *)ccol_thread_ls_get(_pool_key);
}

/* The block of the calling thread. It makes the block, and sets it as the
 * value of the pool key, when the thread has none. It gives NULL when the key
 * does not exist or memory runs out. The caller must have run the once-guard
 * of the key. */
static _cyaml_tls_t *_cyaml_tls_get(void) {
  _cyaml_tls_t *t = _cyaml_tls_peek();
  if (t || !atomic_load(&_pool_key_live)) return t;
  t = (_cyaml_tls_t *)calloc(1, sizeof(*t));
  if (!t) return NULL;
  bool set = false;
  ccol_rw_lock_rdlock(_pool_key_rwlock);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&_cyaml_pool_key_lock_count_for_tests, 1,
                            memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
  if (atomic_load(&_pool_key_live) && ccol_thread_ls_set(_pool_key, t) == 0)
    set = true;
  ccol_rw_lock_unlock(_pool_key_rwlock);
  if (!set) {
    free(t);
    return NULL;
  }
  return t;
}
#else
/* Sets the value of the pool key for the calling thread, once per thread, so
 * that _pool_drain runs when the thread exits. The caller must have run the
 * once-guard of the key. It is always inlined, so node_free() keeps the
 * shape that it has with the code written in place. */
static inline __attribute__((always_inline)) void _pool_key_arm(void) {
  if (!_pool_key_armed && atomic_load(&_pool_key_live)) {
    ccol_rw_lock_rdlock(_pool_key_rwlock);
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add_explicit(&_cyaml_pool_key_lock_count_for_tests, 1,
                              memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
    if (atomic_load(&_pool_key_live) &&
        ccol_thread_ls_set(_pool_key, (void *)1) == 0)
      _pool_key_armed = true;
    ccol_rw_lock_unlock(_pool_key_rwlock);
  }
}

/* The per-thread message buffer of a failed parse; see cyaml_err_storage().
 * The drain at thread exit frees it with the node pool. */
static __thread char *cyaml_err_buf;
#endif /* _CCOL_EMULATE_DARWIN_TLS */

/* Lazy key init. It runs exactly once, on the first node_alloc call.
 * _pool_key_live gates pthread_setspecific in node_free, and it also gates
 * the destructor below. A process that never calls a cyaml function therefore
 * pays no cost. The flag is also the only signal of whether this one-time
 * init succeeded. It stays false, which is its static start value, when
 * either pthread call below fails. Such a failure is a real exhaustion of a
 * resource, for example a process that already reached PTHREAD_KEYS_MAX. A
 * caller must therefore never use the rwlock or the key before it checks this
 * flag. The cold path of node_free is the matching read side. */
static ccol_once_flag_t _pool_key_once = CCOL_ONCE_INIT;

static void _do_pool_key_init(void) {
  if (ccol_rw_lock_init(_pool_key_rwlock) != 0) return;
  if (ccol_thread_ls_key_create(_pool_key, _pool_drain) != 0) return;
  atomic_store(&_pool_key_live, true);
}

/* The DSO destructor. It drains the pool of the thread that calls it. It then
 * marks the key dead, so that a concurrent thread skips the
 * pthread_setspecific call. It then deletes the key. Without that deletion,
 * repeated dlopen and dlclose cycles exhaust PTHREAD_KEYS_MAX. The
 * _pool_key_live guard makes this function a no-op when no code ever called a
 * cyaml function. In that case _do_pool_key_init never ran, so the library
 * never initialized _pool_key_rwlock either. The flag change and the key
 * deletion both happen under the write lock. They therefore can never
 * interleave with the read-locked use of the key in a concurrent node_free().
 * See the doc comment of _pool_key_rwlock. */
__attribute__((destructor)) static void _pool_key_fini(void) {
  if (!atomic_load(&_pool_key_live)) return;
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  _cyaml_tls_t *t = _cyaml_tls_peek();
  if (t) {
    (void)ccol_thread_ls_set(_pool_key, NULL);
    _pool_drain(t);
  }
#else
  _pool_drain(NULL);
#endif
  ccol_rw_lock_wrlock(_pool_key_rwlock);
  atomic_store(&_pool_key_live, false);
  ccol_thread_ls_key_delete(_pool_key);
  ccol_rw_lock_unlock(_pool_key_rwlock);
}

/* Allocate a new node. When mp == NULL, this function prefers a recycled node
 * from the thread-local pool. The node that it gives back has the given type
 * tag and a zeroed value union. Returns NULL on an allocation failure. One
 * such failure is a parse that runs now on this thread and that used all of
 * its _parse_node_budget. See the doc comment of that variable. */
_CYAML_PARSE_HOT cyaml_node_t *node_alloc(cyaml_node_type_t type,
                                          ccol_memmgmt_procs_t *mp) {
  ccol_call_once(_pool_key_once, _do_pool_key_init);
  if (_parse_node_budget != (size_t)-1 && _parse_node_budget == 0) {
    _parse_node_budget_exhausted = true;
    return NULL;
  }
  /* Charge the struct of the node. Also charge the empty backing container
   * that a list node or a dictionary node allocates right after this call
   * returns. cyaml_create_list_mp() and cyaml_create_dictionary_mp() are the
   * only two callers that pass those types. The backing store of a container
   * costs several times the node struct that it hangs from. A budget that
   * counted only the struct would therefore miss most of the real memory of
   * a tree with many containers. */
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
      /* The budget accepted the charge above for a node that never came
       * into existence. Give those bytes back, so that they stay available
       * to the rest of this parse. See the doc comment of
       * parse_bytes_refund(). */
      parse_bytes_refund(node_bytes);
      return NULL;
    }
  }
  /* Charge the budget only after a node exists. The node comes either from
   * the pool or from a new allocation. A decrement before the allocation try
   * above would consume one unit of budget forever for a node that never came
   * into existence. That happens each time that the allocator itself fails
   * for a short time, for example under a test that injects a single OOM
   * fault. The budget would then state too small a number of nodes that the
   * rest of this parse may still create. The byte budget keeps the same
   * property through parse_bytes_refund(). See the doc comment of that
   * function. */
  if (_parse_node_budget != (size_t)-1) _parse_node_budget--;
  n->type = type;
  n->m_procs = mp;
  return n;
}

/* Put a node back into the thread-local pool. This function does that when
 * m_procs == NULL and the pool is not full. In every other case it frees the
 * node directly through the allocator of that node. */
_CYAML_PARSE_HOT void node_free(cyaml_node_t *n) {
  if (n->m_procs != NULL) {
    _ccol_mem_free(n->m_procs, n);
    return;
  }
  /* Every function that touches _pool_key_rwlock directly runs the once-guard
   * itself. This obeys the rule of this codebase for every pthread primitive
   * and every ccol_once_flag_t. An argument about the call graph is never a
   * substitute. One such argument is "node_alloc() allocated every node that
   * reaches this function, and node_alloc() already ran this guard". That
   * argument stops to hold, and reports nothing, the moment that a new call
   * path reaches this point. */
  ccol_call_once(_pool_key_once, _do_pool_key_init);
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
    /* The cold path. It runs when the pool of this thread is empty, and it
     * takes the lock only until this thread has armed the key once (see
     * _pool_key_armed). The doc comment of _pool_key_rwlock tells you
     * why a lock must cover the load and the action. It also tells you why
     * an atomic check alone is not enough. The outer check here holds no
     * lock. Its only job is to skip _pool_key_rwlock completely when
     * _do_pool_key_init() never completed, for example after a real
     * exhaustion of a pthread resource. In that case the library never
     * initialized the rwlock, and a lock of it is undefined behavior. This
     * outer check does not reopen the race that the inner lock closes. The
     * flag _pool_key_live goes from false to true only inside
     * _do_pool_key_init(). The ccol_call_once() above already ran that
     * function to completion, with the usual happens-before guarantee of
     * pthread_once. A true value here therefore proves that the rwlock is
     * fully initialized. The rwlock also stays valid memory for the rest of
     * the process. This is true even when a concurrent DSO unload sets the
     * flag back to false right after this check. The inner check under the
     * lock is what makes that later change to false safe to race against. */
    _pool_key_arm();
  }
  memcpy((cyaml_node_t **)n, &_pool_head, sizeof(_pool_head));
  _pool_head = n;
  _pool_sz++;
#endif /* _CCOL_EMULATE_DARWIN_TLS */
}

/* Walk the free-list of the pool and call free() on every node. The pthread
 * destructor calls this function when a thread exits. The library also calls
 * it on a DSO unload.
 *
 * The drain also clears _pool_key_armed. The C library resets the value of
 * the key to NULL before it calls this destructor, so the key is no longer
 * armed for this thread. Another thread-specific destructor can still run
 * after this one on the same thread and free nodes, for example the
 * destructor of an application key whose value holds a document. The first
 * such free then finds an empty pool, sets the key again, and the C library
 * runs one more destructor round that drains those nodes too. With the flag
 * left true, that free skips the set, and the nodes stay in a pool that no
 * destructor drains any more. */
#if defined(_CCOL_EMULATE_DARWIN_TLS)
/* Under _CCOL_EMULATE_DARWIN_TLS the drain frees the block that arg names.
 * The C library has already cleared the value of the key, so a node that a
 * destructor of another key frees later makes a new block and sets the value
 * again, and the C library calls this destructor once more for it. */
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
/* Gives the size of the node-pool free-list of the thread that calls it.
 * White-box unit tests use this to check two properties of the pool. The
 * first is how the pool evicts a node above its cap (_CYAML_POOL_CAP). The
 * second is how the pool keeps each thread separate. It is not part of the
 * public API. */
#if defined(_CCOL_EMULATE_DARWIN_TLS)
size_t cyaml_debug_pool_size(void) {
  _cyaml_tls_t *t = _cyaml_tls_peek();
  return t ? (size_t)t->sz : 0;
}
#else
size_t cyaml_debug_pool_size(void) { return (size_t)_pool_sz; }
#endif
#endif

/* The destructor callback for chmap_destroy_with_dtor(). The teardown of the
 * anchor table of each document (anchors_destroy) uses it. It destroys the
 * child node of one dictionary entry at once. Each anchor is therefore an
 * independent __cyaml_destroy() call. It is not part of the worklist walk
 * that node_clear() and __cyaml_destroy() use below. The doc comment of
 * chmap_destroy_with_dtor in chashmap.h tells you why this is the way to
 * reach every child during a teardown without an allocation. The other way is
 * to walk the dictionary with chashmap_begin_iter() first. That way needs its
 * own small allocation, which can fail under a long OOM. It then leaks every
 * child that the map already holds and that the walk can no longer reach, and
 * it reports nothing. */
static void _cyaml_destroy_dict_child(const cmap_pair *val_pair,
                                      void *dtor_ctx) {
  (void)dtor_ctx;
  cyaml_node_t *child = _cyaml_read_child(val_pair->ptr);
  __cyaml_destroy((cyaml)child);
}

/*
 * An explicit worklist on the heap. node_clear() and __cyaml_destroy() below
 * use it to tear down a whole subtree with no recursion for each level of
 * nesting. A tree that reaches either of them need not come from
 * cyaml_parse(), which CYAML_MAX_PARSE_DEPTH bounds separately. The public
 * cyaml_list_push() and cyaml_dictionary_set() API can build a tree of any
 * depth. A destroy has no "fail cleanly" contract to fall back on, because
 * both functions are void. The library cannot give the caller back a tree
 * that the caller still owns and that is half freed, for another try. This is
 * different from clone_node(), serialize_block() and serialize_flow(). To
 * bound the recursion in the way that those three do, with
 * CYAML_CLONE_MAX_DEPTH and CYAML_MAX_SERIALIZE_DEPTH, is therefore not an
 * option here. It would only trade an immediate crash for a permanent memory
 * leak of everything past the cap, with no report. An explicit worklist
 * prevents both of those failures. It keeps the native call stack at O(1)
 * depth for a tree of any depth and any width. This is also true for any
 * number of levels that it drains.
 */
typedef struct destroy_worklist {
  cyaml_node_t **items;
  size_t cap;
  size_t len;
} destroy_worklist_t;

/* Push child onto wl and grow wl when it is full. The growth uses the default
 * allocator, because the worklist is short-lived scratch state. It has no tie
 * to the m_procs of any node. This function ignores a NULL child and reports
 * nothing. Every other "destroy this child pointer" call site in this file
 * does the same.
 *
 * When the growth of the worklist fails to allocate, this function tears the
 * child down at once with an ordinary recursive __cyaml_destroy() call. It
 * does not defer the child. This can bring back stack use in proportion to
 * the depth, but only for the subtree of that one child. It also needs BOTH
 * conditions at the same time. The tree must be deep enough to matter, AND
 * the allocator must be unable to grow a small scratch array. The library
 * accepts that compound failure as a rare, graceful degradation. It does not
 * engineer around it further. That failure is much narrower than the stack
 * exhaustion that this worklist prevents, which needs depth alone and no
 * memory pressure at all. */
static void destroy_worklist_push(destroy_worklist_t *wl, cyaml_node_t *child) {
  if (!child) return;
  if (wl->len == wl->cap) {
    size_t new_cap = wl->cap == 0 ? 32 : wl->cap * 2;
    cyaml_node_t **grown =
        ccol_mem_realloc(wl->items, new_cap * sizeof(*wl->items));
    if (!grown) {
      __cyaml_destroy((cyaml)child);
      return;
    }
    wl->items = grown;
    wl->cap = new_cap;
  }
  wl->items[wl->len++] = child;
}

/* The destructor callback for chmap_destroy_with_dtor(). The CYAML_DICTIONARY
 * case of node_clear_value() uses it. It puts the child node of one
 * dictionary entry onto the worklist that dtor_ctx carries. It does not
 * recurse into that child. The same worklist loop therefore drains a
 * dictionary value and everything else. */
static void _cyaml_enqueue_dict_child(const cmap_pair *val_pair,
                                      void *dtor_ctx) {
  cyaml_node_t *child = _cyaml_read_child(val_pair->ptr);
  destroy_worklist_push((destroy_worklist_t *)dtor_ctx, child);
}

/* Deep-free the value payload of n. That payload is the string bytes, or a
 * list container, or a dictionary container. This function pushes each direct
 * child onto wl and does not recurse into it. It does not change n->type,
 * n->tag, or n itself. */
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

/* Drain wl until it is empty. This function destroys every node in wl fully,
 * and that includes the tag and the node struct. node_clear_value() above
 * puts the children of each drained node onto the worklist. The worklist
 * therefore stays fed until the whole subtree of the first contents of wl is
 * gone. This is a plain loop and not recursion. That is what keeps the stack
 * use of __cyaml_destroy() independent of the depth of the tree. */
static void destroy_worklist_drain(destroy_worklist_t *wl) {
  while (wl->len > 0) {
    cyaml_node_t *n = wl->items[--wl->len];
    node_clear_value(n, wl);
    _ccol_mem_free(n->m_procs, n->tag);
    n->tag = NULL;
    node_free(n);
  }
}

/* Deep-free the resources of the value, but do not free the node struct
 * itself. node_reinit_scalar() uses this to drop the old list value or
 * dictionary value of a node. It does that before it writes a new scalar into
 * the node in place, for example for a cyaml_set() call. The old value can be
 * a tree of any depth. See the doc comment of destroy_worklist_t above. Every
 * node below the direct children of n therefore goes through the same
 * worklist loop that __cyaml_destroy() uses, and not through recursion. */
static void node_clear(cyaml_node_t *n) {
  destroy_worklist_t wl = {NULL, 0, 0};
  node_clear_value(n, &wl);
  destroy_worklist_drain(&wl);
  ccol_mem_free(wl.items);
}

/*
 * Tell whether tag is one of the seven core-schema tag URIs of YAML 1.2.
 * When it is, *expected receives the only node type that the tag can
 * decorate. cyaml_parse() accepts a core tag on exactly that type and
 * refuses it, or converts the value, on every other type. A core tag is
 * therefore valid on a node only while the type of the node is *expected.
 * A tag that is not a core tag decorates any node, and this function
 * returns false for it.
 */
static bool core_tag_expected_type(const char *tag,
                                   cyaml_node_type_t *expected) {
  /* Every core tag starts with the same 18-byte prefix, so one strncmp()
   * rules out most custom tags before any comparison of a suffix runs. */
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

/* Drop the tag of n when it is a core-schema tag that no longer matches the
 * type of n. Every entry point that changes the type of an existing node
 * calls this after the change. A custom tag always stays. Without this, a
 * node parsed from "port: !!int 8080" and then set to the string "auto"
 * serializes as "port: !<tag:yaml.org,2002:int> auto", which cyaml_parse()
 * refuses, and a node tagged !!str and then set to 8080 comes back from a
 * round trip as the string "8080". */
static void node_drop_stale_core_tag(cyaml_node_t *n) {
  cyaml_node_type_t expected;
  if (n->tag && core_tag_expected_type(n->tag, &expected) &&
      expected != n->type) {
    _ccol_mem_free(n->m_procs, n->tag);
    n->tag = NULL;
  }
}

/*
 * True when the C string s, which a caller hands to the public API to store as
 * a scalar value, a dictionary key or a tag, is well-formed UTF-8. NULL is
 * accepted, so that a caller can check an optional string; each entry point
 * decides on its own what a NULL means.
 *
 * Every Unicode scalar value is accepted, the characters that c-printable
 * leaves out included: a C0 control, DEL, a C1 control and U+FFFE or U+FFFF
 * each have an escape in a double-quoted scalar, and the serializer writes
 * every string that holds one double-quoted with that escape (a tag writes
 * it percent-escaped), so the tree still serializes to a document that the
 * parser accepts and that reads back as the same text. Only a byte sequence
 * that is not UTF-8 names no character that any escape can represent, so it
 * is refused. The NUL character cannot occur: the string ends at it.
 */
static inline bool cyaml_api_string_ok(const char *s) {
  return !s || ccol_utf8_is_valid(s, strlen(s));
}

/*
 * Check a scalar payload that cyaml_set() or a direct _cyaml_set_typed() call
 * describes, without touching any node. It returns ccol_success when
 * node_reinit_scalar_checked() can store the payload, which then fails only
 * for lack of memory. ccol_invalid_args reports a `type` that this function
 * does not support, or a composite `type`. That includes
 * _CYAML_TYPE_UNSUPPORTED, which is the sentinel that _cyaml_type_of() in
 * cyaml.h gives for a C value of a type that cyaml_set() does not document. It
 * also includes a raw_size that matches no real integer width and no real float
 * width, a raw_size other than sizeof(bool) for CYAML_BOOL, a NULL raw for
 * any type other than CYAML_NULL, and a CYAML_STRING that is not well-formed
 * UTF-8.
 *
 * This function is where the library enforces the list of types that
 * cyaml_set() documents. Without it, a type that the library does not support
 * reaches the store as whatever the default case of _cyaml_type_of() gives.
 * The library then writes that value as a real value and reports nothing.
 * It does not refuse the call.
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

  /* Every type that this function accepts, except CYAML_NULL, carries a
   * payload that the function reads through raw. A NULL raw has nothing to
   * read, and each size check below would dereference it. Only CYAML_NULL
   * accepts raw == NULL, which is how a direct _cyaml_set_typed() call says
   * "no payload". */
  if (type != CYAML_NULL && !raw) return ccol_invalid_args;

  /* The cyaml_set() macro always gives sizeof(bool) for a C expression of
   * type bool, so only a direct call to _cyaml_set_typed() can reach a
   * mismatch. Without this check, *(bool *)raw reads past a narrower buffer,
   * or reads one garbage byte of a wider one and stores that bit pattern in a
   * _Bool, which is a trap representation unless the byte is 0 or 1. */
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

  /* For CYAML_STRING, raw names an object that is a pointer to a character.
   * For CYAML_INTEGER it names an integer object, and for CYAML_FLOAT it
   * names a floating object. This function therefore checks its width in the
   * same way before it reads anything through it. Without this check, a
   * caller of _cyaml_set_typed() that describes a narrower object reads
   * sizeof(const char *) bytes out of that object. It then gives whatever
   * follows to strdup_charged() as a string. */
  if (type == CYAML_STRING && raw_size != sizeof(const char *))
    return ccol_invalid_args;
  /* A string value must be well-formed UTF-8, so that the tree holds text
   * that the serializer can write and every YAML reader can read back. See
   * cyaml_api_string_ok(). */
  if (type == CYAML_STRING && !cyaml_api_string_ok(*(const char **)raw))
    return ccol_invalid_args;
  return ccol_success;
}

/*
 * Write a payload that scalar_payload_check() already accepted over an
 * existing scalar node. This function deep frees every resource that the old
 * content owned. It returns ccol_success, or ccol_not_enough_memory when the
 * copy of a string fails. That copy runs before node_clear(), so the failure
 * leaves the existing node completely unchanged.
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
          case 8: {
            unsigned long long u = *(unsigned long long *)raw;
            if (u > (unsigned long long)LLONG_MAX) {
              /* No long long holds it. It becomes the nearest double, the
               * same node that a parse of its decimal literal gives: that
               * literal overflows the integer grammar of the core schema
               * and resolves as a float. */
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

/* Write a new typed value over an existing scalar node. Every check runs
 * before anything touches the node, so a call that this function refuses
 * leaves the node completely unchanged. The return codes are those of
 * scalar_payload_check() and node_reinit_scalar_checked(). */
static ccol_retval_t node_reinit_scalar(cyaml_node_t *n, cyaml_node_type_t type,
                                        void *raw, size_t raw_size,
                                        bool is_signed) {
  ccol_retval_t r = scalar_payload_check(type, raw, raw_size);
  if (r != ccol_success) return r;
  return node_reinit_scalar_checked(n, type, raw, raw_size, is_signed);
}

/* Allocate a new node, with the allocator that the caller gives, from a
 * payload that scalar_payload_check() already accepted. On success, *out
 * receives the new node. The only failure is ccol_not_enough_memory, for the
 * node or for the copy of a string, and *out then receives NULL. */
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

/* Replaces *mp with its interned copy, so that no node keeps a pointer to the
 * caller's struct. Returns false when the intern table is full; *mp is then
 * NULL and the caller must fail without creating anything. */
static inline bool cyaml_intern_procs(ccol_memmgmt_procs_t **mp) {
  if (*mp == NULL) return true;
  return ccol_procs_intern(*mp, mp) == ccol_success;
}

/*
 * Factory functions for each node type. mp may be NULL, which selects the
 * default allocator. All of them return NULL on an allocation failure.
 *
 * cyaml_create_string_mp with val == NULL gives a CYAML_NULL node.
 * cyaml_create_list_mp and cyaml_create_dictionary_mp give empty containers.
 * A cvec backs the list, and a chmap backs the dictionary.
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

/* Free a node and every node below it, with a loop and not with recursion.
 * The doc comment of destroy_worklist_t above tells you why. A tree that
 * reaches this point need not come from cyaml_parse(), and a tree that the
 * public mutation API builds directly has no depth bound. This function is
 * safe to call on NULL. It does NOT set the pointer of the caller to NULL.
 * Use the cyaml_destroy() macro wrapper for that.
 *
 * This function clears and frees n itself directly. n never goes through
 * destroy_worklist_push(). The OOM fallback of that function calls
 * __cyaml_destroy() recursively on the item that it failed to add to the
 * list. n is the argument of this very call. To push n there would therefore
 * risk a call of this function on the same node during an OOM. This does not
 * affect any real node below n. The worklist still drains each of them, in
 * the same way as node_clear() uses it. */
void __cyaml_destroy(cyaml node) {
  if (!node) return;
  cyaml_node_t *n = (cyaml_node_t *)node;

  destroy_worklist_t wl = {NULL, 0, 0};
  node_clear_value(n, &wl);
  _ccol_mem_free(n->m_procs, n->tag);
  n->tag = NULL;
  node_free(n);

  destroy_worklist_drain(&wl);
  ccol_mem_free(wl.items);
}

/* ========================================================================== */
/*                         LEAF VALUE ACCESS                                  */
/* ========================================================================== */

/* Gives the type tag of the node. Gives CYAML_NULL for a NULL handle. */
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
  /* The empty string is not a tag. A serializer writes a custom tag in its
   * verbatim "!<...>" form. It would therefore write an empty tag as "!<>",
   * and parse_tag_token() refuses that as an empty verbatim tag. To accept
   * an empty tag here would build a DOM that this library cannot parse again
   * from its own serialization. That would break the round trip that the tag
   * accessors promise. Use NULL to clear a tag. */
  if (tag[0] == '\0') return ccol_invalid_args;
  /* A tag is text, so it must be well-formed UTF-8. See
   * cyaml_api_string_ok(). */
  if (!cyaml_api_string_ok(tag)) return ccol_invalid_args;
  /* A core-schema tag is valid only on the one type that it names. Any other
   * pairing serializes to a document that cyaml_parse() refuses, or that it
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
 * The typed value accessors. Each one calls ccol_fatal_err() for a wrong type
 * and for NULL. Guard each call with cyaml_type() when the type is not
 * certain at compile time.
 */
bool cyaml_bool_val(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_BOOL)
    ccol_fatal_err("cyaml_bool_val: node is %s, expected CYAML_BOOL",
                   cyaml_type_str(node));
  return n->value.boolean;
}

long long cyaml_int_val(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_INTEGER)
    ccol_fatal_err("cyaml_int_val: node is %s, expected CYAML_INTEGER",
                   cyaml_type_str(node));
  return n->value.integer;
}

double cyaml_double_val(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_FLOAT)
    ccol_fatal_err("cyaml_double_val: node is %s, expected CYAML_FLOAT",
                   cyaml_type_str(node));
  return n->value.number;
}

const char *cyaml_str_val(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_STRING)
    ccol_fatal_err("cyaml_str_val: node is %s, expected CYAML_STRING",
                   cyaml_type_str(node));
  return n->value.string;
}

/* Give the number of elements in a list, or the number of key and value pairs
 * in a dictionary. Both functions call ccol_fatal_err() when the node has the
 * wrong type.
 */
size_t cyaml_list_len(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_LIST)
    ccol_fatal_err("cyaml_list_len: node is %s, expected CYAML_LIST",
                   cyaml_type_str(node));
  return cvector_elem_count(n->value.list);
}

size_t cyaml_dictionary_size(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  if (!n || n->type != CYAML_DICTIONARY)
    ccol_fatal_err(
        "cyaml_dictionary_size: node is %s, expected CYAML_DICTIONARY",
        cyaml_type_str(node));
  return chmap_elem_count(n->value.dictionary);
}

/* ========================================================================== */
/*                          LIST / DICTIONARY MANIPULATION                    */
/* ========================================================================== */

/*
 * Append child to the list. One return code carries one ownership rule.
 * ccol_invalid_args always means that this function refused the arguments and
 * did not touch child at all. The caller therefore still owns child. Every
 * other failure, which is an OOM or a full list, takes ownership of child and
 * destroys it before the function returns. A caller can therefore act on the
 * code alone. That is the whole purpose of this division into two classes.
 * Nothing else about the call tells the two classes apart. A wrong guess
 * either leaks a subtree or frees it twice. This function matches
 * cjson_list_push, with one difference: this module does not walk the subtree
 * of child to look for seq. cyaml.h documents that the library does not check
 * for a cycle that a caller builds in that way. There is therefore no refusal
 * here for a container inside a subtree.
 */
ccol_retval_t cyaml_list_push(cyaml seq, cyaml child) {
  if (!child) return ccol_invalid_args;
  cyaml_node_t *c = (cyaml_node_t *)child;
  /* Every refusal of an argument answers ccol_invalid_args and leaves child
   * alone. This includes the refusals that can be true at the same time. A
   * caller can pass one handle as both seq and child, where that handle is
   * not a CYAML_LIST. That call is true for the self-attach test and also for
   * the kind test, and both tests answer in the same way. The self-comparison
   * is against seq and not against n. It therefore still holds when seq is
   * NULL or is the wrong kind of node. To destroy a child that is already
   * attached would free memory that its current parent still owns, and that
   * corrupts the tree of that parent. */
  if (c->attached || (const void *)c == (const void *)seq) {
    /* Another parent already owns child, or child is seq itself. To accept
     * it would give one node two owners. Each owner then frees the node when
     * the library destroys the parent of that owner. */
    return ccol_invalid_args;
  }
  if (!seq) return ccol_invalid_args;
  cyaml_node_t *n = (cyaml_node_t *)seq;
  if (n->type != CYAML_LIST) return ccol_invalid_args;
  /* cvector_push_back answers ccol_invalid_args only for a NULL element. It
   * gets &c, which is the address of a local, so it cannot answer that code
   * here. This is important, because ccol_invalid_args is the "child
   * untouched" code of this function. The branch below already destroyed
   * child. Without this property, the two classes would share one code. */
  ccol_retval_t r = cvector_push_back(n->value.list, &c);
  if (r != ccol_success) {
    __cyaml_destroy(child);
    return r;
  }
  c->attached = true;
  return r;
}

/* Gives a borrowed reference to the element at the position index. Gives NULL
 * when index is out of bounds, and when seq is not a CYAML_LIST node. */
cyaml cyaml_list_get(cyaml seq, size_t index) {
  if (!seq) return NULL;
  cyaml_node_t *n = (cyaml_node_t *)seq;
  if (n->type != CYAML_LIST) return NULL;
  void *slot = cvector_at(n->value.list, index);
  if (!slot) return NULL;
  return *(cyaml *)slot;
}

/* Replace the child that the dictionary slot slot_vp holds with child. slot_vp
 * is the accessor of a key that is already present, from
 * ccol_chmap_insert_or_get_elem() or chmap_get_elem_ref(). The function frees
 * the old child first and then writes the new pointer into the slot, so the
 * slot never names freed memory once this returns. Freeing the old child
 * changes no map that holds slot_vp, so the accessor stays valid across the
 * free. child must have no parent, and it becomes attached here. */
static void dict_slot_replace_child(const cmap_pair *slot_vp,
                                    cyaml_node_t *child) {
  cyaml_node_t *old_child = _cyaml_read_child(slot_vp->ptr);
  __cyaml_destroy((cyaml)old_child);
  memcpy((void *)slot_vp->ptr, &child, sizeof(child));
  child->attached = true;
}

/* Insert the value for key into map, or replace it. One return code carries
 * one ownership rule, in the same way as in cyaml_list_push().
 * ccol_invalid_args always means that this function refused the arguments and
 * did not touch child. The caller therefore still owns child. Every other
 * failure takes ownership of child and destroys it before the function
 * returns. A caller can pass back the exact node that the key already holds.
 * That call is a harmless no-op, and this function reports ccol_success for
 * it. For a key that is already present, this function frees the old value
 * and points the slot at child, with no failure possible between the two. The
 * slot therefore never dangles once the function returns. */
static ccol_retval_t dictionary_set_impl(cyaml map, const char *key,
                                         cyaml child) {
  if (!child) return ccol_invalid_args;
  cyaml_node_t *c = (cyaml_node_t *)child;
  /* Every refusal of an argument answers ccol_invalid_args and leaves child
   * alone. This includes the refusals that can be true at the same time. A
   * caller can pass one handle as both map and child, where that handle is
   * not a CYAML_DICTIONARY. That call is true for the self-attach test and
   * also for the kind test. The comparison is against map and not against n.
   * It therefore still holds when map is NULL or is the wrong kind of node.
   * This point decides only the self-check. The "already attached" refusal
   * must stay below the c == old_child no-op. Only an attached child can
   * reach that no-op. See the comment of that check. */
  if ((const void *)c == (const void *)map) return ccol_invalid_args;
  if (!map || !key) return ccol_invalid_args;
  cyaml_node_t *n = (cyaml_node_t *)map;
  if (n->type != CYAML_DICTIONARY) return ccol_invalid_args;

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};

  if (c->attached) {
    /* Only an attached child can be the node that key already holds, so
     * only this branch looks the key up before it decides. The caller sets
     * a key to the value that the key already holds: that is the one case
     * where this function accepts an attached child, and there is nothing to
     * do and nothing to free. To destroy "the old value" there would destroy
     * the node that the dictionary still points at correctly. Any other
     * attached child belongs to another parent: refuse the call, and touch
     * neither child nor the slot. The top of this function already refuses
     * the case child == map. */
    const cmap_pair *held_vp = NULL;
    if (chmap_get_elem_ref(n->value.dictionary, &kp, &held_vp) ==
            ccol_success &&
        _cyaml_read_child(held_vp->ptr) == c)
      return ccol_success;
    return ccol_invalid_args;
  }

  /* A new key costs a dictionary entry against the DOM byte budget of a
   * parse that runs now (see CYAML_MAX_PARSE_BYTES). A dictionary stores a
   * copy of the bytes of the key, so an entry costs more than the child node
   * that this call already charged for, and an alias expansion that copies a
   * mapping also copies every key of that mapping. The charge comes first so
   * that the insert below can be one hash and one probe. The bytes go back
   * when the key turns out to be present already. A charge that would be
   * refused is decided with a lookup first, because a key that is present
   * needs no new entry and must neither fail nor latch the exhausted budget.
   * Outside a parse the budget is unarmed, and the charge never refuses. */
  size_t entry_bytes = _CYAML_BYTES_PER_DICT_ENTRY + kp.size;
  if (parse_bytes_would_refuse(entry_bytes)) {
    const cmap_pair *held_vp = NULL;
    if (chmap_get_elem_ref(n->value.dictionary, &kp, &held_vp) ==
        ccol_success) {
      dict_slot_replace_child(held_vp, c);
      return ccol_success;
    }
  }
  if (!parse_bytes_charge(entry_bytes)) {
    /* This function reports the refusal in the same way as the insert
     * failure below, and that includes the ownership rule. */
    __cyaml_destroy(child);
    return ccol_not_enough_memory;
  }

  /* ccol_chmap_insert_or_get_elem answers ccol_invalid_args in only three
   * cases, exactly as chmap_insert_elem does. The
   * first is a NULL map. The second is a pair that is NULL or has size zero.
   * The third is a key whose size does not agree with a key type of fixed
   * width. This map belongs to the node.
   * cyaml_create_dictionary_mp() built it with a ccol_string key. That key
   * type has no fixed width, so the width check always passes. The map is
   * therefore separately chained, and the value-size check of open addressing
   * is never reached. kp describes the key that this function validated, with
   * the size strlen(key) + 1. vp describes &c. None of the three cases can
   * happen. This is important, because ccol_invalid_args is the "child
   * untouched" code of this function. The failure branch below already
   * destroyed child. Without this property, the two classes would share one
   * code. */
  cmap_pair vp = {.ptr = &c, .size = sizeof(c)};
  const cmap_pair *old_vp = NULL;
  ccol_retval_t r =
      ccol_chmap_insert_or_get_elem(n->value.dictionary, &kp, &vp, &old_vp);
  if (r == ccol_success) {
    c->attached = true;
    return ccol_success;
  }
  parse_bytes_refund(entry_bytes);
  if (r == ccol_key_already_present) {
    dict_slot_replace_child(old_vp, c);
    return ccol_success;
  }
  __cyaml_destroy((cyaml)child);
  return r;
}

/* The public entry point. It refuses a key that is not well-formed UTF-8 with
 * ccol_invalid_args and leaves child untouched, like every other refusal of
 * an argument. The parser calls dictionary_set_impl() directly: the stream
 * check of parse_common() already proved every key it builds. */
ccol_retval_t cyaml_dictionary_set(cyaml map, const char *key, cyaml child) {
  if (!cyaml_api_string_ok(key)) return ccol_invalid_args;
  return dictionary_set_impl(map, key, child);
}

/* Look for key in map and give a borrowed reference to the child of that key.
 * Gives NULL when the key is not there, when map is NULL, and when map is not
 * a CYAML_DICTIONARY node. */
cyaml cyaml_dictionary_get(cyaml map, const char *key) {
  if (!map || !key) return NULL;
  cyaml_node_t *n = (cyaml_node_t *)map;
  if (n->type != CYAML_DICTIONARY) return NULL;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  const cmap_pair *vp = NULL;
  if (chmap_get_elem_ref(n->value.dictionary, &kp, &vp) != ccol_success)
    return NULL;
  return _cyaml_read_child(vp->ptr);
}

/*
 * Remove the element at the position index from a list and deep-free it.
 *
 * This function destroys the element at index. It then moves every later
 * element left by one position. That costs O(n) in the number of elements
 * after index. It then makes the vector one element shorter with
 * cvector_pop_back.
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

/* The cursor reads the successor of a member at the moment it steps onto
 * that member. A removal of the current member frees only that member's
 * entry, so the successor that the cursor holds stays valid. */
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
 * Remove the entry with the given key from a dictionary and deep-free it.
 *
 * This function reads the child pointer out before it deletes the map entry.
 * It therefore frees the subtree only after the hash table stops to point
 * at it.
 */
ccol_retval_t cyaml_dictionary_remove(cyaml map, const char *key) {
  if (!map || !key) return ccol_invalid_args;
  cyaml_node_t *n = (cyaml_node_t *)map;
  if (n->type != CYAML_DICTIONARY) return ccol_invalid_args;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  const cmap_pair *vp = NULL;
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

/* Attach a strdup copy of src_tag onto the dst clone, which is already fully
 * built. Returns false, and destroys dst, on an OOM. It also returns false
 * when dst itself is NULL, because the base constructor already failed. A
 * NULL src_tag is a no-op that succeeds, because a node with no tag gives a
 * clone with no tag. Every branch of cyaml_clone below uses this function.
 * Without it, a branch can miss the tag for some shapes of node. cyaml_clone
 * has no single shared "finish this node" tail to hold such a step. */
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
 * The greatest nesting depth that cyaml_clone() accepts. It matches
 * CYAML_MAX_SERIALIZE_DEPTH, which this file defines later, in the PARSER
 * INTERNALS section. The reason is the same for both. A tree that a caller
 * gives to cyaml_clone() need not come from cyaml_parse(). The public
 * cyaml_list_push() and cyaml_dictionary_set() API can build it directly, to
 * any depth. CYAML_MAX_PARSE_DEPTH bounds only what one document may declare,
 * so it says nothing about such a tree. This constant is a policy limit on
 * how much work and how much worklist memory one cyaml_clone() call can do.
 * It is not a stack limit. clone_node() walks its source with an explicit
 * worklist, and it keeps the native call stack at O(1) depth for a tree of
 * any depth. For a source that nests deeper than this, the function reports
 * the failure in the documented way, with a NULL return.
 *
 * This is its own constant and does not name CYAML_MAX_SERIALIZE_DEPTH,
 * because this file defines that constant later, after clone_node(). Both
 * hold the same value 500 deliberately. A tree exactly at the
 * CYAML_MAX_PARSE_DEPTH limit of the parser can therefore still be cloned
 * or serialized. It does not fail for no good reason.
 */
#define CYAML_CLONE_MAX_DEPTH 500

/*
 * The number of frames of an explicit walk stack that its owner keeps in its
 * own locals. When a tree is deeper than this, the stack moves onto the heap.
 * walk_stack_grow below does that. The walk pushes one frame for each
 * container level. It therefore walks a document of ordinary shape and never
 * touches the allocator.
 */
#define _CYAML_WALK_INLINE_FRAMES 16

/*
 * Double the capacity of an explicit walk stack and give the new storage
 * back. stack points at the inline array of the caller while that array is
 * still big enough. The first growth copies those frames onto the heap. Every
 * later growth reallocates in place. On a failure this function gives NULL
 * back and leaves *cap exactly as it was. The caller can then still walk and
 * free everything that it already pushed. This function returns the new
 * storage and does not write it through a void **. Each caller therefore
 * assigns it to its own type of frame pointer.
 *
 * A *_MAX_DEPTH cap bounds the depth of every caller. new_cap therefore stays
 * far below the point where new_cap * elem_size could overflow.
 */
static void *walk_stack_grow(void *stack, size_t *cap, void *inline_base,
                             size_t elem_size, ccol_memmgmt_procs_t *mp) {
  size_t new_cap = *cap * 2;
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

/*
 * One level of the explicit walk stack of clone_node(). It holds a source
 * container and the clone of that container. The clone is still empty, and
 * the walk copies the children of the source into it. A list frame walks src
 * by index. A dictionary frame walks src through `it`. `it` is NULL when no
 * entry is left, and also when the walk could never reach the entries. `idx`
 * counts how many entries the walk really saw. That count tells a walk that
 * stopped early apart from a walk that completed.
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

/* Copy the node src itself and none of its children. A scalar comes back
 * complete. A list or a dictionary comes back empty, and the caller fills it.
 * This function attaches the tag, so the node that it gives back is finished
 * except for its children. Returns NULL on an allocation failure, and leaves
 * nothing behind. */
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

/* Start the walk of the container src, whose children the walk copies into
 * the empty clone dst. A dictionary is walked in insertion order, and every
 * copied entry is appended to dst, so the clone keeps the order of the
 * source. The walk allocates nothing. */
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

/* Make an independent deep copy of the subtree whose root is src. An explicit
 * stack of container frames drives the walk. Recursion does not. The native
 * call stack therefore stays at O(1) depth for a source of any depth, and
 * CYAML_CLONE_MAX_DEPTH bounds the work and not the stack.
 *
 * This function makes a shallow copy of each child and attaches it to the
 * clone of its parent. It does that before it walks the subtree of that
 * child. A failure at any point therefore leaves everything that the walk
 * already built reachable from `root`. One __cyaml_destroy() call then frees
 * all of it. clone_shallow() attaches a tag as it creates each node. The
 * order of the allocations is not visible to a caller, because a clone either
 * completes or the library destroys it whole. */
static cyaml clone_node(cyaml_node_t *src) {
  ccol_memmgmt_procs_t *mp = src->m_procs;
  cyaml root = clone_shallow(src);
  if (!root) return NULL;
  if (src->type != CYAML_LIST && src->type != CYAML_DICTIONARY) return root;

  _clone_frame_t inline_frames[_CYAML_WALK_INLINE_FRAMES];
  _clone_frame_t *stack = inline_frames;
  size_t cap = _CYAML_WALK_INLINE_FRAMES;
  clone_frame_open(&stack[0], src, (cyaml_node_t *)root);
  size_t sp = 1;
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
      /* key addresses the storage that the source dictionary keeps for the
       * entry. The clone never changes the source, so key stays valid. */
      const cmap_pair *vp;
      f->next = ccol_chmap_entry_read(f->next, &key, &vp);
      child = _cyaml_read_child(vp->ptr);
    }

    /* f->src is at depth sp - 1, so this child is at depth sp. The cap
     * applies to every child in the same way. It does not apply only to the
     * children that would open a frame of their own. */
    if (sp > (size_t)CYAML_CLONE_MAX_DEPTH) {
      failed = true;
      break;
    }

    cyaml_node_t *cc = (cyaml_node_t *)clone_shallow(child);
    if (!cc) {
      failed = true;
      break;
    }
    /* The walk stores the child into the backing container of f->dst
     * directly. It does not go through cyaml_list_push() or
     * cyaml_dictionary_set(). Every question that those two functions ask is
     * already answered here. f->dst is a container of the matching kind that
     * this call just built. cc is a new copy that no parent owns, and it
     * cannot be f->dst. A key that the walk read out of the source dictionary
     * is unique inside that dictionary, so there is never an old value to
     * free. One thing stays open, so this code keeps it: the DOM byte charge
     * of a new dictionary entry. An alias expansion clones whole mappings,
     * and CYAML_MAX_PARSE_BYTES is what bounds that. */
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
    /* The clone now owns this node. Mark it, so that the library refuses a
     * later attach of it through a borrowed reference. Without the mark, that
     * attach gives the node a second owner. */
    cc->attached = true;

    if (child->type == CYAML_LIST || child->type == CYAML_DICTIONARY) {
      /* Do not use f after this point. A growth of the stack can move it. */
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
    }
  }

  if (stack != inline_frames) _ccol_mem_free(mp, stack);
  if (failed) {
    __cyaml_destroy(root);
    return NULL;
  }
  return root;
}

/* Make an independent deep copy of the subtree whose root is node. The clone
 * takes the allocator and the tag of the source node, and every dictionary
 * of the clone keeps the member order of its source. This function destroys
 * a clone that it built only in part, and then returns NULL. It does that in
 * two cases. The first is an OOM. The second is a source that nests deeper
 * than CYAML_CLONE_MAX_DEPTH levels. */
cyaml cyaml_clone(cyaml node) {
  if (!node) return NULL;
  return clone_node((cyaml_node_t *)node);
}

/* ========================================================================== */
/*                         PARSER INTERNALS                                   */
/* ========================================================================== */

/*
 * One change that a speculative parse made to the anchor table. name is an
 * owned copy of the anchor name. old is the clone that the name held before
 * the change, or NULL when the change added the name.
 */
typedef struct {
  char *name;
  cyaml_node_t *old;
} _anchor_undo_entry_t;

/*
 * The changes that a speculative parse made to the anchor table, in order.
 * See anchor_speculation_begin(). A small inline array holds the entries of
 * a typical flow collection, so a speculation that stores few anchors
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
 * anchors: a chmap (char* -> cyaml_node_t*) that the parser creates lazily.
 * It holds a deep clone of each anchored node. It stays NULL until the parser
 * finds the first &. At the end of parse_common the library frees every
 * entry: it destroys each node and frees each key.
 *
 * tag_handles: a chmap (char* -> char*) that the parser creates lazily. It
 * maps a %TAG handle ("!", "!!", or "!name!") to its resolved prefix. Its
 * lifetime matches the lifetime of anchors exactly. It belongs to one
 * document, and the end of parse_one_document destroys it and sets it back to
 * NULL. There is one difference from anchors: the table itself starts truly
 * empty, even after the parser creates it. tag_handles_lookup falls back to
 * the two default YAML 1.2 handles itself when the table holds no explicit
 * entry. Shorthand resolution therefore always has a base case, even for a
 * document with no explicit %TAG directive. The doc comment of
 * tag_handles_ensure tells you why this matters for the detection of a
 * duplicate %TAG directive.
 *
 * indent_stack: not needed. The recursive descent passes the indentation as
 * an integer argument.
 *
 * depth: the current recursion depth of parse_node. The thin wrapper of
 * parse_node maintains it alone. See CYAML_MAX_PARSE_DEPTH below. Every other
 * function in this file that recurses back into node parsing does so only
 * through parse_node. This one counter therefore bounds the call stack use of
 * the whole parser. That holds for every YAML construct that drives the
 * recursion: nested mappings, sequences, explicit keys, anchors, tags, and
 * the others.
 *
 * line_starts, line_starts_len, line_starts_cap, line_scan_pos and
 * line_cache_disabled: these back the amortized line-boundary cache of
 * line_start_pos(). See the doc comment of that function. line_starts holds
 * the byte offset of every line start that the parser found, in increasing
 * order. line_starts[0] starts at first_line_start, which is 0 or the offset
 * past a byte-order mark. line_scan_pos is the highest ctx->pos that the
 * cache reaches. parse_common() allocates the cache once, before any parse
 * function runs. A failure there is a fatal parse failure and not a degraded
 * fallback. line_cache_disabled latches true, and stays true, only when a
 * LATER growth of the cache fails under an OOM. From that point
 * line_start_pos() falls back to a direct backward scan for each call, for
 * the rest of the parse.
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
  /* The last answer that line_start_pos() gave, and the position of that
     question. The line that a byte offset belongs to is a property of the
     document text. That text never changes during a parse. The parser can
     therefore always answer a repeat question about the same offset from
     here. That holds whatever the line cache scanned in the meantime. The
     parser asks the same question several times for each position:
     comparisons of indentation, checks for a tab, and the speculative paths
     that ask again after a rewind. That is what makes one remembered answer
     worth its two fields. memo_pos starts at SIZE_MAX, which is an offset
     that no document can reach. The first call therefore cannot hit a stale
     entry. */
  size_t memo_pos;
  size_t memo_line_start;
  /* Where the content of this stream begins: 0, or the offset past a
     byte-order mark. The first line starts here and not at offset 0. Both
     implementations of "where does the line that holds this byte begin" must
     say so. The cached one does so by construction, because line_starts[0]
     starts with this value. The backward scan needs this value as a floor.
     Without the floor, it walks past the mark and reports a line start three
     bytes early. Every column on the first line is then three too large. That
     is enough to break the block structure that the columns decide. It is not
     only a wrong report of a position. */
  size_t first_line_start;
  size_t *line_starts;
  size_t line_starts_len;
  size_t line_starts_cap;
  size_t line_scan_pos;
  bool line_cache_disabled;
  /* The undo log of the speculative parse that runs now, or NULL when none
     runs. anchors_store() records every change to the anchor table here
     while it is set, and keeps a replaced clone alive for a rollback. */
  _anchor_undo_t *anchor_undo;
} parse_ctx_t;

/*
 * The greatest recursion depth of parse_node. It bounds the stack use
 * against nesting that is very deep, and against nesting that amplifies
 * itself. No real document has this many levels of block structure, flow
 * structure or explicit keys, whatever its exact shape. A document that goes
 * past this limit is a hard parse error. The parser does not truncate it and
 * report nothing. A debug build has no optimization, so each frame uses more
 * stack. The value 500 sits well below the depth where that stack use could
 * overflow the default 8 MiB thread stack. It still accepts any real
 * document, whether a person wrote it or a program generated it.
 */
#define CYAML_MAX_PARSE_DEPTH 500

/*
 * The greatest length, in bytes, of the canonical flow-YAML text of a
 * dictionary key that is not a scalar. See the doc comment of
 * node_to_dict_key_string. The general bound of CYAML_MAX_PARSE_DEPTH does
 * not cover the reason for this limit. A key that is not a scalar can be a
 * mapping whose own single key is another such mapping, nested N levels deep.
 * Each enclosing level then puts double quotes again around the text that the
 * level below it already made. The length of the canonical string therefore
 * grows as O(2^N) in the nesting depth, and not as O(N). It is about 2^N
 * bytes at depth N. CYAML_MAX_PARSE_DEPTH alone cannot bound that. Real deep
 * nesting needs hundreds of levels, so a generous depth value must allow
 * them. Such a value is still far past the 30 or so levels where this
 * pattern already makes strings of gigabytes. A document that stays under
 * the depth cap on purpose can therefore still use all the memory. It can
 * also run for an unbounded time.
 * This length check applies to the canonical text itself and not to the
 * nesting depth. It catches exactly that pattern, and no other, at its real
 * source. It does so whatever value CYAML_MAX_PARSE_DEPTH holds. The
 * builder of the canonical text stops at the first byte past this length,
 * so a refused key costs at most this many bytes of text, whatever its
 * shape and however many aliases it expands. 64 KiB is
 * far above the canonical text of any real key that is not a scalar, in
 * ordinary use. It still stops the O(2^N) pattern at a shallow depth that is
 * cheap to refuse, because depth 20 already goes past it.
 */
#define CYAML_MAX_CANONICAL_KEY_LEN (64 * 1024)

/*
 * The greatest number of cyaml_node_t allocations that one top-level
 * parse_common() call may make. After that count, node_alloc() refuses every
 * further allocation and returns NULL, in the same way as for a real
 * allocator OOM. Neither CYAML_MAX_PARSE_DEPTH nor
 * CYAML_MAX_CANONICAL_KEY_LEN covers the reason for this limit.
 * cyaml_clone() makes a real, unshared deep copy. The parser uses it to take
 * a copy of the subtree of an anchor when it registers that anchor. It uses
 * it again to make an independent copy at every later *alias reference, and
 * once more for the expansion of a merge key. A document that nests aliases
 * of aliases can therefore make the final count of live nodes grow
 * exponentially in the number of anchor levels. The source text and the
 * recursion depth of the parser both stay small. This is the classic "billion
 * laughs" shape of entity expansion. CYAML_MAX_PARSE_DEPTH bounds the
 * recursion depth. It does not bound the fan-out of clones and siblings. This
 * limit bounds how many node structs one parse can create. It accepts any
 * real large document, whether a person wrote it or a program generated it. A
 * document would need millions of distinct scalar values to come near it.
 * This limit is a count. It therefore says nothing about how many bytes each
 * of those nodes brings with it. CYAML_MAX_PARSE_BYTES below bounds that, and
 * the library enforces the two limits together.
 */
#define CYAML_MAX_PARSE_NODES (4 * 1000 * 1000)

/*
 * The node allocations that one parse may make grow with the input: at least
 * CYAML_MAX_PARSE_NODES, and at least this many for each byte of input. The
 * factor is the next power of two at or above 1.5 times the most that any
 * document without aliases, merge keys or nested non-scalar keys allocates
 * for each byte of its text. That worst case, measured by this budget, is a
 * flow sequence of empty pairs ("[:,:,:]"): a mapping, its null key and its
 * null value for every two bytes, 1.5 nodes for each byte. The shapes left
 * out amplify their input on purpose: an anchor and an alias copy a subtree,
 * a merge key copies the members of its source, and the canonical text of a
 * nested non-scalar key doubles at each level. Every node and byte that those
 * copies make is also charged to the amplification budget (see
 * parse_amp_enter()), which keeps the fixed limit CYAML_MAX_PARSE_NODES
 * whatever the input length. A long input therefore raises the limit for the
 * nodes that its text describes, and never the limit for its expansion.
 */
#define CYAML_PARSE_NODES_PER_INPUT_BYTE 4

/*
 * The greatest total DOM memory, in bytes, that one top-level parse_common()
 * call may charge. After that, the parser refuses every further node, string,
 * tag and dictionary entry, in the same way as for a real allocator OOM. This
 * limit exists because CYAML_MAX_PARSE_NODES counts nodes, and a node is not
 * a fixed amount of memory. A scalar owns its text. A tagged node owns its
 * tag. A dictionary entry owns a copy of its key. A container node owns a
 * backing cvec or chmap that is several times the size of the node struct.
 * Alias expansion deep copies all of it. The node count alone therefore
 * bounds one kind of amplification but not amplification in BYTES. A few
 * kilobytes of input can hold one large anchored scalar. An alias of it
 * through two nested levels then reaches gigabytes of live DOM, with a node
 * count far under the limit above. The library charges the byte budget at
 * every DOM allocation site. See the callers of parse_bytes_charge(). Both
 * halves of that shape are therefore bounded.
 *
 * The value 256 MiB is the floor of the limit. The limit grows with the
 * input, at CYAML_PARSE_BYTES_PER_INPUT_BYTE bytes for each input byte, so a
 * large honest document parses whatever its size. The bytes that anchors,
 * aliases, merge keys and collection keys copy are also charged to the
 * amplification budget, which stays at 256 MiB whatever the input length, so
 * padding an input does not raise what its expansion may use. Like the node
 * limit, it applies to a parse only. It never applies to a tree that a
 * caller builds directly through the public API.
 */
#define CYAML_MAX_PARSE_BYTES ((size_t)256 * 1024 * 1024)

/*
 * The DOM bytes that one parse may charge grow with the input: at least
 * CYAML_MAX_PARSE_BYTES, and at least this many bytes for each byte of
 * input. The factor is derived as for CYAML_PARSE_NODES_PER_INPUT_BYTE: the
 * same "[:,:,:]" shape charges about 331 bytes for each input byte, the most
 * of any document without aliases, merge keys or nested non-scalar keys, and
 * 1.5 times that rounds up to 512. Ordinary documents cost far less: a large
 * Kubernetes List of generated manifests costs about 20 bytes for each input
 * byte. A fixed limit alone refuses such an honest document once it passes
 * about 13 MB. Expansion through copies meets the fixed amplification
 * budget instead, whatever the input length.
 */
#define CYAML_PARSE_BYTES_PER_INPUT_BYTE 512

/*
 * The greatest nesting depth that serialize_block() and serialize_flow()
 * accept. A tree that a caller gives to cyaml_serialize() or to
 * cyaml_serialize_flow() need not come from cyaml_parse(). The public
 * cyaml_list_push() and cyaml_dictionary_set() API can build it directly, to
 * any depth. CYAML_MAX_PARSE_DEPTH bounds only what one document may declare,
 * so it says nothing about such a tree. This constant is a policy limit on
 * how much work and how much worklist memory one serialization can do. It is
 * not a stack limit. Both serializers walk the tree with an explicit stack of
 * container frames. They keep the native call stack at O(1) depth for a tree
 * of any depth. A tree that goes past this limit gets the same report as any
 * other serialization failure: the library sets the oom flag of the ybuf_t.
 * cyaml_serialize() and cyaml_serialize_flow() already turn that flag into a
 * NULL return. Neither function has an error-string channel of its own for a
 * more specific diagnostic.
 */
#define CYAML_MAX_SERIALIZE_DEPTH 500

/* Arm and disarm both parse budgets, _parse_node_budget and
 * _parse_byte_budget, for the parse_common() call that runs now on this
 * thread. The THREAD-LOCAL NODE POOL section above declares both of them,
 * beside node_alloc. The library arms and disarms the two together, because
 * they bound two halves of one property and neither one is enough alone. A
 * disarm sets both back to "unlimited". It does not leave the value that
 * remains. A value that a parse decremented can therefore never reach
 * unrelated node creation on this thread after that parse returns. */
/* The two limits of the parse that runs now on this thread, as
 * parse_node_budget_arm() computed them. The parse diagnostics name them. */
static __thread size_t _parse_node_limit = CYAML_MAX_PARSE_NODES;
static __thread size_t _parse_byte_limit = CYAML_MAX_PARSE_BYTES;

/* Gives max(floor, per_byte * input_len), saturated below (size_t)-1, which
 * is the "unarmed" sentinel of both budgets. The saturation matters on
 * ILP32, where 512 times an input of 8 MiB or more does not fit a size_t. */
static inline size_t parse_budget_for_input(size_t floor, size_t per_byte,
                                            size_t input_len) {
  size_t cap = (size_t)-1 - 1;
  size_t scaled = input_len > cap / per_byte ? cap : input_len * per_byte;
  return scaled > floor ? scaled : floor;
}

#ifdef RUNNING_UNIT_TESTS
/* The floors of the two limits. A test lowers them, so that a document of a
 * few kilobytes can show how the limits scale with the input, and restores
 * them before it returns. The test suite declares both extern. */
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
 * The amplification budget of the parse that runs now on this thread. Every
 * node and every DOM byte that the parser makes by COPYING something it has
 * already built is charged here, in addition to the ordinary budgets above.
 * That covers the copy that registers an anchor, the copy that resolves an
 * alias, the copy that a merge key takes of each merged value, and the
 * canonical text of a key that is a collection. These are the only ways in
 * which a parse can make more DOM than its input describes, so these are the
 * only charges that must not grow with the input. The ordinary budgets scale
 * with the input length, so that a large honest document parses. The
 * amplification budget keeps the fixed floors CYAML_MAX_PARSE_NODES and
 * CYAML_MAX_PARSE_BYTES whatever the input length. Padding a nested-alias
 * document with a long comment therefore buys it nothing: its expansion meets
 * the same fixed limit as the unpadded document.
 *
 * node_alloc() and parse_bytes_charge() stay unaware of this budget, so the
 * ordinary allocation path pays nothing for it. parse_amp_enter() narrows the
 * two ordinary budgets to the smaller of what remains of each and of the
 * amplification budget. parse_amp_leave() charges what the copy consumed to
 * both. A region may nest inside another; only the outermost one narrows and
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
 * ordinary one. The
 * diagnostics of the copy sites then name the amplification limit. It stays
 * set until the next arm or disarm, because every refusal fails the parse. */
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
  /* A refund inside the region can only return what the region charged, so
   * the remaining budget never exceeds the start of the region. */
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

/* cyaml_clone() inside an amplification region. Every copy that the parser
 * makes of a node it already built goes through this function. */
static cyaml_node_t *parse_clone(cyaml_node_t *src) {
  parse_amp_enter();
  cyaml_node_t *copy = (cyaml_node_t *)cyaml_clone((cyaml)src);
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
  _parse_node_budget = (size_t)-1;
  _parse_node_budget_exhausted = false;
  _parse_byte_budget = (size_t)-1;
  _parse_byte_budget_exhausted = false;
  _parse_amp_depth = 0;
  _parse_amp_limit_hit = false;
}

/* Format a parse error into ctx->error. Only the last call stays. This
 * function writes over an earlier message and reports nothing.
 *
 * The __attribute__((format(printf, 2, 3))) below is what prevents
 * -Wformat-nonliteral here. The library does not suppress that warning. The
 * warning fires whenever a function of the printf family gets a format
 * string that is not a literal at the call site. That function is vsnprintf
 * here. The compiler can then no longer check that string against the
 * varargs that the call gives it. fmt is such a string, because it is a
 * parameter of this function and not a literal. But GCC and Clang both treat
 * the "pass-through wrapper" pattern as a special case. The enclosing
 * function carries the printf-like attribute for that same parameter. The
 * compiler therefore accepts the inner call as correct by construction. It
 * moves the real check to every call site of parse_err() itself, where fmt
 * IS a literal. This is better than a suppression of the warning. It adds a
 * real compile-time check of the format string and the argument types, under
 * -Werror, to every parse_err() call in this file. That check catches a real
 * class of bug, for example a "%d" for a size_t. A suppression catches
 * nothing. */
static void parse_err(parse_ctx_t *ctx, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void parse_err(parse_ctx_t *ctx, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
  va_end(ap);
}

/*
 * Every byte of input text that a parse error message quotes goes through
 * cyaml_err_echo(), so every message is printable ASCII whatever the input
 * holds. A raw control byte (an ESC that starts a terminal sequence, a line
 * break that forges a second log line, a NUL) would otherwise reach the
 * logs of the caller, and a byte of 0x80 or above would make the message
 * invalid UTF-8, which a JSON or syslog sink refuses or mangles. Decoded
 * scalar text and decoded tags can hold any of those, and an anchor name or
 * a single character can be part of a multi-byte sequence.
 *
 * The echo writes a byte from 0x20 to 0x7E as it is and any other byte as
 * <0xNN>. It holds at most CYAML_ERR_ECHO_BUF - 1 bytes: a longer text is
 * cut at a whole byte, never inside a <0xNN>, and ends with "...". The
 * three helper macros give the echo a buffer of its own, a compound literal
 * that lives until the end of the enclosing block, so a call site writes
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
    /* Keep room for the "..." that a later cut needs. */
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

/* at_end: true when the parser read all of the input.
 * cur:    gives the current character and does not move forward. It gives
 *         '\0' at the end of the input. */
static inline bool at_end(parse_ctx_t *ctx) { return ctx->pos >= ctx->len; }

static inline char cur(parse_ctx_t *ctx) {
  return at_end(ctx) ? '\0' : ctx->src[ctx->pos];
}

/* The fallback implementation of line_start_pos(), for the degraded mode. It
 * scans backward from ctx->pos one byte at a time. That costs O(column) for
 * each call. The cache of line_start_pos() exists to prevent that cost on
 * every call. This is a small separate helper, so that two paths share one
 * implementation. The first path is the one with the cache turned off. It
 * runs only right after an OOM during a growth of the cache, in a state that
 * is already degraded. The second path is the first ever call of the cache
 * itself. */
#ifdef RUNNING_UNIT_TESTS
/* Forces the degraded line-start path that uses no cache. A failed growth of
   line_starts latches that same path. Allocation pressure alone cannot reach
   the one class of position where the two implementations can disagree. The
   latch needs at least 64 line boundaries first. By that point no query lands
   on the first line, and the first line is the only line that a byte-order
   mark moves. The test suite declares this variable extern, and the public
   header does not. That matches how the other modules reach their own
   test-only accessors. */
bool cyaml_test_force_line_cache_disabled = false;
#endif

static size_t line_start_pos_backward_scan(parse_ctx_t *ctx) {
  size_t p = ctx->pos;
  while (p > ctx->first_line_start && ctx->src[p - 1] != '\n' &&
         ctx->src[p - 1] != '\r')
    p--;
  return p;
}

/* Extend ctx->line_starts, so that it covers every line boundary up to the
 * byte offset up_to. It does not cover up_to itself. It appends one entry for
 * each '\n' or '\r' that it crosses. That rule matches the line-boundary rule
 * of line_start_pos_backward_scan(). The caller always passes an up_to that
 * is greater than ctx->line_scan_pos. On an allocation failure this function
 * tears the cache down, and ctx->line_cache_disabled latches and stays true.
 * Its caller, line_start_pos(), then falls back to a direct scan for this
 * call and for every later call. It does not try the same doomed allocation
 * again on each future call. */
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

/* Gives the byte offset of the start of the physical line that holds
 * ctx->pos. That offset is the position right after the nearest '\n' or '\r'
 * before ctx->pos. It is 0 when no such byte comes before ctx->pos.
 * current_col() and line_indent_has_tab() share this function, because both
 * need exactly this "where did the current line begin" answer.
 * parse_node_inner() calls it once for each parsed node, and many other sites
 * call it too. This function treats a lone '\r' as a line boundary, and not
 * only '\n'. That matches skip_to_eol(), at_eol() and skip_newline()
 * elsewhere in this file. The parser never normalizes CR and LF in the input
 * before it parses. For a "\r\n" ending the scan always reaches the '\n'
 * first, because '\n' is the later of the two bytes. The line therefore
 * correctly starts right after the pair, and never right after the '\r'
 * alone.
 *
 * A simple backward scan, one byte at a time, costs O(column) for each call.
 * A flow collection drives one such call for each sibling, and nothing bounds
 * how many siblings share one line. Any other construct that keeps
 * parse_node_inner on one physical line, with no newline crossed, does the
 * same. That alone would make the total parse cost O(n^2) in the length of
 * the line. A flow list of a few tens of thousands of elements on one line
 * then takes tens of seconds. None of CYAML_MAX_PARSE_DEPTH,
 * CYAML_MAX_PARSE_NODES and CYAML_MAX_CANONICAL_KEY_LEN bounds that, because
 * none of them bounds the fan-out of siblings on one line.
 *
 * An amortized cache, ctx->line_starts, is what keeps that cost linear. It is
 * a sorted list of every line-start offset that the parser found. The list
 * only grows. ctx->pos moves forward almost always. The parser rarely goes
 * back, and only by a small, bounded distance, to try the token right before
 * it again. try_parse_scalar_dict_key() is one example. The common case
 * therefore only extends the cache forward, past bytes that no scan ever saw.
 * The total scan work for the whole parse is then O(n), and not O(n) for each
 * call.
 *
 * The last entry answers a query that advanced past everything the cache saw,
 * because the extend above just made that entry the right one. A speculative
 * parse can rewind to an earlier position. A query from such a position needs
 * a binary search of the recorded line starts. That search looks for the
 * greatest recorded start that is <= ctx->pos. The search is correct
 * whether ctx->pos is ahead of or behind the highest point that
 * the parse reached. A scheme that remembers only the single most recent line
 * start is not. This one also stays correct across a backward jump into a
 * region that the parser already scanned, and it scans nothing again.
 *
 * parse_common() itself makes the first allocation of the cache, once, before
 * any parse function runs. A failure there is a fatal parse failure that the
 * library reports at once. Every other early setup step of this parser does
 * the same. This is a real, load-bearing allocation, and this function has no
 * way around it. Its failure must therefore be honest. A graceful fallback
 * deep inside a hot, per-node call path with no error channel of its own must
 * not hide it. Only a LATER growth failure falls back to a direct scan for
 * each call, for the rest of the parse. That failure comes from
 * line_starts_extend(), after a large document uses all of the small first
 * capacity of the cache. The parse reaches it one step at a time, deep into a
 * parse that is otherwise successful, under memory pressure that lasts. There
 * a fallback that is slower but correct is a better trade than the loss of
 * all the completed work. The first allocation is different: it is all or
 * nothing, and the parse has not started at that point. */
static size_t line_start_pos(parse_ctx_t *ctx) {
  if (ctx->pos == ctx->memo_pos) {
#ifdef RUNNING_UNIT_TESTS
    /* The memo rests on one property: the line that a byte belongs to is a
       property of the document text, and that text does not change during a
       parse. That property is the whole correctness of the memo, and no call
       site shows it. Wherever tests run, this check compares the remembered
       answer against a new computation. Without this check, a wrong memo gets
       past every other check in the file. The fast-path assertion below only
       compares two readings of the same array, and a memo hit never reaches
       it. */
    ccol_assert(ctx->memo_line_start == line_start_pos_uncached(ctx));
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
    /* The extend stops exactly at ctx->pos. It appends one entry for each
       line boundary that it crosses. Every entry is therefore now at or below
       ctx->pos. The last entry is therefore the greatest one that can be
       there. It is the answer, and no search is needed. Nearly every call
       takes this path, because a parse moves forward through the document far
       more often than it rewinds. */
    size_t answer = ctx->line_starts[ctx->line_starts_len - 1];
#ifdef RUNNING_UNIT_TESTS
    /* The invariant above is load-bearing and not obvious. This check
       compares it against the general search wherever tests run. Without the
       check, a future change that lets an entry past ctx->pos into the cache
       returns a line start from further down the document and reports
       nothing. That looks like a wrong indentation level, far from its
       cause. */
    {
      size_t vlo = 0, vhi = ctx->line_starts_len;
      while (vlo + 1 < vhi) {
        size_t vmid = vlo + (vhi - vlo) / 2;
        if (ctx->line_starts[vmid] <= ctx->pos)
          vlo = vmid;
        else
          vhi = vmid;
      }
      ccol_assert(answer == ctx->line_starts[vlo]);
    }
#endif
    return answer;
  }

  /* ctx->pos is at or behind the point that the cache already scanned. A
     speculative parse leaves that state behind when it rewinds. The cache
     then holds entries past ctx->pos, and this path must exclude them. This
     path therefore searches. */
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

/* Gives the column of ctx->pos on its line, counted from 0. That column is
 * the distance from the start of the line, as line_start_pos() gives it. The
 * parser uses it to find the indentation levels of block collections. */
_CYAML_PARSE_HOT int current_col(parse_ctx_t *ctx) {
  if (ctx->pos == 0) return 0;
  size_t p = line_start_pos(ctx);
  size_t col = ctx->pos - p;
  /* Every caller uses this value only to compare indentation levels. It
   * compares against another int, or against a small explicit indent
   * indicator. No caller uses it as a byte count. This code therefore clamps
   * the value at INT_MAX. Without the clamp, a line longer than INT_MAX bytes
   * wraps into an int whose value the implementation defines, and that value
   * can be negative. The clamp keeps every one of those comparisons well
   * defined, for a physical line of any length. */
  return col > (size_t)INT_MAX ? INT_MAX : (int)col;
}

/* True when a tab character is at any position between the start of the
 * current line and ctx->pos. YAML 1.2 section 6.1 makes indentation spaces
 * only. A tab may separate a flow node from what comes before it on its line,
 * but the column of a block collection cannot be measured across a tab. The
 * start of every block sequence entry and every block mapping entry therefore
 * refuses a tab anywhere before it on its line: "\ta: 1", "-\t- a" and
 * "key:\tb: c" all fail.
 *
 * This test is deliberately narrower than "does skip_ws_comments or
 * skip_inline_ws see a tab". Those two shared functions also handle
 * whitespace that is INTERNAL to content that is already open, for example
 * the line folding inside a quoted scalar. A tab there is real data and not
 * indentation. To join the two cases makes a real infinite loop, because
 * those callers need the shared skip to always move forward. The parser
 * instead calls this helper explicitly, and only at the points that really
 * are a block structural decision about indentation. */
static bool line_indent_has_tab(parse_ctx_t *ctx) {
  size_t p = line_start_pos(ctx);
  return memchr(ctx->src + p, '\t', ctx->pos - p) != NULL;
}

/* The indentation of the current line, for a comparison against the indent
 * of an enclosing block construct, when ctx->pos sits at the first
 * character after the leading whitespace of its line. YAML 1.2 section 6.1
 * makes indentation spaces only: s-indent(n) is n spaces, and a tab after
 * it is separation whitespace (s-separate-in-line), not indentation. When
 * the leading whitespace holds a tab, this gives the number of spaces before
 * the first tab. In every other case it gives current_col(), which is also
 * the answer when ctx->pos is not at the start of the content of its line.
 *
 * Only a flow node (a scalar, a flow collection or an alias) may follow a
 * tab. A block collection may not: its column is ambiguous, and the start of
 * every block collection refuses a tab before it with line_indent_has_tab()
 * or block_collection_line_has_tab(). */
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

/* Skip only horizontal whitespace, which is spaces and tabs. Do not cross a
 * newline. The parse of a block is sensitive to indentation, and the column
 * of the next token has meaning there. This function is needed for that.
 *
 * It deliberately still skips a tab in every case. It also handles whitespace
 * that is internal to content which is already open, for example the
 * blank-line fold logic of parse_double_quoted. A tab there is real data and
 * not block structural indentation. A version of this function that always
 * stops at a tab makes a real infinite loop. The fold loop of
 * parse_double_quoted calls it and needs it to always move forward. A tab
 * that this function does not read breaks that invariant. The parser instead
 * refuses a tab at each call site that is a real block structural position.
 * It does that with current_col() and line_indent_has_tab(). See the doc
 * comment of that helper. It does not do it inside this shared, lower level
 * function. */
static void skip_inline_ws(parse_ctx_t *ctx) {
  while (!at_end(ctx)) {
    char c = cur(ctx);
    if (c == ' ' || c == '\t')
      ctx->pos++;
    else
      break;
  }
}

/* Drop the rest of the current line, and the newline at its end with it. A
 * lone '\r', a lone '\n', and a "\r\n" pair all end the line. That matches
 * at_eol() and skip_newline() elsewhere in this file. The parser never
 * normalizes CR and LF in the input before it parses. This function must
 * therefore also treat a bare CR as a line ending, and not only an LF. */
static void skip_to_eol(parse_ctx_t *ctx) {
  while (!at_end(ctx) && cur(ctx) != '\n' && cur(ctx) != '\r') ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '\r') ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '\n') ctx->pos++;
}

static inline bool at_eol(parse_ctx_t *ctx) {
  return at_end(ctx) || cur(ctx) == '\n' || cur(ctx) == '\r';
}

/* True when the byte span [start, ctx->pos) holds a line break. That break is
 * a '\n', a bare '\r', or a "\r\n" pair. This parser never normalizes CR and
 * LF in its input. It must therefore treat a bare CR as a line break here,
 * exactly as skip_to_eol(), at_eol() and skip_newline() above already do. A
 * check for '\n' alone misses a document that uses only bare CR line endings
 * inside the span, and it reports nothing. Every decision of the form "did
 * this token cover more than one physical line" uses this function. Those
 * decisions are the single-line rule for an implicit key, and the
 * indentation and tab check for a continuation line of a flow collection.
 *
 * This function reads no byte of the span. line_start_pos() already answers
 * the question. The start of the line that holds ctx->pos is after `start`
 * exactly when a line break falls inside the span. That is so because that
 * line start is by definition the byte after the last break before ctx->pos.
 * Every caller passes a `start` that it saved from an earlier ctx->pos.
 * `start` is therefore never below ctx->first_line_start and never above
 * ctx->pos. ctx->first_line_start is the floor that both implementations of
 * "where does this line begin" share, past any byte-order mark. The
 * assertions below check both bounds, and the equivalence itself. The whole
 * correctness of this function rests on a property of line_start_pos() that
 * no call site shows. The equivalence must hold on the paths that move
 * forward. It must hold just as much on the speculative paths. Those paths
 * rewind ctx->pos into a region that the parser already scanned. */
static bool span_crosses_newline(parse_ctx_t *ctx, size_t start) {
#ifdef RUNNING_UNIT_TESTS
  ccol_assert(start >= ctx->first_line_start && start <= ctx->pos);
  ccol_assert((line_start_pos(ctx) > start) ==
              (memchr(ctx->src + start, '\n', ctx->pos - start) != NULL ||
               memchr(ctx->src + start, '\r', ctx->pos - start) != NULL));
#endif
  return line_start_pos(ctx) > start;
}

/*
 * True when only inline whitespace remains before the next newline or the end
 * of the input. A '#' comment at the end may also remain. This means that the
 * line holds no more real content. It stays true when cur(ctx) is not yet
 * '\n' or the end of the input, for example when cur(ctx) == '#'. This
 * function does not change ctx->pos.
 *
 * The parser uses it for a decision of one form. That form is "does this
 * part of the entry start on THIS line or on a later one". The part is the
 * value, the anchor or the tag. A comment between the indicator and the
 * newline must not fool such a decision. For example, in
 * "a: # comment\n  b: c\n" the value really starts
 * on the next line. This is true although the character right after "a: " is
 * '#' and not '\n'.
 */
static bool rest_of_line_is_blank(parse_ctx_t *ctx) {
  /* The start of the stream is ctx->first_line_start, as in
   * skip_ws_comments(). No current caller asks at that position, because
   * each one runs after an indicator or a token; the test keeps the two
   * comment rules one rule. */
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

/* Skip any mix of whitespace, newlines included, and '#' comments. The parser
 * uses this between top-level structural tokens, where the indentation has no
 * meaning. One such place is between the elements of a flow collection.
 *
 * The YAML spec says that a '#' starts a comment only after whitespace, or at
 * the start of the input. A '#' that touches the token before it is not a
 * comment at all. "]#comment" and "\"value\"#comment" are two examples. This
 * function must leave such a '#' in place, so that the caller can refuse it
 * as content that must not be there. This function must not read it and
 * report nothing. */
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
 * The same as skip_ws_comments, but for content inside a flow collection. The
 * s-separate(n,c) grammar of YAML 1.2 covers the flow-in and flow-out
 * contexts. Inside it, s-separate-lines(n) needs every line that the skip
 * crosses to have an indent strictly greater than `indent`. `indent` is the
 * indent of the enclosing block value. It is the same parameter that
 * parse_node and parse_block_scalar_content pass along, with -1 as the
 * sentinel for the document root, which has no enclosing block context.
 *
 * This function checks only the final line that it lands on. It does not
 * check every blank line that it crosses on the way. That is enough for every
 * case that this parser must tell apart. A reference parser confirms this.
 * "k: {\nk\n:\nv\n}" is refused, because its continuation lines sit at column
 * 0, which is no more indented than the column 0 of "k:" itself. The same
 * content with one more space of indent throughout is accepted.
 *
 * A line that starts with ']' or '}' exactly at column `indent` is the one
 * exception. It closes a flow collection under the key or the "- " that
 * opened it, which libyaml, and therefore PyYAML, Ansible and go-yaml,
 * accept.
 *
 * Returns false when the check fails, and sets ctx->error. Returns true in
 * every other case. That includes the case where it crossed no newline at
 * all, which matches the unconditional success of plain skip_ws_comments.
 */
_CYAML_PARSE_HOT bool flow_skip_ws(parse_ctx_t *ctx, int indent) {
  size_t start = ctx->pos;
  skip_ws_comments(ctx);
  if (!at_end(ctx)) {
    bool crossed_newline = span_crosses_newline(ctx, start);
    /* s-flow-line-prefix(n) is the production for a continuation across a
     * line: s-indent(n), which is spaces only, and then optional
     * separation, which may hold tabs. line_space_indent() therefore
     * measures the spaces before the first tab, and a line such as
     * "  \tvalue" is indented by two.
     *
     * One line is exempt. A line whose content starts with the closing
     * indicator of a flow collection may sit exactly at the indent of the
     * enclosing block value, after spaces only. That is how a flow
     * collection that spans lines is usually closed ("key: [" on one line,
     * the elements indented below, and "]" back under "key"), and libyaml
     * accepts it. Every other flow content at that column, and a closing
     * indicator further left or after a tab, is still refused. */
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

/* Read exactly one newline: a CR, an LF, or a CRLF pair. */
static void skip_newline(parse_ctx_t *ctx) {
  if (!at_end(ctx) && cur(ctx) == '\r') ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '\n') ctx->pos++;
}

/* Looks ahead and reads nothing. True when the rest of the current line holds
 * only inline whitespace, which is spaces and tabs. The rest of the line
 * reaches to the next line break, or to the end of the input. A true answer
 * means that this is a truly blank line. The line folding of a double-quoted
 * scalar and of a single-quoted scalar uses this to find a blank continuation
 * line. Such a line must fold to a newline and not to a space. This holds
 * even when that line carries whitespace of its own at the end, for example a
 * lone tab. A plain cur(ctx) == '\n' or '\r' check right after skip_newline()
 * cannot see past such whitespace to the line break behind it. */
static bool at_blank_line(parse_ctx_t *ctx) {
  size_t p = ctx->pos;
  while (p < ctx->len && (ctx->src[p] == ' ' || ctx->src[p] == '\t')) p++;
  return p >= ctx->len || ctx->src[p] == '\r' || ctx->src[p] == '\n';
}

/*
 * True for a C0 control byte, which is 0x00 to 0x1F, other than the tab
 * (0x09). Also true for DEL (0x7F). YAML 1.2 excludes all of them from
 * literal scalar content with no escape, in every style. c-printable is the
 * production under the nb-char grammar of every scalar style that JSON does
 * not cover. It is "#x9 | #xA | #xD | [#x20-#x7E] | ...". Note that the range
 * stops at 0x7E, one byte short of DEL. nb-json governs double-quoted content
 * only. Its text is wider: "#x9 | [#x20-#x10FFFF]", with no gap at all around
 * 0x7F. On the grammar text alone, nb-json appears to permit a raw DEL byte
 * there. But PyYAML refuses a raw DEL byte in the same way across every style:
 * double-quoted, single-quoted, plain and literal block. It reports
 * "unacceptable character #x007f: special characters are not allowed". PyYAML
 * filters that byte at the character stream level, before it reads any
 * grammar for a specific scalar style. That filter takes priority over the
 * wider text of nb-json for this one byte.
 *
 * The serializer asks this predicate about the bytes of a string that it is
 * about to write: a string that holds one of these bytes, or a line break,
 * goes out double-quoted, where each of them becomes an escape. An explicit
 * "\x01" or "\u0001" escape in a double-quoted scalar is the one way to put
 * such a byte into a document. The parser refuses the raw byte for the whole
 * stream; see cyaml_first_refused_char() below.
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
 * parse_common() checks the whole input against that rule once, before it
 * parses anything, and refuses the document at the first byte that breaks
 * it. The refused bytes are an ill-formed UTF-8 sequence (a truncated
 * sequence, a stray continuation byte, an overlong form, an encoded
 * surrogate, a code point above U+10FFFF), a C0 control other than TAB, LF
 * and CR, DEL, a C1 control other than NEL, and the noncharacters U+FFFE and
 * U+FFFF. The rule covers every byte of the stream: scalars of every style,
 * keys, comments, tags, anchors, directives and the space between tokens.
 * An escape inside a double-quoted scalar is different: "\x01", "\x80" and
 * "\uFFFE" are c-printable text that names such a character, and the parser
 * decodes it into the value.
 *
 * Because every scanner runs after this check, none of them has to test a
 * byte against the rule again. That includes the property that a raw NUL
 * never reaches a token that the parser stores as a NUL-terminated string
 * (an anchor name, a tag, a scalar): it would end that string early and
 * report nothing.
 *
 * The check reads sixteen bytes at a time. A block whose bytes are all TAB,
 * LF, CR or 0x20 to 0x7E is accepted with one vector compare; any other
 * block, which means one that holds non-ASCII text or a refused byte, is
 * walked one character at a time. The vector types are GCC and Clang vector
 * extensions; a target without a vector unit runs the same code on scalar
 * registers.
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

/* True for an ASCII byte that c-printable leaves out: a C0 control other
 * than TAB, LF and CR, and DEL. */
static inline bool cyaml_is_refused_ascii(unsigned char c) {
  return (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) || c == 0x7F;
}

/* Gives the offset of the first byte of p[0..len) that starts an ill-formed
 * UTF-8 sequence or a character outside c-printable. Gives len when there is
 * none. */
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
      /* A well-formed sequence of two or three bytes: a C1 control is
       * C2 80..C2 9F, and NEL (C2 85) is the one c-printable member of that
       * range. U+FFFE and U+FFFF are EF BF BE and EF BF BF. */
      if (c == 0xC2 && p[i + 1] <= 0x9F && p[i + 1] != 0x85) return i;
      if (c == 0xEF && p[i + 1] == 0xBF && p[i + 2] >= 0xBE) return i;
      i += step;
    }
    if (i >= len) return len;
  }
}

/* Writes into msg the diagnostic for the character that starts at p[at],
 * which cyaml_first_refused_char() refused. The line and the column are both
 * counted from 1. A line ends at LF, at CR, or at a CRLF pair, which are the
 * line breaks of YAML 1.2. The column counts characters, not bytes, and a
 * byte order mark at the start of the stream does not count. Every byte of
 * the message is printable ASCII: a byte of the input appears as 0xNN. */
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

  /* The same classification as cjson, through the shared helper. */
  char detail[64];
  const char *what =
      ccol_utf8_describe_ill_formed(p, len, at, detail, sizeof(detail));
  snprintf(msg, msg_size,
           "invalid UTF-8 in a YAML stream (line %zu, column %zu): %s (%s)",
           line, col, what, detail);
}

/* True when ctx->pos is at a document-start marker ('---') or at a
 * document-end marker ('...'). Such a marker is the three characters at
 * column 0, and then whitespace or the end of the input. A '#' that touches
 * the marker, with no whitespace between them, does NOT count. That matches
 * the rule of skip_ws_comments() and rest_of_line_is_blank() elsewhere in
 * this file: real whitespace must come before a comment. It also matches the
 * s-l-comments grammar of YAML 1.2, which needs an s-separate-in-line before
 * a c-nb-comment-text. "---#x" is therefore ordinary plain scalar content. It
 * is not a marker and then a comment. The parser uses this function to stop
 * the block collection parsers at a document boundary. It also uses it to
 * find the start of the next document in a stream of many documents. */
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
 * Section 5.2 of YAML 1.2 says that a byte order mark must not appear inside
 * a document; the stream grammar allows one only in a document prefix, in
 * front of a document. Inside a document, a mark at the start of a line
 * therefore ends the block structure, like a document marker, and the
 * stream loop of parse_common() then accepts it only in front of a '---' or
 * at the end of the input. The first byte is tested first, so the common
 * case costs one comparison. */
static inline bool at_col0_bom(parse_ctx_t *ctx) {
  return ctx->len - ctx->pos >= 3 &&
         (unsigned char)ctx->src[ctx->pos] == 0xEF &&
         (unsigned char)ctx->src[ctx->pos + 1] == 0xBB &&
         (unsigned char)ctx->src[ctx->pos + 2] == 0xBF && current_col(ctx) == 0;
}

/* Anchor / alias helpers */

/*
 * The management of the anchor table.
 *
 * A chmap (char* -> cyaml_node_t*) holds the anchors (&name). The parser
 * creates that map lazily. When the parser finds an anchor, it makes a deep
 * clone of the node and stores the clone here. An alias (*name) resolves to
 * one more deep clone. Every expansion therefore gives an independent
 * subtree, and no two parents share ownership of one node.
 *
 * anchors_ensure: allocate the map lazily, on the first use.
 * anchors_store:  store a clone under name, and destroy any earlier clone.
 * anchors_lookup: give a borrowed reference to the stored clone, or NULL.
 * anchors_destroy: free every stored clone and the map itself.
 *
 * The end of parse_common always destroys the table, for a success and for a
 * failure alike.
 */
static bool anchors_ensure(parse_ctx_t *ctx) {
  if (ctx->anchors) return true;
  char *err = NULL;
  ctx->anchors = chmap_create_mp(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                 ccol_string, ccol_pointer, ctx->mp, &err);
  return ctx->anchors != NULL;
}

/* Returns false on an OOM, and sets a parse error with parse_err(). That OOM
 * comes either from the lazy table creation of anchors_ensure() or from
 * chmap_insert_elem() itself. Returns true on a real store. This function
 * always uses clone exactly once: it stores clone on success, and destroys
 * clone on failure. The caller therefore never needs its own cleanup for
 * clone. The report of the failure here is more than a cosmetic error
 * message. A caller that treated an anchor store as always successful would
 * give back a "successful" parse result. The library would never register
 * that anchor, and it would report nothing. A later '*name' alias reference
 * would then fail with a confusing "unknown alias" error, and not with the
 * real "out of memory" cause. See the callers of this function for how they
 * pass the failure up instead. */
/* Append an entry for name to the undo log of the speculation that runs
 * now, with old set to NULL. Returns the entry, which stays valid until the
 * next append, or NULL on an OOM. */
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

/* Returns false on an OOM, and sets a parse error with parse_err(). That OOM
 * comes either from the lazy table creation of anchors_ensure() or from
 * chmap_insert_elem() itself. Returns true on a real store. This function
 * always uses clone exactly once: it stores clone on success, and destroys
 * clone on failure. The caller therefore never needs its own cleanup for
 * clone. The report of the failure here is more than a cosmetic error
 * message. A caller that treated an anchor store as always successful would
 * give back a "successful" parse result. The library would never register
 * that anchor, and it would report nothing. A later '*name' alias reference
 * would then fail with a confusing "unknown alias" error, and not with the
 * real "out of memory" cause. See the callers of this function for how they
 * pass the failure up instead.
 *
 * While a speculative parse runs (ctx->anchor_undo is set), every store is
 * also recorded in its undo log, and a clone that a redefinition replaces
 * moves into the log instead of being freed. */
static bool anchors_store(parse_ctx_t *ctx, const char *name,
                          cyaml_node_t *clone) {
  if (!anchors_ensure(ctx)) {
    parse_err(ctx, "out of memory registering anchor '%s' at position %zu",
              _cyaml_echo(name), ctx->pos);
    __cyaml_destroy((cyaml)clone);
    return false;
  }
  _anchor_undo_entry_t *undo = NULL;
  if (ctx->anchor_undo) {
    undo = anchor_undo_push(ctx, name);
    if (!undo) {
      parse_err(ctx, "out of memory registering anchor '%s' at position %zu",
                _cyaml_echo(name), ctx->pos);
      __cyaml_destroy((cyaml)clone);
      return false;
    }
  }
  cmap_pair kp = {.ptr = (void *)name, .size = strlen(name) + 1};
  /* One hash and one probe insert the anchor, or hand back the slot of an
   * anchor that the document defines again. The later definition wins: the
   * old clone is freed, or moved into the undo log, and the slot then points
   * at the new one. */
  cmap_pair vp = {.ptr = &clone, .size = sizeof(clone)};
  const cmap_pair *old_vp = NULL;
  ccol_retval_t ins =
      ccol_chmap_insert_or_get_elem(ctx->anchors, &kp, &vp, &old_vp);
  if (ins == ccol_success) return true;
  if (ins == ccol_key_already_present) {
    cyaml_node_t *old_clone = _cyaml_read_child(old_vp->ptr);
    if (undo)
      undo->old = old_clone;
    else
      __cyaml_destroy((cyaml)old_clone);
    memcpy((void *)old_vp->ptr, &clone, sizeof(clone));
    return true;
  }
  /* The insert failed. Keep the old clone in the map and drop the new one,
   * and the log entry that described the store. */
  if (undo) {
    ctx->anchor_undo->len--;
    _ccol_mem_free(ctx->mp, undo->name);
  }
  parse_err(ctx, "out of memory registering anchor '%s' at position %zu",
            _cyaml_echo(name), ctx->pos);
  __cyaml_destroy((cyaml)clone);
  return false;
}

/*
 * A speculative parse reads content that may turn out to be something else,
 * and then the parser reads it again. The only one is the parse of a flow
 * collection that may be an implicit block mapping key. See
 * _try_parse_scalar_dict_key. Such a parse has two side effects that must
 * not survive a rejection. It stores the anchors that it defines, so an
 * alias in the second reading would resolve to an anchor that the document
 * defines only later. And it charges the parse budgets for a tree that the
 * rejection frees, so the second reading would pay again.
 *
 * anchor_speculation_begin() records the budgets and starts an undo log for
 * the anchor table. anchor_speculation_reject() puts the anchor table and the
 * budgets back exactly as they were. anchor_speculation_accept() keeps every
 * change and frees what the log kept alive. Both end the speculation. A
 * rejection allocates nothing, so it cannot fail.
 */
typedef struct {
  _anchor_undo_t undo;
  _anchor_undo_t *outer;
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
  if (sp->undo.entries != sp->undo.inline_entries)
    _ccol_mem_free(ctx->mp, sp->undo.entries);
  ctx->anchor_undo = sp->outer;
}

static void anchor_speculation_accept(parse_ctx_t *ctx,
                                      _anchor_speculation_t *sp) {
  for (size_t i = 0; i < sp->undo.len; i++) {
    __cyaml_destroy((cyaml)sp->undo.entries[i].old);
    _ccol_mem_free(ctx->mp, sp->undo.entries[i].name);
  }
  anchor_speculation_end(ctx, sp);
}

static void anchor_speculation_reject(parse_ctx_t *ctx,
                                      _anchor_speculation_t *sp) {
  /* Undo the stores in the reverse order, so a name that the speculation
   * stored twice ends with the clone that it held before the first store.
   * A name that the speculation added keeps its slot, with a NULL clone,
   * which anchors_lookup() reports as an unknown anchor. That keeps the
   * rejection free of any allocation that a delete from the map could
   * make. */
  for (size_t i = sp->undo.len; i-- > 0;) {
    _anchor_undo_entry_t *e = &sp->undo.entries[i];
    cmap_pair kp = {.ptr = e->name, .size = strlen(e->name) + 1};
    const cmap_pair *vp = NULL;
    if (chmap_get_elem_ref(ctx->anchors, &kp, &vp) == ccol_success) {
      __cyaml_destroy((cyaml)_cyaml_read_child(vp->ptr));
      memcpy((void *)vp->ptr, &e->old, sizeof(e->old));
    } else {
      __cyaml_destroy((cyaml)e->old);
    }
    _ccol_mem_free(ctx->mp, e->name);
  }
  _parse_node_budget = sp->node_budget;
  _parse_byte_budget = sp->byte_budget;
  _parse_amp_node_budget = sp->amp_node_budget;
  _parse_amp_byte_budget = sp->amp_byte_budget;
  _parse_node_budget_exhausted = sp->node_exhausted;
  _parse_byte_budget_exhausted = sp->byte_exhausted;
  _parse_amp_limit_hit = sp->amp_limit_hit;
  anchor_speculation_end(ctx, sp);
}

/* A slot that holds a NULL clone is a name that a rejected speculation added
 * and took back. See anchor_speculation_reject(). It reads as unknown. */
static cyaml_node_t *anchors_lookup(parse_ctx_t *ctx, const char *name) {
  if (!ctx->anchors) return NULL;
  cmap_pair kp = {.ptr = (void *)name, .size = strlen(name) + 1};
  const cmap_pair *vp = NULL;
  if (chmap_get_elem_ref(ctx->anchors, &kp, &vp) != ccol_success) return NULL;
  return _cyaml_read_child(vp->ptr);
}

static void anchors_destroy(parse_ctx_t *ctx) {
  if (!ctx->anchors) return;
  /* Use chmap_destroy_with_dtor from chashmap.h. Do not walk the map first
   * and destroy afterwards. This call reaches every stored clone through the
   * internal teardown walk of chashmap, which needs no allocation. It can
   * therefore never leak a clone under a long OOM. A walk through
   * chashmap_begin_iter() first still can, because that iterator allocates.
   * This matches the destructor callback that the
   * CYAML_DICTIONARY case of node_clear uses for the children of a node. */
  chmap_destroy_with_dtor(ctx->anchors, _cyaml_destroy_dict_child, NULL);
  ctx->anchors = NULL;
}

/* %TAG handle table */

/*
 * tag_handles_ensure, _set, _lookup and _destroy manage a table that maps a
 * %TAG handle to a prefix. Its lifetime matches the lifetime of
 * anchors_ensure, _store, _lookup and _destroy exactly. The parser creates it
 * lazily and destroys it at the end of parse_one_document. There is one
 * difference from anchors: this table starts truly empty. It does not start
 * with the two default YAML 1.2 handles in it. tag_handles_lookup falls back
 * to the fixed defaults itself when the table holds no explicit entry.
 * tag_handles_set can therefore use the "already present" signal of
 * chmap_insert_elem directly. That signal finds a second %TAG directive that
 * defines the same handle again inside one document. No separate structure is
 * needed to track that. The first %TAG for any handle, "!" and "!!" included,
 * always inserts cleanly, because the table starts with nothing in it.
 */
static bool tag_handles_ensure(parse_ctx_t *ctx) {
  if (ctx->tag_handles) return true;
  char *err = NULL;
  ctx->tag_handles = chmap_create_mp(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                     ccol_string, ccol_string, ctx->mp, &err);
  return ctx->tag_handles != NULL;
}

/* Returns ccol_success on a clean first insert. Returns
 * ccol_key_already_present when an earlier %TAG directive in this same
 * document already set this handle explicitly. The caller then refuses that
 * call as a duplicate definition. Returns another ccol_retval_t on an
 * allocation failure. */
static ccol_retval_t tag_handles_set(parse_ctx_t *ctx, const char *handle,
                                     const char *prefix) {
  if (!tag_handles_ensure(ctx)) return ccol_not_enough_memory;
  cmap_pair kp = {.ptr = (void *)handle, .size = strlen(handle) + 1};
  cmap_pair vp = {.ptr = (void *)prefix, .size = strlen(prefix) + 1};
  return chmap_insert_elem(ctx->tag_handles, &kp, &vp);
}

/* Gives a borrowed pointer to the resolved prefix for `handle`. That prefix
 * is the one that a %TAG directive registered, when such an entry is there.
 * If not, it is the built-in YAML 1.2 default for "!" or for "!!". Gives NULL
 * when `handle` is neither registered nor one of the two built-in handles.
 * Such a handle has a name that nothing defines. */
static const char *tag_handles_lookup(parse_ctx_t *ctx, const char *handle) {
  if (ctx->tag_handles) {
    cmap_pair kp = {.ptr = (void *)handle, .size = strlen(handle) + 1};
    const cmap_pair *vp = NULL;
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

/* Read the anchor name or the alias name that comes after a '&' or a '*'. A
 * name is any list of one or more characters that are not whitespace, not
 * flow indicators, and not '#'. parse_common() already refused every
 * character outside c-printable, so a raw NUL can never end the stored name
 * early. Returns false on an empty name and on an allocation failure, and sets
 * a parse error with parse_err(). This function always sets ctx->error on a
 * failure. That matters beyond its own direct callers.
 * try_parse_scalar_dict_key() calls it speculatively. That caller uses
 * ctx->error[0] alone to tell "a real error" from "not a key after all". It
 * does not use a restored ctx->pos, because the scan loop of this function
 * already moved past that point on this path. Without ctx->error set here on
 * an OOM, that caller puts the failure in the wrong class. It then continues
 * the parse from a corrupt position, and the document does not fail
 * cleanly. */
static bool parse_anchor_name(parse_ctx_t *ctx, char **name_out) {
  size_t start = ctx->pos;
  while (!at_end(ctx)) {
    char c = cur(ctx);
    /* An anchor name ends at whitespace, at a flow indicator, or at a
     * comment. */
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' ||
        c == '[' || c == ']' || c == '{' || c == '}' || c == '#')
      break;
    /* A ':' with whitespace or the end of the input after it is a mapping
     * value indicator. It is not part of the anchor name or the alias name.
     * This matches the same rule in scan_plain_scalar_line: a colon and a
     * space, or a colon and an end of line, ends the token. A caller can
     * therefore use an anchor or an alias directly as a mapping key, with no
     * space before the colon. "*x: y" is by far the most natural way to write
     * that, because an ordinary key is written "key: value" in the same way.
     * The parser reads it as the name "x" alone, and not as "x:". A reference
     * parser confirms this: it resolves "*x: y" to the alias "x" used as a
     * key. It does not report an unknown alias "x:". A bare ':' with no
     * whitespace after it stays ordinary name content, for example inside an
     * anchor name that looks like a URL. scan_plain_scalar_line has the same
     * exception. */
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
 * try_parse_float_scalar are the core-schema type tests. make_typed_scalar
 * tries them one after the other to resolve a scalar that carries no tag.
 * They are separate functions that a caller can call on its own. They are not
 * one block of code inside make_typed_scalar. An explicit tag can therefore
 * force exactly one of them. finalize_scalar_node below does that. It does
 * not try every other type first, which is what the ordered fallthrough of
 * make_typed_scalar does.
 */
static bool try_parse_bool_scalar(const char *s, bool *out) {
  /* Dispatch on the first character, before any comparison runs. Every scalar
     in a document reaches this point, and almost none of them is a boolean.
     What decides the cost is therefore how fast this code can rule out a
     scalar that is not a boolean. A chain of strcmp calls rules it out only
     after it enters the C library once for each spelling. That is a call and
     its setup each time, also when the very first byte already settles the
     question. A switch on that byte settles it with one load and one branch.
     Only the two or three spellings that are still possible then need a
     comparison. The set that this code accepts is the set that the YAML 1.2
     core schema accepts: the all-lower form, the capitalized form and the
     all-upper form, and nothing else. */
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

/*
 * Convert a magnitude that is not negative into its final long long value.
 * Negate it first when neg is true. The caller must already check that
 * uval <= (unsigned long long)LLONG_MAX + (neg ? 1 : 0). That check proves
 * that the result fits in a long long. The only job left for this function is
 * the negation, with no undefined behavior from a signed overflow. There is
 * one boundary magnitude, 2^63, whose negation is LLONG_MIN itself. A cast to
 * a signed long long first, and then a negation, cannot reach that value.
 * The expression -(long long)0x8000000000000000ULL negates LLONG_MIN, which
 * is already signed, and C never permits that. The decimal branch below gives
 * strtoll the full signed string for the same reason. It does not negate a
 * magnitude with the sign removed.
 */
static long long magnitude_to_signed(unsigned long long uval, bool neg) {
  if (!neg) return (long long)uval;
  if (uval == (unsigned long long)LLONG_MAX + 1ULL) return LLONG_MIN;
  return -(long long)uval;
}

_CYAML_PARSE_HOT bool try_parse_int_scalar(const char *s, long long *out) {
  /* The int grammar of the core schema is [-+]?[0-9]+ for a decimal value,
   * and the 0x and 0o forms. It has no production for whitespace at the
   * front. strtoll() below accepts more than that grammar. The C standard
   * says that it skips leading whitespace before the subject sequence. It
   * reports nothing about that. Without this check, " 42" is therefore
   * accepted as 42 and reports nothing, where it must fail.
   * trim_trailing_ws_for_numeric_tag is the caller of this function for an
   * explicit !!int tag. It deliberately trims only the whitespace at the end,
   * and never the whitespace at the front, for exactly this reason. This
   * check refuses such input here, before anything else. Every caller,
   * implicit resolution included, therefore gets the same strict behavior. */
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
    /* The hex form. The grammar of the core schema for it is exactly
     * "0x" [0-9a-fA-F]+. There is no room for a sign, or for anything else,
     * between the prefix and the digits. The 'x' itself must be lowercase.
     * The digit sequence is different: it is explicitly case-insensitive. An
     * uppercase "0X" prefix has no int representation in the core schema at
     * all. It must fall through to the decimal branch below, which correctly
     * refuses it. strtoll with an explicit base of 10 never reads "0X..." as
     * a hex value, in any case. strtoull() accepts more than the grammar
     * above: the C standard says that it accepts an optional '+' or '-'
     * before the digit sequence that it reads. Without a check, a malformed
     * literal like "0x-0" or "0x+5" is therefore accepted as a valid integer,
     * with the magnitude 0 or 5, and reports nothing. It must fall back to
     * CYAML_STRING instead. This code therefore validates every digit after
     * the prefix by hand first. The octal branch of the float fallback below
     * does the same. */
    const char *digits = p + 2;
    if (*digits == '\0') return false;
    for (const char *d = digits; *d; d++) {
      bool is_hex_digit = (*d >= '0' && *d <= '9') ||
                          (*d >= 'a' && *d <= 'f') || (*d >= 'A' && *d <= 'F');
      if (!is_hex_digit) return false;
    }
    unsigned long long uval = strtoull(digits, &endp, 16);
    if (endp == digits || *endp != '\0' || errno == ERANGE) return false;
    /* strtoull refuses only a magnitude that overflows ULLONG_MAX, which is
     * 2^64-1. A value between 2^63 and 2^64-1 still fits in an unsigned
     * 64-bit word. But it has no representation as a long long of the
     * requested sign. This code must therefore also refuse it here. Without
     * that, the library reads it again as another value and reports nothing:
     * 0xFFFFFFFFFFFFFFFF becomes -1, and 0x8000000000000000 becomes negative.
     * A refusal here lets the usual cascade of the caller, make_typed_scalar,
     * fall back to a CYAML_FLOAT for a value this large. The errno==ERANGE
     * check of the decimal branch already does the same for a plain decimal
     * literal wider than 64 bits. */
    unsigned long long limit =
        (unsigned long long)LLONG_MAX + (neg ? 1ULL : 0ULL);
    if (uval > limit) return false;
    ival = magnitude_to_signed(uval, neg);
  } else if (p[0] == '0' && p[1] == 'o') {
    /* The octal form. The 'o' must be lowercase. The hex branch above has the
     * same rule about case. An uppercase "0O" prefix falls through to the
     * decimal branch below, which correctly refuses it. The comment of the
     * hex branch above tells you why this code validates every digit by hand
     * before it calls strtoull(). It does not trust strtoull() to refuse a
     * malformed literal like "0o-0" or "0o+7" on its own. */
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
    /* The decimal form. Pass the full original string, and the optional sign
     * with it. strtoll then handles LLONG_MIN (-9223372036854775808)
     * correctly. A call of strtoll on p, the substring with the sign removed,
     * overflows for 2^63. */
    ival = strtoll(s, &endp, 10);
    if (endp == s || *endp != '\0' || errno == ERANGE) return false;
  }
  *out = ival;
  return true;
}

/* Handles the special float values (.inf, -.inf and .nan), and also ordinary
 * parsing with strtod. This matches the definition of the YAML 1.2 core
 * schema, which has the dot-prefixed special values only. On a C99 platform
 * strtod also accepts a bare "nan", "inf" or "infinity". This function
 * excludes all three, because they are not YAML floats.
 *
 * strtod() follows LC_NUMERIC. Every caller runs inside the "C" locale scope
 * that cyaml_parse_n_mp(), cyaml_parse_mp(), cyaml_serialize() and
 * cyaml_serialize_flow() open, so '.' is always the decimal point here. */
_CYAML_PARSE_HOT bool try_parse_float_scalar(const char *s, double *out) {
  /* try_parse_int_scalar above has the same check and the same comment. The
   * float grammar of the core schema has no production for whitespace at the
   * front. strtod() below skips it and reports nothing. Without this check,
   * " 3.5" is therefore accepted as 3.5, where it must fail. */
  if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') return false;

  /* Every spelling of infinity and of not-a-number begins with '.', '+' or
     '-'. One look at the first character therefore excludes every ordinary
     number here, before any comparison runs. See try_parse_bool_scalar for
     why that matters on a path that every scalar in the document takes. A
     leading '+' or '-' must still fall through to the numeric parse below
     when it is not one of these spellings. That is why those two cases do not
     return early. */
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
  bool neg = false;
  if (*q == '+') {
    q++;
  } else if (*q == '-') {
    neg = true;
    q++;
  }
  /* Dispatch on the first byte, before any comparison runs. Every spelling
     that the four case-insensitive comparisons here can match begins with n,
     N, i or I. One load and one branch therefore settle all of them for any
     other scalar. A plain string is the case that dominates a real document.
     Without this guard, the chain enters the C library up to four times for
     each scalar only to be ruled out. A measurement on a document with many
     strings puts the case-insensitive comparisons at 4.6 percent of every
     instruction that the parse executes. */
  const char qc = *q;
  if (qc == 'n' || qc == 'N' || qc == 'i' || qc == 'I') {
    if (strcasecmp(q, "nan") == 0 || strcasecmp(q, "inf") == 0 ||
        strcasecmp(q, "infinity") == 0)
      return false;
    /* The strtod() of glibc also accepts the nan(n-char-sequence) syntax of
     * C99, for example "nan(123)" and "nan()". That syntax is not part of the
     * float grammar of the YAML core schema at all. Only the dot-prefixed
     * forms above are. Without a guard, the general strtod() call below
     * accepts it as a NaN float and reports nothing. It then drops the
     * original text, and every distinct "nan(...)" spelling collides onto one
     * canonical dictionary key. Refuse the whole of q here, so that it falls
     * through to CYAML_STRING like any other token that is not a number. */
    if (strncasecmp(q, "nan(", 4) == 0) return false;
  }

  /* Hex and octal integer syntax has no float representation of its own in
   * the YAML core schema at all. This function accepts one for exactly one
   * reason: to reproduce the implicit typing cascade of make_typed_scalar().
   * That cascade tries an int first, and it falls back to a float only after
   * the int overflows an int64. An oversized decimal literal already falls
   * back to a float of the same magnitude below. That reason does NOT hold
   * for a small hex or octal literal that is in range. try_parse_int_scalar
   * already accepts such a literal as a real CYAML_INTEGER, so it must never
   * reach this point as float syntax at all. The reason also does not hold
   * for the explicit !!float tag path of finalize_scalar_node(), which calls
   * this function directly and never tries an int first. An in-range "0x10"
   * or "0o17" with an !!float tag has no valid float meaning. This code must
   * refuse it and must not convert it and report nothing. This function
   * therefore derives the "really overflows an int64" check itself. It uses
   * the same limit arithmetic as try_parse_int_scalar. It does not trust a
   * caller to have made that check already. */
  if (q[0] == '0' && q[1] == 'o') {
    const char *c = q + 2;
    if (*c == '\0') return false;
    for (; *c; c++) {
      if (*c < '0' || *c > '7') return false;
    }
    errno = 0;
    unsigned long long ouval = strtoull(q + 2, NULL, 8);
    if (errno == ERANGE) return false; /* too large even for a u64 */
    unsigned long long limit =
        (unsigned long long)LLONG_MAX + (neg ? 1ULL : 0ULL);
    if (ouval <= limit) return false; /* fits in an int64, so not a float */
    *out = neg ? -(double)ouval : (double)ouval;
    return true;
  }

  /* Check that the rest holds only hex digits, before this code treats the
   * text as a hex integer literal at all. strtod() itself accepts the real
   * hex-float syntax of C99, which is an explicit p or P exponent, with an
   * optional '.'. That is a GNU and C99 extension. It is not a YAML float,
   * and this code must not read it as one and report nothing. "0x1p3" gives
   * 8.0, and "0x1.8p10" gives 1536. Neither is valid YAML of any kind, and
   * both must fall through to CYAML_STRING. The loop below already refuses
   * both by construction, because '.', 'p' and 'P' are not hex digits. The
   * 'x' must be lowercase. try_parse_int_scalar has the same rule about case
   * for the "0x" grammar of the core schema. */
  if (q[0] == '0' && q[1] == 'x') {
    const char *c = q + 2;
    if (*c == '\0') return false;
    for (; *c; c++) {
      bool is_hex_digit = (*c >= '0' && *c <= '9') ||
                          (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F');
      if (!is_hex_digit) return false;
    }
    /* See the doc comment of the octal branch above. Only a magnitude that
     * really overflows an int64 is a valid float fallback. A hex literal that
     * is in range, for example "0x10", has no float representation. This code
     * must refuse it here. Without that, strtod() gets it and accepts it
     * through its own hex-integer extension, and reports nothing. */
    errno = 0;
    unsigned long long hxuval = strtoull(q + 2, NULL, 16);
    if (errno == ERANGE) return false; /* too large even for a u64 */
    unsigned long long limit =
        (unsigned long long)LLONG_MAX + (neg ? 1ULL : 0ULL);
    if (hxuval <= limit) return false; /* fits in an int64, so not a float */
    *out = neg ? -(double)hxuval : (double)hxuval;
    return true;
  }

  /* An uppercase "0X" or "0O" prefix has no numeric representation in the
   * core schema at all. Only the lowercase forms above are defined. That is
   * true at any position, and it includes the position after the '+' or '-'
   * sign that q already has removed. This code must refuse such a prefix
   * here. It must not fall through to the general strtod() call below. The
   * octal case is different: strtod has no "0o" syntax in any case, so "0O17"
   * already fails the parse of strtod by itself. But strtod() does recognize
   * an uppercase "0X" hex prefix. The C standard describes its accepted
   * subject sequence with the case-insensitive words "0x or 0X". Without this
   * check, strtod() therefore accepts "0X10" as 16.0 through that extension,
   * and reports nothing. */
  if (q[0] == '0' && (q[1] == 'X' || q[1] == 'O')) return false;

  char *endp = NULL;
  errno = 0;
  double dval = strtod(s, &endp);
  if (endp == s || *endp != '\0') return false;
  /* strtod() sets errno to ERANGE in two cases. The first is a real overflow,
   * where it clamps the result to positive or negative infinity. The second
   * is an underflow to a valid subnormal value, or to 0.0. That second result
   * is correct and fully valid. Only the first case is a real failure. A
   * refusal on ERANGE alone puts a valid tiny float, for example "5e-324",
   * into the wrong class as a CYAML_STRING, and reports nothing. */
  if (errno == ERANGE && (dval == __builtin_inf() || dval == -__builtin_inf()))
    return false;
  *out = dval;
  return true;
}

static bool is_null_scalar_text(const char *s) {
  /* Test the first character first, for the same reason as
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
 * Try to parse s as a typed scalar of the YAML 1.2 core schema.
 * Gives a new node that this function allocates. The caller owns it.
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
 * Remove every whitespace character and newline at the end of text, in place.
 * This runs before a forced !!int or !!float tag tries to parse text as a
 * number. Without it, the chomped content of a literal or folded block scalar
 * never matches. That holds for every chomp mode, and KEEP mode keeps several
 * newlines at the end. A measurement against the construct_yaml_int and
 * construct_yaml_float of PyYAML confirms this. Both accept a KEEP-chomped
 * "42\n\n\n" as 42, because the int() and float() of Python themselves accept
 * whitespace. A narrower rule that trims exactly one newline at the end is too
 * strict against that same reference. It refuses exactly the KEEP-chomped
 * case with several newlines that PyYAML accepts. This function is
 * deliberately NOT used for !!bool. The construct_yaml_bool of PyYAML does a
 * strict dictionary lookup with no trim, and it refuses the same shape. A
 * measurement confirms that too. A trim there would only differ from the
 * reference in the other direction. */
static void trim_trailing_ws_for_numeric_tag(char *text) {
  size_t len = strlen(text);
  while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
                     text[len - 1] == ' ' || text[len - 1] == '\t'))
    len--;
  text[len] = '\0';
}

/*
 * An explicit !!bool tag accepts a wider set of words than the implicit
 * typing default of this module, and it ignores the case. The implicit
 * default accepts only true, True, TRUE, false, False and FALSE, through
 * try_parse_bool_scalar. A measurement against the construct_yaml_bool of
 * PyYAML confirms the wider set. That function matches yes, no, true, false,
 * on and off, and it ignores the case. It does not match the single letters y
 * and n. This is true for every loader. An explicit tag is a more deliberate
 * act by the author, so it gets the wider set of words of PyYAML. It does not
 * get the narrow set that implicit resolution uses.
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
 * Build a CYAML_STRING node that takes over *text, the text that a scalar
 * scanner accumulated in a growable buffer. On success *text becomes NULL,
 * because the node owns it. On failure *text stays with the caller.
 *
 * A scanner buffer starts at 256 bytes and doubles, so it is usually far
 * larger than its content. The node keeps the text shrunk to its exact
 * length, and the byte budget of the parse is charged for that length, as
 * strdup_charged() charges every other string of the DOM. Without the shrink
 * a short quoted scalar keeps its whole scanner buffer for the life of the
 * document, and the budget counts none of it. A shrink that the allocator
 * refuses leaves the text where it is, which is still valid.
 *
 * Returns NULL when the byte budget refuses the charge, and when the node
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
 * Finish a scalar node from its raw text. An explicit tag overrides the
 * default type where such a tag applies. This function takes ownership of
 * `text`: it either frees text, or moves text into the node that it gives
 * back. The caller must not touch text again after this call. `resolved_tag`
 * is a borrowed pointer that this function does not own. A NULL
 * `resolved_tag` means that the scalar carries no tag. `implicit_ok` is true
 * only for the natural default of a plain scalar, which is the implicit
 * core-schema resolution of make_typed_scalar. It is false for the four
 * quoted and block styles. Their natural default is always CYAML_STRING, with
 * no implicit typing.
 *
 * Returns NULL on an OOM, and does not change ctx->error. Every other return
 * for an allocation failure in this file does the same. Returns NULL also on
 * a real disagreement between the tag and the content, and then it sets
 * ctx->error. That is a hard parse failure for the whole document, which
 * matches the fail-fast convention of this parser.
 */
static cyaml_node_t *finalize_scalar_node(parse_ctx_t *ctx, char *text,
                                          const char *resolved_tag,
                                          bool implicit_ok) {
  cyaml_node_t *n = NULL;

  if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_NULL) == 0) {
    /* The YAML spec says that an empty tag value defaults to null. This
     * code therefore forces a null node, whatever the text content is. */
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
    /* A !!seq or !!map tag on a scalar. This is the reverse of the
     * structural kind check that finalize_collection_node does for a
     * scalar-only core-schema tag on real collection syntax. A scalar can
     * never satisfy either tag. The block syntax or the flow syntax of a
     * collection fully decides its type, and the library never infers it. */
    parse_err(ctx, "tag '%s' does not match the node it decorates",
              _cyaml_echo(resolved_tag));
    _ccol_mem_free(ctx->mp, text);
    return NULL;
  } else if (implicit_ok) {
    /* There is no tag that forces a type. The scalar has no tag, or it has
     * the non-specific "!", or a custom tag, or !!binary. Fall through to
     * ordinary implicit resolution. */
    n = make_typed_scalar(ctx, text);
  } else {
    /* A quoted scalar or a block scalar with no tag that forces a type is
     * always a string. It gets no implicit typing. */
    n = string_node_adopting_text(ctx, &text);
  }

  _ccol_mem_free(ctx->mp, text); /* A no-op when the node already owns it. */

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
 * Attach an explicit tag to a collection node that the parser already built
 * in full. That node is a list or a dictionary. This function validates the
 * tag against the structure first. A tag can force the type of a scalar,
 * through finalize_scalar_node. A collection is different: its own block
 * syntax or flow syntax already fully decides its type. A tag here is
 * therefore one of two things. It is a structural tag that matches, which is
 * !!seq, !!map, or a custom tag that expects no structure. Or it is a real
 * disagreement, which is any of the 5 scalar core-schema tags, or a !!seq or
 * !!map on the wrong kind of collection. A disagreement is a hard parse
 * failure.
 *
 * Each dispatch branch in parse_node that builds a collection calls this
 * function at the exact point where it builds its result.
 * finalize_scalar_node already follows the same "as early as possible" rule
 * for scalars. This function is not deferred until after the recursive call
 * of the enclosing tag returns. A tag can pass through one or more '&' anchor
 * layers, for example in "!!seq &x\n  - 1\n  - 2\n". The tag must already be
 * attached by the time that the anchor branch clones the result for its
 * anchor table. Without that, the clone ends up with no tag and reports
 * nothing, and so does every later alias of it.
 *
 * `resolved_tag` is a borrowed pointer, exactly like the same parameter of
 * finalize_scalar_node. This function never takes ownership of it, and it
 * always makes a strdup copy. The same pointer may still need to reach other
 * collection-construction sites deeper in the same recursion, through more
 * '&' layers, after this call returns. This function must therefore not take
 * ownership of it.
 *
 * Returns NULL on a structural disagreement and on an OOM, and destroys
 * `coll` in both cases. On success it returns `coll` unchanged, with a strdup
 * copy of the tag in its ->tag field. A NULL `resolved_tag` is always a no-op
 * that succeeds.
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

/* Encode a code point as UTF-8 and append it to b. */
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

/* Read exactly 4 hex digits and decode them into a Unicode code point. On
 * success this function moves ctx->pos forward by 4. Returns false when the
 * input is too short, and when a digit is not a hex digit. */
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

/* Read exactly 8 hex digits for a YAML \UXXXXXXXX escape. Such an escape
 * names a code point in a Unicode supplementary plane. On success this
 * function moves ctx->pos forward by 8. */
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
 * The shared newline fold step for both quoted scalar styles. YAML 1.2
 * section 8.1.2 defines it. The caller calls it only when cur(ctx) is '\r' or
 * '\n'. This function first removes the inline whitespace at the end of b,
 * down to strip_floor bytes but never past it. The double-quoted style passes
 * its own escape_boundary, because the output of an escape sequence is real
 * content and not whitespace from the source. The single-quoted style has no
 * escape mechanism at all, so it always passes 0. This function then folds
 * the line break. It folds to a literal newline when one or more blank
 * continuation lines follow, which at_blank_line() reports. It folds to one
 * space for an ordinary line break. Both styles get the same treatment.
 *
 * On success this function updates b, leaves ctx->pos just past the folded
 * whitespace, and returns true. It returns false when it finds a document
 * marker in the middle of a fold. It then sets ctx->error with the name
 * style_name in it. That case is an unclosed quote that reaches a '---' or
 * '...' boundary, and such a boundary is never literal content to fold
 * across. On that path the caller must do its own cleanup and free b. The two
 * callers do that in different ways: one uses a goto to a fail label, and the
 * other frees b and returns in place.
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
  /* A blank line can hold only inline whitespace, for example a lone tab.
   * at_blank_line() is what makes such a line fold to a newline and not to a
   * space. That matches YAML 1.2 section 6.5. A plain cur(ctx) check does not
   * do this. */
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
 * Parse a double-quoted YAML scalar that starts at the opening '"'.
 * This function handles the YAML escape sequences, which are a superset of
 * the JSON escapes. On success it puts a string from the heap into *out and
 * returns true.
 */
static bool parse_double_quoted(parse_ctx_t *ctx, char **out) {
  if (at_end(ctx) || cur(ctx) != '"') {
    parse_err(ctx, "expected '\"' at position %zu", ctx->pos);
    return false;
  }
  ctx->pos++;

  ybuf_t b;
  yb_init(&b, ctx->mp);

  /* Counts how many bytes at the START of the buffer the code below must not
   * remove. The code below strips the whitespace at the end before a fold.
   * This value is b.len at the end of the most recent escape sequence. An
   * escaped space or tab is real content that the author asked for
   * explicitly. Two examples are a "\t" escape, and a backslash right before
   * a literal tab byte. Such a byte is not whitespace from the source that
   * sits before a line break. The rule "trailing white space is stripped" in
   * YAML 1.2 section 8.1.2 applies only to literal whitespace with no escape,
   * copied straight from the source. It never applies to a byte that an
   * escape sequence produced. Without this value, the next fold removes an
   * escaped tab at the end and reports nothing. */
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
          /* A "\0" escape resolves to the code point U+0000. The shared
           * comment on the \\x, \\u and \\U cases below tells you why this
           * parser cannot append it as a real byte. */
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
          /* The \xXX escape, which has 2 hex digits. */
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
           * byte. It has no separate length field. See the contract of
           * cyaml_str_val. An escape can decode into a real NUL byte inside
           * the value. Any later strlen(), printf() or serialize call would
           * then truncate that value. It would report nothing. That is the
           * exact shape of data corruption that the policy of this library
           * against NUL bytes inside a value prevents.
           * percent_decode_tag_inplace applies the same rule to the text of a
           * tag. Refuse the escape, and do not truncate the value and report
           * nothing. */
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
           * (0xD800-0xDBFF) has a meaning only as the first half of a pair:
           * a second \uXXXX escape with a low surrogate (0xDC00-0xDFFF) must
           * follow it at once, and the two name one code point in a
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
          /* The \\x case above tells you why this code must refuse a
           * result that is a null byte. It must not append that byte and
           * report nothing. */
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
          /* A \U escape names any 32-bit hex value. Only a value from 0 to
           * 0x10FFFF, outside the UTF-16 surrogate range 0xD800-0xDFFF, is a
           * Unicode scalar value. Any other value names no character, so the
           * document is refused. encode_utf8() could not encode it either. */
          if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            parse_err(ctx,
                      "\\U escape at position %zu names 0x%08X, which is "
                      "not a Unicode scalar value (a surrogate, or above "
                      "U+10FFFF)",
                      esc_pos, (unsigned)cp);
            goto fail;
          }
          /* The \\x case above tells you why this code must refuse a
           * result that is a null byte. It must not append that byte and
           * report nothing. */
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
          /* An escaped newline. YAML 1.2 section 8.1.2 defines it with the
           * s-double-escaped production, "\" b-non-content
           * l-empty(n,flow-in)* s-flow-line-prefix(n). Only the escaped break
           * ITSELF is non-content, and it adds nothing. An ordinary break
           * with no escape is different. It is s-flow-folded, and
           * fold_quoted_newline() below handles it. Such a break always
           * folds to a space or to literal newlines. Each l-empty blank line
           * after the escaped break still ends in a real b-as-line-feed. The
           * at_blank_line() loop of fold_quoted_newline() works the same way.
           * This code must therefore still append one literal '\n' for each
           * blank line. It skips only the very first break, which is the
           * escaped one, and appends nothing for it. */
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
      /* The strip of the whitespace at the end below must not remove what
       * this escape appended, if it appended anything. See the doc comment
       * of escape_boundary above. */
      escape_boundary = b.len;
      continue;
    }

    /* A newline inside a double-quoted scalar. It folds to a space. For one
     * or more blank continuation lines it folds to newlines. The doc comment
     * of escape_boundary above tells you why the strip of the whitespace at
     * the end stops at that boundary and not at 0. */
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
 * Parse a single-quoted YAML scalar. This style has one escape only: two
 * single quotes together ('') mean one literal single quote. This function
 * folds a newline inside the scalar, with the same rules as the
 * double-quoted style.
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
     * strip_floor = 0. The single-quoted style has no escape mechanism at
     * all. There is therefore no protected span like escape_boundary to
     * stop at. */
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
 * Parse the header of a block scalar. The caller already read the style
 * indicator, which is | or >. The header sits on the same line. It holds an
 * optional chomp indicator, which is - or +. It also holds an optional
 * explicit indent indicator, which is one digit from 1 to 9. The header may
 * hold either one, or both.
 *
 * This function sets *chomp and *explicit_indent. An *explicit_indent of 0
 * means that the header gave no indent indicator.
 */
static bool parse_block_scalar_header(parse_ctx_t *ctx, chomp_t *chomp,
                                      int *explicit_indent) {
  *chomp = CHOMP_CLIP;
  *explicit_indent = 0;

  /* The header may hold both a chomp indicator and an indent indicator, in
   * either order. But c-b-block-header permits at most ONE of each kind.
   * had_chomp and had_indent refuse a second one of either kind, which is a
   * duplicate or a contradiction. "|--", "|+-" and "|24" are three examples.
   * Without them, the later indicator writes over the earlier one and the
   * parser reports nothing. */
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

  /* A comment at the end needs whitespace before the '#'. The s-b-comment
   * production of YAML 1.2 says so. The comment text is reachable only
   * through the optional s-separate-in-line group that gates it. This
   * matches the general "a comment must follow whitespace" rule that
   * skip_ws_comments already applies elsewhere. The text '>#comment' has
   * nothing between the header and the '#'. It is therefore not a comment.
   * It is an invalid character at the end of the header. */
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

/* The result of scan_block_scalar_line. It tells the caller what to do with
 * the line that the scan just classified. parse_block_scalar_content, for the
 * literal | style, and parse_folded_scalar_content, for the folded > style,
 * share it. */
typedef enum {
  SCAN_BLOCK_LINE_ERROR,  /* ctx->error is already set. The caller cleans up
                              and returns false. */
  SCAN_BLOCK_LINE_END,    /* The scalar ends before this line. That happens
                              for a first line that is not blank and that is
                              not indented past parent_indent. It also
                              happens for a later line that is less indented
                              than block_indent. The scan rewinds ctx->pos to
                              the start of this line, so that the caller of
                              the caller sees it as input that nothing read
                              yet. */
  SCAN_BLOCK_LINE_BLANK,  /* An empty line. Before indent_determined, it is a
                              line that the scan cannot yet classify. Here
                              *spaces_out has no meaning. Both callers
                              already know that the extra indentation of a
                              blank line is zero, or they do not need it.
                              ctx->pos sits exactly at the end of the line,
                              which is the end of the input, a '\r', or a
                              '\n'. The caller must still move past it, with
                              its own mechanism for that. */
  SCAN_BLOCK_LINE_CONTENT /* A real content line. *spaces_out receives the
                              count of leading spaces that the scan already
                              read. That count is >= block_indent. ctx->pos
                              sits just past those spaces, ready for the
                              caller to read the text of the line. */
} scan_block_line_t;

/*
 * Measure the leading spaces of one block scalar line and classify that line.
 * This function changes indent_determined, block_indent and
 * max_leading_blank_spaces exactly as parse_block_scalar_content and
 * parse_folded_scalar_content both need. It enforces three rules here, once,
 * and no hand-written copy in each caller has to stay in step. The first rule
 * is from YAML 1.2 section 6.1: a tab as indentation is ambiguous while the
 * indent is still undetermined. The second is from section 8.1.1: no leading
 * empty line may be more indented than the first line that is not empty. The
 * third is the rule that auto-detects the indent from the first content line.
 *
 * This function deliberately does not also unify what happens AFTER the
 * classification. That later work commits buffered blank lines, reads the
 * text of a content line, and moves past the newline at the end of the line.
 * The two callers do that work differently enough. One streams into a single
 * ybuf_t. The other collects into a line_t array and folds it after that.
 * One shared implementation would have to bring that difference back as
 * internal branches. Those branches would hide the rules about tabs and
 * indentation that this helper exists to state exactly once, and the gain
 * would be small.
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
  /* Clamp the value. Without the clamp, a line with more than INT_MAX leading
   * spaces overflows a signed int and reports nothing. current_col() clamps
   * in the same way, and its doc comment gives the full reason. */
  int spaces = space_count > (size_t)INT_MAX ? INT_MAX : (int)space_count;

  /* The doc comment of parse_block_scalar_content gives the full reason from
   * YAML 1.2 section 6.1 in more detail. A tab as the very first character of
   * this line, while block_indent is still undetermined, is truly ambiguous.
   * It is a hard error. After the scan reads even one real space, or after
   * block_indent is fixed, a tab is ordinary content. The step of the caller
   * that reads the content then handles it. */
  if (!*indent_determined && spaces == 0 && !at_end(ctx) && cur(ctx) == '\t') {
    parse_err(ctx,
              "tab cannot be used as block scalar indentation at "
              "position %zu",
              ctx->pos);
    return SCAN_BLOCK_LINE_ERROR;
  }

  /* A document-start marker ('---') or a document-end marker ('...') at
   * column 0 always ends the block scalar. YAML 1.2 section 6.9 makes a
   * document marker c-forbidden, so it is never valid scalar content. This
   * check must come before the code below that determines the indent and
   * decides between content and end. The indent of the block can be
   * auto-detected as exactly 0, for a scalar at the document root whose
   * content sits at column 0. A marker line then also has spaces == 0.
   * Without this check, that code treats the marker line as one more content
   * line. It takes the marker into the scalar text, reports nothing, and does
   * not end the scalar or the document. Every other parser of block content
   * in this file guards against this with at_doc_marker(). Those parsers are
   * parse_block_dictionary, parse_one_document, parse_plain_scalar_multiline,
   * and the two quoted-scalar folders. This one must guard too.
   * at_doc_marker() itself already needs column 0, so this check is a no-op
   * whenever spaces > 0. A line with more indent can never be a marker. */
  if (spaces == 0 && at_doc_marker(ctx)) {
    ctx->pos = line_start;
    return SCAN_BLOCK_LINE_END;
  }

  /* After block_indent is known, a line with MORE spaces than block_indent is
   * never "blank" for the purposes of chomping and folding. That holds even
   * when the line has no other content. A parser may chomp or fold a blank
   * line at the end. The l-strip-empty grammar production of YAML 1.2
   * matches such a line only when its indentation is AT MOST block_indent.
   * A line with more
   * indent is ordinary literal content, even when it holds only whitespace. A
   * leading blank line, while the indent is still undetermined, is always
   * still a candidate. The error check below runs once the indent IS
   * determined. It guarantees that the indentation of every leading blank
   * line was already <= the final block_indent. */
  bool is_blank =
      at_eol(ctx) && (!*indent_determined || spaces <= *block_indent);

  if (is_blank) {
    if (!*indent_determined && spaces > *max_leading_blank_spaces)
      *max_leading_blank_spaces = spaces;
    return SCAN_BLOCK_LINE_BLANK;
  }

  if (!*indent_determined) {
    if (spaces <= parent_indent) {
      /* The first line that is not blank is not more indented than the
       * parent. The scalar is therefore empty. */
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
    /* This line is less indented than the block. The scalar ends here. */
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
 *   scalar. This function uses it as the base for the block indent when the
 *   header gives no explicit indent.
 *
 * On success, *out receives a string from the heap, and the function returns
 * true. The caller must free that string with ctx->mp.
 */
static bool parse_block_scalar_content(parse_ctx_t *ctx, int parent_indent,
                                       chomp_t chomp, int explicit_indent,
                                       char **out) {
  /*
   * Determine the block indent. When the header gave an explicit indicator,
   * that indicator is relative to the parent. If not, auto-detect the indent
   * from the first content line that is not empty.
   */
  int block_indent = 0;
  bool indent_determined = (explicit_indent > 0);
  if (indent_determined) {
    /* The explicit indent indicator is always relative to an effective parent
     * indentation of at least 0. The value -1 is only a sentinel for "no
     * enclosing block context", which is the document root. The code below
     * uses that sentinel to let an auto-detected indentation be 0. The
     * arithmetic for an indicator has no such case with a negative base. A
     * reference parser confirms this. */
    int effective_parent = parent_indent < 0 ? 0 : parent_indent;
    block_indent = effective_parent + explicit_indent;
  }

  ybuf_t b;
  yb_init(&b, ctx->mp);

  /* Holds every trailing line that this function has not committed yet. For
   * each blank line it holds the extra indentation past block_indent, and
   * then a '\n', in that order. It also holds the single '\n' that ends the
   * most recent content line. It holds those bytes until one of two things
   * happens. More content arrives, and this function then copies the buffer
   * into b byte for byte, with the extra spaces of each line kept. Or the
   * scalar ends, and the chomp code below handles the buffer. This delay is
   * what lets the chomp mode decide, only after the whole scalar is read,
   * what happens to these trailing lines. CHOMP_KEEP keeps them in full.
   * CHOMP_CLIP reduces them to at most one newline. CHOMP_STRIP drops all of
   * them. */
  ybuf_t pending;
  yb_init(&pending, ctx->mp);
  bool have_content = false;

  /* True when a REAL line break character follows the most recent line in the
   * source. That line is a blank line or a content line. It is false when the
   * input ends right there. The b-chomped-last grammar production of YAML 1.2
   * governs the very last line of the scalar, under every chomp mode, CLIP
   * and KEEP included. That production is "b-as-line-feed | <end of file>".
   * The source can have no line break at all after the last line of the
   * scalar. This function then adds nothing for that line. It does not even
   * add a newline that a chomp mode would otherwise need. CLIP and KEEP must
   * not make a newline that the input never had. */
  bool had_trailing_newline = false;

  /* The same as had_trailing_newline, but this function updates it only for a
   * real CONTENT line. A trailing blank line never updates it. The
   * b-chomped-last production of YAML 1.2 governs the break after the very
   * last TEXT line of the scalar. It does not govern whatever trailing blank
   * line this function handles last. had_trailing_newline alone cannot tell
   * two cases apart. In the first, the last content line had a real break. A
   * blank line then follows it with no break of its own, and that blank line
   * ends at the end of the input. In the second, the last content line had no
   * break. Every line, blank or not, writes over had_trailing_newline. CLIP
   * is the only mode that needs this difference, and it reads this variable
   * instead. */
  bool content_had_trailing_newline = false;

  /* Holds the greatest indent of any LEADING empty line so far. This applies
   * while the indent of the block is still auto-detected, which means that
   * explicit_indent <= 0. YAML 1.2 section 8.1.1 says: "It is an error for
   * any of the leading empty lines to contain more spaces than the first
   * non-empty line." This value has a meaning only before indent_determined.
   * After real content, or an explicit indicator, fixes block_indent, the
   * indentation of a blank line has no limit. */
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
     * before this one. Keep the extra indentation of each of them. */
    yb_append_buf(&b, &pending);
    pending.len = 0;
    have_content = true;

    /* Skip exactly block_indent spaces. The scan above already read them, but
     * it may have read more than block_indent. */
    int extra = spaces - block_indent;

    /* Append the extra leading spaces of a line with more indent. */
    for (int i = 0; i < extra; i++) yb_append_c(&b, ' ');

    /* Read the content of this line. */
    while (!at_end(ctx) && !at_eol(ctx)) {
      yb_append_c(&b, cur(ctx));
      ctx->pos++;
    }

    /* This function also delays the newline at the end of this line, exactly
     * as it delays the newline of a blank line. This line may still be the
     * very last line of the scalar, and the chomp mode alone decides what
     * happens to it. See the doc comment of had_trailing_newline above. When
     * the source has no real line break here at all, because the input ends,
     * this function delays nothing for this line. */
    had_trailing_newline = !at_end(ctx);
    content_had_trailing_newline = had_trailing_newline;
    if (had_trailing_newline) yb_append_c(&pending, '\n');
    skip_newline(ctx);
  }

  /* Apply the chomp mode. */
  switch (chomp) {
    case CHOMP_STRIP:
      /* Keep no newline at the end. Drop all of pending. */
      break;
    case CHOMP_CLIP:
      /* Keep one newline at the end, but only when the scalar had content AND
       * the LAST CONTENT line really had a line break to keep. See the doc
       * comment of content_had_trailing_newline. The very last text line of
       * the content can also be the very last byte of the input, with no line
       * break at all. Such content gets no newline, also under CLIP. A
       * trailing blank line with no break of its own must not stop this
       * newline, because the content line before it did have a break. This
       * mode drops the contents of pending in both cases. Those contents may
       * hold the extra indentation of several blank lines. */
      if (have_content && content_had_trailing_newline) yb_append_c(&b, '\n');
      break;
    case CHOMP_KEEP:
      /* Keep every trailing line. That includes the lines before the first
       * content line, and the extra indentation of each of them. YAML 1.2
       * section 8.1.1.2 says that trailing empty lines are part of the
       * content of the scalar. That holds whether or not any line that is not
       * empty appears. pending already leaves out a final '\n' with no real
       * line break behind it. See the doc comment of had_trailing_newline.
       * This mode therefore needs no separate check here. */
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
 * The folded style folds one newline between two content lines to a space. It
 * keeps a blank line as a literal newline. It also keeps the newline before a
 * more indented line, and the newline after one.
 *
 * This function works in two steps over the literal result. It first collects
 * the lines. It then folds them by the folded rules of YAML 1.2.
 */
static bool parse_folded_scalar_content(parse_ctx_t *ctx, int parent_indent,
                                        chomp_t chomp, int explicit_indent,
                                        char **out) {
  int block_indent = 0;
  bool indent_determined = (explicit_indent > 0);
  if (indent_determined) {
    /* See the same comment in parse_block_scalar_content. The value -1 is
     * only a sentinel for auto-detection at the document root. It is not a
     * real base for the arithmetic of the indicator. */
    int effective_parent = parent_indent < 0 ? 0 : parent_indent;
    block_indent = effective_parent + explicit_indent;
  }

  /* Collect the lines into a dynamic array of {content, extra_indent,
   * is_blank}. text and text_len are a borrowed span straight into ctx->src.
   * They are not an owned copy. By the time that the scan below reaches a
   * content line, ctx->pos already moved past the leading spaces of that
   * line. Those spaces are the mandatory block_indent part, and also any
   * "extra" part of a more indented line. YAML 1.2 section 8.1.3 makes that
   * extra part real scalar content. The whole span, with the extra spaces in
   * it, therefore already sits in ctx->src as one unbroken, unchanged run of
   * bytes. Nothing changes or frees ctx->src before this function returns. A
   * borrowed span therefore prevents one allocate, copy and free cycle for
   * each line. A wrapped scalar can hold many thousands of lines.
   * fold_quoted_newline uses the same reasoning for quoted-scalar folding:
   * make no copy while the source stays stable. */
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

  /* The same counter as in parse_block_scalar_content. YAML 1.2 section 8.1.1
   * forbids a leading empty line that is more indented than the first line
   * which is not empty. That rule applies while the indent of the block is
   * still auto-detected. */
  int max_leading_blank_spaces = -1;

  /* See the doc comment of had_trailing_newline in
   * parse_block_scalar_content. This value describes the line that the loop
   * collected most recently. After the loop below ends, it therefore
   * describes the true final line of the scalar. */
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

    /* Grow the lines array when it is full. */
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
    /* A truly blank line never has an extra indent of its own. The
     * definition of is_blank above already excludes every line that is more
     * indented than block_indent, and it classifies such a line as ordinary
     * content. A blank line is therefore one of two things. It is a leading
     * blank line, while the indent is not yet determined. The error check
     * above then guarantees that its indentation was already <= the final
     * block_indent. Or it is a blank line that sits after the content, or
     * between two content lines. The definition of is_blank then already
     * makes its indentation <= block_indent. */
    ln->extra = is_blank ? 0 : spaces - block_indent;
    /* YAML 1.2 section 8.1.3 defines a "more-indented" line with the
     * s-nb-spaced-text production, which is s-indent(n) s-white nb-char*.
     * s-white is a space OR a tab. The first byte past the mandatory
     * block_indent spaces can be a tab. Such a line is therefore also
     * "spaced" for the choice between a fold and a newline. It adds no extra
     * literal SPACE of its own, so ln->extra stays 0, because the counter of
     * leading spaces above counts only ' '. The tab itself still becomes
     * ordinary line content below, and this flag does not change that. Only
     * the fold decision further below reads this flag. */
    ln->spaced =
        !is_blank && (ln->extra > 0 || (!at_end(ctx) && cur(ctx) == '\t'));
    ln->text = NULL;
    ln->text_len = 0;

    if (!is_blank) {
      /* ctx->pos already sits ln->extra bytes past the point where the
       * "extra" leading spaces of this line begin. Those are the spaces past
       * block_indent. The contract of scan_block_scalar_line says so. Those
       * spaces, and the real content right after them, are therefore already
       * one unbroken, unchanged run of bytes in ctx->src. See the doc comment
       * of line_t above. span_start is therefore ln->extra bytes behind
       * ctx->pos, and no copy is needed. */
      const char *span_start = ctx->src + (ctx->pos - (size_t)ln->extra);
      while (!at_end(ctx) && !at_eol(ctx)) ctx->pos++;
      ln->text = span_start;
      ln->text_len = (size_t)((ctx->src + ctx->pos) - span_start);
    }
    /* See the doc comment of had_trailing_newline in
     * parse_block_scalar_content. The question is whether a real line break
     * follows THIS line in the source, or whether the input ends right there.
     * The answer matters for whichever line is the very last line of the
     * scalar. */
    had_trailing_newline = !at_end(ctx);
    ctx->pos = line_start;
    skip_to_eol(ctx);
  }

  /* Now fold the lines into the output buffer. */
  ybuf_t b;
  yb_init(&b, ctx->mp);

  size_t trailing_blanks = 0;
  bool have_content = false;
  /* True when the line before this one, which was not blank, had more
     indent. */
  bool prev_extra = false;

  for (size_t i = 0; i < lines_len; i++) {
    line_t *ln = &lines[i];
    if (ln->blank) {
      trailing_blanks++;
      continue;
    }

    if (trailing_blanks > 0) {
      /* There are blank lines before this line. They are leading lines, or
       * lines between two content lines. Write the extra indentation of each
       * one, when it has any, as literal spaces. Write those spaces right
       * before the newline of that same line. */
      for (size_t j = i - trailing_blanks; j < i; j++) {
        for (int k = 0; k < lines[j].extra; k++) yb_append_c(&b, ' ');
        yb_append_c(&b, '\n');
      }
      /* YAML 1.2 section 8.1.3 says that a move into a "spaced" run of more
       * indented lines, or out of one, always costs one literal newline. That
       * newline comes ON TOP OF the blank lines that separate the two chunks.
       * It adds to the newlines of the blank lines that this code wrote
       * above. It does not replace them. A reference parser confirms this.
       * This rule applies only when a content line comes before this break,
       * so that the break can sit next to it. The leading blank lines before
       * the very first content line of the scalar have no such line before
       * them. have_content therefore guards this check, in the same way as it
       * already guards the same check in the other branch below. */
      if (have_content && (ln->spaced || prev_extra)) yb_append_c(&b, '\n');
    } else if (have_content) {
      /* A line break next to a more indented line stays a newline. YAML 1.2
       * section 8.1.1.2 says so. That more indented line is the current line,
       * or the line before it that was not blank. In every other case the
       * break folds to a space. */
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

  /* Apply the chomp mode. lines[] must stay alive through the use that
   * CHOMP_KEEP makes of it below, and so must the extra indentation of each
   * line. This function therefore frees lines[] after this switch, and not
   * right after the fold. */
  switch (chomp) {
    case CHOMP_STRIP:
      break;
    case CHOMP_CLIP:
      /* A trailing_blanks greater than 0 means that at least one blank line
       * follows the last real content line. That can only be true when a real
       * line break already separated them in the source. This code must
       * therefore keep the break of that content line, also when the LAST
       * trailing blank line has none of its own. Every line, a trailing blank
       * line included, writes over had_trailing_newline, so that value cannot
       * tell the two cases apart on its own. The KEEP mode below has the same
       * check. See the doc comment of had_trailing_newline in
       * parse_block_scalar_content for the case that this code still honors:
       * no newline at all when the source had none. */
      if (have_content && (trailing_blanks > 0 || had_trailing_newline))
        yb_append_c(&b, '\n');
      break;
    case CHOMP_KEEP:
      /* First write the newline that ends the last real content line. That
       * newline is always there when any trailing blank line follows it. Only
       * the true final line of the scalar can lack a real line break in the
       * source. When a trailing blank line exists, that blank line is the
       * final line, and this content line is not. */
      if (have_content && (trailing_blanks > 0 || had_trailing_newline))
        yb_append_c(&b, '\n');
      /* Then write each trailing blank line. Those are the lines[] entries
       * after the last content line. The main fold loop above never wrote
       * them, because no further content line arrived to trigger that write.
       * For each of them, write the extra indentation and then the newline.
       * Only the very last one depends on had_trailing_newline, for the same
       * reason as above. */
      for (size_t j = lines_len - trailing_blanks; j < lines_len; j++) {
        for (int k = 0; k < lines[j].extra; k++) yb_append_c(&b, ' ');
        if (j + 1 < lines_len || had_trailing_newline) yb_append_c(&b, '\n');
      }
      break;
  }

  /* Free the lines[] array itself. The text and text_len of each entry are a
   * borrowed span into ctx->src. See the doc comment of line_t above. They
   * are not an owned allocation, so there is nothing to free for each
   * entry. */
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
 * Scan the plain-scalar content of one physical line, from the current
 * position. Append the characters to *b, which the caller already
 * initialized. parse_plain_scalar, for the first line, and
 * parse_plain_scalar_multiline, for every continuation line, share this
 * function. The same rules end a token for each character on a continuation
 * line as on the first line.
 *
 * line_start is the position where the content scan of this line began. This
 * function needs it so that it does not treat a ':' at the very first scanned
 * character as a character with content before it. Only the '#' comment check
 * below uses this, and that check does not apply to ':'.
 *
 * Returns true when the scan hits a real end of the token. That end is an
 * inline comment, or a ':', or a flow character that indicates a value or a
 * collection. It then leaves ctx->pos exactly at that character and does not
 * read it. Returns false when the line ended at the end of the line, or at
 * the end of the input, and it then leaves ctx->pos there.
 */
_CYAML_PARSE_HOT bool scan_plain_scalar_line(parse_ctx_t *ctx, bool in_flow,
                                             size_t line_start, ybuf_t *b) {
  while (!at_end(ctx)) {
    char c = cur(ctx);

    /* The end of the line ends this line, and not the whole scalar. */
    if (c == '\n' || c == '\r') return false;

    /* An inline comment ends the token. It is a space and then a '#'. */
    if (c == '#' && ctx->pos > line_start) {
      char prev = ctx->src[ctx->pos - 1];
      if (prev == ' ' || prev == '\t') return true;
    }

    /* A colon and then a space, or a colon at the end of the line, ends the
     * token. The colon is the value indicator of a dictionary. */
    if (c == ':') {
      if (ctx->pos + 1 >= ctx->len) return true; /* colon at very end */
      char next = ctx->src[ctx->pos + 1];
      if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
        return true;
      /* In a flow context a ':' is also a value indicator when a flow
       * terminator follows it. The check above already handles whitespace. */
      if (in_flow) {
        char safe = ctx->src[ctx->pos + 1]; /* pos+1 < len guaranteed above */
        if (safe == ',' || safe == ']' || safe == '}') return true;
      }
    }

    /* Flow terminators. */
    if (in_flow && (c == ',' || c == ']' || c == '}')) return true;

    yb_append_c(b, c);
    ctx->pos++;
  }
  return false; /* reached end of input, not a real terminator */
}

/*
 * Parse a plain scalar that sits on one line. The scalar ends at the end of
 * the line, at an inline comment (' #'), or at a value indicator (': ' or
 * ':\n').
 *
 * In a flow context (in_flow=true) the scalar also ends at ',', ']' and '}'.
 * It ends at a ':' when whitespace or a flow terminator follows that ':'.
 * A bare ':' with none of those characters after it is part of the scalar.
 * YAML 1.2 section 7.3.3 says so. A URL such as "http://example.com" is one
 * example.
 *
 * When hit_real_terminator is not NULL, it reports how the scalar stopped.
 * The value true means a real terminator. The value false means the end of
 * the line or the end of the input. Some callers try a continuation over
 * more than one line. See parse_plain_scalar_multiline. Those callers try it
 * only in the false case. An implicit dictionary key always stays on one
 * line. Such a key must never try a continuation, whatever follows it.
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
 * Extend the first line of a plain scalar that the parser already read, with
 * more continuation lines. YAML 1.2 section 7.3.3 defines this with the
 * ns-plain-multi-line production. The parser calls this function only from a
 * value position or a standalone-node position. It calls it only after it
 * confirms that the first line does NOT introduce a dictionary key. An
 * implicit key always stays on one line. See the doc comment of
 * parse_plain_scalar. The parser must therefore look at the first line for a
 * ':' after it, before it can decide whether to try a continuation at all.
 *
 * This function takes ownership of first_owned. That is the text of the
 * first line, which parse_plain_scalar already read. The parameter
 * first_terminated tells it whether that call hit a real terminator, or only
 * the end of the line or the end of the input. The function tries a
 * continuation only in the second case. On success *out receives the final
 * text. That text is still first_owned itself, unchanged, when no
 * continuation applied.
 *
 * The function reads further lines only when the first line ended at the end
 * of the line or at the end of the input. It then reads every line that is
 * blank, and every line that is indented more than `indent`. The parameter
 * `indent` is the indent of the enclosing block. parse_node and
 * parse_block_map_node already pass that same parameter through. A value
 * continuation is therefore never confused with an unrelated sibling entry
 * at or below that indent.
 *
 * The fold rules differ from a '>' folded block scalar. The line-fold
 * grammar of a plain scalar is s-flow-folded. It has no exception that lets
 * a line with more indent keep its own newline. Every move from one line to
 * the next folds to a single space. A run of blank lines folds to one
 * newline for each blank line. This is true whatever the extra indent of a
 * continuation line is. The leading whitespace of a continuation line past
 * `indent` is therefore not kept.
 *
 * The continuation stops, and reads nothing more, in three cases. The first
 * is a document marker ('---' or '...') at column 0. The second is a line
 * indented at or below `indent`. The third is a continuation line that
 * itself hits a real terminator. That third case mirrors the rules that end
 * the first line.
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
    /* Clamp the value. Without the clamp, a continuation line with more than
     * INT_MAX leading spaces overflows a signed int and reports nothing.
     * current_col() clamps in the same way. scan_block_scalar_line clamps its
     * own separate counter of leading spaces in the same way too. */
    int spaces =
        line_space_count > (size_t)INT_MAX ? INT_MAX : (int)line_space_count;

    if (at_end(ctx)) {
      ctx->pos = save;
      break;
    }
    if (at_blank_line(ctx)) {
      /* A blank line. It counts toward the fold, so the scan goes on. The
       * remaining whitespace of such a line can be a tab and not more
       * spaces. at_blank_line() still recognizes the line as blank in that
       * case. A bare at_eol() check on the byte right after the leading
       * spaces does not. Such a check cannot see past the tab to the real
       * line break behind it. The fold of a double-quoted or single-quoted
       * scalar handles this in the same way. See the doc comment of
       * at_blank_line. The code below moves past the remaining whitespace,
       * up to the line terminator itself but not past it. The
       * skip_newline() call at the top of the next iteration then reads
       * exactly one line break. Without this, it reads the terminator of
       * this line and then the leading bytes of the next line. */
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
    /* The block needs its own indentation, which is spaces only. More
     * separator whitespace can follow it. That whitespace is spaces, tabs,
     * or both. The s-separate-in-line production of YAML 1.2 is s-white+,
     * and s-white is a space or a tab. Such whitespace is not scalar content
     * itself, so this code skips it. The code above already read every space
     * of the indentation, however far past indent those spaces went. Without
     * this skip, scan_plain_scalar_line() below gets a tab in this position
     * and treats it as a literal content byte. */
    while (!at_end(ctx) && (cur(ctx) == ' ' || cur(ctx) == '\t')) ctx->pos++;
    if (in_flow && (cur(ctx) == ']' || cur(ctx) == '}' || cur(ctx) == ',')) {
      /* A flow terminator can never be the content of a plain scalar. Take
       * a line whose first character that is not whitespace is such a
       * terminator. That line is the end of the enclosing flow collection
       * or entry. It is not a continuation of this scalar. For example,
       * "baz\n]" must give the value "baz". It must not give "baz " with a
       * fold space at the end, taken from a ']' that was never content. */
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
        /* This ':' value indicator sits on a new line, and the scan read
         * nothing else from that line first. It is therefore not a
         * continuation of this scalar. The ':' of an implicit key must stay
         * on the SAME line as the last real content of that key. Two
         * independent reference parsers agree. They reject "{k\n: v}". They
         * accept "{k: \nv}", where the colon follows the key at once on its
         * own line and only the VALUE folds onto the next line. Leave the
         * ':' for the key-detection logic of the caller. That logic must see
         * the input as if this try over more than one line never looked past
         * the line before. */
        ctx->pos = save;
        break;
      }
    }
    if (cur(ctx) == '#') {
      /* A comment line is a line whose first character that is not
       * whitespace is a '#'. Such a character can never be the content of a
       * plain scalar on its own, here or anywhere else. A comment line is
       * therefore not a continuation of this scalar. Leave it for the
       * ordinary skip_ws_comments of the caller to read in the normal way.
       * That code must see the input as if this try over more than one line
       * never looked past the line before. */
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

    /* Scan the content of this line straight into the shared buffer b. The
     * code does not use one throwaway ybuf_t for each line. It uses the same
     * strip_floor technique as fold_quoted_newline, which folds a
     * double-quoted or single-quoted scalar. A long wrapped scalar therefore
     * needs no allocate, copy and free cycle for each continuation line.
     * line_floor marks where the content of this line starts inside b. That
     * point is right after the fold separators that the code appended above.
     * The trim of trailing whitespace below can therefore never reach back
     * into the already folded content of an earlier line. */
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

/* A forward declaration. This file defines the function below, beside
 * parse_flow_dictionary, which is one of its users. try_parse_scalar_dict_key
 * also needs it here. That function handles an alias as a key, and a flow
 * collection as a key. */
static char *node_to_dict_key_string(parse_ctx_t *ctx, cyaml_node_t *key_node,
                                     const char *context_label);

/* A forward declaration. This function is the canonical-mode variant of the
 * flow serializer, and this file defines it below beside that serializer.
 * The declaration is needed here, because node_to_dict_key_string is its
 * only caller in this file and sits well before that section. */
static char *serialize_flow_canonical(cyaml_node_t *n, bool *too_long);

/* Forward declarations. try_parse_scalar_dict_key needs them for its own
 * handling of a flow collection as a key. A flow list or flow dictionary is
 * unambiguous on its own. It needs no "was this a key after all" rewind, the
 * way a bare plain scalar does. A direct call of these two functions here is
 * therefore deliberate, instead of a call through the general parse_node
 * dispatch. It leaves out the key-against-value ambiguity handling of
 * parse_node completely. That handling does not apply to a flow collection
 * at all. */
static cyaml_node_t *parse_flow_list(parse_ctx_t *ctx, int indent);
static cyaml_node_t *parse_flow_dictionary(parse_ctx_t *ctx, int indent);

/*
 * Answer whether the current position is a real value continuation for a
 * block construct that `indent` governs. The caller must already have
 * skipped past the leading whitespace, at the start of a line that is not
 * blank. The other case is a position that belongs to a sibling entry. The
 * value or content of this construct is then absent, which means null. The
 * answer is true whenever the position is more indented than `indent`.
 *
 * The flag seq_ok_at_indent also permits exactly `indent` itself. It permits
 * it only when a block sequence entry ('- ') starts there. YAML 1.2 section
 * 8.2.2 gives this one exception to the rule "a value must be more indented
 * than its key". It is an exception only for the value of a MAPPING key. The
 * caller must pass true only when `indent` is the indent of a mapping.
 * That value is the map_indent of parse_block_map_node, or that same value
 * passed on through an anchor or a tag that decorates the value. The caller
 * must never pass true for the indent of a sequence, which is the
 * seq_indent of parse_block_list. At exactly the indent of a sequence, a
 * further '-' is a sibling element of that same sequence. It is not nested
 * content. This holds whatever anchor or tag sits between them.
 */
static bool at_block_value_col(parse_ctx_t *ctx, int indent,
                               bool seq_ok_at_indent) {
  /* A tab in the leading whitespace is separation, so only the spaces
   * before it count. See line_space_indent(). */
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
 * True when the current position holds a bare '-' block sequence indicator.
 * Whitespace or the end of the input must follow that '-' at once. The other
 * case is a '-' that is only the first character of ordinary plain scalar
 * content, for example "-1" or "-foo".
 */
static bool at_bare_seq_indicator(parse_ctx_t *ctx) {
  if (at_end(ctx) || cur(ctx) != '-') return false;
  char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
  return nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0';
}

/*
 * True when the current character is a valid first character for a plain
 * scalar in a flow context. YAML 1.2 section 6.6 defines this with the
 * ns-plain-first(c) production. Every character is valid except '-' and '?'.
 * Those two also need an ns-plain-safe(c) character right after them. Such a
 * character is one that may itself appear INSIDE a flow plain scalar. Three
 * things fail that rule. They are whitespace, the end of a line or of the
 * input, and any flow indicator (',', '[', ']', '{' and '}'). A bare "-" or
 * "?" with one of those right after it therefore has no valid reading as a
 * plain scalar in a flow context. The lone "-" in "[-, -]" or in "[-]" is
 * one example. A reference parser confirms this.
 *
 * A block context has no matching gap. A '-' or '?' with whitespace or the
 * end of the line right after it is already taken earlier there. The block
 * sequence dispatch, or the explicit-key dispatch, takes it before anything
 * calls this function.
 *
 * This function deliberately leaves ':' out, although YAML 1.2 gives it the
 * same grammar restriction. scan_plain_scalar_line already has its own,
 * earlier special case for a ':' that it reaches with nothing scanned yet.
 * That case returns an empty scalar and leaves the ':' unread. The
 * bare-colon-key shorthand of parse_flow_list depends on exactly that
 * mechanism. An example is "[: value]", which holds an empty implicit key.
 * A rejection of ':' here would block that mechanism before it ever runs.
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
 * Skip a tag token that starts at a '!'. The caller already confirmed that
 * the '!' is at the current position. This function handles the shorthand
 * forms ("!", "!foo" and "!!foo"). It also handles the verbatim form
 * ("!<...>"). The content of the verbatim form runs through the closing '>'.
 * It may validly hold characters that the character set of a shorthand tag
 * excludes, a literal ',' among them. The verbatim syntax exists to permit
 * exactly such characters. The two forms therefore cannot share one rule for
 * where the token ends.
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
  /* A shorthand tag token never holds a flow indicator. YAML 1.2 section
   * 5.5 says so, because ns-tag-char excludes c-flow-indicator. Without this
   * check, a tag with ',', '[', ']', '{' or '}' right after it in a flow
   * context swallows that indicator as part of the tag name. The text
   * "!!str," is one example. The flow-terminator scan of the caller then
   * loses its place. */
  while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
         cur(ctx) != '\n' && cur(ctx) != '\r' && cur(ctx) != ',' &&
         cur(ctx) != '[' && cur(ctx) != ']' && cur(ctx) != '{' &&
         cur(ctx) != '}')
    ctx->pos++;
}

/*
 * Percent-decode the content of a verbatim tag in place. The ns-uri-char
 * production of YAML 1.2 sections 5.6 and 5.7 permits a "%" and then two hex
 * digits. That is an escape for characters that the URI grammar otherwise
 * excludes. This function refuses a %00 escape outright. The tag of a node
 * is a plain char* that ends with a NUL byte, with no separate length field.
 * A decode into a real NUL byte inside the tag would therefore truncate the
 * tag and report nothing. That is the exact shape of data corruption that
 * the policy of this library against NUL bytes inside a value prevents.
 * Returns false on a malformed escape, on an escape that makes a NUL
 * byte, and on escapes that decode to bytes that are not well-formed UTF-8.
 * It sets a parse error on every one of those paths. It returns true otherwise,
 * and shortens text in place as needed. */
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
  /* Percent-escapes name the bytes of the UTF-8 encoding of a character.
   * Escapes that decode to a byte sequence that is not well-formed UTF-8
   * name no character, and a tag that held them could not be stored as text.
   * Only a decoded byte at 0x80 or above can break the encoding: the raw
   * bytes around it are already well-formed. */
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
 * Parse a tag token that starts at a '!', and resolve it to one fully
 * normalized tag string. The caller already confirmed that the '!' is at the
 * current position. This function handles the shorthand forms ("!", "!foo",
 * "!!foo" and "!name!foo"). It also handles the verbatim form ("!<...>"). It
 * splits the token into exactly the same pieces as skip_tag_token. It keeps
 * and resolves the content, where skip_tag_token only skips it.
 *
 * On success *resolved_out receives an owned, fully resolved tag string, and
 * the caller must free it. For a bare "!" non-specific tag it receives NULL.
 * Such a tag forces no type, and the parser treats it exactly as no tag at
 * all.
 *
 * Returns false on five kinds of failure. The first is a malformed token.
 * The second is a shorthand handle with no suffix. The third is a named
 * handle that nothing defines. The fourth is an allocation failure. The
 * fifth is a malformed percent-escape. An escape that makes a NUL byte, and
 * escapes that decode to bytes that are not well-formed UTF-8, count as the
 * fifth kind. The raw spelling of those bytes never reaches this function:
 * parse_common() refuses it for the whole stream. Every failure path, the
 * allocation failure included, sets a parse error with parse_err().
 *
 * That always-set error matters past the direct callers of this function.
 * try_parse_scalar_dict_key() calls it speculatively. That function reads
 * ctx->error[0] alone to tell a real error apart from "not a key after all".
 * It does so because this path does not restore ctx->pos.
 */
static bool parse_tag_token(parse_ctx_t *ctx, char **resolved_out) {
  size_t tag_start = ctx->pos;
  ctx->pos++; /* consume the leading '!' */

  if (!at_end(ctx) && cur(ctx) == '<') {
    /* The verbatim form, which is !<content> */
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
    /* The emptiness test above reads the RAW span. This one reads the string
     * that the node will actually carry, after the copy and the decode. The
     * two can only differ if some byte shortens the stored form, and the
     * control-byte rejection in the scan loop above now makes that
     * impossible. This test is therefore a standing guard and not a live
     * one: no current path reaches it. It states the invariant that matters,
     * which is that a stored tag is never the empty string.
     *
     * cyaml_node_set_tag refuses an empty tag for exactly this invariant,
     * because the serializer writes a custom tag in its verbatim "!<...>"
     * form and would emit "!<>", which this very function then refuses to
     * parse again. The guard belongs on the parse path too, so that the
     * invariant holds however a tag arrives rather than only through the
     * public setter. */
    if (content[0] == '\0') {
      parse_err(ctx, "empty verbatim tag at position %zu", tag_start);
      _ccol_mem_free(ctx->mp, content);
      return false;
    }
    *resolved_out = content;
    return true;
  }

  /* The shorthand form. This code decides which handle introduces the
   * token. The choices are the primary "!", the secondary "!!", and a named
   * "!name!". It checks the secondary handle first, because that check is
   * unambiguous. A bare second '!' can never begin the word-char name of a
   * named handle, because such a name is never empty. Otherwise a
   * speculative look ahead scans ns-word-char+, which is alphanumeric
   * characters and '-'. It looks for a closing '!' to confirm a named
   * handle. When it finds none, the code falls back to the primary handle.
   * The look ahead leaves ctx->pos unchanged in that case. */
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

  /* A shorthand tag token never holds a flow indicator. YAML 1.2 section
   * 5.5 says so, because ns-tag-char excludes c-flow-indicator. The same
   * comment in skip_tag_token gives the full reason. */
  size_t suffix_start = ctx->pos;
  while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
         cur(ctx) != '\n' && cur(ctx) != '\r' && cur(ctx) != ',' &&
         cur(ctx) != '[' && cur(ctx) != ']' && cur(ctx) != '{' &&
         cur(ctx) != '}') {
    ctx->pos++;
  }
  size_t suffix_len = ctx->pos - suffix_start;

  if (suffix_len == 0 && handle_len == 1) {
    /* A bare "!" with nothing after it at all. This is the non-specific
     * tag, which is the c-non-specific-tag production. It is a separate
     * grammar production from a shorthand tag. A shorthand tag needs
     * ns-tag-char+ after its handle. */
    _ccol_mem_free(ctx->mp, handle);
    *resolved_out = NULL;
    return true;
  }
  if (suffix_len == 0) {
    /* A "!!" or a "!name!" with nothing after it. This is not a valid
     * shorthand tag. The c-ns-shorthand-tag production needs at least one
     * ns-tag-char. */
    parse_err(ctx, "tag handle '%s' with no suffix at position %zu",
              _cyaml_echo(handle), tag_start);
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

  /* The grammar of the shorthand suffix is ns-tag-char, from YAML 1.2
   * section 5.5. It derives from ns-uri-char, exactly as the content of the
   * verbatim form does. A suffix may therefore hold the same "%"
   * ns-hex-digit ns-hex-digit escape syntax. This code decodes the suffix
   * into its own buffer first, before it joins the suffix to the prefix.
   * percent_decode_tag_inplace shortens in place, so the decoded length can
   * only be <= suffix_len. The verbatim branch decodes before use in the
   * same way. Neither branch copies the raw source bytes unchanged. */
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
 * Register anchor_name so that it points at a value. When anchor_name is
 * NULL this function does nothing and returns true. When anchor_value is not
 * NULL, the anchor points at it and this function takes ownership of it.
 * Otherwise the anchor points at a fresh CYAML_STRING clone of key_str.
 *
 * This is the shared step that says "a later '*name' alias resolves to this
 * key text". Every place where a dictionary key may itself carry an anchor
 * needs it. Those places are the '&' and '!' dispatch branches in
 * parse_node, and the matching branch for a later entry in
 * parse_one_dict_entry_key. All of them get anchor_name, key_str and
 * anchor_value from try_parse_scalar_dict_key.
 *
 * When anchor_value is not NULL, it is the real, fully typed node of the
 * key. For a flow-collection key that is the actual list or mapping. For a
 * plain-scalar key that is the scalar with its implicit type. The caller
 * builds it for one reason. An alias to this anchor must resolve to the
 * same kind of value as an anchor in an ordinary value position. Without
 * it, the alias always collapses to the plain string that the dictionary
 * keeps as its own key. A quoted-scalar key has no such richer form, because
 * a quoted scalar is always a string that matches key_str exactly. Its
 * caller therefore passes NULL, and this function then builds the
 * CYAML_STRING wrapper straight from key_str.
 *
 * When anchor_name is NULL, this function destroys any anchor_value that the
 * caller passed in. It does not leak it. This is a standing defence: no
 * current caller does this.
 *
 * This function always frees anchor_name. When anchor_name is not NULL, it
 * also always consumes anchor_value, by a store or by a destroy. It returns
 * false only on an OOM. A parse error is already set on that path, either by
 * this function or by anchors_store().
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
  bool stored = anchors_store(ctx, anchor_name, anchor_val);
  _ccol_mem_free(ctx->mp, anchor_name);
  return stored;
}

/* True when ctx->pos, after any inline whitespace, is a ':' value indicator
 * of a block mapping: a ':' that a space, a tab, a line break or the end of
 * the input follows. It leaves ctx->pos on that ':' when it answers true,
 * and past the whitespace when it answers false. */
static inline __attribute__((always_inline)) bool at_block_value_indicator(
    parse_ctx_t *ctx) {
  skip_inline_ws(ctx);
  if (at_end(ctx) || cur(ctx) != ':') return false;
  char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
  return after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
         after == '\0';
}

/*
 * Try to parse a block dictionary key, through its own ':' indicator. The
 * key is a plain scalar, a double-quoted scalar, a single-quoted scalar, a
 * flow collection, or a resolved alias. An anchor, a tag, or both may
 * decorate it. Two places share this function. The first is the '&' dispatch
 * of parse_node, for the very first entry of a dictionary. That dispatch uses
 * this function when the anchored node introduces a key and is not a
 * standalone value. The second place is parse_one_dict_entry_key, for every
 * later entry.
 *
 * A tag on the key applies exactly as it does on a value, and as it does on
 * a key of a flow dictionary or an explicit "? key": the key node is built
 * through finalize_scalar_node() or finalize_collection_node() with the
 * resolved tag, and its canonical text becomes the dictionary key. "!!str
 * 010" and "!!str 10" are therefore two keys, and "!!int abc" is a parse
 * error.
 *
 * The ':' check comes before any work that only a real key needs: the clone
 * of an alias, the typing of a scalar, and the canonical text of a flow
 * collection. The same text is far more often a value ("- *a", "- [..]")
 * than a key. A canonical text also has a length limit
 * (CYAML_MAX_CANONICAL_KEY_LEN) that a value does not, so it must never be
 * computed for a value.
 *
 * On a real match this function returns true and leaves ctx->pos just past
 * the ':' that it read. It fills the out parameters as follows.
 *
 * *anchor_name_out receives an owned anchor name when one was there. It
 * receives NULL for an alias key, and for a key with no decoration. The
 * caller must still call anchors_store() once it knows the final node of the
 * key.
 *
 * *key_out receives the owned key text.
 *
 * *is_merge_candidate_out, when it is not NULL, receives whether this key is
 * a real merge-key trigger. See below.
 *
 * *anchor_value_out, when it is not NULL, receives the real, fully typed
 * node of the key, which the caller then owns. It receives that node only
 * when anchor_name_out came back not NULL AND a richer form than the plain
 * key string exists. Such a form is the actual list or mapping of a
 * flow-collection key, the implicitly typed scalar of a plain-scalar key, or
 * the node of a tagged key. Otherwise it receives NULL. The caller must then
 * register the anchor as a plain string of the key text. See
 * register_key_anchor.
 *
 * On no match this function returns false. It restores ctx->pos to exactly
 * where it was on entry, and it changes no out parameter. ctx->error[0]
 * tells a real parse error apart from an ordinary "this was not a key after
 * all" result. A real error is a malformed quoted scalar, a malformed tag, a
 * tag that disagrees with the key, or an alias to an anchor that is unknown.
 *
 * *is_merge_candidate_out is true only when the key text is exactly "<<"
 * and one of two things holds. The first is a key from the plain-scalar
 * branch that carries no tag at all. That mirrors the "only a plain scalar
 * with no tag" rule of implicit core-schema type resolution, which resolves
 * such a "<<" to the merge type. The second is a key whose tag is the core
 * merge tag (!!merge, or its verbatim form), from the plain or a quoted
 * branch: the explicit tag names the merge type directly. A quoted "<<"
 * with no tag, and a "<<" with any other tag, is an ordinary literal key.
 * PyYAML reads every one of these cases the same way. The alias-as-key and
 * the flow-collection-as-key branches never give a merge key. An anchor
 * on a "<<" does NOT disqualify it. An anchor is a separate c-ns-properties
 * concern with no tie to typing. The text "&x <<: *y" is therefore still a
 * real merge-key trigger.
 */
static bool _try_parse_scalar_dict_key(parse_ctx_t *ctx, char **anchor_name_out,
                                       char **key_out,
                                       bool *is_merge_candidate_out,
                                       cyaml_node_t **anchor_value_out) {
  size_t saved_pos = ctx->pos;
  char *anchor_name = NULL;

  /* An alias may itself serve directly as a key, for example "*b : *a".
   * This code resolves it now. It then turns its value into a canonical
   * string key, exactly as it does for any other typed scalar. There is no
   * anchor name to register. The clone happens only after the ':' check,
   * because "- *a" is far more common than "*a : v". */
  if (!at_end(ctx) && cur(ctx) == '*') {
    ctx->pos++;
    char *alias_name = NULL;
    if (!parse_anchor_name(ctx, &alias_name)) return false;
    cyaml_node_t *aliased = anchors_lookup(ctx, alias_name);
    if (!aliased) {
      parse_err(ctx, "unknown alias '*%s' at position %zu",
                _cyaml_echo(alias_name), ctx->pos);
      _ccol_mem_free(ctx->mp, alias_name);
      return false;
    }
    _ccol_mem_free(ctx->mp, alias_name);

    if (!at_block_value_indicator(ctx)) {
      ctx->pos = saved_pos;
      return false;
    }
    ctx->pos++; /* consume ':' */

    cyaml_node_t *clone = parse_clone(aliased);
    if (!clone) {
      /* cyaml_clone() never calls parse_err() on an OOM. Each caller makes
       * its own check of the form "ctx->error[0] means a real error, and no
       * error means not a key after all". See the doc comment of this
       * function. Without an explicit error here, that check classifies
       * this OOM as "not a key" and goes on from ctx->pos, which is already
       * past the ':'. The parser then loses its place, instead of failing
       * the whole document cleanly. */
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

  /* The c-ns-properties(n,c) production permits an anchor and a tag in
   * either order. It is c-tag-property s-separate c-ns-anchor-property, or
   * the reverse. Each one appears at most once, so two passes are enough,
   * whatever the order is. The !had_tag guard protects the tag branch, in
   * the same way as the !anchor_name guard protects the anchor branch.
   * Without it, this code reads a second tag as part of the property list
   * and reports nothing.
   *
   * key_tag is the resolved tag, owned by this function until the key node
   * is built. It stays NULL for the non-specific "!", which forces no type,
   * exactly as on a value. */
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
      /* parse_tag_token() sets a parse error on every failure, so an
       * undefined %TAG handle, a malformed verbatim tag and a
       * percent-escaped null byte are real errors here, exactly as they are
       * anywhere else that a tag appears. */
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
   * the tag:yaml.org,2002:merge type defines. Any other tag keeps it an
   * ordinary key. */
  const bool key_has_merge_tag =
      key_tag && strcmp(key_tag, _CYAML_TAG_MERGE) == 0;
  if (!at_end(ctx) && cur(ctx) == '!' && had_tag) {
    /* A second tag on the same key. The c-ns-properties production permits
     * at most one tag for each node. This code reports that explicitly, as
     * a real error that the parser cannot recover from. parse_node_inner
     * makes the same check for a value position. Without this check, the
     * input falls through to the plain-scalar dispatch below. That dispatch
     * has no guard of its own against a leading '!', unlike '&' and '*',
     * which the code just below excludes. It therefore takes the text of
     * the unread second tag into the key string itself, and reports
     * nothing. */
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
      c == '@' || c == '`') {
    /* The characters '%', '@' and '`' are c-indicator characters, like the
     * '#' just above. YAML 1.2 section 6.6 lists them under ns-plain-first.
     * They have no valid reading as a plain scalar key at all. The
     * characters '|', '>', '&' and '*' are different. Each of those is a
     * real indicator for ANOTHER kind of node, which a caller may still
     * want to read this position as. Those kinds are a block scalar, an
     * anchor and an alias. The characters '%', '@' and '`' have no such
     * other reading. This branch calls no parse_err() and still gives a
     * clear, specific error. The caller reads this position as an ordinary
     * value. That path goes back through the same check for these three
     * characters in parse_node_inner. */
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
      /* The same one-line rule applies to a flow-collection key just
       * below. An implicit key always stays on one line, which the
       * ns-s-implicit-yaml-key production states. A quoted scalar is no
       * exception. Two independent reference parsers confirm this. */
      _ccol_mem_free(ctx->mp, key_str);
      ctx->pos = saved_pos;
      _ccol_mem_free(ctx->mp, key_tag);
      _ccol_mem_free(ctx->mp, anchor_name);
      return false;
    }
  } else if (c == '[' || c == '{') {
    /* A flow collection is itself a valid implicit block mapping key, and
     * it is not a scalar. YAML 1.2 section 8.2.2 permits ns-flow-node in a
     * block-key context, and not only a scalar. A flow collection is
     * unambiguous on its own. Its '[' or '{' and ']' or '}' boundaries need
     * no look ahead and rewind, the way a bare plain scalar does. This code
     * therefore parses it directly, and not through parse_node. */
    size_t coll_start = ctx->pos;
    /* This function serves only a block-context implicit key. The check
     * below always holds such a key to one physical line, whatever indent
     * value this call uses. Any newline inside the collection fails that
     * check, in every case. The indent-based validation of flow_skip_ws
     * would therefore add nothing. This call passes -1, which is the most
     * permissive value, because no more meaningful enclosing indent is
     * available at this call site.
     *
     * The parse runs as a speculation. The collection is a key only when a
     * ':' follows it, and otherwise the caller reads the same text again as
     * a value. The anchors that the collection defines, and the budget that
     * it charges, must then be exactly as if this parse never ran. See
     * anchor_speculation_begin(). */
    anchor_speculation_begin(ctx, &spec);
    coll =
        (c == '[') ? parse_flow_list(ctx, -1) : parse_flow_dictionary(ctx, -1);
    if (!coll) {
      /* The whole parse fails here, so the speculation ends by keeping its
       * changes: the budget flags still name the limit that stopped it, and
       * the anchor table goes when the parse unwinds.
       *
       * A real syntax error inside the flow collection already set
       * ctx->error with parse_err(). But an OOM at the very top of
       * parse_flow_list() or parse_flow_dictionary() returns NULL and never
       * calls parse_err(). A failure of cyaml_create_list_mp() itself is
       * one example. Without an error here, the caller reads that OOM as
       * "not a key after all" and goes on from a position that this
       * function did not restore. */
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
    /* Every implicit key must fit on one line, whether or not it is a
     * scalar. The ns-s-implicit-yaml-key production states this. An
     * explicit '? key' is different, and this function serves only keys in
     * the implicit style. */
    parsed = !span_crosses_newline(ctx, coll_start);
  } else
    /* An implicit key always stays on one line, which the ns-plain-one-line
     * production states. This call therefore needs no hit_real_terminator
     * out parameter. The parser never tries a continuation for a key,
     * whatever follows it. */
    parsed = parse_plain_scalar(ctx, false, &key_str, NULL);
  bool from_plain_branch = (c != '"' && c != '\'' && c != '[' && c != '{');
  if (!parsed && !coll) {
    /* This is usually a real parse error. One of parse_double_quoted(),
     * parse_single_quoted() and parse_plain_scalar() already set
     * ctx->error. But each of those can also fail on an allocator OOM
     * alone, and never call parse_err(). Without this fallback, every
     * caller classifies such a failure as "not a key after all", and
     * reports nothing. See the doc comment of this function. It is a real
     * failure and not that. */
    if (!ctx->error[0])
      parse_err(ctx, "out of memory parsing dictionary key at position %zu",
                saved_pos);
    _ccol_mem_free(ctx->mp, key_tag);
    _ccol_mem_free(ctx->mp, anchor_name);
    return false;
  }

  if (!parsed || !at_block_value_indicator(ctx)) {
    /* Not a key: a flow collection that spans lines, or any text with no
     * ':' after it. Nothing that only a key needs has been computed. The
     * tree of a flow collection goes before the rejection of its
     * speculation, which restores the budget that the tree charged. */
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
  if (coll) anchor_speculation_accept(ctx, &spec);
  ctx->pos++; /* consume ':' */

  /* The node of the key. A flow collection is one already. A plain scalar
   * always gets one, for its core-schema typing: without it, "~: v" stores
   * the literal text "~", while "? ~\n: v" stores "null" for the identical
   * value, and YAML 1.2 treats the two notations as the same entry. A tagged
   * quoted scalar gets one for its tag. An untagged quoted scalar needs
   * none: its value is already the string in key_str. Each finalize call
   * takes ownership of its input, including on failure. */
  cyaml_node_t *key_node = NULL;
  if (!key_tag && from_plain_branch) {
    /* The common case: an untagged plain key. This is exactly what
     * finalize_scalar_node() does for it, without the tag dispatch. */
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
    /* finalize_scalar_node() and finalize_collection_node() set a parse
     * error for a tag that disagrees with the key, and return NULL with no
     * error on an OOM. */
    if (!ctx->error[0])
      parse_err(ctx, "out of memory typing dictionary key at position %zu",
                saved_pos);
    _ccol_mem_free(ctx->mp, anchor_name);
    return false;
  }

  cyaml_node_t *anchor_value = NULL;
  if (key_node) {
    if (anchor_name) {
      /* Keep the real node of the key for the anchor registration.
       * node_to_dict_key_string() below destroys key_node and puts a
       * canonical string in its place. An alias to the anchor of this key
       * must resolve to the typed scalar, the tagged node, or the real list
       * or mapping, exactly as an anchor in a value position does. It must
       * not resolve to the string that only the key storage of the
       * dictionary uses. */
      anchor_value = parse_clone(key_node);
      if (!anchor_value) {
        parse_err(ctx,
                  "out of memory registering key anchor '%s' at position %zu",
                  _cyaml_echo(anchor_name), ctx->pos);
        __cyaml_destroy((cyaml)key_node);
        _ccol_mem_free(ctx->mp, anchor_name);
        return false;
      }
    }
    key_str = node_to_dict_key_string(ctx, key_node, "block dictionary");
    if (!key_str) {
      if (anchor_value) __cyaml_destroy((cyaml)anchor_value);
      _ccol_mem_free(ctx->mp, anchor_name);
      return false;
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
 * not to be a key. For a '[' or '{' key that is a complete, independent
 * parse of a flow collection. When the parser declines the speculation, the
 * caller goes on from ctx->pos and keeps using the nesting budget in
 * ctx->depth. Both must therefore be exactly what they were before the try.
 * The position must be, because the parser reads the declined content again
 * as something else. The depth must be, because the nested walk shares that
 * budget and gets no budget of its own. The code above provides both, and
 * the type system enforces neither. The whole construct rests on that
 * rewind, so a test build checks it instead of assuming it.
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
  ccol_assert(ctx->depth == saved_depth);
  if (!accepted && !ctx->error[0]) ccol_assert(ctx->pos == saved_pos);
#endif
  return accepted;
}

/* ========================================================================== */
/*                         FORWARD DECLARATIONS                               */
/* ========================================================================== */

/*
 * An explicit stack of frames drives the parse of a node, and every
 * collection parser that it descends into. The native call stack does not.
 * The stack that one parse needs is therefore a constant. It does not grow
 * with the nesting depth of the document. CYAML_MAX_PARSE_DEPTH then bounds
 * the work that a parse may be asked to do, and not the stack that a caller
 * must provide.
 *
 * There is one frame kind for each construct that can hold another node. A
 * frame runs until one of two things happens. It finishes, and hands its
 * node to the frame below it. Or it asks the driver to push a child frame.
 * After that child finishes, the parent goes on from the state that it
 * recorded. A frame holds only the values that are really live across a
 * descent. Everything else stays an ordinary local of the step function.
 */
typedef enum {
  _PFK_NODE,       /* one node dispatch: the depth guard plus _pni_step */
  _PFK_FLOW_LIST,  /* '[' elem (',' elem)* ']' */
  _PFK_FLOW_DICT,  /* '{' key ':' value (',' ...)* '}' */
  _PFK_BLOCK_LIST, /* '-' entries at one indent */
  _PFK_BLOCK_DICT  /* 'key:' / '? key' entries at one indent */
} _pframe_kind_t;

/*
 * What a frame asks the driver to push next. The fields that one kind reads
 * are exactly the arguments of the matching call. A descent request is
 * therefore an exact record of the call that it stands for.
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

/* Resume points. Each one names the descent where the driver suspended the
 * frame. The state of a frame is therefore always "which child do I wait
 * for". */
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
 * One suspended parser. `child` holds the node that the frame above just
 * finished. This frame owns that node until its resume point consumes it.
 * `result` holds the finished node of this frame, once this frame is done.
 *
 * _pframe_cleanup() frees every pointer that a frame owns. When a parse
 * fails, the driver runs it for EVERY frame still on the stack, and not
 * only for the innermost one. A container that is still under construction
 * is not reachable from any root, until the frame hands it to the frame
 * below. Each frame therefore owns its own container outright. Without that
 * sweep, an abandoned stack leaks one container for each open level.
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
      cyaml_node_t *map; /* owned */
      char *key_str;     /* owned */
      bool double_lt_is_merge_candidate;
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
      bool have_colon, is_explicit, double_lt_is_merge_candidate;
      bool entry_is_merge_candidate;
    } bd;
  } u;
} _pframe_t;

/* What _pbmn_begin() decided about the node that sits in a key position or
 * a value position of a block dictionary. */
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
 * The number of frames of the walk stack of the parser that the locals of
 * the driver hold. When a document nests deeper than this, the stack moves
 * onto the heap. Each nesting level opens two frames, which are the node
 * dispatch and the collection that the node turns out to be. Each anchor or
 * tag that decorates that node opens one more. This count therefore covers
 * a document of ordinary shape, and touches the allocator not at all.
 */
#define _CYAML_PARSE_INLINE_FRAMES 24

static cyaml_node_t *_parse_drive(parse_ctx_t *ctx, const _preq_t *root_req);

/*
 * Parse one node. The depth guard lives in the frame that this function
 * opens, and not here. See _pframe_open. CYAML_MAX_PARSE_DEPTH therefore
 * bounds every descent, whatever YAML construct drove it. Those constructs
 * are nested mappings, sequences, explicit keys, anchors, tags and other
 * such nesting. No matching check is needed at each of the many separate
 * descent sites in this file. A document that goes past the limit is a hard
 * parse error, which matches the fail-fast convention of this parser
 * elsewhere. Every exit path releases ctx->depth, the error path for the
 * exceeded depth included. A caller higher up that then reports its own,
 * different error therefore always sees a consistent depth count.
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

/* The definition of this function, near parse_block_dictionary, carries the
 * full doc comment. The declaration is here because parse_flow_dictionary
 * needs the function too, and this file defines that parser earlier. */
static bool expand_merge_key(parse_ctx_t *ctx, cyaml_node_t *map);

/* The definition of this function, near parse_one_dict_entry_key, carries
 * the full doc comment. The declaration is here because parse_flow_dictionary
 * needs the function too, for its own "{? key: value}" explicit-key
 * shorthand, and this file defines that parser earlier. */
static bool explicit_key_peek_is_merge_candidate(parse_ctx_t *ctx, int indent,
                                                 bool in_flow);
/*
 * Fill in a request for one node dispatch. The argument list reads exactly
 * like the parse_node() call that each descent site stands for.
 *
 * Each builder here writes only the fields that _pframe_open() reads for
 * that kind. A request therefore never carries anything that a frame of its
 * kind could act on. The two sides are the interface, and they sit a few
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

/* Fill in a request for a block dictionary. The request borrows first_key.
 * The frame that issues the request keeps the ownership of it. That frame
 * also stays below the new frame on the stack, for as long as the new frame
 * can read it. */
static void _preq_block_dict(_preq_t *rq, int map_indent, const char *first_key,
                             bool first_key_colon_consumed,
                             bool first_key_is_merge_candidate) {
  rq->kind = _PFK_BLOCK_DICT;
  rq->indent = map_indent;
  rq->first_key = first_key;
  rq->first_key_colon_consumed = first_key_colon_consumed;
  rq->first_key_is_merge_candidate = first_key_is_merge_candidate;
}

/* Fill in a request for a block sequence at seq_indent. */
static void _preq_block_list(_preq_t *rq, int seq_indent) {
  rq->kind = _PFK_BLOCK_LIST;
  rq->indent = seq_indent;
}

/* Fill in a request for a flow collection. A '[' gives a list, and a '{'
 * gives a dictionary. */
static void _preq_flow(_preq_t *rq, bool is_list, int indent) {
  rq->kind = is_list ? _PFK_FLOW_LIST : _PFK_FLOW_DICT;
  rq->indent = indent;
}

/* Record the finished node of a node frame. Every producer in this file
 * reports a failure with a NULL node. It therefore stays a failure here. */
static _pstep_t _pni_finish(_pframe_t *f, cyaml_node_t *n) {
  f->result = n;
  return n ? _PSTEP_DONE : _PSTEP_FAIL;
}

/* Free the anchor name that a node frame owns across the descent into the
 * value that the anchor decorates. This function also clears the field. The
 * cleanup of the frame is idempotent only because of that clear. */
static void _pni_release_name(parse_ctx_t *ctx, _pframe_t *f) {
  _ccol_mem_free(ctx->mp, f->u.n.name);
  f->u.n.name = NULL;
}

/* Free the tag text that a node frame owns across the descent into the node
 * that the tag decorates. See _pni_release_name. */
static void _pni_release_tag(parse_ctx_t *ctx, _pframe_t *f) {
  _ccol_mem_free(ctx->mp, f->u.n.tag);
  f->u.n.tag = NULL;
}

/* ========================================================================== */
/*                         FLOW COLLECTION PARSERS                            */
/* ========================================================================== */

/* The result of flow_list_expect_comma_or_close. It tells the switch on the
 * caller side in parse_flow_list what to do next. */
typedef enum {
  FLOW_LIST_TAIL_CLOSE,    /* Read a ']'. The caller gives the list as it
                              is. */
  FLOW_LIST_TAIL_CONTINUE, /* Read a ','. The caller loops for one more
                              entry. */
  FLOW_LIST_TAIL_ERROR     /* ctx->error is already set. The caller goes to
                              its fail label. */
} flow_list_tail_t;

/* The shared tail step that runs after one element of a flow list. It skips
 * whitespace and then needs a ']' or a ','. Both entry shapes of
 * parse_flow_list reach it in exactly the same way. Those shapes are the
 * "[? key: value]" explicit-pair branch and the bare-element branch below
 * it. This function reads the ']' or the ',' itself. The caller therefore
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

/* Parse a flow list, which is '[' elem (',' elem)* ']', into a CYAML_LIST
 * node. The backing cvec of that node holds cyaml_node_t * child pointers.
 * `f->u.fl.indent` is the indent of the enclosing block value. It is -1
 * at the document root. It is also -1 when this flow list is itself nested
 * inside another flow collection that already fixed the indent. See the doc
 * comment of flow_skip_ws.
 *
 * There are four points where this function parses an element, or the key
 * or the value of a "[? key: value]" pair. Each of those points is a
 * descent. Everything between them is ordinary straight-line code. The
 * frame keeps only the values that live across a descent. */
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
      /* A ',' with no content since the '[' or since the ',' before it is
       * an empty entry. An entry of a sequence has no valid meaning when it
       * is empty, because every element must really be there. The
       * bare-key-with-no-value shorthand of a flow mapping is different.
       * Without this check, parse_node and parse_plain_scalar give a
       * zero-length string "" here and report nothing. */
      parse_err(ctx, "empty flow list entry at position %zu", ctx->pos);
      return _PSTEP_FAIL;
    }

    /* The form "[? key: value]" is a single-pair entry in the explicit
     * style. YAML 1.2 section 7.4.1 defines it, in Spec Example 7.20. It
     * matches the bare "[key: value]" shorthand below, with two
     * differences. A '?' introduces the key, which permits key content over
     * more than one line, and key content that is not a scalar. The
     * implicit-key restriction of the bare form forbids both of those. And
     * a separate ':' introduces the value, when a value is there. */
    bool is_explicit_pair_indicator = false;
    if (cur(ctx) == '?') {
      char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
      if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
        is_explicit_pair_indicator = true;
    }
    if (is_explicit_pair_indicator) {
      ctx->pos++;
      if (!flow_skip_ws(ctx, indent)) return _PSTEP_FAIL;
      /* This matches the "{? key: value}" handling of
       * parse_flow_dictionary. A plain "<<" key with no tag here is just as
       * real a merge trigger as the bare "[<<: value]" shorthand below. The
       * flow-dictionary and block-dictionary forms of both are the same
       * again. The code must look ahead for it before it reads the key. */
      f->u.fl.is_merge_candidate =
          explicit_key_peek_is_merge_candidate(ctx, indent, true);
      _preq_node(rq, indent, true, false, false, false, false, NULL);
      f->state = _FLS_AFTER_PAIR_KEY;
      return _PSTEP_DESCEND;
    after_pair_key:
      key_node = f->child;
      f->child = NULL;
      /* A child that failed unwinds the whole stack, and never resumes this
       * frame. key_node is therefore never NULL here. The guard stands in
       * case a future frame kind reports a failure by finishing with no
       * node. Without it, such a frame kind makes this a null
       * dereference. */
      if (!key_node) return _PSTEP_FAIL;
      key_str = node_to_dict_key_string(ctx, key_node, "flow sequence entry");
      if (!key_str) return _PSTEP_FAIL;

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
        f->u.fl.key_str = key_str;
        _preq_node(rq, indent, true, false, false, false, false, NULL);
        f->state = _FLS_AFTER_PAIR_VALUE;
        return _PSTEP_DESCEND;
      after_pair_value:
        key_str = f->u.fl.key_str;
        f->u.fl.key_str = NULL;
        val = f->child;
        f->child = NULL;
        if (!val) {
          _ccol_mem_free(ctx->mp, key_str);
          return _PSTEP_FAIL;
        }
      } else {
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
        if (dictionary_set_impl((cyaml)pair, key_str, (cyaml)val) !=
            ccol_success) {
          _ccol_mem_free(ctx->mp, key_str);
          __cyaml_destroy((cyaml)pair);
          return _PSTEP_FAIL;
        }
        _ccol_mem_free(ctx->mp, key_str);
        if (f->u.fl.is_merge_candidate && !expand_merge_key(ctx, pair)) {
          __cyaml_destroy((cyaml)pair);
          return _PSTEP_FAIL;
        }

        cyaml_node_t *pep = pair;
        if (cvector_push_back(f->u.fl.seq->value.list, &pep) != ccol_success) {
          __cyaml_destroy((cyaml)pair);
          return _PSTEP_FAIL;
        }
        /* The list now owns the node. This flag records that. A later
         * attach of this node, reached through a borrowed
         * cyaml_list_get() reference, is then refused. Without the flag,
         * that attach gives the node a second owner. */
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
    /* This look ahead runs before the code reads the element. The
     * implicit-key look ahead of parse_flow_dictionary works the same way.
     * Only a plain, unquoted scalar with no tag, whose text is exactly
     * "<<", is a real merge-key trigger. An anchor does not disqualify it,
     * and the anchor handling of explicit_key_peek_is_merge_candidate
     * covers that case too. The doc comment of try_parse_scalar_dict_key
     * gives the general rule. This place needs a look ahead over the whole
     * property chain, and not a check of one character. A tag that follows
     * an anchor, as in "&y !!str <<: *x", must still be seen and must
     * disqualify the key. A check of the very first character alone cannot
     * do that. */
    f->u.fl.is_merge_candidate =
        explicit_key_peek_is_merge_candidate(ctx, indent, true);
    _preq_node(rq, indent, true, false, false, false, false, NULL);
    f->state = _FLS_AFTER_ELEM;
    return _PSTEP_DESCEND;
  after_elem:
    elem = f->child;
    f->child = NULL;
    if (!elem) return _PSTEP_FAIL;

    /* The text "[foo: bar]" is shorthand for "[{foo: bar}]". A bare "key:
     * value" pair inside a flow sequence, with no '{' and '}' around it,
     * means a mapping element with one entry. YAML 1.2 section 7.4.1
     * defines this with the ns-flow-pair production. Like every implicit
     * key, it must fit on one line, which the ns-s-implicit-yaml-key
     * production states. A ':' that the parser reaches only after it
     * crosses a newline is therefore not this shorthand. It is the next
     * real error, which is a missing ',' between this element and whatever
     * follows. This holds for both ways of crossing that newline. The parse
     * of the element itself can cross it, for example when a plain scalar
     * folds over more than one line right up to the ':'. Or the skip of
     * whitespace after the element can cross it, before the parser finds
     * the ':'. */
    if (!flow_skip_ws(ctx, indent)) {
      __cyaml_destroy((cyaml)elem);
      return _PSTEP_FAIL;
    }
    if (!span_crosses_newline(ctx, f->u.fl.elem_start) && !at_end(ctx) &&
        cur(ctx) == ':') {
      key_str = node_to_dict_key_string(ctx, elem, "flow sequence entry");
      if (!key_str) return _PSTEP_FAIL;
      ctx->pos++; /* consume ':' */
      if (!flow_skip_ws(ctx, indent)) {
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
        /* pair is a dictionary that this code just created. key_str is not
         * NULL. val is a fresh node that nothing attached, and it is not
         * pair itself. The only failures that this call can reach are
         * therefore the ones that take ownership of val and destroy it.
         * pair is empty either way, and it is the only thing left to clean
         * up on that path. */
        if (dictionary_set_impl((cyaml)pair, key_str, (cyaml)val) !=
            ccol_success) {
          _ccol_mem_free(ctx->mp, key_str);
          __cyaml_destroy((cyaml)pair);
          return _PSTEP_FAIL;
        }
        _ccol_mem_free(ctx->mp, key_str);
        if (f->u.fl.is_merge_candidate && !expand_merge_key(ctx, pair)) {
          __cyaml_destroy((cyaml)pair);
          return _PSTEP_FAIL;
        }
        elem = pair;
      }
    }

    {
      cyaml_node_t *ep = elem;
      if (cvector_push_back(f->u.fl.seq->value.list, &ep) != ccol_success) {
        __cyaml_destroy((cyaml)elem);
        return _PSTEP_FAIL;
      }
      /* The list now owns the node. This flag records that. A later attach
       * of this node, reached through a borrowed cyaml_list_get()
       * reference, is then refused. Without the flag, that attach gives the
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

/* Parse a flow list as a walk that stands on its own. One place parses a
 * flow collection outside any node dispatch. That place is the speculative
 * read in try_parse_scalar_dict_key of a flow-collection block-mapping key.
 * That read drops the result and rewinds ctx->pos when the collection turns
 * out not to be a key. The speculation is a complete, independent parse. It
 * has its own frame stack, which the driver drains or sweeps before this
 * function returns. Nothing about the outer walk therefore has to be saved
 * across it. Both parses share ctx, and ctx->depth with it. The same
 * remaining depth budget that reached this list therefore bounds the
 * elements inside it. */
static cyaml_node_t *parse_flow_list(parse_ctx_t *ctx, int indent) {
  _preq_t rq;
  _preq_flow(&rq, true, indent);
  return _parse_drive(ctx, &rq);
}

/*
 * Write the canonical dictionary-key text of the float v into buf. The text
 * is the shortest decimal that strtod() reads back as exactly v. See
 * format_shortest_double(). yb_append_double() uses the same digits for a
 * float value, and adds the ".0" that a value needs and a key does not. A
 * NaN gives ".nan", an infinity gives ".inf" or "-.inf", and both zeros give
 * "0", because -0.0 and 0.0 are one number.
 * The text of an integral value carries no ".0", so the float key 1.0 and
 * the integer key 1 are the same key, exactly as the two numbers are equal.
 *
 * Every piece of this text parses back through try_parse_int_scalar() or
 * try_parse_float_scalar() to a value whose own canonical text is the same
 * string. key_is_canonical_number() depends on that, and it lets the
 * serializer write such a key without quotes.
 *
 * Like try_parse_float_scalar(), this runs only inside the "C" locale scope
 * of a public parse or serialize call, so the text always uses '.'.
 */
#define CYAML_FLOAT_KEY_BUF_LEN 32

/* Write into buf the shortest "%.Ng" text, for N from 15 to 17, that strtod()
 * reads back as exactly the finite double v. Every double round trips at 17
 * significant digits, and no shorter text of that form is written when a
 * shorter one round trips: 1.0 / 3.0 gives "0.3333333333333333" and not the
 * 17-digit "0.33333333333333331". Fewer than 15 digits never needs a try of
 * its own, because "%.15g" already drops the trailing zeros. */
static void format_shortest_double(double v,
                                   char buf[static CYAML_FLOAT_KEY_BUF_LEN]) {
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
 * Convert a parsed node into its canonical string form, so that a
 * dictionary can store it as a key. The dictionaries of this DOM are always
 * char* -> node, and a chmap backs them, so every key is a string in the
 * end. This function always destroys key_node.
 *
 * A key_node that is not a scalar is a list or another dictionary. YAML 1.2
 * sections 7.4.1 and 8.2.2 both permit that. This function makes its
 * canonical form with serialize_flow_canonical. That is an internal-only
 * variant of cyaml_serialize_flow, written for exactly this use. See its
 * own doc comment. The result is the compact flow-YAML text of the node,
 * for example "[a, b]" or "{x: 1}", and it becomes the key string.
 *
 * Two keys that are not scalars and that have the same structure therefore
 * collide, because their flow output matches. This DOM already treats the
 * numeric key 1 and the string key "1" as the same string key, in the same
 * way. The order in which either dictionary received its own keys does not
 * matter. This canonical form does not depend on context_label. Three places
 * therefore get the same treatment. They are an explicit key of a block
 * dictionary, a key of a flow dictionary, and the "key: value" shorthand of
 * a flow sequence.
 */
static char *node_to_dict_key_string(parse_ctx_t *ctx, cyaml_node_t *key_node,
                                     const char *context_label) {
  char *key_str = NULL;
  switch (key_node->type) {
    case CYAML_STRING:
      /* This code moves the string out of the node. It does not copy it.
         This function destroys key_node before it returns. A copy made here
         would therefore be paid for twice. It costs one allocation and one
         copy to make the copy. It costs one free for the original a few
         lines below. The caller then copies the string again into the
         storage of the dictionary. This code takes the pointer and clears
         the reference of the node to it. The teardown of the node then has
         nothing to free for this member. node_clear_value already handles
         that, because a cleared node is in exactly this state.

         The move happens only when the two allocators are the same object.
         The caller frees this string through ctx->mp, and the node would
         have freed it through its own. They are the same for every node
         that this parser builds. The copy stays correct for any node that
         ever arrives from somewhere else. */
      if (key_node->m_procs == ctx->mp && key_node->value.string) {
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
        /* See the doc comment of CYAML_MAX_CANONICAL_KEY_LEN. A key that is
         * not a scalar can nest inside another such key, many levels deep.
         * Its canonical text then grows exponentially in the nesting depth,
         * because each level puts quotes again around the text that the
         * level below it made. This has no tie to CYAML_MAX_PARSE_DEPTH,
         * and it is far cheaper to reach. Many aliases of one long scalar
         * reach the same length without any nesting.
         * serialize_flow_canonical() stops at the first byte past the
         * limit, so this refusal costs at most one limit's worth of text. */
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
        /* The canonical text of a collection key can be far longer than the
         * text that declared it, because each nested level quotes the level
         * below it again. That makes it amplification, so it is charged to
         * the fixed amplification budget as well as to the ordinary one. */
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
  /* The LIST and DICTIONARY branch above always reports its own failure
   * with parse_err() before it falls through to here. That failure is an
   * OOM, or a canonical form that is too long. The five scalar branches
   * have only one failure mode of their own, which is a bare ccol_strdup
   * OOM. None of them reports it on its own. An unreported OOM here is
   * neither a crash nor a loss of place. The top-level fallback of
   * parse_common still reports a general out-of-memory message when
   * ctx->error is empty. But a real OOM here deserves the specific
   * diagnostic of this call site. That matches the convention of this file
   * elsewhere, for example in try_parse_scalar_dict_key. */
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
 * Parse a flow dictionary, which is '{' key ':' value (',' ...)* '}', into a
 * CYAML_DICTIONARY node. A key may be a node of any type. It may be a
 * scalar, which is a string, an integer, a null or a bool. It may also be a
 * sequence, a mapping or a flow collection. node_to_dict_key_string() makes
 * the canonical form of every key, so that the chmap stores every key as a
 * char *. See the doc comment of that function.
 *
 * The key and the value are each a descent. Everything else is
 * straight-line code. The frame keeps only what lives across a descent.
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

  /* The field f->u.fd.double_lt_is_merge_candidate records one thing. It
   * records whether the entry that wrote the literal key "<<" into the map
   * most recently was a real merge-key trigger. The code derives it again
   * each time the key text of an entry is exactly "<<". It writes over the
   * old value, and it never accumulates the values of every earlier entry
   * with an OR. The reason is that a duplicate "<<" key collapses to
   * whichever entry wrote it LAST. cyaml_dictionary_set gives a duplicate
   * key the "last value wins" rule. A later "<<" entry with explicit quotes
   * or an explicit tag writes an ordinary literal value over the earlier
   * one. An earlier "<<" entry that triggers a merge must therefore never
   * keep this field true after that. An OR across entries instead lets the
   * earlier entry force an expansion of the later, literal value. It can
   * also make the code reject that later value for no good reason. */
  while (1) {
    if (!flow_skip_ws(ctx, indent)) return _PSTEP_FAIL;
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow dictionary");
      return _PSTEP_FAIL;
    }
    if (cur(ctx) == '}') {
      ctx->pos++;
      if (f->u.fd.double_lt_is_merge_candidate &&
          !expand_merge_key(ctx, f->u.fd.map))
        return _PSTEP_FAIL;
      goto done;
    }

    /* The form "{? key: value}" comes from YAML 1.2 section 7.4.2, in the
     * ns-flow-map-explicit-entry production. An explicit '?' may introduce
     * the key of a flow dictionary entry. This works exactly like the
     * block-style "? key\n: value" form. It permits the key to be a flow
     * collection itself. It also permits the key to be left out fully,
     * which is a bare "?" with nothing else and means a null key. Without
     * this check, a '?' here reaches the general node dispatch below. The
     * explicit-key handling of that dispatch is for block context only,
     * because !in_flow gates it. The '?' is then scanned as ordinary plain
     * scalar TEXT. The result is a key that starts with the literal "? ",
     * and not the real key after it, with nothing reported. */
    f->u.fd.is_explicit_key_indicator = false;
    if (cur(ctx) == '?') {
      char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
      if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
        f->u.fd.is_explicit_key_indicator = true;
    }

    /* Parse the key. Both forms use the same look ahead. Those forms are
     * the implicit one, with no '?', and the explicit '?' one. In each of
     * them explicit_key_peek_is_merge_candidate looks at the leading anchor
     * and tag property chain of the key, in either order. It looks before
     * the general dispatch reads the key. That confirms a real merge-key
     * candidate.
     * The doc comment of try_parse_scalar_dict_key gives the same rule on
     * the block-dictionary side. The doc comment of
     * explicit_key_peek_is_merge_candidate tells you why this needs a look
     * ahead over the whole property chain, and not a check of one
     * character. An anchor does NOT disqualify the key. A tag that follows
     * an anchor, as in "&y !!str <<: *x", must still be seen and must
     * disqualify the key. A look ahead at the very first character alone
     * cannot do that. */
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

    /* The text "{key}" is shorthand for "{key: null}". It is a bare key
     * with no ':' at all. The text "{key:}" has a null value too. It is a
     * ':' with nothing before the next ',' or '}'. The ns-flow-map-entry
     * production of YAML 1.2 section 7.4.2 permits a pair to leave out its
     * key half, its value half, or both. */
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

    {
      bool key_is_double_lt = strcmp(key_str, "<<") == 0;
      ccol_retval_t r =
          dictionary_set_impl((cyaml)f->u.fd.map, key_str, (cyaml)val);
      _ccol_mem_free(ctx->mp, key_str);
      if (r != ccol_success) return _PSTEP_FAIL;
      if (key_is_double_lt)
        f->u.fd.double_lt_is_merge_candidate = f->u.fd.is_merge_candidate;
    }

    if (!flow_skip_ws(ctx, indent)) return _PSTEP_FAIL;
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow dictionary");
      return _PSTEP_FAIL;
    }
    if (cur(ctx) == '}') {
      ctx->pos++;
      if (f->u.fd.double_lt_is_merge_candidate &&
          !expand_merge_key(ctx, f->u.fd.map))
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

/* Parse a flow dictionary as a walk that stands on its own. The doc comment
 * of parse_flow_list tells you when this is needed. It also tells you why
 * the speculation that it serves needs nothing saved across it. */
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
 * The field indent is the map_indent of the mapping. It is the column where
 * the keys of the dictionary, and the '?' indicators, must appear.
 *
 * When first_key is not NULL, it is the key string of the implicit ("key:
 * value") key of the first entry. The frame that issued the request already
 * read it, before that frame knew it had a dictionary and not a standalone
 * scalar. That frame keeps the ownership of the string. It also stays below
 * this one on the stack, so the borrow stays valid for as long as anything
 * reads it.
 *
 * The field first_key_colon_consumed reports whether the code also already
 * read the ':' that follows that key. It is true for a key with an anchor
 * or a tag, which try_parse_scalar_dict_key parses and which always reads
 * the ':'. It is false for the plain and quoted scalar dispatch branches,
 * which leave the ':' for the block dictionary to find and read itself.
 *
 * When first_key is NULL, ctx->pos is at map_indent, on a line that starts
 * with a '?'. That is the explicit key style, which is "? key" and then ":
 * value". It is the only way that the node dispatch reaches a block
 * dictionary with no key read in advance. That mode does not use
 * first_key_colon_consumed. Every entry after the first may mix the
 * explicit and the implicit style freely inside one dictionary. YAML 1.2
 * section 8.2.2 permits this.
 *
 * The field first_key_is_merge_candidate reports whether first_key is a
 * real merge-key trigger. That means a plain "<<" with no tag. Whichever
 * site produced the key confirms it. See the doc comment of
 * try_parse_scalar_dict_key for the exact rule. It is always false when
 * first_key is NULL. A NULL first_key means that the first entry of the
 * mapping uses the '?' explicit-key form. The block dictionary then decides
 * the merge candidacy of that entry itself. See
 * explicit_key_peek_is_merge_candidate.
 */

/*
 * The core node parser. It dispatches to the right sub-parser, from the
 * first characters that carry meaning.
 *
 * indent: the block collection indent level of the current container. The
 *   value -1 means the top level, a flow context, or an indent that nothing
 *   fixed yet.
 * in_flow: true inside a flow collection, which is {} or [].
 * seq_ok_at_indent: whether `indent` is the indent of a mapping. True
 *   means a mapping. A '-' sequence value at exactly that column is then
 *   the valid compact-sequence-under-a-key exception of YAML 1.2 section
 *   8.2.2. False means the indent of a sequence. A '-' at exactly that
 *   column is then always a sibling element, and never nested content. See
 *   the doc comment of at_block_value_col. This flag has no meaning when
 *   in_flow is true, or when indent is -1. The '&' and '!' branches below
 *   still pass it on in every case. Without that, a decorated value loses
 *   its own indent context.
 * allow_inline_map: whether a plain or quoted scalar found here may start a
 *   NEW block mapping right here. It may do so when a ':' turns out to
 *   follow it at once. This call then becomes the first key of that
 *   mapping. It is true at every position where the parser really expects
 *   a fresh key. Those positions are the document root, a sequence element
 *   ("- key: value"), the key or value content of the explicit '?' and ':'
 *   style, and a value that starts on its own, more indented line. A
 *   reference parser confirms the explicit-style case. It gives that
 *   content the same "compact" privilege for a nested mapping that it
 *   already gives for a nested sequence. The flag is false only for the
 *   SAME-LINE content of an ordinary implicit value ("key: value"). The
 *   recursive parse_node call that parses the "value" half receives false.
 *   A second ':' chained there ("a: b: c") has no valid reading. It is a
 *   hard error and not unbounded nesting that nothing reports. A reference
 *   parser confirms this. It rejects the chain in the same way wherever it
 *   sits. That covers the document root, the inside of the compact mapping
 *   of a sequence element, and any nesting depth. The flag has no meaning
 *   once in_flow is true, because flow mappings have their own separate '{'
 *   and '}' dispatch. The '&', '!' and '*' branches below still pass it on
 *   in every case, for the same reason as seq_ok_at_indent. Without that, a
 *   decorated value loses its own inline-mapping privilege, or the lack of
 *   one.
 * had_anchor: whether the "ordinary content" recursion of an ENCLOSING
 *   anchor already applied that anchor. It applies it to the exact node
 *   position that this call is about to resolve. The result of this call
 *   would then become the direct content of that enclosing anchor, when
 *   this call does not itself give a key. See below. The c-ns-properties
 *   production permits at most one anchor for each node. THIS call may also
 *   turn out to need an anchor, while had_anchor is already true. That is
 *   two anchors on one node, which has no valid reading. Two independent
 *   reference parsers confirm this. Both reject "&a &b val", which puts
 *   two bare anchors around one scalar with nothing else between them.
 *   Both reject it whether the two anchors sit on one line or on two. The
 *   parser reads this flag only at the very top of the '&' branch itself.
 *   That branch has an "is this a key after all" fast path. The parser
 *   reads the flag only after that path fails, or after the code skips it.
 *   A property that DOES resolve to a key is never affected, whatever
 *   had_anchor holds. One example is "&node1\n  &k1 key1: val1", where &k1
 *   decorates the key scalar "key1". That is a different node from the
 *   mapping value of &node1. That path returns before it ever reads the
 *   flag. The flag is false at every other call site. That includes the
 *   "is this a key" fast path of the '&' and '!' branches, and every call
 *   site outside those two branches. Each of those starts a genuinely fresh
 *   node position, with no pending property to clash with.
 * had_tag: whether the recursion of an ENCLOSING '!' branch already applied
 *   a tag to the exact node position that this call is about to resolve. It
 *   has exactly the role for tags that had_anchor has for anchors. Only two
 *   guards read it. They are the two-tags guard of the '!' branch, and the
 *   cannot-carry-a-tag guard of the '*' branch. This is a SEPARATE signal
 *   from resolved_tag itself. A bare non-specific tag, which is a "!" with
 *   nothing after it, resolves to a resolved_tag of NULL. See the doc
 *   comment of parse_tag_token, which says it "resolves to no forced type,
 *   treated identically to no tag at all". It is still a real tag for the
 *   stacking rule. The parser must reject "! !!str x" and "! *alias"
 *   exactly as it rejects "!!str !!str x" and "!!str *alias". The first tag
 *   of each pair carries no resolved_tag value of its own, so it cannot
 *   make resolved_tag != NULL true.
 * resolved_tag: the fully resolved tag string that the recursion of an
 *   ENCLOSING '!' branch passes to this call. It is NULL for every
 *   ordinary call with no tag. It is also NULL for a call that a
 *   non-specific "!" encloses, because such a tag sets had_tag and leaves
 *   resolved_tag NULL. This is the real value that the scalar-construction
 *   dispatch of this call reads. See finalize_scalar_node. It overrides
 *   which type the parser builds in the first place. It is not metadata
 *   that something attaches to an already finished node afterwards. The
 *   recursive calls of the '&' branch pass it on unchanged, exactly as
 *   this call received it. An anchor never introduces a tag of its own,
 *   and it never consumes one. This is what carries a tag all the way down
 *   to the place where the parser reads the real content. It works
 *   whatever number of anchor and tag layers sit between them. The
 *   c-ns-properties production permits either order, so "!!str &a foo" and
 *   "&a !!str foo" both work.
 */
/*
 * Dispatch one node. This is the whole decision tree that the parser runs
 * for each node. The driver suspends and resumes it at the four points
 * where it descends into another node or into a collection. See the _NS_*
 * resume points.
 *
 * The compiler inlines this function into the driver, beside the four
 * collection steps. It is by far the largest of the five, so the inline is
 * not free in code size. Re-measure it with instruction counts if the
 * balance between the steps ever changes. As they stand, half of every step
 * that a parse runs is a node dispatch. A call for each of those costs
 * more.
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

    /* A tab in the whitespace just skipped is separation whitespace in
     * front of a flow node, which YAML 1.2 permits after the spaces of the
     * indentation. A block collection may not follow it: the start of
     * every block collection refuses the tab itself. See
     * line_space_indent(). */

    if (at_end(ctx)) {
      if (!in_flow) return _pni_finish(f, node_alloc(CYAML_NULL, ctx->mp));
      parse_err(ctx, "unexpected end of input in flow context at position %zu",
                ctx->pos);
      return _PSTEP_FAIL;
    }

    /* A node never starts with a byte order mark at the start of a line.
     * Such a mark is not content; see at_col0_bom(). Reading it as the first
     * character of a plain scalar would turn "\xEF\xBB\xBFb: 2" into a
     * key that no other reader produces. */
    if (at_col0_bom(ctx)) {
      parse_err(ctx,
                "byte order mark inside a document at position %zu; a byte "
                "order mark may only precede a document",
                ctx->pos);
      return _PSTEP_FAIL;
    }

    if (in_flow && at_doc_marker(ctx)) {
      /* A '---' or '...' document marker at the start of a line is
       * c-forbidden, from YAML 1.2 section 6.9. It can never be plain scalar
       * content, in any context. A block context handles it easily: the
       * enclosing loop treats it as the end of the current collection, which
       * is an ordinary document boundary. A flow collection that is still
       * open has no valid way to end here at all. It still needs its own
       * closing ']' or '}', and not a document boundary at the level of the
       * stream. Two independent reference parsers confirm this. */
      parse_err(ctx,
                "a document marker cannot appear inside an unclosed "
                "flow collection at position %zu",
                ctx->pos);
      return _PSTEP_FAIL;
    }

    char c = cur(ctx);

    /* Anchor */
    if (c == '&') {
      /* In a block context, "&anchor key: value" anchors only the key
       * scalar. It does not anchor the whole dictionary that the key turns
       * out to introduce. This code checks for that shape first. Without the
       * check, the input falls through to the general anchor-then-recurse
       * handling below. The recursive parse_node call there reads every
       * sibling entry too, and anchors the whole dictionary that results.
       * parse_one_dict_entry_key handles a later entry in the same way.
       * allow_inline_map gates this fast path. The path always concludes
       * "this is a valid key". It must therefore not run when this call has
       * no licence to open a fresh mapping here at all. The chained implicit
       * value "a: &x b: c" is one such case. That input then falls through to
       * the ordinary anchor handling below. That handling routes it through
       * the same allow_inline_map rejection that the plain-scalar dispatch
       * already has. */
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
        /* Not a key after all. try_parse_scalar_dict_key already restored
         * ctx->pos, so the code falls through to the ordinary anchor
         * handling below. */
      }

      if (had_anchor) {
        /* This anchor does not decorate a key. The fast path above already
         * ruled that out, or it never applies here. The anchor is ordinary
         * content for an ENCLOSING anchor. That enclosing anchor already
         * used the one anchor that a single node may carry. See the doc
         * comment of had_anchor. */
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
        /* The ns-anchor-char production excludes every c-flow-indicator
         * character, in every case. parse_anchor_name therefore always stops
         * right before one. Whatever comes next is valid content only when
         * s-separate, which is real whitespace, truly separates it from the
         * anchor. A flow-collection OPENER, which is '[' or '{', with no
         * separator before it, has no valid reading in ANY context. Nothing
         * in the grammar lets a brand-new nested collection follow the name
         * of an anchor with no separator. This code therefore always rejects
         * it, a flow context included. The characters ',', ']' and '}' are
         * different. Each of those is valid, meaningful flow-collection
         * structure right after an anchor INSIDE a flow collection. The texts
         * "[&a, b]" and "[&a]" are two examples. This code rejects those
         * three only in a block context, where they have no valid meaning at
         * all. A reference parser confirms this. It rejects "[&a{x: 1}, *a]"
         * and "[&a[1,2], *a]", which put an anchor directly against the
         * opener of a nested collection. It still accepts an anchor directly
         * against a flow terminator. */
        parse_err(ctx,
                  "unexpected '%s' directly after anchor name at "
                  "position %zu",
                  _cyaml_echo_c(cur(ctx)), ctx->pos);
        _pni_release_name(ctx, f);
        return _PSTEP_FAIL;
      }

      cyaml_node_t *n;
      if (!in_flow && rest_of_line_is_blank(ctx)) {
        /* Nothing else is on this line, except possibly a comment. This code
         * makes the same check as parse_block_map_node before it descends. A
         * value on a later line counts only when it is really more indented.
         * For a sequence it counts when it is exactly as indented. Without
         * the check, a sibling entry at or below `indent` is read as the
         * value of this anchor. One example is "b: 2" that follows
         * "a: &anchor" at the same column. A descent with no check here
         * walks straight past the newline. It takes that sibling, and
         * everything after it, into the anchor. */
        skip_ws_comments(ctx);
        bool is_value_col =
            !at_end(ctx) && at_block_value_col(ctx, indent, seq_ok_at_indent);
        if (at_end(ctx) || !is_value_col) {
          /* Nothing at all follows this anchor. This code routes the case
           * through finalize_scalar_node, with empty text and
           * implicit_ok=true. It does not build a bare CYAML_NULL node
           * directly. resolved_tag can be an ENCLOSING tag that passes work
           * to this anchor, as in "!!str &a" with nothing after the anchor
           * either. That tag must still get the chance to override the type
           * of this empty content. It gets that chance at any other empty
           * scalar position. A direct CYAML_NULL node drops the tag and
           * reports nothing, only because no anchor content follows. */
          char *empty = ccol_strdup(ctx->mp, "");
          n = empty ? finalize_scalar_node(ctx, empty, resolved_tag, true)
                    : NULL;
        } else {
          /* A value that this code confirms starts on its own fresh line
           * always gets the "may open a fresh mapping here" privilege. The
           * allow_inline_map that this call itself received does not matter.
           * The later-line branch of parse_block_map_node writes true for the
           * same reason. Only SAME-LINE chaining after an implicit value that
           * is already open is ever restricted. This call passes
           * had_anchor=true, because the anchor of this call is now used. It
           * passes had_tag and resolved_tag on unchanged. The doc comment of
           * parse_node tells you why. */
          _preq_node(rq, indent, in_flow, seq_ok_at_indent, true, true, had_tag,
                     resolved_tag);
          f->state = _NS_AFTER_ANCHOR_VALUE;
          return _PSTEP_DESCEND;
        }
      } else if (in_flow && at_eol(ctx)) {
        /* A flow context with nothing else on this line. A flow collection
         * may wrap over more than one line, so the value may still follow on
         * a later line. There is no sibling-entry ambiguity to guard against
         * here, the way a block context has. The ',', ']' and '}' terminators
         * of a flow collection separate its entries, and indentation does
         * not. This code must cross the newline with flow_skip_ws, and not
         * with a plain skip_ws_comments. That holds this continuation line to
         * the same s-separate(n,c) rules about indentation and tabs that
         * every other flow-collection continuation line in this file obeys.
         * skip_ws_comments alone accepts a continuation line here that is
         * indented with a tab, or indented too little, and reports nothing.
         * flow_skip_ws rejects such a line anywhere else. */
        if (!flow_skip_ws(ctx, indent)) {
          _pni_release_name(ctx, f);
          return _PSTEP_FAIL;
        }
        _preq_node(rq, indent, in_flow, seq_ok_at_indent, allow_inline_map,
                   true, had_tag, resolved_tag);
        f->state = _NS_AFTER_ANCHOR_VALUE;
        return _PSTEP_DESCEND;
      } else if (!in_flow && at_bare_seq_indicator(ctx)) {
        /* A block sequence cannot start on the same line, right after an
         * anchor. Only a mapping gets that "compact" privilege. It gets that
         * privilege only when a '-' itself introduces it, and not when a node
         * property before it does. try_parse_scalar_dict_key already ruled
         * out "this is an anchored key" above. Nothing valid therefore
         * remains for a bare '-' here. */
        parse_err(ctx,
                  "a block sequence cannot start on the same line as an "
                  "anchor at position %zu",
                  ctx->pos);
        _pni_release_name(ctx, f);
        return _PSTEP_FAIL;
      } else if (!in_flow && cur(ctx) == '&') {
        /* The same-line content of this anchor is itself another anchor. The
         * c-ns-properties production permits at most one anchor for each
         * node. The had_anchor check below cannot catch this on its own. The
         * "is this an anchored key" fast path above takes priority. That path
         * accepts the input, and reports nothing, whenever what follows the
         * second anchor looks like a valid key. The text "&a &b foo: bar" is
         * one such input. The same path correctly accepts the valid case that
         * spans two lines, as in "top: &a\n  &b key: val". The
         * rest_of_line_is_blank branch above handles that case, and this
         * branch is not that one. This code therefore catches the error here,
         * before any descent. It does not rely on the had_anchor check of the
         * callee. That check runs only once the fast path of the callee itself
         * fails, for example when nothing after the second anchor looks like
         * a key. */
        parse_err(ctx, "a node cannot carry two anchors at position %zu",
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
      cyaml_node_t *clone = parse_clone(n);
      if (!clone) {
        if (_parse_amp_limit_hit)
          parse_err(ctx,
                    "anchor '%s' registration exceeded the %zu-node, "
                    "%zu-byte alias expansion memory limit at position %zu; "
                    "check for exponential anchor nesting",
                    _cyaml_echo(name), (size_t)_CYAML_PARSE_NODE_FLOOR,
                    (size_t)_CYAML_PARSE_BYTE_FLOOR, ctx->pos);
        else if (_parse_node_budget_exhausted)
          parse_err(ctx,
                    "anchor '%s' registration exceeded the %zu node-allocation "
                    "limit at position %zu; check for exponential anchor "
                    "nesting",
                    _cyaml_echo(name), _parse_node_limit, ctx->pos);
        else if (_parse_byte_budget_exhausted)
          parse_err(ctx,
                    "anchor '%s' registration exceeded the %zu-byte document "
                    "memory limit at position %zu; check for exponential "
                    "anchor nesting",
                    _cyaml_echo(name), _parse_byte_limit, ctx->pos);
        else
          parse_err(ctx, "out of memory cloning anchor '%s' at position %zu",
                    _cyaml_echo(name), ctx->pos);
        __cyaml_destroy((cyaml)n);
        _pni_release_name(ctx, f);
        return _PSTEP_FAIL;
      }
      if (!anchors_store(ctx, name, clone)) {
        /* anchors_store() already reported the specific error, and it already
         * consumed clone. n is still a fully valid parsed node. The document
         * as a whole must still fail here. Without that, a later '*name'
         * alias reference to this anchor resolves against nothing. Worse, it
         * can resolve against a stale entry from an outer scope. Either way
         * it never gets the real "out of memory" failure. */
        __cyaml_destroy((cyaml)n);
        _pni_release_name(ctx, f);
        return _PSTEP_FAIL;
      }
      _pni_release_name(ctx, f);
      return _pni_finish(f, n);
    }

    /* Alias */
    if (c == '*') {
      /* The text "*alias : value" uses the alias directly as an implicit
       * key. try_parse_scalar_dict_key handles that in the same way. This
       * top-level dispatch needs it too, because the parser also reaches this
       * dispatch in the general way. One example is a descent through an
       * anchor with nothing else on its own line, as in
       * "top: &node\n  *alias : value\n". allow_inline_map gates this fast
       * path, for the same reason as the matching fast path of the '&' branch
       * above. */
      if (!in_flow && allow_inline_map) {
        int col = current_col(ctx);
        char *anchor_name = NULL;
        char *key_str = NULL;
        if (try_parse_scalar_dict_key(ctx, &anchor_name, &key_str, NULL,
                                      NULL)) {
          /* The parser reaches this point only when the dispatch character
           * itself is '*'. This can therefore only be the alias-as-key
           * branch inside try_parse_scalar_dict_key. That branch is never a
           * merge candidate, by construction. See the doc comment of that
           * function. This call therefore does not even ask for the out
           * parameter. */
          _ccol_mem_free(ctx->mp,
                         anchor_name); /* an alias key has no name of its own */
          f->u.n.pending_key = key_str;
          _preq_block_dict(rq, col, key_str, /*colon_consumed=*/true, false);
          f->state = _NS_AFTER_COLLECTION;
          return _PSTEP_DESCEND;
        }
        if (ctx->error[0]) return _PSTEP_FAIL;
        /* Not a key after all. try_parse_scalar_dict_key already restored
         * ctx->pos, so the code falls through to the ordinary alias
         * resolution below. */
      }

      if (had_anchor || had_tag) {
        /* The c-ns-alias-node production is its own top-level alternative in
         * the grammar of ns-flow-node. It is fully separate from the
         * c-ns-properties branch that an anchor or a tag decorates. An alias
         * can therefore never carry a property at all. Two independent
         * reference parsers confirm this. Both reject "&anchor *alias" and
         * "!!tag *alias" in the same way. This rule is different from, and
         * stricter than, the two-stacked-properties check of the '&' and '!'
         * branches above. Even a SINGLE anchor or tag on a bare alias is
         * invalid, and not only a second one. An alias-as-key is different,
         * and the fast path above already handles it. This check reads
         * had_tag and not resolved_tag. That catches a bare non-specific
         * "! *alias" too, although the resolved_tag of a non-specific tag is
         * NULL. */
        parse_err(ctx, "an alias cannot carry an anchor or tag at position %zu",
                  ctx->pos);
        return _PSTEP_FAIL;
      }

      ctx->pos++;
      char *name = NULL;
      if (!parse_anchor_name(ctx, &name)) return _PSTEP_FAIL;
      cyaml_node_t *anchored = anchors_lookup(ctx, name);
      if (!anchored) {
        parse_err(ctx, "unknown alias '*%s' at position %zu", _cyaml_echo(name),
                  ctx->pos);
        _ccol_mem_free(ctx->mp, name);
        return _PSTEP_FAIL;
      }
      cyaml_node_t *clone = parse_clone(anchored);
      if (!clone && _parse_amp_limit_hit)
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
      /* This matches the same check in the '&' branch just above. The text
       * "!!tag key: value" tags only the key scalar. It does not tag the
       * whole dictionary that the key turns out to introduce.
       * try_parse_scalar_dict_key already handles a bare tag prefix, with no
       * anchor before it, on its own. But c-ns-properties permits the anchor
       * and the tag in EITHER order. This key may therefore still carry an
       * anchor that the same call found, as in "!!str &a1 key:". This code
       * must register that anchor exactly as the '&' branch does. Without
       * that, the anchor is dropped with nothing reported, only because the
       * parser entered THIS branch through the tag and not through the
       * anchor. allow_inline_map gates this fast path, for the same reason as
       * the matching fast path of the '&' branch above. */
      if (!in_flow && allow_inline_map) {
        int col = current_col(ctx);
        char *anchor_name = NULL;
        char *key_str = NULL;
        cyaml_node_t *anchor_value = NULL;
        bool is_merge_candidate = false;
        if (try_parse_scalar_dict_key(ctx, &anchor_name, &key_str,
                                      &is_merge_candidate, &anchor_value)) {
          /* The parser reaches this point only when the dispatch character
           * itself is '!', so the key carries a tag. It is a merge key only
           * when that tag is the core merge tag. See the doc comment of
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
        /* Not a key after all. try_parse_scalar_dict_key already restored
         * ctx->pos, so the code falls through to the ordinary tag handling
         * below. */
      }

      if (had_tag) {
        /* This matches the had_anchor check of the '&' branch above. This tag
         * is ordinary content for an ENCLOSING tag. That enclosing tag
         * already used the one tag that a single node may carry. This check
         * reads had_tag and not resolved_tag. An enclosing bare non-specific
         * "!" has a resolved_tag of NULL. See the doc comment of
         * parse_tag_token. The had_tag check still rejects a second tag
         * stacked under such a "!". */
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
        /* This matches the same check in the '&' branch above, whose own
         * comment is fuller. A flow-collection OPENER, which is '[' or '{',
         * placed directly against a tag has no valid reading in any context.
         * The text "!!seq[1,2]" inside "[!!seq[1,2]]" is one example. This
         * code therefore rejects it even in a flow context. The characters
         * ',', ']' and '}' are valid right after a tag INSIDE a flow
         * collection, as in "[!!str, x]". This code rejects those three only
         * in a block context, where they have no valid meaning at all. The
         * text "!!str,xxx" is one example, and a reference parser confirms
         * it. */
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
        /* The c-ns-properties production needs an s-separate(n,c) before any
         * ns-flow-content that follows. A verbatim or shorthand tag with no
         * separating whitespace at all before ordinary content has no valid
         * grammar path. That content can be "!<a>b", a quote, a block scalar
         * indicator, or plain scalar text. A reference parser confirms this,
         * and rejects both "!<a>b" and "!<a>|\n  x\n". The flow-opener case
         * just above already covers '[' and '{'. This code leaves a following
         * '#', '!', '&' or '*' to the more specific checks elsewhere in this
         * branch. Those checks cover a comment, a stacked tag, and an anchor
         * or alias that follows the tag. */
        parse_err(ctx,
                  "tag must be separated from its content by whitespace at "
                  "position %zu",
                  ctx->pos);
        _pni_release_tag(ctx, f);
        return _PSTEP_FAIL;
      }

      cyaml_node_t *n;
      if (!in_flow && rest_of_line_is_blank(ctx)) {
        /* Nothing else is on this line, except possibly a comment. This code
         * makes the same guard as the '&' branch, whose own comment is
         * above. Without it, a sibling entry at or below `indent` is read as
         * the value of this tag. One example is a list entry at the same
         * column as the "- !!str" itself. A descent with no check, whenever
         * the rest of the line is blank, walks straight past the newline. It
         * takes that sibling, and everything after it, into the value of
         * this tag. The comment of the '&' branch warns against exactly
         * that. */
        skip_ws_comments(ctx);
        bool is_value_col =
            !at_end(ctx) && at_block_value_col(ctx, indent, seq_ok_at_indent);
        if (at_end(ctx) || !is_value_col) {
          /* Nothing at all follows this tag. This code routes the case
           * through finalize_scalar_node, with empty text. It does not build
           * a bare CYAML_NULL directly. A "!!str" with nothing after it
           * therefore resolves to an empty string, and not to null. */
          char *empty = ccol_strdup(ctx->mp, "");
          n = empty ? finalize_scalar_node(ctx, empty, tag, true) : NULL;
        } else {
          _preq_node(rq, indent, in_flow, seq_ok_at_indent, true, had_anchor,
                     true, tag);
          f->state = _NS_AFTER_TAG_VALUE;
          return _PSTEP_DESCEND;
        }
      } else if (in_flow && at_eol(ctx)) {
        /* A flow context. The tagged value may wrap onto a following line.
         * The '&' branch above has the same case. This code must cross the
         * newline with flow_skip_ws, and not with a plain skip_ws_comments.
         * That holds this continuation line to the same s-separate(n,c) rules
         * about indentation and tabs that every other flow-collection
         * continuation line in this file obeys. The skip at the top of
         * parse_node covers only inline whitespace when in_flow is true. This
         * code must therefore cross the newline explicitly, before the
         * descent. Without that, the dispatch sees a '\n' next and reads it
         * as an empty plain scalar. There is no sibling-entry ambiguity to
         * guard against here, the way the block-context branch above has. The
         * ',', ']' and '}' terminators of a flow collection separate its
         * entries, and indentation does not. */
        if (!flow_skip_ws(ctx, indent)) {
          _pni_release_tag(ctx, f);
          return _PSTEP_FAIL;
        }
        _preq_node(rq, indent, in_flow, seq_ok_at_indent, allow_inline_map,
                   had_anchor, true, tag);
        f->state = _NS_AFTER_TAG_VALUE;
        return _PSTEP_DESCEND;
      } else if (!in_flow && at_bare_seq_indicator(ctx)) {
        /* This matches the same check in the '&' branch above. A block
         * sequence cannot start on the same line, right after a tag either.
         * Only a mapping gets that "compact" privilege. It gets that
         * privilege only when a '-' itself introduces it, and not when a node
         * property before it does. */
        parse_err(ctx,
                  "a block sequence cannot start on the same line as a "
                  "tag at position %zu",
                  ctx->pos);
        _pni_release_tag(ctx, f);
        return _PSTEP_FAIL;
      } else if (!in_flow && cur(ctx) == '!') {
        /* The same-line content of this tag is itself another tag. The
         * c-ns-properties production permits at most one tag for each node.
         * This matches the same-line double-anchor check of the '&' branch
         * above. Its own comment tells you why the had_tag check further up
         * cannot catch this on its own. */
        parse_err(ctx, "a node cannot carry two tags at position %zu",
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

      /* By this point something already consumed `tag`, at the exact place
       * of construction. That place can be many layers of descent deep. For
       * a scalar it is finalize_scalar_node. For a collection it is
       * finalize_collection_node. Both of them receive `tag` unchanged,
       * through any '&' anchor layers between. The doc comment of either
       * function tells you why the attach cannot wait until here, after the
       * descent returns. An anchor can sit between this tag and the real
       * content. Such an anchor clones a node with no tag into its own anchor
       * table, before this point ever runs. That whole chain only borrowed
       * this local copy, and nothing ever took it over. This code therefore
       * frees it here in every case, whatever path the parse took. */
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

      /* In a block context, a flow collection with a ':' right after it is
       * itself an implicit block mapping key, and it is not a scalar. The
       * "compact mapping" form of YAML 1.2 section 8.2.2, which is
       * ns-l-compact-mapping, permits any ns-flow-node as a key here, and not
       * only a scalar. The plain and quoted scalar dispatch branches already
       * make the same check for a scalar key. But like every implicit key,
       * whether or not it is a scalar, it must fit on one line. The
       * ns-s-implicit-yaml-key production states that. A flow collection that
       * covers more than one line is a valid standalone value. It is never an
       * implicit key, whatever follows it. */
      bool spans_multiple_lines = span_crosses_newline(ctx, f->u.n.coll_start);
      if (!in_flow && !spans_multiple_lines) {
        skip_inline_ws(ctx);
        if (!at_end(ctx) && cur(ctx) == ':') {
          char after =
              (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
          if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
              ctx->pos + 1 >= ctx->len) {
            if (!allow_inline_map) {
              /* The same-line chain restriction as the scalar-key cases
               * above. The text "a: [1,2]: c" has no valid reading, exactly
               * as "a: b: c" has none. See the doc comment of
               * allow_inline_map. */
              parse_err(ctx,
                        "mapping values are not allowed here "
                        "(position %zu)",
                        ctx->pos);
              __cyaml_destroy((cyaml)coll);
              return _PSTEP_FAIL;
            }
            char *key_str =
                node_to_dict_key_string(ctx, coll, "block dictionary");
            if (!key_str) return _PSTEP_FAIL;
            /* A flow collection can never be a merge-key candidate, because
             * "<<" is always a plain scalar. */
            /* The matching comment in the double-quoted scalar branch tells
             * you why resolved_tag is clearly safe to attach to the mapping
             * that results here. It does not go on the flow collection that
             * turned out to become the key of this mapping. */
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
        /* A tab after the '-' is separation whitespace (s-separate-in-line
         * of YAML 1.2 section 6.2), exactly like a space. What follows it
         * may be a scalar or a flow collection. A block collection may not
         * follow a tab, because a tab has no defined width, and the start of
         * every block collection refuses a tab in the whitespace before it
         * on its line. See line_indent_has_tab(). */
        if (next == ' ' || next == '\t' || next == '\n' || next == '\r') {
          _preq_block_list(rq, current_col(ctx));
          f->state = _NS_AFTER_COLLECTION;
          return _PSTEP_DESCEND;
        }
      } else {
        /* A '-' at the very end of the input is a list with one element, and
         * that element has a null value. */
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
        /* A tab is separation whitespace here too. See the '-' branch
         * above. */
        if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
          is_explicit_key = true;
      } else {
        is_explicit_key = true; /* a '?' at the very end of the input */
      }
      if (is_explicit_key) {
        if (!allow_inline_map) {
          /* The same-line chain restriction as every other "this could open a
           * fresh mapping here" case above. Those cases are a plain or quoted
           * scalar key, and a flow-collection key. The
           * ns-l-block-map-implicit-value production has no compact-mapping
           * alternative. A NEW explicit-key mapping that starts from the
           * same-line content of an ordinary implicit value therefore has no
           * valid reading either. The text "a: ? b\n   : c" is one example.
           * The explicit form is otherwise fully legal once the parser is
           * already inside explicit-style content. allow_inline_map is
           * already true there. */
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
              /* An implicit key always stays on one line, which the
               * ns-s-implicit-yaml-key production states. This is true for a
               * quoted key exactly as it is for a plain scalar key. A quoted
               * scalar that covers more than one line therefore has no valid
               * reading as a key. Two independent reference parsers confirm
               * this. */
              parse_err(ctx,
                        "implicit keys cannot span multiple lines at "
                        "position %zu",
                        ctx->pos);
              _ccol_mem_free(ctx->mp, s);
              return _PSTEP_FAIL;
            }
            if (!allow_inline_map) {
              /* The same-line chain restriction as the plain-scalar case
               * above. The text "a: 'b': c" has no valid reading, exactly as
               * "a: b: c" has none. See the doc comment of
               * allow_inline_map. */
              parse_err(ctx,
                        "mapping values are not allowed here "
                        "(position %zu)",
                        ctx->pos);
              _ccol_mem_free(ctx->mp, s);
              return _PSTEP_FAIL;
            }
            /* A quoted scalar can never be a merge-key candidate. See the
             * doc comment of try_parse_scalar_dict_key. Only the plain
             * scalar production is ever eligible. */
            f->u.n.pending_key = s;
            _preq_block_dict(rq, col, s, false, false);
            f->state = _NS_AFTER_COLLECTION;
            /* A resolved_tag can reach this "the value turns out to be a
             * key" shape in only one way. One or more '&' anchor layers must
             * pass that tag through. A tag that sits directly beside a key is
             * always caught earlier, by the preamble of
             * try_parse_scalar_dict_key in the '&' and '!' branches. There is
             * therefore no doubt here about what the tag belongs to. It
             * decorates the whole mapping that results. The explicit '?' case
             * and the flow-collection-as-key case elsewhere in this function
             * work in the same way. */
            return _PSTEP_DESCEND;
          }
        }
      }
      /* A quoted scalar is always a string, with no implicit typing. An
       * explicit tag can still force another type. */
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
              /* The same-line chain restriction as the plain-scalar case
               * above. The text "a: 'b': c" has no valid reading, exactly as
               * "a: b: c" has none. See the doc comment of
               * allow_inline_map. */
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

    /* A '#' can never start a plain scalar in any context. The
     * ns-plain-first production of YAML 1.2 section 6.6 excludes it
     * outright. A '#' that reaches this point means that skip_ws_comments or
     * skip_inline_ws already tried to treat it as a comment and failed. No
     * whitespace, no start of the input, and no newline came before it. No
     * valid reading is therefore left. */
    if (c == '#') {
      parse_err(ctx,
                "unexpected '#' at position %zu (a comment must be "
                "preceded by whitespace)",
                ctx->pos);
      return _PSTEP_FAIL;
    }

    /* The characters '%', '@' and '`' can also never start a plain scalar in
     * any context. The ns-plain-first production of YAML 1.2 section 6.6
     * excludes all three c-indicator characters outright. Section 5.5 also
     * reserves '@' and '`' for future use. One of them that reaches this
     * point means that every earlier, more specific dispatch already declined
     * to claim it. Those dispatches cover directives, tags, anchors, aliases,
     * block and flow indicators, and quoted scalars. No valid reading is
     * therefore left, exactly as for the '#' just above. needs_quoting(),
     * which the serializer uses, already needs quotes on output for all
     * three. Without this check, the parser accepts unquoted input that the
     * serializer of this library would never produce, and reports nothing. */
    if (c == '%' || c == '@' || c == '`') {
      parse_err(ctx,
                "unexpected '%s' at position %zu (not a valid plain scalar "
                "start character)",
                _cyaml_echo_c(c), ctx->pos);
      return _PSTEP_FAIL;
    }

    /* In a flow context, a '-', '?' or ':' with a flow indicator right after
     * it has no valid reading at all. The same holds when whitespace, the end
     * of the line, or the end of the input follows it. See the doc comment of
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

      /* Check whether a ':' value indicator follows. That makes this scalar
       * a dictionary key. An implicit key always stays on one line. This
       * check must therefore run before any try at a continuation over more
       * than one line below. */
      if (!in_flow) {
        skip_inline_ws(ctx);
        if (!at_end(ctx) && cur(ctx) == ':') {
          char after =
              (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
          if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
              ctx->pos + 1 >= ctx->len) {
            if (!allow_inline_map) {
              /* This scalar is itself the same-line content of an ordinary
               * implicit value. It has no licence to open one more nested
               * mapping here. The text "a: b: c" is one such input. The doc
               * comment of allow_inline_map, above parse_node, gives the
               * reasoning and the reference-parser check. */
              parse_err(ctx,
                        "mapping values are not allowed here "
                        "(position %zu)",
                        ctx->pos);
              _ccol_mem_free(ctx->mp, s);
              return _PSTEP_FAIL;
            }
            /* This is a block dictionary key. The comment in the
             * double-quoted branch above tells you why resolved_tag is
             * clearly safe to attach to the mapping that results here.
             *
             * This is also the one "becomes a key" site that a real plain
             * "<<" merge-key trigger with no tag can reach. An
             * anchor-decorated "<<" reaches the preamble of the '&' branch
             * instead. The doc comment of try_parse_scalar_dict_key gives the
             * exact rule that it must also satisfy.
             *
             * resolved_tag here is whatever tag an ENCLOSING '!' layer passes
             * down to the whole mapping that results. The text
             * "m: !!map\n  <<: *b\n" is one example. It says nothing about
             * whether the KEY "<<" itself carries a tag. The fast path of the
             * '!' branch above already catches a tag placed directly on the
             * key. Such a tag never reaches this plain-scalar dispatch at
             * all. A rule must not disqualify the merge whenever the
             * enclosing collection happens to carry a tag. Such a rule leaves
             * "<<" as a literal key with no expansion, and reports nothing. */
            bool is_merge_candidate = strcmp(s, "<<") == 0;
            /* This plain-scalar key must reach its canonical form through the
             * same core-schema typing that node_to_dict_key_string() already
             * applies at every OTHER place that captures a key. Those places
             * are three branches of try_parse_scalar_dict_key. Those branches
             * handle a flow collection, an alias as a key, and a plain scalar
             * with an anchor or a tag. They also include a key of a flow
             * dictionary or flow sequence, and an explicit "? key" block
             * key. Without this, a plain first entry such as "~: v" stores
             * the literal text "~", while "? ~\n: v" stores "null" for the
             * identical value. YAML 1.2 treats the two notations as exactly
             * the same entry. This code computes is_merge_candidate above
             * from the raw text s, and that is deliberate. Every other
             * merge-key check in this file does the same. Only a literal "<<"
             * with no type resolution is ever a trigger. */
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

      /* This is not a dictionary key. The value of a plain scalar may cover
       * more than one line. YAML 1.2 section 7.3.3 says so. */
      char *full = NULL;
      if (!parse_plain_scalar_multiline(ctx, in_flow, indent, s,
                                        hit_real_terminator, &full))
        return _PSTEP_FAIL;

      return _pni_finish(f,
                         finalize_scalar_node(ctx, full, resolved_tag, true));
    }
  }

after_collection: {
  /* Every block collection that this node turned out to be arrives here.
   * Those are a key that opens a mapping, where an anchor, an alias or a
   * tag decorates that key. They are also a flow collection that turned
   * out to be a mapping key, a '-' sequence, an explicit '?' mapping, and
   * a quoted or plain scalar key that opens a mapping.
   * Each of them frees the key text that it owns, when it owns one.
   * Each then decorates the finished collection with whatever tag an
   * enclosing layer passed down to it. The order is that one. */
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
 * Parse a block sequence, which is '-' entries at column
 * f->u.bl.seq_indent. The value of each entry is a descent. Everything else
 * is straight-line code.
 */
static _pstep_t _pbl_step(parse_ctx_t *ctx, _pframe_t *f, _preq_t *rq) {
  const int seq_indent = f->u.bl.seq_indent;
  cyaml_node_t *elem = NULL;

  if (f->state == _BLS_AFTER_ELEM) goto after_elem;

  f->u.bl.seq = (cyaml_node_t *)cyaml_create_list_interned(ctx->mp);
  if (!f->u.bl.seq) return _PSTEP_FAIL;

  while (!at_end(ctx)) {
    /* Check that the position is at the right indent, and at the '-'
     * indicator. */
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

    /* Ask whether the '-' is a list indicator. */
    {
      bool is_seq_entry = false;
      if (ctx->pos + 1 < ctx->len) {
        char next = ctx->src[ctx->pos + 1];
        /* See the '-' branch of the node dispatch: a tab is separation
         * whitespace, like a space. */
        if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
          is_seq_entry = true;
      } else {
        is_seq_entry = true; /* a '-' at the end of the input */
      }

      if (!is_seq_entry) break;
    }

    ctx->pos++; /* consume '-' */

    /* Skip optional space after '-'. Any further spaces and tabs are
     * separation whitespace, which the node dispatch skips. "- \t-" and
     * "-\t-" still fail: the nested block sequence refuses the tab in the
     * whitespace before it on its line. */
    if (!at_end(ctx) && cur(ctx) == ' ') ctx->pos++;

    /* Decide where the value of the element is. This code uses
     * rest_of_line_is_blank(), and not a bare at_eol() or at_end() check.
     * That is what treats "- # Empty", which is a dash and then only a
     * comment, in the same way as a bare "-". cur(ctx) is a '#' at this
     * point, and not a newline. at_eol() alone therefore concludes that
     * real content starts here. It hands the comment text itself to the
     * node dispatch as the value of this entry. That value then takes
     * every following sibling entry in as nested content. The correct
     * result is a null entry, followed by its own separate siblings. */
    if (rest_of_line_is_blank(ctx) || at_end(ctx)) {
      /* Nothing follows the '-' on this line. Look ahead at the next line
       * that is not empty. That line is the value of the element when it
       * is more indented than seq_indent. The element is null in every
       * other case, which is the same indent, less indent, or the end of
       * the input. */
      skip_ws_comments(ctx);
      /* A tab in the leading whitespace is separation, so only the spaces
       * before it count. See line_space_indent(). */
      if (at_end(ctx) || line_space_indent(ctx) <= seq_indent) {
        elem = node_alloc(CYAML_NULL, ctx->mp);
        goto have_elem;
      }
    }
    /* The element starts on the same line, or on a later line with more
     * indent. This call passes seq_indent. A block scalar nested inside the
     * element then uses the indent of the list as its parent_indent. The
     * YAML spec needs that. seq_ok_at_indent is false, because seq_indent
     * is the indent of a sequence and not of a mapping. A '-' at
     * exactly that column is therefore always a sibling element of this
     * same list, and never nested content. The parser reaches such a '-'
     * only through an anchor or a tag that decorates this very element.
     * allow_inline_map is true, because "- key: value" is always a valid,
     * ordinary pattern. That is a compact mapping right after the dash. The
     * "a: b: c" chain restriction applies only to the same-line content of
     * an ordinary implicit mapping VALUE. It does not apply to a sequence
     * element. */
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
      /* The list now owns the node. This flag records that. A later attach
       * of this node, reached through a borrowed cyaml_list_get()
       * reference, is then refused. Without the flag, that attach gives the
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
 * Parse the node that sits in a key position or a value position of a block
 * dictionary. That node is on the same line as the '?' or ':' indicator
 * before it. Or, when nothing follows before the end of the line, it is on
 * a later line with more indent. In every other case the position holds an
 * implicit null. Three call sites share this function. They are the
 * explicit-key side and the explicit-value side of parse_block_dictionary,
 * and the value of an implicit entry. All three share the placement grammar
 * of YAML 1.2 section 8.2.2 for content on a following line.
 *
 * allow_inline_seq marks the one point where an explicit entry and an
 * implicit entry really differ for a SEQUENCE. Explicit-style content is
 * s-l+block-indented in the grammar. That covers both the content of the
 * '?' key and the ':' value after it. The "compact" alternative of that
 * production permits a bare '-' sequence to start on the very same line. A
 * reference parser confirms this, and the code does not assume it. The
 * value of an ordinary implicit entry ("key: value") is instead
 * ns-l-block-map-implicit-value. That production has no compact alternative
 * at all. A sequence value there must always begin on its own line.
 *
 * The same asymmetry holds for a nested MAPPING chained on the very same
 * line. The text "a: b: c" has no valid reading and is a hard error, while
 * "? k\n: a: b" is accepted. Both restrictions apply together, and both
 * lift together, at exactly the same call site. This function therefore
 * passes allow_inline_seq directly as the allow_inline_map argument of
 * parse_node below. A second parameter that always held the same value
 * would add nothing. Callers pass true only for explicit-style key or value
 * content.
 *
 * is_merge_candidate_out, when it is not NULL, receives whether the node
 * that this function is about to parse is a real "<<" merge-key candidate.
 * See the doc comment of explicit_key_peek_is_merge_candidate. This
 * function decides that here, and the caller does not. This is the one
 * point that already resolved whether the content starts on this line or on
 * a later one. It is therefore the only position where the documented
 * precondition of the look ahead holds in both cases. That precondition is
 * "already at the start of the real content of the key". Pass NULL for a
 * value position, which can never be a merge key.
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
    if (at_end(ctx) || !is_value_col) {
      *node_out = node_alloc(CYAML_NULL, ctx->mp);
      return _PBMN_NODE;
    }
    /* ctx now sits exactly at the start of the real content of the node.
     * That content can begin on the line of this same call. It can also sit
     * on a fresh line, as it does here. This is the one position where the
     * documented precondition of explicit_key_peek_is_merge_candidate holds
     * for BOTH cases. That precondition is "already confirmed to be the
     * start of the content of a dictionary key". The look ahead therefore
     * lives here, and not in the caller of this function. A look ahead on
     * the caller side runs too early. At that point nothing knows whether
     * the content of the key starts on this line or on a later one. It
     * sees only whatever follows the '?' on the '?' line itself. That is
     * usually nothing but the newline that this branch exists to skip
     * past. It then concludes "not a merge candidate" for every explicit
     * key that covers more than one line. It does so even for a real
     * "? \n  <<\n: ..." merge. Only the key call site asks for this out
     * parameter. The value call site passes NULL, because a value is never
     * a merge key. */
    if (is_merge_candidate_out)
      *is_merge_candidate_out =
          explicit_key_peek_is_merge_candidate(ctx, map_indent, false);
    /* A value that starts on its own fresh line is never an ambiguous
     * same-line chain. The entry style does not matter. allow_inline_map is
     * therefore always true here. The same-line case below is different. */
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
 * Look ahead at the key text of a dictionary entry, and answer whether that
 * key is a real merge-key trigger. This function reads nothing: it restores
 * ctx->pos before it returns. A plain scalar with no tag, whose text is
 * exactly "<<", counts. So does a plain or quoted "<<" whose tag is the core
 * merge tag. An anchor does not disqualify it. A quote without that tag, any
 * other tag, or a key that is not a scalar does disqualify it.
 *
 * The name says "explicit key", and the parser still uses this function at
 * the start of the content of EVERY dictionary key. That covers the
 * implicit forms ("key:", "{key:...}" and "[key:...]"). It also covers the
 * explicit forms ("? key" and "{? key:...}"). A check of one character is
 * not enough to rule out a tag. Such a check only asks whether the very
 * first byte is a quote or a tag sigil. The c-ns-properties production
 * permits an anchor and a tag in either order. The text "&y !!str <<" has a
 * tag, although its first character is a '&'. Only a walk over the whole
 * anchor and tag property chain sees a tag that follows an anchor. This
 * function makes that walk. Two independent reference parsers confirm the
 * rule. The explicit '? <<' form is a real merge trigger under the same
 * conditions as the implicit '<<:' shorthand. That holds in a block context
 * and in a flow context. The explicit form is not exempt from merging
 * whatever quotes it carries.
 *
 * in_flow selects which whitespace skip this function uses. A true value
 * selects flow_skip_ws, which crosses lines. That matches the real parse of
 * a flow dictionary key, which this look ahead comes before. A false value
 * selects skip_inline_ws, because a block context keeps its own "a key
 * stays on one physical line" convention. Only flow_skip_ws reads indent.
 *
 * The boundary check after "<<" is deliberately coarser than the
 * termination rules of scan_plain_scalar_line. It is an OVER-approximation
 * of them, and not an exact copy. scan_plain_scalar_line does not stop at
 * ordinary whitespace inside a scalar. It stops at four things. Those are
 * the end of the line, a ': ' or a ':' at the end of the input, an inline
 * ' #' comment, and a flow terminator. A key such as "<< foo" therefore
 * really parses as the single scalar "<< foo", and not as "<<".
 * This function treats any whitespace right after "<<" as worthy of a
 * candidate. It can therefore flag a key that is not a merge key as a false
 * positive.
 *
 * That is safe, for one of two reasons at each call site. Most callers
 * check the real, fully parsed key text against the literal string "<<"
 * before they trust the flag. See the strcmp gate of
 * try_parse_scalar_dict_key and of parse_one_dict_entry_key. Two callers in
 * parse_flow_list skip that second check. Those are the "[? key: value]"
 * form and the bare "[key: value]" compact-mapping shorthand. They rely on
 * expand_merge_key instead. Its own cyaml_dictionary_get(map, "<<") lookup
 * is a safe no-op whenever the key of the freshly built single-entry map is
 * not "<<".
 *
 * The ':' handling does match the rule of scan_plain_scalar_line exactly.
 * That handling covers a ':' with whitespace or the end of the input right
 * after it. In a flow context it also covers a ':' with a flow terminator
 * right after it. A flow key of "<<:", with the colon against the "<<" and
 * no whitespace between, is one different plain scalar in a block context.
 * In a flow context it really resolves to just "<<". This one case must be
 * exact, unlike the whitespace over-approximation above, because that
 * distinction depends on it.
 *
 * The parser calls this function only at a position that it already
 * confirmed to be the start of the content of a dictionary key. For the
 * explicit form that is just past the '?' and its required separator. For
 * the implicit form that is the key position itself.
 *
 * A malformed anchor or tag gives the answer "not a merge candidate". A
 * flow_skip_ws failure, which means an unterminated flow collection, gives
 * the same answer. The real parse runs right after this call, from the
 * caller of this function. That parse finds and reports such an error
 * properly. This function clears ctx->error before it returns on either
 * failure path. parse_anchor_name() or flow_skip_ws() may already have
 * called parse_err(). Without the clear, the diagnostic of this throwaway
 * look ahead stays behind. It then hides a later, unrelated error message
 * from a different part of the same parse. Such a message is correctly
 * guarded with "only set it if nothing set it yet".
 *
 * That clear is also what makes the internal use of skip_tag_token() safe
 * here, in place of parse_tag_token(). This function needs the tag to be
 * valid only to recognise the merge tag on a key that can still be "<<". It
 * otherwise only skips past the tag, so that it can set had_tag. The real
 * parse still validates the tag when it runs.
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
        /* This is only a throwaway look ahead. See the doc comment of this
         * function. A failure here is never the real, final error for this
         * position. The real parse, right after this call, must report it.
         * parse_anchor_name() may have set ctx->error with parse_err().
         * This code clears it. Without that clear, the diagnostic of this
         * look ahead stays behind and is stale by then. It then hides a
         * later, unrelated error message elsewhere in the same parse. Such
         * a message is correctly guarded with "only set it if nothing set
         * it yet". */
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
      /* The same reasoning as the parse_anchor_name() failure above. The
       * error of this look ahead must not outlive the look ahead. */
      ctx->error[0] = '\0';
      ctx->pos = saved_pos;
      return false;
    }
  }
  /* A tag disqualifies the key unless it is the core merge tag, behind any
   * %TAG handle or in the verbatim form. The tag is resolved only for a key
   * that can still be "<<", so an ordinary tagged key pays nothing more
   * than the skip. A tag that does not resolve gives the answer "not a
   * merge candidate"; the real parse reports it. */
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
     * decoded, so that every spelling of "<<" counts. */
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
 * Finish the key of an explicit ("? key") entry, once its own node is in
 * hand. This function makes the canonical key string that the dictionary
 * stores. It then looks for the ':' value indicator that may follow that
 * key on a later line. It is split from _pode_begin() below, because the
 * node of the key is a descent. The two halves therefore run on either side
 * of that descent. Returns 1 on success, and -1 on an error.
 */
static int _pode_finish(parse_ctx_t *ctx, int map_indent,
                        cyaml_node_t *key_node, bool is_merge_candidate,
                        char **key_out, bool *have_colon_out,
                        bool *is_explicit_out, bool *is_merge_candidate_out) {
  if (!key_node) return -1;
  char *key_str = node_to_dict_key_string(ctx, key_node, "block dictionary");
  if (!key_str) return -1;

  /* The ':' value indicator, when there is one, is always on its own line
   * at exactly map_indent. YAML 1.2 section 8.2.2 says so: its
   * l-block-map-explicit-value production starts with s-indent(n) ":". The
   * indicator never comes on the same line after the key. This code
   * compares current_col() against an indentation level. It therefore needs
   * the same line_indent_has_tab() rejection that every other such
   * comparison in this file carries. The next-sibling-entry lookup of
   * parse_block_dictionary, a little further down, is one example.
   * current_col() counts a tab as one byte of width, the same as a space.
   * Without this check, a ':' indented with a tab lines up with map_indent
   * by byte offset alone, and the code accepts it and reports nothing. */
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
    /* A ':' acts as the value indicator only when a space, a tab, an end
     * of the line or the end of the input follows it. Every other decision
     * of this kind in this file uses the same rule. The rule of
     * scan_plain_scalar_line is one example. The ns-plain-first(c)
     * production permits a ':' as the first character of a plain scalar
     * when something other than a space follows it. The text ":adapter: pg"
     * at the indent of this mapping is therefore an ordinary entry
     * whose key is ":adapter". It is not the value indicator of this
     * explicit key. A ':' with no separator, accepted here, swallows that
     * entry whole. It also invents a submap for a key that the document
     * gives as null. */
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
 * Read the key of one block dictionary entry. Returns 1 when it made a key.
 * Returns 0 when this position is not a key at all, which means that the
 * mapping ends here. Returns -1 on a real error. Returns 2 when the key is
 * an explicit one whose own node the parser must read first. This function
 * then fills in the request, and the caller resumes through _pode_finish()
 * with the node that results.
 *
 * On the path that returns 2, *is_merge_candidate_out is already final.
 * _pbmn_begin() decides it before the descent. _pode_finish() then writes
 * the same value again. A -1 from _pode_finish() therefore leaves that out
 * parameter holding the value from the look ahead, and not false. No caller
 * reads it there, because every caller abandons the mapping on a -1.
 */
static int _pode_begin(parse_ctx_t *ctx, int map_indent, char **key_out,
                       bool *have_colon_out, bool *is_explicit_out,
                       bool *is_merge_candidate_out, _preq_t *rq) {
  char c = cur(ctx);
  *is_merge_candidate_out = false;

  /* A '?' is an explicit-key indicator only when whitespace, the end of the
   * line, or the end of the input follows it. The '?' check of the node
   * dispatch uses the same rule. Otherwise the '?' validly starts a plain
   * scalar key, as in "?foo: value". */
  bool is_explicit_key_indicator = false;
  if (c == '?') {
    if (ctx->pos + 1 < ctx->len) {
      char next = ctx->src[ctx->pos + 1];
      /* See the '?' branch of the node dispatch: a tab is separation
       * whitespace, like a space. */
      if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
        is_explicit_key_indicator = true;
    } else {
      is_explicit_key_indicator = true;
    }
  }

  if (is_explicit_key_indicator) {
    ctx->pos++;

    /* Skip at most one optional separator space. Further spaces and tabs
     * are separation whitespace. A block collection after a tab still
     * fails, as after a '-'. */
    if (!at_end(ctx) && cur(ctx) == ' ') ctx->pos++;

    /* _pbmn_begin itself decides this, once it knows whether the content
     * of the key starts on this line or on a later one. YAML 1.2 section
     * 8.2.2 lets an explicit key reach a fresh line. A merge-candidate
     * look ahead taken any earlier sees only the newline right after the
     * '?'. It never sees the real key. */
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

  /* Indicator characters that cannot start any kind of implicit key. A '-'
   * is a list indicator only when whitespace or the end of the input
   * follows it. Otherwise it may validly start a plain scalar key, as in
   * "-key:". This code does not exclude a '*', because
   * try_parse_scalar_dict_key handles an alias used directly as a key. It
   * does not exclude a '[' or a '{' either. A flow collection is a valid
   * implicit key that is not a scalar. YAML 1.2 section 8.2.2 permits
   * ns-flow-node in a block-key context, and not only a scalar. The '[' and
   * '{' branch of try_parse_scalar_dict_key parses exactly that. An
   * exclusion here breaks only the entries after the first entry of the
   * mapping. The node dispatch finds the first entry directly, and it has
   * no such filter. Whether the construct is accepted would then depend on
   * the position of the entry alone. */
  if (c == '-') {
    char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
    if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
      return 0;
  } else if (c == '|' || c == '>' || c == '#') {
    return 0;
  }

  /* An implicit key may itself carry an anchor, a tag, or both. The text
   * "&anchor key: value" is one example. try_parse_scalar_dict_key handles
   * both. It restores ctx->pos itself when this turns out not to be a
   * key. */
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
 * Merge the entries of one source mapping into target. This function skips
 * any key that target already holds. An explicit key always wins over
 * anything that a merge brings in. By the time this function runs, target
 * already holds every real explicit key. A merge expansion is always a
 * later pass, and it runs only after the parser finishes the whole mapping.
 *
 * This function clones each value with cyaml_clone before it inserts it.
 * cyaml_dictionary_set always takes ownership of its child. A value that
 * comes from a merge source still belongs to the tree of that source. The
 * same anchor may also be merged into two different target mappings, and
 * then several targets need it at once. An insert of the same pointer gives
 * that pointer two owners.
 *
 * Returns false on a source that is not a mapping, and on an OOM. It sets
 * ctx->error on both paths.
 */
static bool merge_one_source_into(parse_ctx_t *ctx, cyaml_node_t *target,
                                  cyaml_node_t *source) {
  if (!source || source->type != CYAML_DICTIONARY) {
    parse_err(ctx, "merge key source is not a mapping at position %zu",
              ctx->pos);
    return false;
  }
  /* Walk the source in its own insertion order, so the merged members of
   * one source arrive in the order that the source declares them. */
  for (const ccol_chmap_entry_ref *e = dict_first_entry(source); e;) {
    const cmap_pair *kp, *vp;
    e = ccol_chmap_entry_read(e, &kp, &vp);
    const char *key = (const char *)kp->ptr;
    if (cyaml_dictionary_get((cyaml)target, key)) continue;
    /* The copy and its new dictionary entry are both amplification: the
     * same source may be merged into any number of targets. */
    parse_amp_enter();
    cyaml clone = cyaml_clone((cyaml)_cyaml_read_child(vp->ptr));
    bool ok =
        clone && dictionary_set_impl((cyaml)target, key, clone) == ccol_success;
    parse_amp_leave();
    if (!ok) {
      if (_parse_amp_limit_hit)
        parse_err(ctx,
                  "merge key expansion exceeded the %zu-node, %zu-byte alias "
                  "expansion memory limit at position %zu",
                  (size_t)_CYAML_PARSE_NODE_FLOOR,
                  (size_t)_CYAML_PARSE_BYTE_FLOOR, ctx->pos);
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
  }
  return true;
}

/*
 * Move count members of map, starting at the entry first and following the
 * insertion order, to the end of that order, keeping their relative order.
 * Each member is taken out of the map and inserted again with the same key
 * and the same child, which an insert places after every other member. The
 * child is never freed or copied. A member that the insert refuses for lack
 * of memory is destroyed, and the function reports the failure; the parse
 * then fails as a whole.
 *
 * The key bytes live in the entry that the delete frees, so the insert reads
 * a copy of them: a stack buffer for an ordinary key, one allocation for a
 * longer one. Only a mapping that holds a "<<" entry with explicit members
 * after it pays for this.
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

/*
 * Expand the "<<" merge-key entry of a mapping. The caller must already
 * have confirmed a real merge-key trigger, which is a plain "<<" with no
 * tag, while it parsed map. The merge source may be one mapping. It may
 * also be a sequence of mappings. On a conflict, an earlier source wins
 * over a later one, by the precedence rule of this module.
 *
 * A mapping that a merge brings in may have had its own "<<" entry. That
 * entry is already fully expanded and removed by the time the parser
 * finished THAT mapping. A nested mapping is always fully complete,
 * including its own merge expansion, before anything registers an anchor
 * that names it. See the call sites of anchors_store. Expansion through
 * several levels therefore works with no special case here.
 *
 * This function works entirely through the public dictionary API, which is
 * cyaml_dictionary_get, _set and _remove, against the map that is now fully
 * built. It never touches the internals of a chmap directly.
 *
 * Returns false on a malformed merge source, and on an OOM. It sets
 * ctx->error on both paths. It returns true otherwise. It does nothing when
 * "<<" is not there. That is a standing defence: the caller calls this
 * function only when it already knows that it saw a candidate.
 */
static bool expand_merge_key(parse_ctx_t *ctx, cyaml_node_t *map) {
  cmap_pair kp = {.ptr = (void *)"<<", .size = sizeof("<<")};
  const cmap_pair *vp = NULL;
  if (chmap_get_elem_ref(map->value.dictionary, &kp, &vp) != ccol_success)
    return true;
  cyaml_node_t *merge_val = _cyaml_read_child(vp->ptr);
  /* Find the explicit members that follow the "<<" entry in the order of
   * map. merge_move_to_end() puts them back behind the merged members. */
  const ccol_chmap_entry_ref *after_first = NULL;
  size_t after_count = 0;
  {
    bool seen_merge = false;
    for (const ccol_chmap_entry_ref *e = dict_first_entry(map); e;) {
      const cmap_pair *ekp, *evp;
      const ccol_chmap_entry_ref *cur_e = e;
      e = ccol_chmap_entry_read(e, &ekp, &evp);
      if (seen_merge) {
        if (!after_first) after_first = cur_e;
        after_count++;
      } else if (ekp->size == sizeof("<<") &&
                 memcmp(ekp->ptr, "<<", sizeof("<<")) == 0) {
        seen_merge = true;
      }
    }
  }
  /* Detach the "<<" slot of map before any merge of a source into map.
   * Do not destroy it yet. The duplicate check of merge_one_source_into
   * below asks "does target already hold this key". It asks through the
   * public cyaml_dictionary_get(target, key) API. The "<<" entry of map
   * must therefore not still be attached during the merge loop. A source
   * may validly hold its own literal "<<" key, which is quoted and
   * triggers nothing. That key would then look "already present in
   * target", because its duplicate check matches the merge-trigger slot of
   * map that nothing removed yet. The code drops the real entry of the
   * source and reports nothing. The slot of map is then destroyed outright
   * once the merge finishes, which makes the loss worse. This code removes
   * the slot from the chmap of map first, and it does not destroy
   * merge_val, which the code below still needs. */
  if (chmap_delete_elem(map->value.dictionary, &kp) != ccol_success) {
    /* No real input reaches this. chmap_get_elem_ref just confirmed that
     * the key is there, and nothing between the two calls can change
     * that. */
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
  /* The merged members now follow every explicit member. Move the explicit
   * members that came after the "<<" entry to the end, so the merged
   * members take the place of the "<<" entry itself. */
  if (ok) ok = merge_move_to_end(ctx, map, after_first, after_count);
  return ok;
}

/*
 * Parse a block dictionary. The doc comment of the forward declaration
 * covers three things. It covers the two modes of first_key, which are a
 * key that the caller already read, and NULL for a first entry that a '?'
 * leads. It covers the meaning of first_key_colon_consumed. It covers the
 * meaning of first_key_is_merge_candidate. The frame carries all three.
 *
 * The key of each entry, when that key is explicit, and the value of each
 * entry are descents. Everything else is straight-line code.
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

  /* The first key of a block mapping sits on the current line at column
   * map_indent. A tab in front of it on that line leaves the column of the
   * mapping undefined, because a tab has no width: "\tb: c", "- \tb: c"
   * and "key:\tb: c" are all refused. The same key after spaces only is a
   * mapping at a defined column. Every later entry is refused the same way
   * by the line_indent_has_tab() test of the loop below. */
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

  /* The field f->u.bd.double_lt_is_merge_candidate records one thing. It
   * records whether the entry that wrote the literal key "<<" into the map
   * most recently was a real merge-key trigger. The code derives it again
   * each time the key text of an entry is exactly "<<". It writes over the
   * old value, and it never accumulates the values of every earlier entry
   * with an OR. The reason is that a duplicate "<<" key collapses to
   * whichever entry wrote it LAST. cyaml_dictionary_set gives a duplicate
   * key the "last value wins" rule. A later "<<" entry with explicit quotes
   * or an explicit tag writes an ordinary literal value over the earlier
   * one. An earlier "<<" entry that triggers a merge must therefore never
   * keep this field true after that. An OR across entries instead lets the
   * earlier entry force an expansion of the later, literal value. It can
   * also make the code reject that later value for no good reason. */
  if (f->u.bd.key != NULL) {
    /* A first key that the caller already read is always in the implicit
     * style. The scalar, flow-collection, anchor, tag and alias branches of
     * the node dispatch never reach here for an entry that a '?' leads. See
     * the doc comment of the forward declaration. */
    if (strcmp(f->u.bd.key, "<<") == 0)
      f->u.bd.double_lt_is_merge_candidate = f->u.bd.entry_is_merge_candidate;
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
      /* No real input reaches this. The code enters this mode only after
       * it confirms a '?' at the current position, and _pode_begin can
       * never turn that down. This branch is a standing defence. */
      if (r == 0)
        parse_err(ctx, "expected block dictionary key at position %zu",
                  ctx->pos);
      return _PSTEP_FAIL;
    }
    f->u.bd.key = f->u.bd.key_owned;
    /* The code reaches this mode only for the '?' explicit-key form. See
     * the doc comment of the forward declaration. Its own merge candidacy
     * is known only once the parser really reads the key. This code
     * therefore folds that in here. It does not arrive as
     * first_key_is_merge_candidate, which means something only for an
     * implicit first key that the caller already read. */
    if (strcmp(f->u.bd.key, "<<") == 0)
      f->u.bd.double_lt_is_merge_candidate = f->u.bd.entry_is_merge_candidate;
  }

  while (1) {
    /* Skip optional space after ':'. Further spaces and tabs are
     * separation whitespace ("key:\tvalue"). A block collection after a
     * tab still fails, as after a '-'. */
    if (f->u.bd.have_colon && !at_end(ctx) && cur(ctx) == ' ') ctx->pos++;

    /* Parse the value. An explicit key with no ':' at all has have_colon
     * false. Its value is null, by the "| e-node" alternative of YAML 1.2
     * section 8.2.2. The parser then reads nothing more for it.
     * allow_inline_seq is true only for the value of an explicit entry. The
     * doc comment of _pbmn_begin tells you why an implicit value and an
     * explicit value really differ here. */
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
      /* val is NULL for one of two reasons. The descent already set
       * ctx->error. Or node_alloc returned NULL on an OOM. This code sets a
       * diagnostic when no other message is there. The caller then always
       * gets a useful string. */
      if (!ctx->error[0])
        parse_err(ctx, "out of memory parsing dictionary value at position %zu",
                  ctx->pos);
      return _PSTEP_FAIL;
    }

    if (dictionary_set_impl((cyaml)f->u.bd.map, f->u.bd.key, (cyaml)val) !=
        ccol_success) {
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
    if (strcmp(f->u.bd.key, "<<") == 0)
      f->u.bd.double_lt_is_merge_candidate = f->u.bd.entry_is_merge_candidate;
  }

  if (f->u.bd.double_lt_is_merge_candidate &&
      !expand_merge_key(ctx, f->u.bd.map))
    return _PSTEP_FAIL;

  f->result = f->u.bd.map;
  f->u.bd.map = NULL;
  return _PSTEP_DONE;
}

/* ========================================================================== */
/*                         PARSER DRIVER                                      */
/* ========================================================================== */

/*
 * Initialize one frame from the request that asked for it. Returns false
 * when the frame cannot open at all. For a node frame that means the parse
 * reached the nesting cap. Nothing is then pushed. ctx->pos is still
 * exactly where the frame that made the request left it. The position that
 * the error names is therefore the position where something asked for the
 * descent.
 *
 * A node frame holds one ctx->depth level for as long as it stays on the
 * stack. That covers the whole parse of the collection that the node turns
 * out to be. _pframe_cleanup() releases that level again on every exit
 * path.
 */
static bool _pframe_open(parse_ctx_t *ctx, _pframe_t *f, const _preq_t *rq) {
  f->kind = rq->kind;
  f->state = 0;
  f->child = NULL;
  f->result = NULL;
  /* More than three quarters of the frames that a parse opens are node
   * frames. This code therefore answers that kind before the jump table
   * that the other four kinds share. */
  if (rq->kind == _PFK_NODE) {
    if (ctx->depth >= CYAML_MAX_PARSE_DEPTH) {
      parse_err(ctx, "maximum nesting depth (%d) exceeded at position %zu",
                CYAML_MAX_PARSE_DEPTH, ctx->pos);
      return false;
    }
    ctx->depth++;
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
  /* This code clears only the fields that a kind reads before it writes
   * them. Those are every pointer that the frame can come to own, so that
   * the cleanup sweep always sees a definite value. They are also every
   * flag that the frame carries across one turn of a loop. The code writes
   * every other field before it reads it. */
  switch (rq->kind) {
    case _PFK_NODE:
      break; /* the code above already answered this kind */
    case _PFK_FLOW_LIST:
      f->u.fl.indent = rq->indent;
      f->u.fl.seq = NULL;
      f->u.fl.key_str = NULL;
      break;
    case _PFK_FLOW_DICT:
      f->u.fd.indent = rq->indent;
      f->u.fd.map = NULL;
      f->u.fd.key_str = NULL;
      f->u.fd.double_lt_is_merge_candidate = false;
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
      f->u.bd.double_lt_is_merge_candidate = false;
      f->u.bd.entry_is_merge_candidate = rq->first_key_is_merge_candidate;
      break;
  }
  return true;
}

/*
 * Free everything that a frame still owns. When a parse fails, the driver
 * runs this for every frame left on the stack, innermost first. Each frame
 * owns its container outright. A collection that is still under
 * construction is not reachable from any root until the frame below it
 * takes that collection. Without this sweep, an abandoned stack leaks one
 * container for each open level.
 *
 * A finished frame goes through _pframe_release() instead. That function
 * asserts that the frame holds nothing, and it releases only the depth
 * level of the frame. This sweep belongs to the failure path alone.
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
      ctx->depth--;
      break;
    case _PFK_FLOW_LIST:
      __cyaml_destroy((cyaml)f->u.fl.seq);
      f->u.fl.seq = NULL;
      _ccol_mem_free(ctx->mp, f->u.fl.key_str);
      f->u.fl.key_str = NULL;
      break;
    case _PFK_FLOW_DICT:
      __cyaml_destroy((cyaml)f->u.fd.map);
      f->u.fd.map = NULL;
      _ccol_mem_free(ctx->mp, f->u.fd.key_str);
      f->u.fd.key_str = NULL;
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
      break;
  }
}

/*
 * Release a finished frame. A frame that reports itself done already handed
 * its node to the driver. It also let go of everything else that it owned.
 * The only thing left is the nesting level that a node frame holds. A test
 * build checks that instead of assuming it. The full sweep above is what
 * would otherwise catch anything the frame still holds.
 */
static void _pframe_release(parse_ctx_t *ctx, _pframe_t *f) {
#ifdef RUNNING_UNIT_TESTS
  ccol_assert(f->child == NULL);
  switch (f->kind) {
    case _PFK_NODE:
      ccol_assert(f->u.n.name == NULL && f->u.n.tag == NULL &&
                  f->u.n.pending_key == NULL);
      break;
    case _PFK_FLOW_LIST:
      ccol_assert(f->u.fl.seq == NULL && f->u.fl.key_str == NULL);
      break;
    case _PFK_FLOW_DICT:
      ccol_assert(f->u.fd.map == NULL && f->u.fd.key_str == NULL);
      break;
    case _PFK_BLOCK_LIST:
      ccol_assert(f->u.bl.seq == NULL);
      break;
    case _PFK_BLOCK_DICT:
      ccol_assert(f->u.bd.map == NULL && f->u.bd.key_owned == NULL);
      break;
  }
#endif
  /* A node frame is the only kind that holds a nesting level. It holds one
   * from the moment it opens. _pframe_open() refuses the frame outright
   * when the parse reaches the cap. Nothing therefore pushes such a frame
   * without a level. */
  if (f->kind == _PFK_NODE) ctx->depth--;
}

/* Run one step of whichever state machine this frame belongs to. */
static _pstep_t _pframe_step(parse_ctx_t *ctx, _pframe_t *f, _preq_t *rq) {
  /* Half of every step that a parse runs is a node dispatch. This code
   * answers that kind here, before the jump table that the four collection
   * kinds share. */
  if (f->kind == _PFK_NODE) return _pni_step(ctx, f, rq);
  switch (f->kind) {
    case _PFK_NODE:
      break; /* the code above already answered this kind */
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
 * Run one complete walk, from root_req, and return the node that it made.
 * Returns NULL on any failure. ctx->error is set on such a failure, unless
 * that failure was a bare allocation failure. Every caller already treats
 * both the same way.
 *
 * The frame stack lives in the locals of this function. When a document
 * nests deeper than _CYAML_PARSE_INLINE_FRAMES, the stack moves onto the
 * heap. Only two things ever cross between frames. The first is the request
 * that a frame fills in to ask for a child. The second is the finished node
 * that a child hands back. The driver stores that node into the `child`
 * slot of the parent. The cleanup of the parent then owns it, until the
 * resume point consumes it.
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
    /* A pointer carries the frame that this loop steps. The loop does not
     * index it from sp on each pass. A frame is neither 8 nor 16 bytes
     * wide, so an index costs a multiply. This loop would pay two of them
     * for each step. One finds the frame to step, and one hands the
     * finished node to the frame below. Only a growth moves the stack, and
     * that is the one place that builds the pointer again from sp. */
    _pframe_t *f = stack;
    for (;;) {
      _pstep_t r = _pframe_step(ctx, f, &req);
      if (r == _PSTEP_DESCEND) {
        if (sp == cap) {
          _pframe_t *grown = walk_stack_grow(stack, &cap, inline_frames,
                                             sizeof(*stack), ctx->mp);
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
      /* The result is _PSTEP_DONE. Hand the finished node to the frame
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
    /* Nothing is ever in flight outside a frame. The driver hands a
     * finished node straight into the frame below it, and that frame owns
     * it from then on. The unwind therefore only has to sweep the
     * frames. */
    while (sp > 0) {
      sp--;
      _pframe_cleanup(ctx, &stack[sp]);
    }
    result = NULL;
  }
  if (stack != inline_frames) _ccol_mem_free(ctx->mp, stack);

#ifdef RUNNING_UNIT_TESTS
  /* Every node frame releases the nesting level that it took. It does so on
   * the finished path and on the abandoned path alike. A walk therefore
   * always leaves the depth count exactly as it found it. That is what lets
   * a speculative key parse run a walk of its own. That parse is
   * try_parse_scalar_dict_key. It drops the result and rewinds ctx->pos.
   * The depth budget that it shares with the surrounding walk does not
   * drift. */
  ccol_assert(ctx->depth == entry_depth);
#endif
  return result;
}

/*
 * Read one directive line. ctx->pos is at the leading '%'. This function
 * accepts every directive name. The ns-reserved-directive production of
 * YAML 1.2 section 6.8.2 permits a name that the parser does not know. Such
 * a name may carry any number of parameters, and the parser ignores them.
 *
 * The name "YAML" gets its own strict grammar, which the spec fixes. That
 * grammar is l-yaml-directive ::= "YAML" s-separate-in-line
 * ns-yaml-version. It permits exactly one <major>.<minor> version token.
 * Nothing else may be on the line, except optional whitespace at the end
 * and a comment. Like any comment, whitespace must separate it from the
 * version. A comment placed directly against the version, with no space
 * between, is not a valid comment at all. It is malformed content at the
 * end of the line.
 *
 * Returns false and sets ctx->error on a malformed "%YAML" line. It returns
 * true otherwise. That covers a valid "%YAML" line, and any other directive
 * name, which it accepts as it is.
 *
 * *is_yaml_directive_out, when it is not NULL, reports whether the
 * directive name of this line was exactly "YAML". The caller needs that to
 * apply the rule of YAML 1.2 section 6.8.1. That rule says: "it is an error
 * to define more than one YAML directive for the same document". This
 * function itself keeps no state for each document, so it cannot apply that
 * rule.
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
    /* The separators of a directive are s-separate-in-line (YAML 1.2
     * section 6.8): any run of spaces and tabs. */
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

    /* A %TAG handle has exactly three valid forms. It is "!", which is the
     * primary handle. It is "!!", which is the secondary handle. Or it is a
     * "!", then one or more word characters, then a "!", which is a named
     * handle. A word character is alphanumeric or a '-'. The handle of a
     * shorthand tag token needs work to resolve. This one does not, because
     * the handle of a directive is a whole word on its own, with whitespace
     * on both sides. */
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

    /* The same separator rule as between the name and the handle. */
    if (at_end(ctx) || (cur(ctx) != ' ' && cur(ctx) != '\t')) {
      parse_err(ctx, "malformed %%TAG directive at position %zu", start);
      _ccol_mem_free(ctx->mp, handle);
      return false;
    }
    skip_inline_ws(ctx);

    size_t prefix_start = ctx->pos;
    /* A '#' does NOT stop this scan. The "extra content after the prefix"
     * scan further down is different. The grammar of a tag prefix is
     * ns-uri-char, from YAML 1.2 sections 5.6 and 5.7. It permits a '#' as
     * an ordinary URI character. A comment can start only after real
     * separating whitespace, by the s-b-comment production. It can never
     * sit directly against the prefix with nothing between them. A stop at
     * every '#' here truncates any prefix that holds one, for example a
     * fragment identifier, and reports nothing. It also treats the rest as
     * an ordinary comment at the end of the line, with no separator at all.
     * The suffix scan of parse_tag_token and skip_tag_token already treats
     * a '#' as an ordinary tag character, in the same way. */
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

    /* Only whitespace may follow the prefix. A comment may follow too, but
     * only once that whitespace separates it from the prefix. The %YAML
     * branch below applies the same "extra content after the version"
     * check. The l-tag-directive production of YAML 1.2 section 6.8.2 ends
     * in the same s-l-comments production as every other directive line. It
     * does not end in the free-form ns-reserved-directive parameter list
     * that a directive name the parser does not know gets. Without this
     * check, a line such as "%TAG !e! tag:example.com,2000:app/
     * garbage-text\n" drops "garbage-text" and reports nothing, instead of
     * being rejected. */
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

    /* YAML 1.2 section 6.8.2 says that "it is an error to define the same
     * handle more than once". Section 6.8.1 gives the same rule for a
     * duplicate %YAML directive. Two independent reference parsers confirm
     * this. Both reject a duplicate even when the second directive repeats
     * the exact same prefix. */
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
    /* The separator is s-separate-in-line: spaces and tabs. See the %TAG
     * branch above. */
    if (at_end(ctx) || (cur(ctx) != ' ' && cur(ctx) != '\t')) {
      parse_err(ctx, "malformed %%YAML directive at position %zu", start);
      return false;
    }
    skip_inline_ws(ctx);
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
    /* Only whitespace may follow. A comment may follow too, but only once
     * that whitespace separates it from the version. */
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

/* Read a '---' document-start marker. The caller must already have
 * confirmed the three dashes at ctx->pos, with at_doc_marker() and a check
 * for the literal "---". This function moves past the marker, and past the
 * separator whitespace and newline after it. It reports through
 * *same_line_content_out whether real content follows on the same physical
 * line. The separator may hold spaces and tabs ("---\tscalar").
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
 * Skip a UTF-8 byte order mark (EF BB BF) at ctx->pos when ctx->pos is at the
 * start of a line and no document is open. The l-document-prefix production
 * of YAML 1.2 (section 9.1.1) allows a byte order mark before every document
 * of a stream and not only before the first, which is what a concatenation of
 * files that each start with one produces. libyaml skips such a mark at
 * column 0 in the same way.
 *
 * The mark is not content, so the line that holds it starts right after it
 * for the purpose of column tracking. This function therefore restarts the
 * line bookkeeping at the new position: ctx->first_line_start, the line
 * cache and the memo of line_start_pos() all move there. Without that,
 * every column on the line comes out 3 too high, and the column-0 test of
 * at_doc_marker() refuses a "---" that follows the mark. The parser never
 * asks about a position before a completed document, so dropping the
 * earlier line starts loses nothing. The '#' tests of skip_ws_comments() and
 * rest_of_line_is_blank() treat ctx->first_line_start as the start of the
 * stream, so a comment may follow the mark directly.
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
 * Parse one YAML document, from the current position of the context.
 *
 * This function reads an optional leading '---' document-start marker. It
 * also reads any '%YAML' and '%TAG' directives before that marker, which it
 * ignores. It then parses one root node. It resets the anchor table of the
 * document. It finally reads an optional trailing '...' document-end
 * marker.
 *
 * On success it returns the root node, with ctx->pos past the document that
 * it read. That includes any trailing '...'. It leaves the position just
 * before any leading '---' of the NEXT document, so that the caller can
 * loop.
 *
 * *consumed_end_marker reports whether this function found and read a
 * trailing '...'. The l-yaml-stream production of YAML 1.2 section 6.9 lets
 * the following document leave out its own '---' in only one case. One or
 * more '...' end markers must come directly before it. The caller therefore
 * needs this value, to decide whether to need a '---' before the next
 * document.
 *
 * Returns NULL on a parse error, and sets ctx->error in that case. It always
 * destroys the anchor table before it returns.
 */
static cyaml_node_t *parse_one_document(parse_ctx_t *ctx,
                                        bool *consumed_end_marker) {
  /* Whether real root content follows an explicit '---' marker directly, on
   * that SAME physical line. The other case is a '---' alone, with content
   * that starts on a later line. The grammar of YAML 1.2 gives root content
   * reached this way only the "flow-in-block" alternative of s-l+block-node.
   * That alternative is a plain scalar, a quoted scalar, or a flow
   * collection. It is never the "block-in-block" alternative that a block
   * mapping or a block sequence needs. That alternative needs s-l-comments
   * directly after the '---', which means only whitespace, a comment, or a
   * newline. It needs them after any node properties that decorate the
   * '---' too. Two independent reference parsers confirm this. Both reject
   * "--- a: b" and "--- - a". Both accept "---\na: b", and a bare "a: b"
   * with no marker at all. */
  bool doc_marker_same_line_content = false;

  /* Whether this document already read its own leading '---'. A directive
   * can only come before the '---' that it configures. YAML 1.2 section
   * 6.8.1 says so. Once the parser reads the start marker of a document, a
   * '%' found after it can never belong to THIS document. It can only be
   * the directive prefix of the next document. This document is then empty,
   * and it ends right here. Without this flag, a bare '---' can be followed
   * on a later line by a document that carries a directive. The whole
   * "%directive\n---\ncontent" of that next document is then taken as
   * content of this document, and nothing is reported. The empty document
   * that this '---' really introduces is lost. */
  bool consumed_leading_dashes = false;

  /* Accept an optional leading document-start marker. By YAML 1.2, the
   * '---' token is a document-start indicator only when whitespace, a
   * comment, or the end of the input follows its three dashes. A '---X',
   * where X is any other character, is a plain scalar and not a marker. */
  if (at_doc_marker(ctx) && memcmp(ctx->src + ctx->pos, "---", 3) == 0) {
    consume_doc_start_marker(ctx, &doc_marker_same_line_content);
    consumed_leading_dashes = true;
  }

  /* Accept a %YAML or a %TAG directive, and report nothing about it. A
   * directive always needs an explicit '---' document-start marker after
   * it. YAML 1.2 section 6.8.1 ties a directive to one specific document.
   * A directive never belongs to "no document", nor to an implicit empty
   * one. The end of the input, or a plain '...' end marker, with no such
   * '---' between, is therefore a syntax error and not an empty document.
   * This code must track and apply that explicitly. It does not fall out of
   * the loop on its own. The !consumed_leading_dashes test gates this loop.
   * A document that already read its own '---' can never also own a
   * directive. See the doc comment of consumed_leading_dashes above. A '%'
   * found there must therefore stay untouched. The NEXT
   * parse_one_document call takes it as the prefix of the following
   * document. */
  bool saw_directive = false;
  bool saw_yaml_directive = false;
  bool consumed_doc_start_after_directive = false;
  while (!consumed_leading_dashes && !at_end(ctx) && cur(ctx) == '%') {
    saw_directive = true;
    bool is_yaml_directive = false;
    if (!parse_directive_line(ctx, &is_yaml_directive)) {
      /* A %TAG directive may already have filled ctx->tag_handles before
       * this directive line failed. A malformed %TAG after an earlier,
       * valid one is one example. This path never reaches the normal exit
       * of this function. Every early return through this directive section
       * must therefore clean up that table, and not only the exit at the
       * bottom of the function. Each of them cleans up the anchor table
       * too. That keeps the paths the same, and it protects a future
       * change, although nothing can fill the anchor table this early. */
      anchors_destroy(ctx);
      tag_handles_destroy(ctx);
      return NULL;
    }
    if (is_yaml_directive) {
      /* YAML 1.2 section 6.8.1 states this rule. "It is an error to define
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
      /* This store keeps consumed_leading_dashes true to its name, because
       * the code just read the marker. No current path reads the flag
       * again. The break below leaves the only loop that tests it, and
       * nothing after that loop looks at it. A static analyzer therefore
       * reports this store as dead. The store stays, because the
       * alternative is a variable that says no dashes were read, directly
       * after the code read them. That is wrong the moment the break
       * changes, or the moment a later reader appears. */
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

  /* An empty document. The position is at the end of the input, at another
   * document boundary, or at a fresh '%' directive line at column 0.
   * at_doc_marker() does not cover the last case, because it recognizes
   * only a literal '---' or '...'. But '%' is a c-indicator character, so a
   * plain scalar can never validly start with it. A bare '%' reached here
   * can only be the directive prefix of the next document. It is never the
   * content of this document. Without this check, two directive-only
   * documents in a row lose the directive line of the second one. The text
   * "%YAML 1.2\n---\n%YAML 1.2\n---\n" is one example. That line is read as
   * scalar content of the first document, with nothing reported, because
   * parse_node() knows nothing about directive syntax. */
  cyaml_node_t *root;
  if (at_end(ctx) || at_doc_marker(ctx) ||
      (current_col(ctx) == 0 && cur(ctx) == '%')) {
    root = node_alloc(CYAML_NULL, ctx->mp);
  } else if (doc_marker_same_line_content && at_bare_seq_indicator(ctx)) {
    /* A block sequence has no "flow-in-block" alternative either. It can
     * therefore no more start directly on the same line as a '---' than a
     * block mapping can. See the doc comment of
     * doc_marker_same_line_content. */
    parse_err(ctx,
              "a block sequence cannot start on the same line as "
              "its document start marker at position %zu",
              ctx->pos);
    root = NULL;
  } else {
    /* The value -1 is the indentation sentinel of the document root, from
     * YAML 1.2 section 8.1.1.1. The root node has no enclosing block
     * context. A block scalar at column 0, for example right after "--- >",
     * is therefore still more indented than its "parent". The parser must
     * not reject it as empty. allow_inline_map is false only when real
     * content sits directly on the same line as the '---'. An implicit
     * "key: value" mapping there has no valid grammar path either. That is
     * the same underlying rule as the bare sequence check above. This code
     * must therefore report it as an ordinary chained-mapping-value error.
     * It must not accept it and report nothing. */
    root = parse_node(ctx, -1, false, false, !doc_marker_same_line_content,
                      false, false, NULL);
  }

  /* The anchor table and the %TAG handle table both belong to one document.
   * Reset them before this function returns. */
  anchors_destroy(ctx);
  tag_handles_destroy(ctx);

  if (!root) return NULL;

  /* Read any trailing '...' document-end markers. The l-yaml-stream grammar
   * of YAML 1.2 section 6.9 groups one or more '...' markers in a row, with
   * its l-document-suffix+ production. That group is one unit of separator
   * material between documents. It is not one document boundary for each
   * marker. A second '...' found here therefore belongs to the closing of
   * THIS same document. Nothing but whitespace and comments sits between it
   * and the one that the code just read. It is not a fresh, separate
   * document of its own, which the empty-document check above would then
   * wrongly make empty. Without this loop, "a: 1\n...\n...\n" leaves the
   * second '...' for the next parse_one_document call. That call runs the
   * empty-document check above again. It then invents an extra CYAML_NULL
   * document out of what is only repeated separator noise. PyYAML parses
   * this input as one document.
   *
   * at_doc_marker() needs column 0. An indented '...' sequence is therefore
   * never swallowed as a document boundary with nothing reported. The outer
   * parse_common loop then reports it as content at the end of the
   * stream. */
  skip_ws_comments(ctx);
  *consumed_end_marker = false;
  while (at_doc_marker(ctx) && memcmp(ctx->src + ctx->pos, "...", 3) == 0) {
    ctx->pos += 3;
    /* Spaces and tabs may separate the marker from a comment. */
    skip_inline_ws(ctx);
    /* Only whitespace may share the line of the '...' marker. An optional
     * comment may share it too. Real content there, such as "... invalid",
     * has no valid reading. Without this check, the code leaves that
     * content for the caller and reports nothing. The caller then reads it
     * as the start of a bare next document. */
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
 * One rule covers the err out-parameter of EVERY module in this library: the
 * string belongs to the library, and a caller must never free it. Ten other
 * modules keep that rule by handing back a CCOL_ERR_STR() literal. This
 * parser cannot, because its message names a position and a token, so the
 * text is not known until it fails.
 *
 * A heap allocation is the obvious alternative and it is the wrong one. It
 * gives the same-looking `char **err_str` parameter a second, opposite
 * ownership rule, and nothing in the type or at the call site tells the two
 * apart. A caller that learned "never free it" from cvector or chashmap
 * leaks here; a caller that learned "free it" here calls free() on a string
 * literal there, which crashes. It would also allocate on a path that a
 * failed allocation can reach.
 *
 * This buffer removes the question. The library owns the storage, the caller
 * never frees anything, and the rule is the same everywhere. The lifetime is
 * the strerror(3) and dlerror(3) convention, which this header documents: the
 * text stays valid until the NEXT failing parse on the SAME thread. It is
 * per-thread, so two threads that parse at the same time never overwrite one
 * another's message.
 *
 * The buffer lives on the heap and the thread holds a pointer to it, which
 * the first failing parse of the thread allocates and the thread-exit drain
 * of the node pool frees. A thread-local array of this size would sit in the
 * thread-local block of the library, which the loader must fit into its
 * small reserve when a process loads the library with dlopen(). When the
 * allocation fails, *err_str points at a fixed message instead, which the
 * caller does not free either.
 *
 * 512 bytes is the size of parse_ctx_t.error, which is where every message
 * of this parser is built. Nothing can arrive here longer than that.
 */
#define CYAML_ERR_BUF_LEN 512
static const char cyaml_err_unstored[] =
    "parse failed; no memory was left to store the error message";

/* The message buffer of the calling thread, allocated on its first use, or
 * NULL when the allocation fails. */
static char *cyaml_err_storage(void) {
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  ccol_call_once(_pool_key_once, _do_pool_key_init);
  _cyaml_tls_t *t = _cyaml_tls_get();
  if (!t) return NULL;
  if (!t->err_buf) t->err_buf = malloc(CYAML_ERR_BUF_LEN);
  return t->err_buf;
#else
  if (!cyaml_err_buf) {
    char *buf = malloc(CYAML_ERR_BUF_LEN);
    if (!buf) return NULL;
    cyaml_err_buf = buf;
    ccol_call_once(_pool_key_once, _do_pool_key_init);
    _pool_key_arm();
  }
  return cyaml_err_buf;
#endif /* _CCOL_EMULATE_DARWIN_TLS */
}

#ifdef RUNNING_UNIT_TESTS
/* Every message that a parse reports is printable ASCII (see
 * cyaml_err_echo()). Under the unit tests and the fuzz targets, a message
 * that breaks that rule ends the process at the point that produced it. */
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

/* Copies msg into the per-thread buffer and points *err_str at it. It is a
 * no-op when the caller passed no err_str. */
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
 * This function handles an optional UTF-8 BOM (EF BB BF). It then loops over
 * every document in the stream. Optional '---' and '...' markers separate
 * those documents. It resets the anchor table between documents, so that no
 * anchor crosses a boundary.
 *
 * A stream with one document gives the root node directly.
 *
 * A stream with more than one document gives one CYAML_LIST node. The
 * elements of that node are the individual document roots, in order. Every
 * document after the first must begin with a '---' marker. Bare content
 * after the end of a document is a parse error.
 *
 * On failure this function points *err_str, when err_str is not NULL, at
 * the per-thread message buffer that cyaml_report_err() fills. The library
 * owns that buffer and the caller never frees it. This function destroys
 * every document that it parsed successfully before it returns NULL.
 */
static cyaml parse_common(const char *src, size_t len, char **err_str,
                          ccol_memmgmt_procs_t *mp) {
  if (!src) {
    cyaml_report_err(err_str, "null input");
    return NULL;
  }
  /* The whole stream must be c-printable UTF-8 before any scanner reads it.
   * See cyaml_first_refused_char(). */
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
                        asked cannot match a memo that was never filled in.
                        Stated rather than left to the zero this field would
                        otherwise get, because zero is a real position, and the
                        BOM branch below rewrites line_starts[0] after this
                        point: a memo that appeared valid at position zero would
                        answer with the pre-BOM line start. */
                     .memo_pos = SIZE_MAX};
  parse_node_budget_arm(len);

  /* Allocate the position cache of line_start_pos() here, before any parse
   * function runs. The doc comment of that function tells you why a failure
   * of this one allocation is fatal. A later growth of the same cache is
   * different, and it degrades gracefully. */
  ctx.line_starts = _ccol_mem_alloc(mp, 64 * sizeof(size_t));
  if (!ctx.line_starts) {
    cyaml_report_err(err_str, "out of memory");
    parse_node_budget_disarm();
    return NULL;
  }
  ctx.line_starts_cap = 64;
  ctx.line_starts[0] = 0;
  ctx.line_starts_len = 1;

  /* Skip the byte order marks of the first document prefix. See
   * skip_document_prefix_bom(). */
  while (skip_document_prefix_bom(&ctx)) {
  }

#ifdef RUNNING_UNIT_TESTS
  if (cyaml_test_force_line_cache_disabled) ctx.line_cache_disabled = true;
#endif

  /* The leading whitespace of the stream may hold tabs: they are
   * separation in front of a flow node. A block collection after them fails
   * at its own start. See line_space_indent(). A comment line may sit
   * between two byte order marks of the prefix, as between documents. */
  skip_ws_comments(&ctx);
  while (skip_document_prefix_bom(&ctx)) skip_ws_comments(&ctx);

  /* Temporary array that accumulates parsed document roots. */
  cyaml_node_t **docs = NULL;
  size_t ndocs = 0;
  size_t dcap = 0;
  bool prev_consumed_end_marker = false;
  size_t bom_pos = SIZE_MAX;

  while (!at_end(&ctx)) {
    /* After the first document, the next document must start with a '---'.
     * There is one exception. A '...' end marker may have ended the
     * document before it. A bare document, with no '---', may then follow
     * directly. The l-yaml-stream production of YAML 1.2 section 6.9 says
     * so. A document with no '...' before it must be explicit. A document
     * that does follow a '...' may be bare, may carry directives, or may be
     * explicit. Bare content with no '...' before it is ambiguous material
     * at the end of the stream. */
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

    bool consumed_end_marker = false;
    cyaml_node_t *root = parse_one_document(&ctx, &consumed_end_marker);
    if (!root) {
      if (ctx.error[0]) {
        cyaml_report_err(err_str, ctx.error);
      } else {
        /* Every real syntax rejection in this parser reports a specific
         * diagnostic with parse_err(), before it returns a failure. That is
         * the convention of this file throughout. Only two things reach
         * this point with ctx.error still empty.
         *
         * The first is an allocation that failed deep in the call chain,
         * with no diagnostic of its own to report. Many low-level helpers
         * that build the DOM are shared with the public API, which does no
         * parsing. node_alloc() and cyaml_create_dictionary_mp() are two
         * examples. They have no parse_ctx_t to report through at all.
         *
         * The second is one of the two parse budgets of this thread running
         * out at such a construction site. Those budgets are
         * CYAML_MAX_PARSE_NODES and CYAML_MAX_PARSE_BYTES. See the doc
         * comments of those macros. node_alloc() itself has no way to
         * report through ctx.error either, for the same reason.
         *
         * This code tells the cases apart, instead of reporting every one
         * of them as a general allocation failure. That matters, because
         * neither budget case is an OOM at all. Each of them fires with
         * memory freely available. A caller that saw "out of memory" for an
         * ordinary large document would have no way to tell the two
         * apart. */
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

    /* Grow the docs array if needed. */
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
    /* A byte order mark may open the prefix of the next document. See
     * skip_document_prefix_bom(). The position of the first one names the
     * error when no document start follows. */
    bom_pos = ctx.pos;
    if (skip_document_prefix_bom(&ctx)) {
      skip_ws_comments(&ctx);
      while (skip_document_prefix_bom(&ctx)) skip_ws_comments(&ctx);
    } else {
      bom_pos = SIZE_MAX;
    }
  }

  /* The input is empty. Give a null node back. That matches the parse of an
   * empty plain scalar. The YAML 1.2 core schema resolves an empty value to
   * null. */
  if (ndocs == 0) {
    _ccol_mem_free(mp, docs);
    cyaml_node_t *empty = node_alloc(CYAML_NULL, mp);
    if (!empty) {
      /* This code must not set *err_str to NULL before it confirms
       * success. Every other OOM path in this function sets a real message
       * before it returns NULL. The documented contract says that *err_str
       * is NULL only on success. A caller that obeys that contract must
       * therefore never see a NULL result beside a NULL message here. */
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

  /* Single document: return the root node directly. */
  if (ndocs == 1) {
    cyaml_node_t *result = docs[0];
    _ccol_mem_free(mp, docs);
    if (err_str) *err_str = NULL;
    parse_node_budget_disarm();
    _ccol_mem_free(mp, ctx.line_starts);
    return (cyaml)result;
  }

  /* More than one document. Wrap them in a CYAML_LIST. */
  cyaml_node_t *list = (cyaml_node_t *)cyaml_create_list_interned(mp);
  if (!list) {
    cyaml_report_err(err_str, "out of memory");
    goto fail;
  }
  for (size_t i = 0; i < ndocs; i++) {
    cyaml_node_t *elem = docs[i];
    if (cvector_push_back(list->value.list, &elem) != ccol_success) {
      /* Destroy every document that the list does not own yet. */
      for (size_t j = i; j < ndocs; j++) __cyaml_destroy((cyaml)docs[j]);
      _ccol_mem_free(mp, docs);
      __cyaml_destroy((cyaml)list);
      cyaml_report_err(err_str, "out of memory");
      parse_node_budget_disarm();
      _ccol_mem_free(mp, ctx.line_starts);
      return NULL;
    }
    /* The stream list now owns this node. See the guard of
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
 * parse_common() inside one "C" locale scope. The core schema writes the
 * decimal point of a float as '.', whatever LC_NUMERIC says, and the strtod()
 * of try_parse_float_scalar() and the "%g" of format_float_key() follow
 * LC_NUMERIC. One scope around the whole parse costs two uselocale() calls
 * for each document, and nothing for each scalar. Without it, a thread whose
 * locale writes ',' as its decimal point reads "1.5" as a string.
 */
static cyaml parse_in_c_locale(const char *src, size_t len, char **err_str,
                               ccol_memmgmt_procs_t *mp) {
  ccol_c_locale_scope_t scope = ccol_c_locale_enter();
  cyaml root = parse_common(src, len, err_str, mp);
  ccol_c_locale_leave(scope);
  return root;
}

/* Parse a YAML string that ends with a NUL byte. mp may be NULL, which
 * selects the default allocator. On a failure, when err_str is not NULL,
 * *err_str points at a per-thread message that the library owns. The caller
 * never frees it, and it stays valid until the next failing parse on the
 * same thread. cyaml.h documents that contract. */
cyaml cyaml_parse_mp(const char *yaml_str, char **err_str,
                     ccol_memmgmt_procs_t *mp) {
  if (!yaml_str) return parse_common(NULL, 0, err_str, mp);
  return parse_in_c_locale(yaml_str, strlen(yaml_str), err_str, mp);
}

/* The same as cyaml_parse_mp, but this function takes an explicit byte
 * length. The input therefore does not have to end with a NUL byte. */
cyaml cyaml_parse_n_mp(const char *yaml_str, size_t len, char **err_str,
                       ccol_memmgmt_procs_t *mp) {
  return parse_in_c_locale(yaml_str, len, err_str, mp);
}

/* ========================================================================== */
/*                         SERIALIZER HELPERS                                 */
/* ========================================================================== */

/*
 * Returns true in one case only. The tag of n must be one of the seven
 * core-schema tag URIs of YAML 1.2. It must also match the type of n.
 * CYAML_TAG_INT on a CYAML_INTEGER node is one example, and CYAML_TAG_SEQ
 * on a CYAML_LIST node is another. The serializer never writes such a tag.
 * Its own output discipline already makes sure that the value round trips
 * through its implicit type with no help. That discipline has three parts.
 * The first is the full coverage of needs_quoting() against a misread as
 * another type. The second is the float-against-integer guard of
 * yb_append_double(). The third is the fixed, unambiguous literal form of
 * every other type.
 * A tag that only restates the type would therefore add nothing.
 *
 * No public entry point builds a node whose core-schema tag names another
 * type. cyaml_node_set_tag() refuses such a tag, and every call that changes
 * the type of a node drops a core tag that stops matching. This function
 * still compares the type instead of treating every one of the seven URIs as
 * omittable, so that a mismatched core tag, if one ever reached the
 * serializer, would go out in verbatim !<...> form like a custom tag and
 * would not vanish without a report.
 */
static bool tag_matches_node_type(const cyaml_node_t *n) {
  if (!n->tag) return false;
  cyaml_node_type_t expected;
  /* A tag that is not a core-schema tag is never redundant. */
  if (!core_tag_expected_type(n->tag, &expected)) return false;
  return n->type == expected;
}

/*
 * Returns true when the serializer must quote the string s. It returns
 * false when s can go out as a plain YAML scalar with no quotes. Such a
 * string does not look like a null, a bool or a number. It holds no
 * indicator character, and it is not empty. Its strtod() runs inside the
 * "C" locale scope of the serialize call.
 */
static bool needs_quoting(const char *s) {
  if (!s || s[0] == '\0') return true;

  /* A U+FEFF in UTF-8, at the very start of a document, is a byte order
   * mark and not content. A parse removes it. A scalar whose own first
   * character is U+FEFF therefore needs quotes. The mark is then inside the
   * content of a double-quoted scalar, where nothing removes it. Only a
   * scalar that the serializer writes as the very first bytes of the output
   * can reach offset 0. This code still applies the test to every scalar,
   * for the same reason as the "<<" case below. One shared rule stays in
   * force at a future emission site, where a special case for each call
   * site can go missing.
   * Without this, a dictionary whose first key is "\xEF\xBB\xBFz"
   * serializes to "\xEF\xBB\xBFz: 7". It then parses again under the key
   * "z", and it loses the original key and its value with no report. */
  if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
      (unsigned char)s[2] == 0xBF)
    return true;

  /* The strings that a parse would read again as another type. This list
   * always holds "<<", in a key position and in a value position alike.
   * The ambiguity that "<<" guards against is merge-key detection, and only
   * a key position ever reaches that. One shared check here is still safer
   * than a special case at the key-emission call sites alone. Such a
   * special case can miss a future use in a value position. The cost is one
   * harmless quote that nothing needs, on the rare literal scalar value
   * "<<".
   *
   * The list covers two schemas, and not one. The first group is what THIS
   * parser resolves as a null, a bool or a special float under the YAML 1.2
   * core schema. The second group, which starts at "y", is the set that YAML
   * 1.1 resolves as a bool and that 1.2 leaves as a plain string. This
   * module reads "yes" as the string "yes", and it is right to. But a plain
   * unquoted "yes" in its OUTPUT is read as the boolean true by every YAML
   * 1.1 reader, which is most of the deployed ones: PyYAML in its default
   * mode, Ruby's Psych and Go's yaml.v2 among them. A document that goes out
   * of this serializer and into one of those tools would change meaning with
   * nothing to report it. That is the "Norway problem", where the country
   * code NO becomes false. Quoting these on the way out costs two bytes and
   * settles the question for every reader, whichever schema it follows. It
   * changes nothing about how this module PARSES them.
   *
   * The switch on the first byte is what keeps the list cheap. A scalar such
   * as "record_5" settles every question below with one comparison of one
   * byte, instead of entering the C library once for each candidate. */
  switch (s[0]) {
    case '~':
    case 'n':
    case 'N':
    case 't':
    case 'T':
    case 'f':
    case 'F':
    case '.':
    case '+':
    case '-':
    case 'y':
    case 'Y':
    case 'o':
    case 'O':
    case '<':
      if (strcmp(s, "~") == 0 || strcmp(s, "null") == 0 ||
          strcmp(s, "Null") == 0 || strcmp(s, "NULL") == 0 ||
          strcmp(s, "true") == 0 || strcmp(s, "True") == 0 ||
          strcmp(s, "TRUE") == 0 || strcmp(s, "false") == 0 ||
          strcmp(s, "False") == 0 || strcmp(s, "FALSE") == 0 ||
          strcmp(s, ".inf") == 0 || strcmp(s, ".Inf") == 0 ||
          strcmp(s, ".INF") == 0 || strcmp(s, "+.inf") == 0 ||
          strcmp(s, "+.Inf") == 0 || strcmp(s, "+.INF") == 0 ||
          strcmp(s, "-.inf") == 0 || strcmp(s, "-.Inf") == 0 ||
          strcmp(s, "-.INF") == 0 || strcmp(s, ".nan") == 0 ||
          strcmp(s, ".NaN") == 0 || strcmp(s, ".NAN") == 0 ||
          strcmp(s, "<<") == 0 ||
          /* YAML 1.1 booleans. See the comment above. */
          strcmp(s, "y") == 0 || strcmp(s, "Y") == 0 || strcmp(s, "yes") == 0 ||
          strcmp(s, "Yes") == 0 || strcmp(s, "YES") == 0 ||
          strcmp(s, "n") == 0 || strcmp(s, "N") == 0 || strcmp(s, "no") == 0 ||
          strcmp(s, "No") == 0 || strcmp(s, "NO") == 0 ||
          strcmp(s, "on") == 0 || strcmp(s, "On") == 0 ||
          strcmp(s, "ON") == 0 || strcmp(s, "off") == 0 ||
          strcmp(s, "Off") == 0 || strcmp(s, "OFF") == 0)
        return true;
      break;
    case '=':
      /* A lone "=" is the YAML 1.1 "value" key type. PyYAML resolves it to
       * tag:yaml.org,2002:value and then refuses to construct it. */
      if (s[1] == '\0') return true;
      break;
    default:
      break;
  }

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
   * number. A '+' needs quotes only when a digit follows it, for example
   * "+42" and "+100". The explicit strcmp block above already catches
   * "+.inf", "+.Inf" and "+.INF". Another '+.' string, such as "+.foo", is
   * a valid plain scalar. The indicator-character check above already
   * catches '-'. */
  if (fc >= '0' && fc <= '9') return true;
  if (fc == '+' && (s[1] >= '0' && s[1] <= '9')) return true;
  /* strtod parses a "+.N" pattern as a float. Two examples are "+.3" and
   * "+.5e-10". Neither the digit check nor the "+.inf" check above catches
   * such a pattern. strtod() sets errno to ERANGE in two cases. The first
   * is a real overflow, where it clamps the result to positive or negative
   * infinity. The second is a valid underflow to a subnormal value or to
   * 0.0, which is a correct numeric result. Only the first case means that
   * the string does NOT parse as a real float. A bare "errno != ERANGE"
   * check here would disagree with try_parse_float_scalar(). That function
   * makes the identical distinction between ERANGE and a real overflow. See
   * its own doc comment. Such a check lets a CYAML_STRING value like
   * "+.1e-400" go out with no quotes. It then parses again as the
   * CYAML_FLOAT 0.0, instead of round tripping as a string. */
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
    /* A tab inside a plain scalar is valid YAML 1.2, but PyYAML and libyaml
     * end a plain scalar at it and then refuse the rest of the line. */
    if (c == '\t') return true;
    /* PyYAML and libyaml treat '?' as an indicator inside a plain scalar in
     * a flow context, and a '?' followed by a space as a mapping key in a
     * block context. Quoting every '?' keeps the output readable by both in
     * every context. */
    if (c == '?') return true;
    /* NEL (U+0085), LS (U+2028) and PS (U+2029) are line breaks in YAML 1.1.
     * A 1.1 reader folds or splits a plain scalar that holds one raw, so the
     * string goes out double-quoted, where yb_append_yaml_dquoted() writes
     * each as its \N, \L or \P escape. */
    if ((unsigned char)c == 0xE2 && (unsigned char)p[1] == 0x80 &&
        ((unsigned char)p[2] == 0xA8 || (unsigned char)p[2] == 0xA9))
      return true;
    /* The c-printable production of YAML 1.2 section 5.1 leaves out the C1
     * controls U+0080..U+009F, except NEL, and the noncharacters U+FFFE and
     * U+FFFF. libyaml, PyYAML and go-yaml refuse a whole document that holds
     * one of them raw in any scalar style. Such a string goes out
     * double-quoted, where yb_append_yaml_dquoted() writes each one as an
     * escape. NEL (C2 85) is in the same byte range and needs quoting for
     * the reason given above. */
    if ((unsigned char)c == 0xC2 && (unsigned char)p[1] >= 0x80 &&
        (unsigned char)p[1] <= 0x9F)
      return true;
    if ((unsigned char)c == 0xEF && (unsigned char)p[1] == 0xBF &&
        ((unsigned char)p[2] == 0xBE || (unsigned char)p[2] == 0xBF))
      return true;
    /* A raw C0 control byte, other than a tab, has no valid unescaped form
     * in ANY scalar style. See the doc comment of
     * is_disallowed_control_byte. Such a byte written straight into a plain
     * scalar makes output that no conformant YAML 1.2 parser can read back.
     * That includes this parser, because parse_common() refuses it.
     * yb_append_yaml_dquoted() is what really escapes the byte, through its own
     * "\xXX" fallback for c < 0x20. */
    if (is_disallowed_control_byte(c)) return true;
    /* The c-flow-indicator production of YAML 1.2 section 7.4 holds five
     * characters: ',', '[', ']', '{' and '}'. Each of them ends a plain
     * scalar in a flow context. A string that holds one of them anywhere
     * therefore needs quotes to survive a flow round trip. This code applies
     * the rule always, and not only for real flow-style output. That matches
     * the standing rule of this function, which quotes for the strictest
     * context that the string may reach. Block-style output pays one
     * harmless quote that nothing needs. A plain scalar in a block context
     * accepts all five characters with no restriction. */
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
  /* The plain scalar parser drops the spaces and tabs at the end. */
  if (p > s && (*(p - 1) == ' ' || *(p - 1) == '\t')) return true;
  return false;
}

/*
 * Return true when the dictionary key s is exactly the canonical text of an
 * integer or a float key, as node_to_dict_key_string() makes it. Such a key
 * came from a plain numeric scalar, or it spells one exactly. The
 * serializer writes it with no quotes, so that the output keeps the numeric
 * type for every reader, and a parse of that output canonicalizes the plain
 * scalar back to this same key string.
 *
 * Any other spelling of a number is not canonical ("010", "0x10", "+5",
 * "3.10", "-0"), because a parse of it plain would store a different key.
 * needs_quoting() then quotes it, and a parse keeps its exact text.
 */
static int _lltoa_y(char *buf, long long v);

static bool key_is_canonical_number(const char *s) {
  /* Every canonical numeric text starts with a digit, a '-' or a '.'. The
   * test of the first byte keeps the cost off every other key. */
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

/* Tell whether the serializer must quote the dictionary key s. A canonical
 * numeric key goes out plain. Every other key follows needs_quoting(). */
static bool key_needs_quoting(const char *s) {
  if (s && key_is_canonical_number(s)) return false;
  return needs_quoting(s);
}

/*
 * Append n bytes of d to b, but never let the content of b grow past
 * limit + 1 bytes. A limit of SIZE_MAX means no limit, and every caller
 * that passes it passes the constant, so the test folds away there. Once
 * the content is longer than limit, every later call adds nothing. The
 * canonical key builder relies on that: it stops at the first byte past
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

/* The lowercase hex digits of the "\xNN" escape of a double-quoted
 * scalar. */
static const char _cyaml_hex_lower[] = "0123456789abcdef";

/*
 * Nonzero for a byte that ends a run of bytes that a double-quoted scalar
 * holds as they are. Those are the bytes that always need an escape (every
 * C0 control, '"', '\\' and DEL) and the lead bytes of the multi-byte
 * sequences that may need one: 0xC2 (a C1 control or NEL), 0xE2 (LS and PS)
 * and 0xEF (U+FFFE and U+FFFF). The NUL that ends the string is a C0 byte,
 * so the scan of a run needs no separate test for the end.
 */
static const unsigned char _cyaml_dq_stop[256] = {
    [0x00 ... 0x1F] = 1, ['"'] = 1,  ['\\'] = 1, [0x7F] = 1,
    [0xC2] = 1,          [0xE2] = 1, [0xEF] = 1};

/*
 * Write a double-quoted YAML string, with every escape that it needs, and
 * never let b hold more than limit + 1 bytes (see yb_put()). Each run of
 * bytes that needs no escape goes out in one append, and each escape is
 * built from a table, so the cost of a byte that needs nothing is one table
 * load.
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
      /* A lead byte that starts no sequence with an escape stays part of
       * the run. */
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
        /* NEL, LS and PS are line breaks to a YAML 1.1 reader, which folds
         * a raw one inside a double-quoted scalar into a space. Their
         * escapes read back as the same code point under 1.1 and 1.2. A C1
         * control other than NEL is outside c-printable, so every mainstream
         * reader refuses it raw. "\xNN" names the code point U+00NN, which a
         * parse encodes back as the same two bytes. */
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
        /* The noncharacters U+FFFE and U+FFFF are outside c-printable as
         * well. */
        p += 2;
        out = *p == 0xBE ? "\\uFFFE" : "\\uFFFF";
        out_len = 6;
        break;
      default:
        /* Every other C0 control byte, and DEL (0x7F), which has no valid
         * unescaped form either. See the doc comment of
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
 * implicit key ("key: value"). The ns-s-implicit-yaml-key production of
 * YAML 1.2 limits an implicit key to 1024 Unicode characters, and PyYAML and
 * libyaml both refuse a longer one. A key whose rendered text is longer goes
 * out in the explicit "? key" form instead, which has no such limit and which
 * the parser reads back as the same entry. Counting bytes instead of characters
 * can only choose the explicit form for a key that did not strictly need it.
 */
#define CYAML_MAX_IMPLICIT_KEY_LEN 1024

/* Write the text of a dictionary key: plain when that reads back as the
 * same key, double-quoted otherwise. The quoted form escapes every line
 * break, so a key always stays on one line. */
static inline void yb_append_key_text(ybuf_t *b, const char *key) {
  if (key_needs_quoting(key))
    yb_append_yaml_dquoted(b, key);
  else
    yb_append_cstr(b, key);
}

/* yb_append_key_text() under the limit of yb_put(). */
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

/* The indent helper. It writes two spaces for each level of depth. */
static void yb_indent(ybuf_t *b, int depth) {
  for (int i = 0; i < depth * 2; i++) yb_append_c(b, ' ');
}

/* Give the text of a double in YAML notation. That notation is ".nan",
 * ".inf", or the shortest decimal that round trips, which this function
 * writes into buf. The serializer calls it only inside the "C" locale scope
 * of cyaml_serialize() or cyaml_serialize_flow(), so the decimal point is
 * always '.'.
 */
static const char *yaml_double_text(double v,
                                    char buf[CYAML_FLOAT_KEY_BUF_LEN]) {
  if (__builtin_isnan(v)) return ".nan";
  if (__builtin_isinf(v)) return v > 0 ? ".inf" : "-.inf";
  format_shortest_double(v, buf);
  /* The mantissa always carries a '.'. Without one, an integral value such
   * as "1" reads back as an integer, and an exponent form such as "1e+20" or
   * "1e-05" reads back as a string under YAML 1.1, whose float grammar
   * requires the '.': PyYAML in its default mode and Ruby's Psych both load
   * it that way. "1.0", "1.0e+20" and "1.0e-05" read back as the same double
   * under both versions of the spec. The longest text that "%.17g" writes
   * is 24 bytes, so the two bytes always fit. */
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

/* Convert an integer to decimal, with no call to snprintf. */
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

/* Write a scalar string in a block context. This function puts double
 * quotes around a string that a parse would read as another YAML type. It
 * also quotes a string that holds an indicator character. It writes every
 * other string as a plain scalar. */
static void serialize_block_scalar(ybuf_t *b, cyaml_node_t *n) {
  const char *s = n->value.string;
  if (needs_quoting(s)) {
    yb_append_yaml_dquoted(b, s);
  } else {
    yb_append_cstr(b, s);
  }
}

/*
 * Tell whether c may appear literally inside a verbatim tag. This is the
 * ns-uri-char production of YAML 1.2 section 5.6 without its "%" escape
 * form and without '#': a letter, a digit, and one of -;/?:@&=+$,_.!~*'()[].
 * A '%' is not here, because it starts an escape. A '#' is valid in the
 * production, but libyaml and PyYAML refuse a verbatim tag that holds one
 * literally, and its escape "%23" decodes to the same byte in every reader.
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
 * Write the tag of a node in its verbatim "!<...>" form. Every byte that
 * ns-uri-char does not allow literally goes out percent-escaped. A parse of
 * the written document therefore gives back the identical tag string, and
 * every conforming YAML reader accepts the token.
 *
 * That covers the bytes that would break the token for this parser too. A
 * '>' and a line break each end the verbatim token, and a '%' starts an
 * escape. It also covers every byte that this parser reads literally and a
 * stricter reader refuses: a space, a quote, '<', '{', '}', '|', a backslash,
 * '^', '`', every control byte, and every byte of a UTF-8 sequence. The
 * escaped form of such a byte decodes to the same byte. Every core-schema
 * tag and any ordinary URI goes out byte for byte.
 *
 * The separator after the closing '>' belongs to the caller. The tag of a
 * collection sits on its own line, and the tag of a scalar is a prefix on
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

/* yb_append_verbatim_tag() under the limit of yb_put(). */
static void yb_append_verbatim_tag_capped(ybuf_t *b, const char *tag,
                                          size_t limit) {
  yb_verbatim_tag_body(b, tag, limit);
}

/*
 * What serialize_block_head() found at a node. It either wrote the whole
 * node itself, or the node is a container that is not empty. In the second
 * case the caller must now walk the children of that container.
 */
typedef enum {
  _CYAML_EMIT_DONE, /* fully written. Nothing is left to walk. */
  _CYAML_EMIT_LIST, /* a list that is not empty. Open a frame for it. */
  _CYAML_EMIT_DICT  /* a dictionary that is not empty. Open a frame. */
} _cyaml_emit_head_t;

/*
 * One level of the explicit walk stack of the block serializer. A list
 * frame walks n by index. A dictionary frame walks it in insertion order
 * through `next`, which is NULL when no entry is left.
 */
typedef struct {
  cyaml_node_t *n;
  size_t idx;
  size_t count;
  const ccol_chmap_entry_ref *next;
  int depth;
} _sblock_frame_t;

/*
 * Write everything about n that comes before its children. That is three
 * things. The first is the null literal for a NULL pointer. The second is a
 * verbatim tag, where the type of the node does not already imply the
 * tag of that node. The third is the whole value of a scalar or of an empty
 * container. Returns which kind of container is left to walk, when one is
 * left.
 *
 * depth decides the indentation. Each level adds 2 spaces. A depth past
 * CYAML_MAX_SERIALIZE_DEPTH goes through the oom flag of the buffer,
 * exactly as an allocation failure does. An oom flag that is already set
 * does the same. A walk that already failed therefore writes nothing more.
 *
 * The compiler always inlines this function. This is the body that the walk
 * below runs for each node. To leave it out of line puts a call, and the
 * setup of its arguments, on every node that the walk touches.
 */
static inline __attribute__((always_inline)) _cyaml_emit_head_t
serialize_block_head(ybuf_t *b, cyaml_node_t *n, int depth, size_t *count_out) {
  if (b->oom) return _CYAML_EMIT_DONE;
  if (depth > CYAML_MAX_SERIALIZE_DEPTH) {
    b->oom = true;
    return _CYAML_EMIT_DONE;
  }
  if (!n) {
    yb_append_cstr(b, "~");
    return _CYAML_EMIT_DONE;
  }

  /* A custom tag, which is one outside the core schema, always serializes
   * in its fully resolved verbatim form. A %TAG shorthand belongs to the
   * parse alone. The tree keeps no such state, so this code has nothing to
   * expand a shorthand back from. The tag of a collection sits on its own
   * line, indented to match the entries that follow it. The '&' and '!'
   * branches of this parser already accept a node property whose content
   * starts on a later, more indented line. The tag of a scalar is a prefix
   * on the same line, directly before the value. */
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
      *count_out = cvector_elem_count(n->value.list);
      if (*count_out == 0) {
        yb_indent(b, depth);
        yb_append_cstr(b, "[]\n");
        break;
      }
      return _CYAML_EMIT_LIST;
    case CYAML_DICTIONARY:
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

/* Start the walk of the children of the container n at depth. That
 * container is not empty.
 *
 * The compiler always inlines this function. It runs once for each
 * container that the walk enters. It does little more than fill in a struct
 * that the caller already holds. */
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
 * Write block-style YAML for the subtree whose root is n. depth decides the
 * indentation, and each level adds 2 spaces. This function puts each
 * mapping child and each list child on its own indented line. It writes a
 * NULL node pointer as the YAML null literal '~'.
 *
 * An explicit stack of container frames drives the walk. Recursion does
 * not. The native call stack therefore stays at O(1) depth for a tree of
 * any depth, and CYAML_MAX_SERIALIZE_DEPTH bounds the work and not the
 * stack. Only a container opens a frame. serialize_block_head() writes
 * every other child in full, on the spot.
 *
 * The whole walk stops as soon as anything sets the oom flag of the buffer.
 * To go on would write nothing. Every yb_append_* call is already a no-op
 * by then, and the library discards the whole buffer. The walk would still
 * pay for itself for every branch that is left. That is exactly the cost
 * that CYAML_MAX_SERIALIZE_DEPTH bounds against a tree of an extreme shape.
 */
static void serialize_block(ybuf_t *b, cyaml_node_t *n, int depth) {
  size_t count = 0;
  _cyaml_emit_head_t kind = serialize_block_head(b, n, depth, &count);
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
      /* A dictionary child or a list child goes on the next line. Write
       * only "-", with no space after it, so that no line holds
       * whitespace alone. */
      if (child &&
          (child->type == CYAML_DICTIONARY || child->type == CYAML_LIST)) {
        yb_append_cstr(b, "-\n");
        kind = serialize_block_head(b, child, f->depth + 1, &count);
      } else {
        yb_append_cstr(b, "- ");
        (void)serialize_block_head(b, child, f->depth, &count);
        yb_append_c(b, '\n');
        continue;
      }
    } else {
      if (!f->next) {
        sp--;
        continue;
      }
      /* key addresses the storage that the dictionary keeps for the entry.
       * A serialization never changes the tree, so key stays valid. */
      const cmap_pair *kp, *vp;
      f->next = ccol_chmap_entry_read(f->next, &kp, &vp);
      const char *key = (const char *)kp->ptr;
      child = _cyaml_read_child(vp->ptr);

      yb_indent(b, f->depth);
      size_t key_start = b->len;
      yb_append_key_text(b, key);
      if (!b->oom && b->len - key_start > CYAML_MAX_IMPLICIT_KEY_LEN) {
        /* Too long for an implicit key. Write it again in the explicit
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
        kind = serialize_block_head(b, child, f->depth + 1, &count);
      } else {
        yb_append_c(b, ' ');
        (void)serialize_block_head(b, child, f->depth, &count);
        yb_append_c(b, '\n');
        continue;
      }
    }

    if (kind != _CYAML_EMIT_DONE) {
      int child_depth = f->depth + 1;
      /* Do not use f after this point. A growth of the stack can move it. */
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

/* Serialize node to block YAML that a person can read. The string that this
 * function gives back is on the heap, and it always ends with a newline. The
 * caller must free it with cyaml_serialize_free() or with
 * cyaml_serialize_free_mp(). Returns NULL on an OOM. */
char *cyaml_serialize(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  ybuf_t b;
  yb_init(&b, mp);
  /* needs_quoting() and yb_append_double() convert numbers with strtod()
   * and "%g", which follow LC_NUMERIC. One "C" locale scope around the whole
   * walk keeps the output in the '.' form that the core schema reads. */
  ccol_c_locale_scope_t scope = ccol_c_locale_enter();
  serialize_block(&b, n, 0);
  ccol_c_locale_leave(scope);
  /* Make sure that the output ends with a newline. */
  if (b.len > 0 && !yb_ends_with_char(&b, '\n')) yb_append_c(&b, '\n');
  if (b.oom) {
    _ccol_mem_free(b.m_procs, b.buf);
    return NULL;
  }
  return b.buf;
}

/* Write one document of a stream: the "---" marker on its own line, and then
 * the block serialization of root. The marker on a line of its own is valid
 * in front of every kind of root, a block collection included, and a root
 * tag then sits on the line after it, exactly as cyaml_serialize() writes
 * it. */
static void serialize_stream_document(ybuf_t *b, cyaml_node_t *root) {
  yb_append_cstr(b, "---\n");
  size_t start = b->len;
  serialize_block(b, root, 0);
  if (b->len > start && !yb_ends_with_char(b, '\n')) yb_append_c(b, '\n');
}

char *cyaml_serialize_stream(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  ybuf_t b;
  yb_init(&b, mp);
  /* The same "C" locale scope as cyaml_serialize(). */
  ccol_c_locale_scope_t scope = ccol_c_locale_enter();
  if (n && n->type == CYAML_LIST) {
    size_t count = cyaml_list_len(node);
    for (size_t i = 0; i < count && !b.oom; i++)
      serialize_stream_document(&b, (cyaml_node_t *)cyaml_list_get(node, i));
  } else {
    serialize_stream_document(&b, n);
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

/* One key and value pair that the code took out of the chmap of a
 * dictionary, so that canonical mode can sort it. See the "canonical"
 * parameter of serialize_flow, and the doc comment of
 * serialize_flow_canonical below. key is borrowed. It points into the
 * storage that the dictionary keeps for each entry. It stays valid for as
 * long as nothing changes the dictionary itself. */
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
 * One level of the explicit walk stack of the flow serializer. A list frame
 * walks n by index. A dictionary frame walks it in one of two ways. The
 * first is through `entries`, which is a sorted copy that canonical mode
 * builds. The second is in insertion order through `next`, which is NULL
 * when no entry is left.
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
 * Write everything about n that comes before its children, in flow style.
 * That is four things. The first is the null literal for a NULL pointer.
 * The second is a verbatim tag, where the type of the node does not already
 * imply the tag of that node. The third is the whole value of a scalar.
 * The fourth is the opening bracket or brace of a container. An empty
 * container gets both brackets at once. Returns which kind of container is
 * left to walk, when one is left.
 *
 * A flow context has no question of indentation. A custom tag is therefore
 * always a plain prefix on the same line, for a scalar and for a collection
 * alike. A core-schema tag that does not match the type of the node gets
 * the same treatment. A depth past CYAML_MAX_SERIALIZE_DEPTH goes through
 * the oom flag of the buffer, exactly as an allocation failure does. An
 * oom flag that is already set does the same. A walk that already failed
 * therefore writes nothing more.
 *
 * Every append goes through yb_put() under limit; see serialize_flow().
 *
 * The compiler always inlines this function, for the reason that the doc
 * comment of serialize_block_head gives.
 */
static inline __attribute__((always_inline)) _cyaml_emit_head_t
serialize_flow_head(ybuf_t *b, cyaml_node_t *n, int depth, size_t *count_out,
                    size_t limit) {
  if (b->oom) return _CYAML_EMIT_DONE;
  if (depth > CYAML_MAX_SERIALIZE_DEPTH) {
    b->oom = true;
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
      *count_out = cvector_elem_count(n->value.list);
      if (*count_out == 0) {
        yb_put(b, "[]", 2, limit);
        break;
      }
      yb_put(b, "[", 1, limit);
      return _CYAML_EMIT_LIST;
    case CYAML_DICTIONARY:
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

/* Release the copy of the entries that canonical mode built for a flow
 * frame, when it has one. */
static void sflow_frame_close(ybuf_t *b, _sflow_frame_t *f) {
  f->next = NULL;
  if (f->entries) {
    _ccol_mem_free(b->m_procs, f->entries);
    f->entries = NULL;
  }
}

/*
 * Start the walk of the children of the container n at depth. That
 * container is not empty. Returns false when the code cannot build the
 * entry array of a canonical dictionary. It then also sets the oom flag of
 * the buffer.
 *
 * The compiler always inlines this function, for the reason that the doc
 * comment of sblock_frame_open gives.
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
    /* This is one of two cases. The first is the ordinary public path,
     * which is not canonical and which writes the members in insertion
     * order. The second is a dictionary in canonical mode with 0 or 1
     * entries, where no order matters at all. */
    f->next = dict_first_entry(n);
    return true;
  }

  /* Collect into a plain array first. Canonical mode exists so that the
   * result does not depend on the order of the inserts. This code must
   * therefore hold every entry before it decides any order. qsort then
   * sorts them by key, in
   * lexicographic order. That matches the precedent of this codebase for a
   * small array sort. The order of the rotated file names in clogger.c uses
   * the identical shape of a qsort with a strcmp comparator. */
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

/* Write one flow dictionary key, and then its ": " separator. A key whose
 * text is longer than CYAML_MAX_IMPLICIT_KEY_LEN goes out as the explicit
 * "? key: value" entry of a flow mapping. The compiler always inlines this
 * function. It runs once for each dictionary entry that the walk writes. */
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
 * Write compact flow YAML, on one line, for the subtree whose root is n.
 * This function puts '[' and ']' around a list, and '{' and '}' around a
 * dictionary. It writes a null as '~'. It quotes a string by the same
 * needs_quoting() rules as the block output. The result is therefore always
 * a valid YAML value that a parser can read.
 *
 * canonical decides the order of the dictionary entries, and nothing else.
 * The order of a list already carries meaning, and this function never
 * changes it. A false value is the public cyaml_serialize_flow() path. It
 * writes the members of each dictionary in insertion order. A true value is the
 * path of serialize_flow_canonical; see its doc comment. It writes the
 * entries of every dictionary sorted by key, in lexicographic order, at
 * every level of nesting. The result is then a pure function of the
 * content, and it does not depend on the order of the inserts.
 *
 * An explicit stack of container frames drives the walk. Recursion does
 * not. The native call stack therefore stays at O(1) depth for a tree of
 * any depth, and CYAML_MAX_SERIALIZE_DEPTH bounds the work and not the
 * stack. See the doc comment of that constant for why it is not a repeat of
 * CYAML_MAX_PARSE_DEPTH. Only a container opens a frame.
 * serialize_flow_head() writes every other child in full, on the spot. The
 * whole walk stops as soon as anything sets the oom flag of the buffer, for
 * the reason that the doc comment of serialize_block gives.
 *
 * limit bounds the content of b at limit + 1 bytes (see yb_put()), and the
 * walk ends as soon as the content is longer than limit. SIZE_MAX means no
 * limit. The function is always inlined into its two callers, each of which
 * passes a constant, so the public path carries none of the tests.
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
      /* key addresses the storage that the dictionary keeps for the entry.
       * A serialization never changes the tree, so key stays valid. */
      const cmap_pair *kp, *vp;
      f->next = ccol_chmap_entry_read(f->next, &kp, &vp);
      child = _cyaml_read_child(vp->ptr);
      f->idx++;
      sflow_emit_key(b, (const char *)kp->ptr, limit);
    }

    int child_depth = f->depth + 1;
    kind = serialize_flow_head(b, child, child_depth, &count, limit);
    if (kind != _CYAML_EMIT_DONE) {
      /* Do not use f after this point. A growth of the stack can move it. */
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

/* Serialize node to compact flow YAML on one line. Gives back a string on
 * the heap. The caller must free it with cyaml_serialize_free() or with
 * cyaml_serialize_free_mp(). Returns NULL on an OOM. */
char *cyaml_serialize_flow(cyaml node) {
  cyaml_node_t *n = (cyaml_node_t *)node;
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  ybuf_t b;
  yb_init(&b, mp);
  /* The same "C" locale scope as cyaml_serialize(). */
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
 * node_to_dict_key_string() uses it, to make the canonical form of a value
 * that is not a scalar and that serves as a dictionary key.
 *
 * This function differs from the public one in one way. It writes the
 * entries of every dictionary that it meets, at any level of nesting inside
 * n, sorted by key in lexicographic order. It does not use the insertion
 * order of each dictionary. Two keys
 * that are not scalars and that have the same structure therefore always
 * make the identical string, whatever order built them. They then correctly
 * collide as the same dictionary key.
 *
 * The library never exposes this function. Every other caller gets the
 * documented, unsorted output order of cyaml_serialize_flow().
 *
 * The walk stops at the first byte past CYAML_MAX_CANONICAL_KEY_LEN, so the
 * text that it builds never holds more than that limit plus one byte,
 * whatever the size of n. A key made of many aliases of one long scalar is
 * therefore refused after one limit's worth of work, and never after the
 * whole rendering. Returns NULL and sets *too_long when the canonical form
 * is longer than the limit. Returns NULL with *too_long false on an OOM,
 * which matches the contract of cyaml_serialize_flow(). */
#ifdef RUNNING_UNIT_TESTS
/* The largest buffer, in bytes, that serialize_flow_canonical() allocated
 * on this thread. A test resets it to 0 before the parse that it checks. */
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

/* Release a string that cyaml_serialize or cyaml_serialize_flow gave back,
 * through the matching allocator. Pass mp == NULL for the default
 * allocator. s may be NULL, and _ccol_mem_free() treats NULL as a safe
 * no-op. The *err_str of a failed parse is not such a string: the library
 * owns it, and it must never reach this function. */
void cyaml_serialize_free_mp(char *s, ccol_memmgmt_procs_t *mp) {
  _ccol_mem_free(mp, s);
}

/* ========================================================================== */
/*                         PATH NAVIGATION                                    */
/* ========================================================================== */

/*
 * The escape sequences of a path. They apply to every navigate helper and
 * every set helper below.
 *
 *   \.   -> a literal '.' in the key, and not a path separator
 *   \\   -> a literal '\' in the key
 *
 * A '\' before any other character stays unchanged, and the code passes it
 * through as it is. The code resolves the escape for each component, after
 * it divides the path at the separator.
 */

/* Gives a pointer to the first '.' in s that carries no escape. Gives NULL
 * when there is none. */
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

/* Gives a pointer to the last '.' in s that carries no escape. Gives NULL
 * when there is none. */
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

/* Resolve the escape sequences in s, in place. The string becomes shorter
 * or keeps its length. It never becomes longer. */
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
 * past the '#'). A bare strtol() call accepts leading whitespace and an
 * explicit sign before the first digit, and it reports neither. This
 * function refuses both. It therefore matches the documented path grammar
 * of this module for a list index exactly. That grammar is "a component
 * beginning with '#' followed by digits". The texts "#  3" and "#+3" are
 * malformed, and they are not index 3.
 *
 * Returns false, and leaves idx_out untouched, in three cases. Those are an
 * empty digit string, a malformed digit string, and a value that does not
 * fit in a long that is not negative. Returns true in every other case,
 * with *idx_out set.
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

/* Why navigate_y() failed to reach the end of a path. This value carries a
 * meaning only when that function returns NULL. It lets a caller that
 * returns a ccol_retval_t tell two classes of failure apart. Those callers
 * are _cyaml_set_typed and _cyaml_delete. The first class is an error in
 * the path that the caller built, which is ccol_invalid_args. That covers a
 * parent of the wrong type, a malformed "#N" index syntax, and an empty
 * path component. The second class is a path component that is well formed
 * and absent, which is ccol_key_not_found. The division holds at ANY
 * position in the path, and not at the leaf alone.
 *
 * CYAML_NAV_EMPTY_COMPONENT matches the empty-component check that
 * _cyaml_set_typed() and _cyaml_delete() already apply to their own leaf
 * component. A path with nothing between two dots, or with a leading dot,
 * has no valid reading. The library never treats it as an address for a
 * dictionary key that is the empty string. The same input therefore gets
 * the same refusal, whether the empty component is an intermediate one or
 * the leaf. */
typedef enum {
  CYAML_NAV_NOT_FOUND = 0,
  CYAML_NAV_WRONG_TYPE,
  CYAML_NAV_MALFORMED_INDEX,
  CYAML_NAV_EMPTY_COMPONENT,
} cyaml_nav_fail_t;

/*
 * Walk a path through a YAML tree. A dot separates the components of that
 * path. Gives back the node at the end of the path. Gives NULL when any
 * component is not there.
 *
 * path_copy must be writable. This function puts a '\0' over each dot that
 * carries no escape, so that it can carve out each component in place. It
 * then resolves the escapes of that component before it uses it.
 *
 * A '#' prefix addresses a list element. The path "items.#0.name" reaches
 * the 'name' key of the first element of 'items'.
 *
 * fail_reason_out, when it is not NULL, receives why the walk stopped
 * early. This function writes it only when it returns NULL. A caller that
 * has no way to report a distinct error code does not use it. _cyaml_get is
 * such a caller.
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
      const cmap_pair *vp = NULL;
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

/* Map a failure reason of navigate_y() onto the ccol_retval_t that a caller
 * reports for a parent path that it could not resolve. Those callers are
 * _cyaml_set_typed and _cyaml_delete, and each returns a ccol_retval_t. An
 * error in the path that the caller built gives ccol_invalid_args. That
 * covers a parent of the wrong type and a malformed "#N" syntax. A
 * component that is well formed and absent gives ccol_key_not_found. */
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

/* The public implementation of the cyaml_get(root, path) macro. It copies
 * path before it gives it to navigate_y(). It therefore never changes the
 * string of the caller. */
cyaml _cyaml_get(cyaml root, const char *path) {
  if (!root) return NULL;
  if (!path || path[0] == '\0') return root;
  ccol_memmgmt_procs_t *mp = ((cyaml_node_t *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return NULL;
  cyaml result = navigate_y(root, copy, NULL);
  _ccol_mem_free(mp, copy);
  return result;
}

/*
 * The public implementation of the cyaml_set(root, path, value) macro.
 *
 * This function divides the path at the LAST dot, to separate the parent
 * path from the leaf key. It copies the leaf with strdup before it frees
 * the copy of the parent path. Without that order, the leaf string is a
 * suffix of the full path and the code reads freed memory.
 *
 * For CYAML_STRING, raw always names a pointer to the string. The
 * cyaml_set() macro copies its argument into a local of the decayed,
 * unqualified type, so a string literal and a char array both arrive as a
 * pointer to their first character.
 *
 * node_reinit_scalar changes a scalar node that the leaf already holds, in
 * place. This function allocates and inserts a new key. It never creates an
 * intermediate container by itself. The parent node must already be there.
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
    cyaml_nav_fail_t fail_reason = CYAML_NAV_NOT_FOUND;
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    parent = navigate_y(root, copy, &fail_reason);
    _ccol_mem_free(mp, copy);
    if (!leaf_copy) return ccol_not_enough_memory;
    if (!parent) {
      _ccol_mem_free(mp, leaf_copy);
      return nav_fail_to_retval(fail_reason);
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
    /* Refuse a bad key or a bad payload before the map changes at all, so
     * that a refusal never needs to undo an insert. A key must be
     * well-formed UTF-8; see cyaml_api_string_ok(). */
    ret = ccol_utf8_is_valid(leaf_comp, kp.size - 1)
              ? scalar_payload_check(type, raw, raw_size)
              : ccol_invalid_args;
    if (ret == ccol_success) {
      /* One hash and one probe either find the leaf or insert the key with
       * a NULL placeholder child. A leaf that exists is updated in place, so
       * a borrowed handle to it and a custom tag on it both survive. For a
       * new key, nothing runs between the insert and the write of the real
       * child except the build of that child, and that build reads no map of
       * this tree. No parse runs here, so the byte budget is unarmed and
       * charges nothing for the new entry. */
      cyaml_node_t *placeholder = NULL;
      cmap_pair vp = {.ptr = &placeholder, .size = sizeof(placeholder)};
      const cmap_pair *slot = NULL;
      ccol_retval_t r =
          ccol_chmap_insert_or_get_elem(pn->value.dictionary, &kp, &vp, &slot);
      if (r == ccol_key_already_present) {
        cyaml_node_t *existing = _cyaml_read_child(slot->ptr);
        ret = node_reinit_scalar_checked(existing, type, raw, raw_size,
                                         is_signed);
      } else if (r == ccol_success) {
        cyaml_node_t *new_node = NULL;
        ret = node_make_scalar(type, raw, raw_size, is_signed, pn->m_procs,
                               &new_node);
        if (ret == ccol_success) {
          memcpy((void *)slot->ptr, &new_node, sizeof(new_node));
          /* The dictionary now owns the node. See the guard of
           * cyaml_dictionary_set(). */
          new_node->attached = true;
        } else {
          /* The build ran out of memory. Remove the placeholder, so that the
           * map never keeps a NULL child. A delete of a key that the map
           * holds always succeeds: a shrink of the bucket array that cannot
           * get memory is skipped, and the entry is still removed. */
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
      long idx;
      if (!parse_list_index_component(leaf_comp + 1, &idx)) {
        ret = ccol_invalid_args;
      } else {
        void *slot = cvector_at(pn->value.list, (size_t)idx);
        if (!slot) {
          /* An index that is valid syntax and that simply is not there is
           * an absent path component. It is exactly like a dictionary key
           * that is not there. It is not an error in the path. The
           * CYAML_DICTIONARY branch above creates the leaf instead of
           * reporting an error, and it has no matching case. This code
           * cannot sensibly "create" a list slot that is out of range.
           * Nothing says what to put in the gap before it. The
           * function _cyaml_delete uses the same reasoning and the same
           * return value for the same situation. */
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
 * The public implementation of the cyaml_delete(root, path) macro.
 *
 * This function divides the path at the LAST dot that carries no escape, to
 * find the parent node and the leaf key. When there is no dot, root is the
 * parent. It resolves the escapes of the leaf key before it uses that key.
 * '\.' and '\\' therefore work exactly as they do in cyaml_get and
 * cyaml_set.
 *
 * It removes the node that the path addresses, and deep-frees it. Returns:
 *   ccol_success          - the library removed and freed the node.
 *   ccol_invalid_args     - a NULL root, a NULL path or an empty path. It
 *                           also covers a node of the wrong type, which is
 *                           not a container, before the leaf. It covers a
 *                           malformed "#N" index syntax too. Any path
 *                           component can give this code, the leaf and an
 *                           intermediate one alike.
 *   ccol_key_not_found    - a path component that is well formed is absent.
 *                           That component is a dictionary key or a "#N"
 *                           index, at any position. It includes a list
 *                           index whose syntax is valid and whose value is
 *                           out of range.
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
    cyaml_nav_fail_t fail_reason = CYAML_NAV_NOT_FOUND;
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    parent = navigate_y(root, copy, &fail_reason);
    _ccol_mem_free(mp, copy);
    if (!leaf_copy) return ccol_not_enough_memory;
    if (!parent) {
      _ccol_mem_free(mp, leaf_copy);
      return nav_fail_to_retval(fail_reason);
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
    ret = cyaml_dictionary_remove(parent, leaf_comp);
  } else if (pn->type == CYAML_LIST) {
    if (leaf_comp[0] != '#') {
      ret = ccol_invalid_args;
    } else {
      long idx;
      if (!parse_list_index_component(leaf_comp + 1, &idx)) {
        ret = ccol_invalid_args;
      } else if ((size_t)idx >= cvector_elem_count(pn->value.list)) {
        /* An index that is valid syntax and that simply is not there is an
         * absent path component. It is exactly like a dictionary key that
         * is not there, and it is not an error in the path. See the doc
         * comment of this function, and the public doc comment of
         * _cyaml_delete in cyaml.h. */
        ret = ccol_key_not_found;
      } else {
        ret = cyaml_list_remove(parent, (size_t)idx);
      }
    }
  } else {
    ret = ccol_invalid_args;
  }

  _ccol_mem_free(mp, leaf_copy);
  return ret;
}
