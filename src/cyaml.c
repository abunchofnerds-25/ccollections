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

// The DOM node. The public handle is a pointer to it, but nothing outside this
// file reads the fields. m_procs is NULL for the default allocator or an
// interned procs copy, never the caller's own struct. tag is an owned, resolved
// tag string or NULL, and only cyaml__destroy() frees it (node_clear() keeps
// it).
typedef struct cyaml_node {
  cyaml_node_type type;
  // attached is set once the node is stored in a list or dictionary and never
  // cleared; push and set refuse an attached child, so no node gets two owners.
  // anchored is set while a parse's anchor table points at the node, so
  // parse_discard() keeps it alive. Both flags sit in padding on LP64.
  bool attached;
  bool anchored;
  ccol_memmgmt_procs *m_procs;
  char *tag;
  union {
    bool boolean;
    long long integer;
    double number;
    char *string;
    cvec list;
    chmap dictionary;
  } value;
} cyaml_node;

// The core merge tag. A "<<" key with this tag is a merge key, like an
// untagged plain "<<".
#define CYAML_TAG_MERGE "tag:yaml.org,2002:merge"
// The resolved form of a bare "!". No written tag may resolve to it and no node
// ever carries it; it only marks the non-specific tag during a parse.
#define CYAML_TAG_NON_SPECIFIC "!"

// The serializer's output buffer, a ccol_growbuf. Once oom is set every yb_*
// call does nothing.
typedef ccol_growbuf ybuf;

// Initialize a ybuf and append to it, for every serializer. They're thin
// wrappers that give each growbuf operation a short local name.
static inline void yb_init(ybuf *b, ccol_memmgmt_procs *mp) {
  ccol_growbuf_init(b, mp);
}

static inline void yb_append(ybuf *b, const char *data, size_t n) {
  ccol_growbuf_append(b, data, n);
}

static inline void yb_append_c(ybuf *b, char c) { ccol_growbuf_append_c(b, c); }

static inline void yb_append_cstr(ybuf *b, const char *s) {
  ccol_growbuf_append_cstr(b, s);
}

// Drops trailing spaces and tabs from b, but never below floor. The
// quoted-scalar folds and the plain-scalar trim need it, since YAML drops that
// whitespace before a line break. Like the two helpers after it, it reads the
// buffer only when oom is clear, because a failed append leaves a store nobody
// may index.
static inline void yb_trim_trailing_inline_ws(ybuf *b, size_t floor) {
  if (b->oom) return;
  while (b->len > floor &&
         (b->buf[b->len - 1] == ' ' || b->buf[b->len - 1] == '\t'))
    b->buf[--b->len] = '\0';
}

// Appends the content of src to dst, when a scanner joins a temporary buffer
// into its result. A src in the OOM state adds nothing; the caller reports that
// failure itself.
static inline void yb_append_buf(ybuf *dst, const ybuf *src) {
  if (src->oom || src->len == 0) return;
  ccol_growbuf_append(dst, src->buf, src->len);
}

// True when b has readable content and its last byte is c; the serializers use
// it to end the output with exactly one newline.
static inline bool yb_ends_with_char(const ybuf *b, char c) {
  return !b->oom && b->len > 0 && b->buf[b->len - 1] == c;
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: Runs the three helpers above on a buffer in the state a failed
// append leaves (oom set, no store, a stale length). Gives one bit per helper
// that answered without reading the store, 7 when all three did. The volatile
// values keep the compiler from folding the calls away.
unsigned cyaml_debug_growbuf_guard_bits(void) {
  char *volatile absent_store = NULL;
  volatile size_t stale_len = 8;
  unsigned bits = 0;

  ybuf hostile = {.buf = absent_store,
                  .len = stale_len,
                  .cap = 0,
                  .oom = true,
                  .m_procs = NULL};
  yb_trim_trailing_inline_ws(&hostile, 0);
  if (hostile.len == stale_len) bits |= 1u;

  ybuf dst;
  yb_init(&dst, NULL);
  yb_append_buf(&dst, &hostile);
  if (!dst.oom && dst.len == 0) bits |= 2u;
  ccol__mem_free(dst.m_procs, dst.buf);

  if (!yb_ends_with_char(&hostile, '\n')) bits |= 4u;
  return bits;
}
#endif

// Forces a helper inline into the parse driver, which is one very large
// function whose inlining budget GCC spends elsewhere. Each user was chosen by
// measuring a whole parse; measure again before adding or removing one.
#define CYAML_PARSE_HOT static inline __attribute__((always_inline))

// Reads a child pointer out of a list slot or dictionary value, for every walk
// over a container. It uses memcpy, the pattern the other modules built on
// chmap use for their child pointers.
static inline cyaml_node *cyaml_read_child(const void *src) {
  cyaml_node *p;
  memcpy(&p, src, sizeof(p));
  return p;
}

// Gives the oldest member of dictionary n, where every walk in insertion order
// starts. The map keeps its entries in that order, so serializers, clone and
// the iterator all see it.
static inline const ccol_chmap_entry_ref *dict_first_entry(cyaml_node *n) {
  return ccol_chmap_oldest_entry(n->value.dictionary);
}

// Each thread keeps a free list of up to CYAML_POOL_CAP default-allocator
// nodes, so a workload that parses and destroys many documents doesn't pay
// calloc and free for each node. The key's destructor drains a thread's pool
// when it exits.
#define CYAML_POOL_CAP 512U
#if defined(CCOL__EMULATE_DARWIN_TLS)
// One thread's pool and error buffer under CCOL__EMULATE_DARWIN_TLS. The block
// is the value of the pool key, so pool_drain() gets it as its argument and
// reads no __thread variable (macOS releases those before key destructors).
typedef struct {
  cyaml_node *head;
  unsigned sz;
  char *err_buf;
} cyaml_tls;
#else
// The calling thread's node pool.
static __thread cyaml_node *pool_head = NULL;
static __thread unsigned pool_sz = 0;
// True once this thread has set its value on the pool key. One set is enough
// for the destructor to run at exit, so the lock is taken once per thread and
// not on every refill of the pool.
static __thread bool pool_key_armed = false;
#endif
#ifdef RUNNING_UNIT_TESTS
// Counts how often any thread took the pool key lock to arm the key.
atomic_ulong cyaml__pool_key_lock_count_for_tests;
#endif

// True while the pool key exists. It goes true at the end of the one-time init
// and false before the destructor deletes the key.
static atomic_bool pool_key_live = false;

// Set first thing in the shared-object destructor. A later first use (from an
// application destructor, say) then leaves the pool unarmed, because nothing
// would delete the key or drain the pool.
static atomic_bool pool_key_finalized = false;

// The key whose destructor, pool_drain(), frees each thread's pool and error
// buffer when the thread exits.
static ccol__thread_ls_key pool_key;

// Readers hold it across "check pool_key_live, then set the key", and the
// destructor holds it as writer to clear the flag and delete the key. A plain
// atomic check isn't enough: The key could be deleted under a
// pthread_setspecific() that is already running. Initialized lazily under
// pool_key_once.
static ccol__rw_lock pool_key_rwlock;

static void pool_drain(void *);

// The number of nodes the running parse on this thread may still make;
// (size_t)-1 when no parse runs, which node_alloc() treats as unlimited. Only
// parse_node_budget_arm() and _disarm() set it, and parse_common() disarms it
// on every exit, so a spent budget never leaks into later node creation.
static __thread size_t parse_node_budget = (size_t)-1;

// Set when node_alloc() refuses because the node budget ran out (not on a real
// OOM), so the parser can name the limit in its error.
static __thread bool parse_node_budget_exhausted = false;

// The DOM bytes the running parse on this thread may still use; (size_t)-1
// when no parse runs. It works like parse_node_budget.
static __thread size_t parse_byte_budget = (size_t)-1;

// Set when the byte budget refuses a charge, so the parser can name the limit.
static __thread bool parse_byte_budget_exhausted = false;

// What the byte budget charges per kind of allocation: A node, the empty
// backing store of a list or a dictionary, one dictionary entry, and the
// allocator's own overhead per block. Each figure is at or above the measured
// cost on LP64, so the budget never understates memory. List slots aren't
// charged; each one points at a child that costs far more.
#define CYAML_ALLOC_OVERHEAD ((size_t)16)
#define CYAML_BYTES_PER_NODE (sizeof(cyaml_node) + CYAML_ALLOC_OVERHEAD)
#define CYAML_BYTES_PER_LIST_BASE ((size_t)192)
#define CYAML_BYTES_PER_DICT_BASE ((size_t)320)
#define CYAML_BYTES_PER_DICT_ENTRY ((size_t)192)

// Charges bytes to this thread's byte budget, before each DOM allocation of a
// parse, so a document can't use unbounded memory. True when no parse runs or
// the budget has room; otherwise it zeroes the budget, sets
// parse_byte_budget_exhausted and gives false.
static inline bool parse_bytes_charge(size_t bytes) {
  if (parse_byte_budget == (size_t)-1) return true;
  if (bytes > parse_byte_budget) {
    parse_byte_budget = 0;
    parse_byte_budget_exhausted = true;
    return false;
  }
  parse_byte_budget -= bytes;
  return true;
}

// Tells whether parse_bytes_charge(bytes) would refuse, without changing
// anything. dictionary_set_impl() needs it, because a key that already exists
// must not trip the budget.
static inline bool parse_bytes_would_refuse(size_t bytes) {
  return parse_byte_budget != (size_t)-1 && bytes > parse_byte_budget;
}

// Gives back bytes to the byte budget when the allocation they were charged for
// failed. We charge before allocating so an oversized request never reaches the
// allocator, and the refund keeps the account exact.
static inline void parse_bytes_refund(size_t bytes) {
  if (parse_byte_budget != (size_t)-1) parse_byte_budget += bytes;
}

// Copies s with ccol_strdup() after charging the copy to the parse byte budget,
// for scalar text and tags, so the budget bounds strings as well as nodes. NULL
// when the budget refuses or the copy fails.
CYAML_PARSE_HOT char *strdup_charged(ccol_memmgmt_procs *mp, const char *s) {
  size_t n = strlen(s) + 1 + CYAML_ALLOC_OVERHEAD;
  if (!parse_bytes_charge(n)) return NULL;
  char *copy = ccol_strdup(mp, s);
  if (!copy) parse_bytes_refund(n);
  return copy;
}

#if defined(CCOL__EMULATE_DARWIN_TLS)
// Gives the calling thread's pool block under CCOL__EMULATE_DARWIN_TLS, without
// making one; node_alloc() and the destructor use it. NULL when the thread has
// none or the key is gone.
static cyaml_tls *cyaml_tls_peek(void) {
  if (!atomic_load_explicit(&pool_key_live, memory_order_acquire)) return NULL;
  return (cyaml_tls *)ccol__thread_ls_get(pool_key);
}

// Gives the calling thread's pool block under CCOL__EMULATE_DARWIN_TLS, made
// and set on the pool key on first use, for node_free() and the error buffer.
// NULL when the key is gone, memory runs out or the set fails. It takes
// pool_key_rwlock as a reader around the set; the caller has run pool_key_once.
static cyaml_tls *cyaml_tls_get(void) {
  cyaml_tls *t = cyaml_tls_peek();
  if (t || !atomic_load(&pool_key_live)) return t;
  t = (cyaml_tls *)calloc(1, sizeof(*t));
  if (!t) return NULL;
  bool set = false;
  ccol__rw_lock_rdlock(pool_key_rwlock);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&cyaml__pool_key_lock_count_for_tests, 1,
                            memory_order_relaxed);
#endif
  if (atomic_load(&pool_key_live) && ccol__thread_ls_set(pool_key, t) == 0)
    set = true;
  ccol__rw_lock_unlock(pool_key_rwlock);
  if (!set) {
    free(t);
    return NULL;
  }
  return t;
}
#else
// Sets the pool key once per thread, so pool_drain() runs at thread exit and a
// pooled node is never leaked. node_free() and cyaml_err_storage() call it. It
// takes pool_key_rwlock as a reader and sets pool_key_armed; the caller has run
// pool_key_once. Forced inline so node_free() keeps its shape.
static inline __attribute__((always_inline)) void pool_key_arm(void) {
  if (!pool_key_armed && atomic_load(&pool_key_live)) {
    ccol__rw_lock_rdlock(pool_key_rwlock);
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add_explicit(&cyaml__pool_key_lock_count_for_tests, 1,
                              memory_order_relaxed);
#endif
    if (atomic_load(&pool_key_live) &&
        ccol__thread_ls_set(pool_key, (void *)1) == 0)
      pool_key_armed = true;
    ccol__rw_lock_unlock(pool_key_rwlock);
  }
}

// The thread's buffer for parse error messages (see cyaml_err_storage()). The
// thread-exit drain frees it with the pool.
static __thread char *cyaml_err_buf;
#endif

// Runs do_pool_key_init() once, on the first node allocation or free, so a
// process that never uses cyaml pays nothing.
static ccol__once_flag pool_key_once = CCOL__ONCE_INIT;

// Creates the rwlock and the pool key, once, through pool_key_once, so the node
// pool can drain at thread exit. pool_key_live goes true only when both work,
// so it is the one sign that the rwlock may be used. Does nothing once
// pool_key_finalized is set.
static void do_pool_key_init(void) {
  if (atomic_load(&pool_key_finalized)) return;
  if (ccol__rw_lock_init(pool_key_rwlock) != 0) return;
  if (ccol__thread_ls_key_create(pool_key, pool_drain) != 0) return;
  atomic_store(&pool_key_live, true);
}

// Tears the node pool down when the library unloads or the process exits: Sets
// pool_key_finalized, drains the calling thread's pool, then clears
// pool_key_live and deletes the key under the write lock. Without the delete,
// dlopen and dlclose cycles use up the process's keys.
__attribute__((destructor)) static void pool_key_fini(void) {
  atomic_store(&pool_key_finalized, true);
  if (!atomic_load(&pool_key_live)) return;
#if defined(CCOL__EMULATE_DARWIN_TLS)
  cyaml_tls *t = cyaml_tls_peek();
  if (t) {
    (void)ccol__thread_ls_set(pool_key, NULL);
    pool_drain(t);
  }
#else
  pool_drain(NULL);
#endif
  ccol__rw_lock_wrlock(pool_key_rwlock);
  atomic_store(&pool_key_live, false);
  ccol__thread_ls_key_delete(pool_key);
  ccol__rw_lock_unlock(pool_key_rwlock);
}

// Allocates a zeroed node of the given type, the one allocation path for every
// node. mp is the allocator (NULL takes a node from the thread pool first). It
// charges the byte budget for the node plus the empty container that a list or
// dictionary gets next, and counts the node against the node budget only once
// it exists. NULL on OOM or when either budget refuses.
CYAML_PARSE_HOT cyaml_node *node_alloc(cyaml_node_type type,
                                       ccol_memmgmt_procs *mp) {
  ccol__call_once(pool_key_once, do_pool_key_init);
  if (parse_node_budget != (size_t)-1 && parse_node_budget == 0) {
    parse_node_budget_exhausted = true;
    return NULL;
  }
  size_t node_bytes = CYAML_BYTES_PER_NODE;
  if (type == CYAML_LIST)
    node_bytes += CYAML_BYTES_PER_LIST_BASE;
  else if (type == CYAML_DICTIONARY)
    node_bytes += CYAML_BYTES_PER_DICT_BASE;
  if (!parse_bytes_charge(node_bytes)) return NULL;
  cyaml_node *n;
#if defined(CCOL__EMULATE_DARWIN_TLS)
  cyaml_tls *t = mp == NULL ? cyaml_tls_peek() : NULL;
  if (t && t->head) {
    n = t->head;
    cyaml_node *next;
    memcpy(&next, (cyaml_node **)n, sizeof(next));
    t->head = next;
    t->sz--;
#else
  if (mp == NULL && pool_head) {
    n = pool_head;
    cyaml_node *next;
    memcpy(&next, (cyaml_node **)n, sizeof(next));
    pool_head = next;
    pool_sz--;
#endif
    memset(n, 0, sizeof(*n));
  } else {
    n = ccol__mem_calloc(mp, 1, sizeof(*n));
    if (!n) {
      parse_bytes_refund(node_bytes);
      return NULL;
    }
  }
  if (parse_node_budget != (size_t)-1) parse_node_budget--;
  n->type = type;
  n->m_procs = mp;
  return n;
}

// Frees a node struct, the one release path for every node: A default-allocator
// node goes into the thread pool unless it's full, any other node goes back to
// its own allocator. A thread that can't arm the pool key (no key left, the set
// fails, or the destructor already ran) frees the node, since nothing would
// drain its pool. The check of pool_key_live without the lock only skips an
// rwlock that was never initialized; the check under the lock is the one that
// matters.
CYAML_PARSE_HOT void node_free(cyaml_node *n) {
  if (n->m_procs != NULL) {
    ccol__mem_free(n->m_procs, n);
    return;
  }
  ccol__call_once(pool_key_once, do_pool_key_init);
#if defined(CCOL__EMULATE_DARWIN_TLS)
  cyaml_tls *t = cyaml_tls_get();
  if (!t || t->sz >= CYAML_POOL_CAP) {
    free(n);
    return;
  }
  memcpy((cyaml_node **)n, &t->head, sizeof(t->head));
  t->head = n;
  t->sz++;
#else
  if (pool_sz >= CYAML_POOL_CAP) {
    free(n);
    return;
  }
  if (pool_sz == 0) {
    pool_key_arm();
    if (!pool_key_armed) {
      free(n);
      return;
    }
  }
  memcpy((cyaml_node **)n, &pool_head, sizeof(pool_head));
  pool_head = n;
  pool_sz++;
#endif
}

#if defined(CCOL__EMULATE_DARWIN_TLS)
// Frees the pooled nodes and the error buffer of the block arg names; the pool
// key destructor under CCOL__EMULATE_DARWIN_TLS. A node freed later by another
// key's destructor makes a new block, and the C library calls this again for
// it.
static void pool_drain(void *arg) {
  cyaml_tls *t = (cyaml_tls *)arg;
  if (!t) return;
  cyaml_node *n = t->head;
  while (n) {
    cyaml_node *next;
    memcpy(&next, (cyaml_node **)n, sizeof(next));
    free(n);
    n = next;
  }
  free(t->err_buf);
  free(t);
}
#else
// Frees every pooled node and the error buffer of the calling thread; the pool
// key destructor. It also clears pool_key_armed, since the C library has
// cleared the key's value; a later free from another key's destructor then sets
// the key again and gets one more drain round.
static void pool_drain(void *arg) {
  (void)arg;
  cyaml_node *n = pool_head;
  while (n) {
    cyaml_node *next;
    memcpy(&next, (cyaml_node **)n, sizeof(next));
    free(n);
    n = next;
  }
  pool_head = NULL;
  pool_sz = 0;
  free(cyaml_err_buf);
  cyaml_err_buf = NULL;
  pool_key_armed = false;
}
#endif

#ifdef RUNNING_UNIT_TESTS
#if defined(CCOL__EMULATE_DARWIN_TLS)
// Test hook: Gives the size of the calling thread's pool, so tests can check
// the cap and that pools stay per thread.
size_t cyaml_debug_pool_size(void) {
  cyaml_tls *t = cyaml_tls_peek();
  return t ? (size_t)t->sz : 0;
}
#else
size_t cyaml_debug_pool_size(void) { return (size_t)pool_sz; }
#endif
#endif

// A heap worklist for tearing down a subtree without recursion. A tree built
// through the public API can be of any depth, and a destroy can't fail, so a
// depth cap would only turn a crash into a silent leak.
typedef struct destroy_worklist {
  cyaml_node **items;
  size_t cap;
  size_t len;
  // true for parse_discard()'s worklist, whose anchored nodes must survive.
  bool rescue;
} destroy_worklist;

static void parse_discard(cyaml_node *n);

// Pushes child onto the worklist (NULL is ignored), so a teardown never
// recurses per level. It grows the list with the default allocator. When the
// list can't grow, the child is destroyed at once by recursion; that needs a
// deep tree and an OOM at the same time.
static void destroy_worklist_push(destroy_worklist *wl, cyaml_node *child) {
  if (!child) return;
  if (wl->len == wl->cap) {
    size_t new_cap = wl->cap == 0 ? 32 : wl->cap * 2;
    cyaml_node **grown =
        ccol__default_mem_realloc(wl->items, new_cap * sizeof(*wl->items));
    if (!grown) {
      if (wl->rescue)
        parse_discard(child);
      else
        cyaml__destroy((cyaml)child);
      return;
    }
    wl->items = grown;
    wl->cap = new_cap;
  }
  wl->items[wl->len++] = child;
}

// Pushes the child of one dictionary entry onto the worklist in dtor_ctx; it's
// the value destructor node_clear_value() gives chashmap_destroy_with_dtor(),
// so dictionary values drain through the same loop as everything else.
static void cyaml_enqueue_dict_child(const cmap_pair *val_pair,
                                     void *dtor_ctx) {
  cyaml_node *child = cyaml_read_child(val_pair->ptr);
  destroy_worklist_push((destroy_worklist *)dtor_ctx, child);
}

// Frees the value payload of n (string, list or dictionary storage) and pushes
// each direct child onto wl, the one step every teardown runs per node. n
// itself, its type and its tag stay.
CYAML_PARSE_HOT void node_clear_value(cyaml_node *n, destroy_worklist *wl) {
  switch (n->type) {
    case CYAML_STRING:
      ccol__mem_free(n->m_procs, n->value.string);
      n->value.string = NULL;
      break;
    case CYAML_LIST: {
      size_t cnt = cvector_size(n->value.list);
      for (size_t i = 0; i < cnt; i++) {
        cyaml_node *child = *(cyaml_node **)cvector_at(n->value.list, i);
        destroy_worklist_push(wl, child);
      }
      cvector__destroy(n->value.list);
      n->value.list = NULL;
      break;
    }
    case CYAML_DICTIONARY: {
      chashmap_destroy_with_dtor(n->value.dictionary, cyaml_enqueue_dict_child,
                                 wl);
      n->value.dictionary = NULL;
      break;
    }
    default:
      break;
  }
}

// Destroys every node on wl, and through node_clear_value() everything below
// them, in a loop, so destroying a tree of any depth uses constant stack.
static void destroy_worklist_drain(destroy_worklist *wl) {
  while (wl->len > 0) {
    cyaml_node *n = wl->items[--wl->len];
    node_clear_value(n, wl);
    ccol__mem_free(n->m_procs, n->tag);
    n->tag = NULL;
    node_free(n);
  }
}

// Frees the value of n and the whole subtree below it, but not n or its tag, so
// node_reinit_scalar() can write a new scalar into n in place.
static void node_clear(cyaml_node *n) {
  destroy_worklist wl = {NULL, 0, 0, false};
  node_clear_value(n, &wl);
  destroy_worklist_drain(&wl);
  ccol__default_mem_free(wl.items);
}

// True when tag is one of the seven core-schema tags, with the one node type it
// may decorate in *expected; the tag setter and node_drop_stale_core_tag() use
// it. The tags share an 18-byte prefix, which one strncmp() checks first.
static bool core_tag_expected_type(const char *tag, cyaml_node_type *expected) {
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

// Drops a core tag of n that doesn't name its type; a custom tag stays.
// Every call that changes the type of a node runs this, so a round trip never
// writes a tag the parser refuses or reads back as another type.
static void node_drop_stale_core_tag(cyaml_node *n) {
  cyaml_node_type expected;
  if (n->tag && core_tag_expected_type(n->tag, &expected) &&
      expected != n->type) {
    ccol__mem_free(n->m_procs, n->tag);
    n->tag = NULL;
  }
}

// True when a string the caller wants to store (a value, a key or a tag) is
// well-formed UTF-8, so the tree only holds text the serializer can write. NULL
// passes. Any character is fine, since the serializer can escape every one;
// only bytes that name no character are refused.
static inline bool cyaml_api_string_ok(const char *s) {
  return !s || ccol_utf8_is_valid(s, strlen(s));
}

// Checks a cyaml__set_typed() payload without touching a node, so a refused
// cyaml_set() changes nothing: The type must be a scalar one, raw_size must
// match the width the type needs, raw may be NULL only for CYAML_NULL, a void
// pointer value must be NULL, and a string must be well-formed UTF-8. Every
// refusal is ccol_invalid_args. Without these checks a wrong size would read
// past the caller's object.
static ccol_retval scalar_payload_check(cyaml_node_type type, void *raw,
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

  if (type != CYAML_NULL && !raw) return ccol_invalid_args;

  if (type == CYAML_NULL && raw && raw_size == sizeof(void *)) {
    void *ptr_val;
    memcpy(&ptr_val, raw, sizeof(ptr_val));
    if (ptr_val != NULL) return ccol_invalid_args;
  }

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

  if (type == CYAML_STRING && raw_size != sizeof(const char *))
    return ccol_invalid_args;
  if (type == CYAML_STRING && !cyaml_api_string_ok(*(const char **)raw))
    return ccol_invalid_args;
  return ccol_success;
}

// Writes an accepted payload over the existing node n and frees what n held,
// for a cyaml_set() on a leaf that exists. A string is copied before
// node_clear(), so ccol_not_enough_memory leaves n unchanged. Integers are read
// with memcpy into a fixed-width type (the caller's object may be any type of
// that width), and an unsigned value above LLONG_MAX becomes the nearest
// double, as its decimal literal would parse.
static ccol_retval node_reinit_scalar_checked(cyaml_node *n,
                                              cyaml_node_type type, void *raw,
                                              size_t raw_size, bool is_signed) {
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

// Checks a payload, then writes it over n, for cyaml__set_typed() on an
// existing leaf; a refused call leaves n unchanged.
static ccol_retval node_reinit_scalar(cyaml_node *n, cyaml_node_type type,
                                      void *raw, size_t raw_size,
                                      bool is_signed) {
  ccol_retval r = scalar_payload_check(type, raw, raw_size);
  if (r != ccol_success) return r;
  return node_reinit_scalar_checked(n, type, raw, raw_size, is_signed);
}

// Makes a new node from an accepted payload with allocator mp, for
// cyaml__set_typed() on a new key. Fails only with ccol_not_enough_memory, and
// *out is NULL then.
static ccol_retval node_make_scalar(cyaml_node_type type, void *raw,
                                    size_t raw_size, bool is_signed,
                                    ccol_memmgmt_procs *mp, cyaml_node **out) {
  *out = NULL;
  cyaml_node *n = node_alloc(CYAML_NULL, mp);
  if (!n) return ccol_not_enough_memory;
  ccol_retval r = node_reinit_scalar_checked(n, type, raw, raw_size, is_signed);
  if (r != ccol_success) {
    node_free(n);
    return r;
  }
  *out = n;
  return ccol_success;
}

// Replaces *mp with its interned copy, for every public entry point that takes
// procs, so no node points at the caller's struct. false when the intern table
// is full.
static inline bool cyaml_intern_procs(ccol_memmgmt_procs **mp) {
  if (*mp == NULL) return true;
  return ccol_procs_intern(*mp, mp) == ccol_success;
}

// Make a node of each type with allocator mp, which is already interned. The
// public factories and the parser call them. NULL on OOM or a refused budget.
static cyaml cyaml_create_null_interned(ccol_memmgmt_procs *mp) {
  return (cyaml)node_alloc(CYAML_NULL, mp);
}

static cyaml cyaml_create_bool_interned(bool val, ccol_memmgmt_procs *mp) {
  cyaml_node *n = node_alloc(CYAML_BOOL, mp);
  if (n) n->value.boolean = val;
  return (cyaml)n;
}

static cyaml cyaml_create_int_interned(long long val, ccol_memmgmt_procs *mp) {
  cyaml_node *n = node_alloc(CYAML_INTEGER, mp);
  if (n) n->value.integer = val;
  return (cyaml)n;
}

static cyaml cyaml_create_double_interned(double val, ccol_memmgmt_procs *mp) {
  cyaml_node *n = node_alloc(CYAML_FLOAT, mp);
  if (n) n->value.number = val;
  return (cyaml)n;
}

static cyaml cyaml_create_string_interned(const char *val,
                                          ccol_memmgmt_procs *mp) {
  if (!val) return cyaml_create_null_interned(mp);
  cyaml_node *n = node_alloc(CYAML_STRING, mp);
  if (!n) return NULL;
  n->value.string = strdup_charged(mp, val);
  if (!n->value.string) {
    node_free(n);
    return NULL;
  }
  return (cyaml)n;
}

static cyaml cyaml_create_list_interned(ccol_memmgmt_procs *mp) {
  cyaml_node *n = node_alloc(CYAML_LIST, mp);
  if (!n) return NULL;
  n->value.list = cvector_create_full(sizeof(cyaml_node *), mp, NULL);
  if (!n->value.list) {
    node_free(n);
    return NULL;
  }
  return (cyaml)n;
}

static cyaml cyaml_create_dictionary_interned(ccol_memmgmt_procs *mp) {
  cyaml_node *n = node_alloc(CYAML_DICTIONARY, mp);
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

// Make a node of each type with the caller's allocator mp, the public
// factories. They intern mp first, so a full intern table fails the call before
// any node exists.
cyaml cyaml_create_null_mp(ccol_memmgmt_procs *mp) {
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_null_interned(mp);
}

cyaml cyaml_create_bool_mp(bool val, ccol_memmgmt_procs *mp) {
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_bool_interned(val, mp);
}

cyaml cyaml_create_int_mp(long long val, ccol_memmgmt_procs *mp) {
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_int_interned(val, mp);
}

cyaml cyaml_create_double_mp(double val, ccol_memmgmt_procs *mp) {
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_double_interned(val, mp);
}

cyaml cyaml_create_string_mp(const char *val, ccol_memmgmt_procs *mp) {
  if (!cyaml_api_string_ok(val)) return NULL;
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_string_interned(val, mp);
}

cyaml cyaml_create_list_mp(ccol_memmgmt_procs *mp) {
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_list_interned(mp);
}

cyaml cyaml_create_dictionary_mp(ccol_memmgmt_procs *mp) {
  if (!cyaml_intern_procs(&mp)) return NULL;
  return cyaml_create_dictionary_interned(mp);
}

// Frees node and its whole subtree, the body of cyaml_destroy(). It uses the
// worklist, so a tree of any depth is fine. node itself is freed directly and
// never pushed, because the push's OOM fallback would call this function on
// node again.
void cyaml__destroy(cyaml node) {
  if (!node) return;
  cyaml_node *n = (cyaml_node *)node;

  destroy_worklist wl = {NULL, 0, 0, false};
  node_clear_value(n, &wl);
  ccol__mem_free(n->m_procs, n->tag);
  n->tag = NULL;
  node_free(n);

  destroy_worklist_drain(&wl);
  ccol__default_mem_free(wl.items);
}

// Gives the type of node, so callers can check before an accessor that is fatal
// on a wrong type. CYAML_NULL for a NULL handle.
cyaml_node_type cyaml_type(cyaml node) {
  if (!node) return CYAML_NULL;
  return ((cyaml_node *)node)->type;
}

// Gives the tag of node, or NULL, so callers can read the tags a document
// carried.
const char *cyaml_node_tag(cyaml node) {
  if (!node) return NULL;
  return ((cyaml_node *)node)->tag;
}

// Sets, replaces or (for NULL) clears the tag of node. It refuses "" (it would
// serialize as "!<>", which the parser refuses), "!", text that isn't UTF-8 and
// a core tag that doesn't name the node's type, so every tagged tree
// round-trips. The copy is made before the old tag is freed.
ccol_retval cyaml_node_set_tag(cyaml node, const char *tag) {
  if (!node) return ccol_invalid_args;
  cyaml_node *n = (cyaml_node *)node;
  if (!tag) {
    ccol__mem_free(n->m_procs, n->tag);
    n->tag = NULL;
    return ccol_success;
  }
  if (tag[0] == '\0') return ccol_invalid_args;
  if (strcmp(tag, CYAML_TAG_NON_SPECIFIC) == 0) return ccol_invalid_args;
  if (!cyaml_api_string_ok(tag)) return ccol_invalid_args;
  cyaml_node_type expected;
  if (core_tag_expected_type(tag, &expected) && expected != n->type)
    return ccol_invalid_args;
  char *copy = strdup_charged(n->m_procs, tag);
  if (!copy) return ccol_not_enough_memory;
  ccol__mem_free(n->m_procs, n->tag);
  n->tag = copy;
  return ccol_success;
}

// Give the value of a scalar node to the caller; each is fatal on NULL or a
// wrong type, so check cyaml_type() first when unsure.
bool cyaml_bool_val(cyaml node) {
  cyaml_node *n = (cyaml_node *)node;
  if (!n || n->type != CYAML_BOOL)
    ccol__fatal_err("cyaml_bool_val: node is %s, expected CYAML_BOOL",
                    cyaml_type_str(node));
  return n->value.boolean;
}

long long cyaml_int_val(cyaml node) {
  cyaml_node *n = (cyaml_node *)node;
  if (!n || n->type != CYAML_INTEGER)
    ccol__fatal_err("cyaml_int_val: node is %s, expected CYAML_INTEGER",
                    cyaml_type_str(node));
  return n->value.integer;
}

double cyaml_double_val(cyaml node) {
  cyaml_node *n = (cyaml_node *)node;
  if (!n || n->type != CYAML_FLOAT)
    ccol__fatal_err("cyaml_double_val: node is %s, expected CYAML_FLOAT",
                    cyaml_type_str(node));
  return n->value.number;
}

const char *cyaml_str_val(cyaml node) {
  cyaml_node *n = (cyaml_node *)node;
  if (!n || n->type != CYAML_STRING)
    ccol__fatal_err("cyaml_str_val: node is %s, expected CYAML_STRING",
                    cyaml_type_str(node));
  return n->value.string;
}

// Give the element count of a list and the key count of a dictionary, for
// callers that walk them; each is fatal on a wrong type.
size_t cyaml_list_size(cyaml node) {
  cyaml_node *n = (cyaml_node *)node;
  if (!n || n->type != CYAML_LIST)
    ccol__fatal_err("cyaml_list_size: node is %s, expected CYAML_LIST",
                    cyaml_type_str(node));
  return cvector_size(n->value.list);
}

size_t cyaml_dictionary_size(cyaml node) {
  cyaml_node *n = (cyaml_node *)node;
  if (!n || n->type != CYAML_DICTIONARY)
    ccol__fatal_err(
        "cyaml_dictionary_size: node is %s, expected CYAML_DICTIONARY",
        cyaml_type_str(node));
  return chashmap_size(n->value.dictionary);
}

// Appends child to seq, which takes ownership of it. Every argument refusal is
// ccol_invalid_args and leaves child alone, even when several apply (child ==
// seq on a node that isn't a list), so the checks compare against seq itself.
// cvector_push_back() can't answer ccol_invalid_args here, so that code stays
// reserved for "child untouched"; every other failure destroys child. No cycle
// check, as cyaml.h says.
ccol_retval cyaml_list_push(cyaml seq, cyaml child) {
  if (!child) return ccol_invalid_args;
  cyaml_node *c = (cyaml_node *)child;
  if (c->attached || (const void *)c == (const void *)seq) {
    return ccol_invalid_args;
  }
  if (!seq) return ccol_invalid_args;
  cyaml_node *n = (cyaml_node *)seq;
  if (n->type != CYAML_LIST) return ccol_invalid_args;
  ccol_retval r = cvector_push_back(n->value.list, &c);
  if (r != ccol_success) {
    cyaml__destroy(child);
    return r;
  }
  c->attached = true;
  return r;
}

// Gives a borrowed reference to the element at index, so callers can read a
// list. NULL for a bad index or a node that isn't a list.
cyaml cyaml_list_get(cyaml seq, size_t index) {
  if (!seq) return NULL;
  cyaml_node *n = (cyaml_node *)seq;
  if (n->type != CYAML_LIST) return NULL;
  void *slot = cvector_at(n->value.list, index);
  if (!slot) return NULL;
  return *(cyaml *)slot;
}

// Puts child into an existing dictionary slot, for a set of a key that already
// exists (public set and the parser's repeated keys). The old child goes first
// (through parse_discard(), so anchored nodes survive a running parse), then
// the pointer is stored. Freeing the old child changes no map, so slot_vp stays
// valid. child gets marked attached.
static void dict_slot_replace_child(const cmap_pair *slot_vp,
                                    cyaml_node *child) {
  cyaml_node *old_child = cyaml_read_child(slot_vp->ptr);
  parse_discard(old_child);
  memcpy((void *)slot_vp->ptr, &child, sizeof(child));
  child->attached = true;
}

// Inserts or replaces key in map with child; the body of cyaml_dictionary_set()
// without the UTF-8 check of the key, which the parser has already done for the
// whole stream. Argument refusals give ccol_invalid_args and leave child alone;
// the "already attached" refusal sits below the lookup, since setting a key to
// the node it already holds is a no-op. A new key is charged to the parse byte
// budget before the single-probe insert and refunded when the key turns out to
// exist; a key that exists never trips the budget. Every other failure destroys
// child.
static ccol_retval dictionary_set_impl(cyaml map, const char *key,
                                       cyaml child) {
  if (!child) return ccol_invalid_args;
  cyaml_node *c = (cyaml_node *)child;
  if ((const void *)c == (const void *)map) return ccol_invalid_args;
  if (!map || !key) return ccol_invalid_args;
  cyaml_node *n = (cyaml_node *)map;
  if (n->type != CYAML_DICTIONARY) return ccol_invalid_args;

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};

  if (c->attached) {
    cmap_pair held_vp;
    if (chashmap_get_elem_ref(n->value.dictionary, &kp, &held_vp) ==
            ccol_success &&
        cyaml_read_child(held_vp.ptr) == c)
      return ccol_success;
    return ccol_invalid_args;
  }

  size_t entry_bytes = CYAML_BYTES_PER_DICT_ENTRY + kp.size;
  if (parse_bytes_would_refuse(entry_bytes)) {
    cmap_pair held_vp;
    if (chashmap_get_elem_ref(n->value.dictionary, &kp, &held_vp) ==
        ccol_success) {
      dict_slot_replace_child(&held_vp, c);
      return ccol_success;
    }
  }
  if (!parse_bytes_charge(entry_bytes)) {
    cyaml__destroy(child);
    return ccol_not_enough_memory;
  }

  cmap_pair vp = {.ptr = &c, .size = sizeof(c)};
  cmap_pair old_vp;
  ccol_retval r =
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
  cyaml__destroy((cyaml)child);
  return r;
}

// Inserts or replaces key in map. It refuses a key that isn't UTF-8 with
// ccol_invalid_args, then defers to dictionary_set_impl().
ccol_retval cyaml_dictionary_set(cyaml map, const char *key, cyaml child) {
  if (!cyaml_api_string_ok(key)) return ccol_invalid_args;
  return dictionary_set_impl(map, key, child);
}

// Gives a borrowed reference to the value of key, so callers can read a
// dictionary. NULL when the key is absent or map isn't a dictionary.
cyaml cyaml_dictionary_get(cyaml map, const char *key) {
  if (!map || !key) return NULL;
  cyaml_node *n = (cyaml_node *)map;
  if (n->type != CYAML_DICTIONARY) return NULL;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair vp;
  if (chashmap_get_elem_ref(n->value.dictionary, &kp, &vp) != ccol_success)
    return NULL;
  return cyaml_read_child(vp.ptr);
}

// Removes and frees the element at index, for callers that edit a list: It
// destroys the element, moves the later ones down and pops the last slot.
// ccol_invalid_args for a bad list or index.
ccol_retval cyaml_list_remove(cyaml seq, size_t index) {
  if (!seq) return ccol_invalid_args;
  cyaml_node *n = (cyaml_node *)seq;
  if (n->type != CYAML_LIST) return ccol_invalid_args;
  size_t cnt = cvector_size(n->value.list);
  if (index >= cnt) return ccol_invalid_args;

  cyaml_node *child = *(cyaml_node **)cvector_at(n->value.list, index);
  cyaml__destroy((cyaml)child);

  for (size_t i = index; i + 1 < cnt; i++) {
    cyaml_node **dst = (cyaml_node **)cvector_at(n->value.list, i);
    cyaml_node **src = (cyaml_node **)cvector_at(n->value.list, i + 1);
    *dst = *src;
  }
  cyaml_node *tmp = NULL;
  cvector_pop_back(n->value.list, &tmp);
  return ccol_success;
}

// Steps it onto the next member of a dictionary walk; false at the end. The
// cursor reads the successor when it steps onto a member, so removing the
// current member leaves it valid.
bool cyaml_dictionary_next(cyaml_dictionary_iter *it) {
  if (!it) return false;
  const ccol_chmap_entry_ref *e = (const ccol_chmap_entry_ref *)it->cyaml__next;
  if (!e) {
    it->key = NULL;
    it->value = NULL;
    return false;
  }
  const cmap_pair *kp, *vp;
  it->cyaml__next = ccol_chmap_entry_read(e, &kp, &vp);
  it->key = (const char *)kp->ptr;
  it->value = (cyaml)cyaml_read_child(vp->ptr);
  return true;
}

// Starts a dictionary walk: Fills it with the oldest member of dict and reads
// its successor. false for an empty, NULL or wrong-type dict, or a NULL it.
bool cyaml_dictionary_first(cyaml dict, cyaml_dictionary_iter *it) {
  if (!it) return false;
  cyaml_node *n = (cyaml_node *)dict;
  it->cyaml__next =
      (n && n->type == CYAML_DICTIONARY) ? dict_first_entry(n) : NULL;
  return cyaml_dictionary_next(it);
}

// Removes and frees the entry of key, for callers that edit a dictionary. The
// child is taken out of the map first and freed afterwards, so the map never
// points at freed memory. ccol_key_not_found when the key is absent.
ccol_retval cyaml_dictionary_remove(cyaml map, const char *key) {
  if (!map || !key) return ccol_invalid_args;
  cyaml_node *n = (cyaml_node *)map;
  if (n->type != CYAML_DICTIONARY) return ccol_invalid_args;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair vp;
  if (chashmap_get_elem_ref(n->value.dictionary, &kp, &vp) != ccol_success)
    return ccol_key_not_found;
  cyaml_node *child = cyaml_read_child(vp.ptr);
  ccol_retval r = chashmap_remove_elem(n->value.dictionary, &kp);
  if (r == ccol_success) cyaml__destroy((cyaml)child);
  return r;
}

// Copies src_tag onto the finished clone dst, so every cloned node keeps its
// tag; clone_shallow() calls it for each node. On OOM it destroys dst and gives
// false; it gives false for a NULL dst too. A NULL src_tag is fine.
static bool clone_attach_tag(cyaml dst, const char *src_tag,
                             ccol_memmgmt_procs *mp) {
  if (!dst) return false;
  if (!src_tag) return true;
  char *t = strdup_charged(mp, src_tag);
  if (!t) {
    cyaml__destroy(dst);
    return false;
  }
  ((cyaml_node *)dst)->tag = t;
  return true;
}

// The most containers a tree may nest on any path, the root included when it's
// a container. Parse, clone and every serializer share it, so each accepts
// exactly what the others accept. All of them walk with explicit stacks, so
// this limits work, not native stack.
#define CYAML_MAX_NESTING_DEPTH 500

// How many walk frames live in the walker's own locals before the stack moves
// to the heap. Ordinary documents never allocate for it.
#define CYAML_WALK_INLINE_FRAMES 16

// Grows an explicit walk stack to new_cap frames, for the parser, the clone and
// the serializers, which walk without recursion. It gives the new storage: The
// first growth copies the inline frames to the heap, later ones realloc. On
// failure it gives NULL and leaves *cap alone, so the caller can unwind what it
// pushed. The depth caps keep new_cap * elem_size far from overflow.
static void *walk_stack_grow_to(void *stack, size_t *cap, size_t new_cap,
                                void *inline_base, size_t elem_size,
                                ccol_memmgmt_procs *mp) {
  void *grown;
  if (stack == inline_base) {
    grown = ccol__mem_alloc(mp, new_cap * elem_size);
    if (!grown) return NULL;
    memcpy(grown, inline_base, *cap * elem_size);
  } else {
    grown = ccol__mem_realloc(mp, stack, new_cap * elem_size);
    if (!grown) return NULL;
  }
  *cap = new_cap;
  return grown;
}

// Grows a walk stack to twice its capacity through walk_stack_grow_to(), for
// walkers that grow by doubling.
static void *walk_stack_grow(void *stack, size_t *cap, void *inline_base,
                             size_t elem_size, ccol_memmgmt_procs *mp) {
  return walk_stack_grow_to(stack, cap, *cap * 2, inline_base, elem_size, mp);
}

// One level of clone_node()'s walk: A source container and its clone. A list
// walks by idx up to count; a dictionary walks through next, and idx counts the
// entries seen so the caller can tell a walk that stopped early.
typedef struct {
  cyaml_node *src;
  cyaml_node *dst;
  size_t idx;
  size_t count;
  const ccol_chmap_entry_ref *next;
} clone_frame;

// Copies src without its children (a container comes back empty) and attaches
// its tag; clone_node() makes every node of a copy through it. NULL on OOM,
// with nothing left behind.
static cyaml clone_shallow(cyaml_node *src) {
  ccol_memmgmt_procs *mp = src->m_procs;
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

// Starts the walk of src's children into the empty clone dst, when clone_node()
// enters a container; in insertion order for a dictionary.
static void clone_frame_open(clone_frame *f, cyaml_node *src, cyaml_node *dst) {
  f->src = src;
  f->dst = dst;
  f->idx = 0;
  f->count = 0;
  f->next = NULL;
  if (src->type == CYAML_LIST)
    f->count = cvector_size(src->value.list);
  else
    f->next = dict_first_entry(src);
}

// Deep-copies src with an explicit frame stack, for cyaml_clone() and every
// alias expansion. max_levels caps the nesting of the copy; past it we give
// NULL with *too_deep set, and any other NULL is an OOM or a spent parse
// budget. On success *height is the nesting of the copy. Each child is attached
// to its parent's clone before its own subtree is walked, so one
// cyaml__destroy() of the root frees a partial copy. Children go straight into
// the containers (no public push or set), but a new dictionary entry is
// charged to the byte budget, since alias expansion clones whole mappings.
static cyaml clone_node(cyaml_node *src, size_t max_levels, bool *too_deep,
                        size_t *height) {
  ccol_memmgmt_procs *mp = src->m_procs;
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

  clone_frame inline_frames[CYAML_WALK_INLINE_FRAMES];
  clone_frame *stack = inline_frames;
  size_t cap = CYAML_WALK_INLINE_FRAMES;
  clone_frame_open(&stack[0], src, (cyaml_node *)root);
  size_t sp = 1;
  size_t max_sp = 1;
  bool failed = false;

  while (!failed && sp > 0) {
    clone_frame *f = &stack[sp - 1];
    cyaml_node *child;
    const cmap_pair *key = NULL;

    if (f->src->type == CYAML_LIST) {
      if (f->idx >= f->count) {
        sp--;
        continue;
      }
      child = *(cyaml_node **)cvector_at(f->src->value.list, f->idx);
      f->idx++;
    } else {
      if (!f->next) {
        sp--;
        continue;
      }
      const cmap_pair *vp;
      f->next = ccol_chmap_entry_read(f->next, &key, &vp);
      child = cyaml_read_child(vp->ptr);
    }

    if (sp >= max_levels &&
        (child->type == CYAML_LIST || child->type == CYAML_DICTIONARY)) {
      *too_deep = true;
      failed = true;
      break;
    }

    cyaml_node *cc = (cyaml_node *)clone_shallow(child);
    if (!cc) {
      failed = true;
      break;
    }
    if (key) {
      if (!parse_bytes_charge(CYAML_BYTES_PER_DICT_ENTRY + key->size)) {
        cyaml__destroy((cyaml)cc);
        failed = true;
        break;
      }
      cmap_pair vp = {.ptr = &cc, .size = sizeof(cc)};
      if (chashmap_insert_elem(f->dst->value.dictionary, key, &vp) !=
          ccol_success) {
        cyaml__destroy((cyaml)cc);
        failed = true;
        break;
      }
    } else if (cvector_push_back(f->dst->value.list, &cc) != ccol_success) {
      cyaml__destroy((cyaml)cc);
      failed = true;
      break;
    }
    cc->attached = true;

    if (child->type == CYAML_LIST || child->type == CYAML_DICTIONARY) {
      if (sp == cap) {
        clone_frame *grown =
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

  if (stack != inline_frames) ccol__mem_free(mp, stack);
  if (failed) {
    cyaml__destroy(root);
    return NULL;
  }
  *height = max_sp;
  return root;
}

// Gives a deep copy of node that the caller owns, the public clone, within the
// module's nesting limit; NULL for a NULL node, on OOM or for a tree that is
// too deep.
cyaml cyaml_clone(cyaml node) {
  if (!node) return NULL;
  bool too_deep;
  size_t height;
  return clone_node((cyaml_node *)node, CYAML_MAX_NESTING_DEPTH, &too_deep,
                    &height);
}

// One change a speculative parse made to the anchor table: An owned copy of the
// name and the node it named before (NULL when the change added it).
typedef struct {
  char *name;
  cyaml_node *old;
} anchor_undo_entry;

// How many undo entries a log keeps inline, so a speculation that stores few
// anchors allocates nothing for its log.
#define ANCHOR_UNDO_INLINE 4
// The changes of one speculative parse to the anchor table, in order, so a
// rejection can undo them. entries points at inline_entries until the log
// grows.
typedef struct {
  anchor_undo_entry *entries;
  size_t len;
  size_t cap;
  anchor_undo_entry inline_entries[ANCHOR_UNDO_INLINE];
} anchor_undo_log;

// The state of one parse. anchors maps names to anchored nodes without owning
// them, and tag_handles maps %TAG handles to prefixes; both are created on
// first use and freed at the end of each document. depth counts the containers
// around the current node (each collection frame and the one-entry mapping of a
// flow pair hold a level); later documents of a stream start at 1 because the
// stream list encloses them.
typedef struct {
  const char *src;
  size_t pos;
  size_t len;
  char error[512];
  ccol_memmgmt_procs *mp;
  chmap anchors;
  chmap tag_handles;
  size_t depth;
  // The deepest depth of the document, aliases included. A first document at
  // the limit is refused once a second document follows.
  size_t peak_depth;
  // The last answer of line_start_pos(); the text never changes, so a repeat
  // question for the same offset is always answered here. SIZE_MAX means none.
  size_t memo_pos;
  size_t memo_line_start;
  // Where the content starts: 0 or the offset past a byte-order mark. Both line
  // lookups must use it as the start of the first line.
  size_t first_line_start;
  // The line-start cache of line_start_pos(). It falls back to a backward scan
  // for good once a growth fails.
  size_t *line_starts;
  size_t line_starts_len;
  size_t line_starts_cap;
  size_t line_scan_pos;
  bool line_cache_disabled;
  // The undo log of the running speculation, or NULL.
  anchor_undo_log *anchor_undo;
  // Anchored nodes that no tree holds any more, owned until anchors_destroy().
  // Each anchors_store() reserves a slot, so keeping a node never allocates.
  cyaml_node **kept;
  size_t kept_len;
  size_t kept_cap;
  size_t kept_reserved;
} parse_ctx;

// The parse running on this thread while it has an anchor table, or NULL.
// parse_discard() reads it, since dict_slot_replace_child() has no context.
static __thread parse_ctx *parse_discard_ctx;

// Moves n into a slot of ctx's kept list that anchors_store() reserved, so an
// anchored node no tree holds stays alive for later aliases.
static void parse_keep(parse_ctx *ctx, cyaml_node *n) {
  ccol__assert(ctx->kept_len < ctx->kept_cap);
  ctx->kept[ctx->kept_len++] = n;
}

// Frees a subtree that a running parse drops, but moves each anchored node in
// it whole to the kept list, since a later alias may copy it. Outside a parse
// with an anchor table it's just cyaml__destroy().
static void parse_discard(cyaml_node *n) {
  if (!n) return;
  parse_ctx *ctx = parse_discard_ctx;
  if (!ctx) {
    cyaml__destroy((cyaml)n);
    return;
  }
  destroy_worklist wl = {NULL, 0, 0, true};
  cyaml_node *node = n;
  for (;;) {
    if (node->anchored) {
      parse_keep(ctx, node);
    } else {
      node_clear_value(node, &wl);
      ccol__mem_free(node->m_procs, node->tag);
      node->tag = NULL;
      node_free(node);
    }
    if (wl.len == 0) break;
    node = wl.items[--wl.len];
  }
  ccol__default_mem_free(wl.items);
}

// The most frames one parse's frame stack may hold. Each container holds one
// collection frame, and a node holds at most three node frames (itself, its
// anchor and its tag), so the nesting limit bounds the stack at this size. The
// driver never grows the stack past it.
#define CYAML_PARSE_MAX_FRAMES (4 * (CYAML_MAX_NESTING_DEPTH + 1))

// The longest canonical text of a collection used as a key. Each nesting level
// of such a key quotes the text of the level below again, so the text grows as
// 2^N with the depth; the nesting limit can't stop that. The builder stops at
// the first byte past this limit, so a refused key costs at most 64 KiB.
#define CYAML_MAX_CANONICAL_KEY_LEN (64 * 1024)

// The fixed floor of the node budget of one parse. Aliases and merge keys make
// real deep copies, so nested aliases can grow the node count exponentially
// from a small text ("billion laughs"); this caps the nodes, and
// CYAML_MAX_PARSE_BYTES caps their bytes.
#define CYAML_MAX_PARSE_NODES (4 * 1000 * 1000)

// The node budget grows with the input at this many nodes per byte: 1.5 times
// the worst honest shape ("[:,:,:]", 1.5 nodes per byte), rounded up to a power
// of two. Copies also count against the fixed amplification budget, so a long
// input never raises the limit for its expansion.
#define CYAML_PARSE_NODES_PER_INPUT_BYTE 4

// The fixed floor of the byte budget of one parse. A node isn't a fixed amount
// of memory (strings, tags, keys, container storage), so a few large anchored
// scalars could blow up memory with few nodes. Every DOM allocation site of the
// parser charges this budget.
#define CYAML_MAX_PARSE_BYTES ((size_t)256 * 1024 * 1024)

// The byte budget grows with the input at this many bytes per byte: The same
// "[:,:,:]" shape costs about 331, times 1.5 rounded up. Without the scaling a
// large honest document (a big Kubernetes List) would be refused.
#define CYAML_PARSE_BYTES_PER_INPUT_BYTE 512

// The two limits of the running parse as parse_node_budget_arm() computed
// them, so the error messages can name them.
static __thread size_t parse_node_limit = CYAML_MAX_PARSE_NODES;
static __thread size_t parse_byte_limit = CYAML_MAX_PARSE_BYTES;

// Gives the budget for an input of input_len bytes, max(floor, per_byte *
// input_len), saturated below the (size_t)-1 sentinel (512 times a large input
// overflows on ILP32). parse_node_budget_arm() uses it for both budgets.
static inline size_t parse_budget_for_input(size_t floor, size_t per_byte,
                                            size_t input_len) {
  size_t cap = (size_t)-1 - 1;
  size_t scaled = input_len > cap / per_byte ? cap : input_len * per_byte;
  return scaled > floor ? scaled : floor;
}

#ifdef RUNNING_UNIT_TESTS
// The floors of the two limits. A test lowers them to show the scaling on a
// small document and restores them afterwards.
size_t cyaml_test_parse_node_floor = CYAML_MAX_PARSE_NODES;
size_t cyaml_test_parse_byte_floor = CYAML_MAX_PARSE_BYTES;
// The floors the budgets start from: The test variables above in test builds,
// the fixed constants otherwise.
#define CYAML_PARSE_NODE_FLOOR cyaml_test_parse_node_floor
#define CYAML_PARSE_BYTE_FLOOR cyaml_test_parse_byte_floor

// Test hook: Gives parse_budget_for_input(), so tests can check the scaling and
// the saturation.
size_t cyaml_debug_parse_budget_for_input(size_t floor, size_t per_byte,
                                          size_t input_len) {
  return parse_budget_for_input(floor, per_byte, input_len);
}
#else
#define CYAML_PARSE_NODE_FLOOR ((size_t)CYAML_MAX_PARSE_NODES)
#define CYAML_PARSE_BYTE_FLOOR CYAML_MAX_PARSE_BYTES
#endif

// The amplification budget of the running parse. Every node and byte made by
// copying something already built (an alias, a merged value, a collection key's
// text) is charged here as well as to the ordinary budgets, and this one keeps
// the fixed floors whatever the input length, so padding a document buys no
// expansion. parse_amp_enter() narrows the ordinary budgets to what this one
// allows and parse_amp_leave() settles; only the outermost of nested regions
// does either.
static __thread size_t parse_amp_node_budget;
static __thread size_t parse_amp_byte_budget;
static __thread size_t parse_amp_saved_node_budget;
static __thread size_t parse_amp_saved_byte_budget;
static __thread size_t parse_amp_start_node_budget;
static __thread size_t parse_amp_start_byte_budget;
static __thread unsigned parse_amp_depth;

// Set by parse_amp_leave() when a refused charge happened with this budget as
// the binding limit, so the error names it. Cleared by the next arm or disarm.
static __thread bool parse_amp_limit_hit;

// Opens an amplification region around a copy (alias, merge, collection key),
// so the copy is bounded by the fixed amplification budget. The outermost
// region saves the ordinary budgets and narrows them to it.
static inline void parse_amp_enter(void) {
  if (parse_node_budget == (size_t)-1) return;
  if (parse_amp_depth++ != 0) return;
  parse_amp_saved_node_budget = parse_node_budget;
  parse_amp_saved_byte_budget = parse_byte_budget;
  if (parse_amp_node_budget < parse_node_budget)
    parse_node_budget = parse_amp_node_budget;
  if (parse_amp_byte_budget < parse_byte_budget)
    parse_byte_budget = parse_amp_byte_budget;
  parse_amp_start_node_budget = parse_node_budget;
  parse_amp_start_byte_budget = parse_byte_budget;
}

// Closes an amplification region: The outermost one charges what the region
// used to both budgets and sets parse_amp_limit_hit when the amplification
// budget was the binding one. A refund inside the region never lifts the budget
// above its start.
static inline void parse_amp_leave(void) {
  if (parse_node_budget == (size_t)-1) return;
  if (--parse_amp_depth != 0) return;
  size_t used_nodes = parse_amp_start_node_budget - parse_node_budget;
  size_t used_bytes = parse_amp_start_byte_budget - parse_byte_budget;
  bool amp_binding_nodes = parse_amp_node_budget <= parse_amp_saved_node_budget;
  bool amp_binding_bytes = parse_amp_byte_budget <= parse_amp_saved_byte_budget;
  parse_amp_node_budget -= used_nodes;
  parse_amp_byte_budget -= used_bytes;
  parse_node_budget = parse_amp_saved_node_budget - used_nodes;
  parse_byte_budget = parse_amp_saved_byte_budget - used_bytes;
  if ((parse_node_budget_exhausted && amp_binding_nodes) ||
      (parse_byte_budget_exhausted && amp_binding_bytes))
    parse_amp_limit_hit = true;
}

// Deep-copies src through clone_node() inside an amplification region. Every
// alias expansion copies through here, with the levels left below the place
// where the copy lands.
static cyaml_node *parse_clone_levels(cyaml_node *src, size_t max_levels,
                                      bool *too_deep, size_t *height) {
  parse_amp_enter();
  cyaml_node *copy =
      (cyaml_node *)clone_node(src, max_levels, too_deep, height);
  parse_amp_leave();
  return copy;
}

// Arms the node, byte and amplification budgets for a parse of input_len bytes,
// at the start of parse_common(), so one document can't use unbounded memory.
// They are always armed and disarmed together.
static inline void parse_node_budget_arm(size_t input_len) {
  parse_node_limit = parse_budget_for_input(
      CYAML_PARSE_NODE_FLOOR, CYAML_PARSE_NODES_PER_INPUT_BYTE, input_len);
  parse_byte_limit = parse_budget_for_input(
      CYAML_PARSE_BYTE_FLOOR, CYAML_PARSE_BYTES_PER_INPUT_BYTE, input_len);
  parse_node_budget = parse_node_limit;
  parse_node_budget_exhausted = false;
  parse_byte_budget = parse_byte_limit;
  parse_byte_budget_exhausted = false;
  parse_amp_node_budget = CYAML_PARSE_NODE_FLOOR;
  parse_amp_byte_budget = CYAML_PARSE_BYTE_FLOOR;
  parse_amp_depth = 0;
  parse_amp_limit_hit = false;
}
#ifdef RUNNING_UNIT_TESTS
// What the last parse on this thread charged to its two budgets, for the tests
// that check the per-byte factors.
__thread size_t cyaml_test_last_parse_nodes_charged;
__thread size_t cyaml_test_last_parse_bytes_charged;
// The largest frame stack capacity a parse reached on this thread.
__thread size_t cyaml_test_peak_parse_frame_cap;
#endif

// Disarms the budgets at every exit of parse_common(): Sets them back to
// unlimited and clears parse_discard_ctx, so nothing a parse left can reach
// later calls on this thread.
static inline void parse_node_budget_disarm(void) {
#ifdef RUNNING_UNIT_TESTS
  if (parse_node_budget != (size_t)-1) {
    cyaml_test_last_parse_nodes_charged = parse_node_limit - parse_node_budget;
    cyaml_test_last_parse_bytes_charged = parse_byte_limit - parse_byte_budget;
  }
#endif
  parse_discard_ctx = NULL;
  parse_node_budget = (size_t)-1;
  parse_node_budget_exhausted = false;
  parse_byte_budget = (size_t)-1;
  parse_byte_budget_exhausted = false;
  parse_amp_depth = 0;
  parse_amp_limit_hit = false;
}

static void parse_err(parse_ctx *ctx, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
// Formats a parse error into ctx->error, the one way the parser reports an
// error; the last call wins. The printf format attribute on the prototype lets
// the compiler check every call site's format string, which also keeps
// -Wformat-nonliteral quiet here.
static void parse_err(parse_ctx *ctx, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
  va_end(ap);
}

// Reports a container past CYAML_MAX_NESTING_DEPTH, so every depth refusal
// names the limit in the same words.
static void parse_depth_err(parse_ctx *ctx) {
  parse_err(ctx, "maximum nesting depth (%d) exceeded at position %zu",
            CYAML_MAX_NESTING_DEPTH, ctx->pos);
}

// Takes the nesting level of a flow-sequence pair. A "key: value" pair in a
// flow sequence is a one-entry mapping one level down, so it holds a level
// while its value is read. false, with the error set, when that level is past
// the limit.
static inline bool flow_pair_level_take(parse_ctx *ctx) {
  if (ctx->depth >= CYAML_MAX_NESTING_DEPTH) {
    parse_depth_err(ctx);
    return false;
  }
  if (++ctx->depth > ctx->peak_depth) ctx->peak_depth = ctx->depth;
  return true;
}

// Gives the nesting of the tree n (0 for a scalar), for
// parse_key_fits_below_mapping(). It walks with a fixed stack of
// CYAML_MAX_NESTING_DEPTH frames; a deeper tree gives the limit plus one.
static size_t parse_tree_height(cyaml_node *n) {
  if (n->type != CYAML_LIST && n->type != CYAML_DICTIONARY) return 0;
  struct {
    cyaml_node *n;
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
    cyaml_node *child;
    if (stack[sp - 1].n->type == CYAML_LIST) {
      cvec list = stack[sp - 1].n->value.list;
      if (stack[sp - 1].idx >= cvector_size(list)) {
        sp--;
        continue;
      }
      child = *(cyaml_node **)cvector_at(list, stack[sp - 1].idx++);
    } else {
      if (!stack[sp - 1].next) {
        sp--;
        continue;
      }
      const cmap_pair *kp, *vp;
      stack[sp - 1].next = ccol_chmap_entry_read(stack[sp - 1].next, &kp, &vp);
      child = cyaml_read_child(vp->ptr);
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

// Checks that a collection used as a key fits below the mapping that holds it.
// Such a key is read before its mapping exists, so this adds the mapping's
// level once the node is known to be a key, and gives false with the error set
// when that passes the limit. It walks the key only when peak_depth is within
// one level of the limit.
static bool parse_key_fits_below_mapping(parse_ctx *ctx, cyaml_node *key) {
  if (ctx->peak_depth + 1 < CYAML_MAX_NESTING_DEPTH) return true;
  size_t top = ctx->depth + 1 + parse_tree_height(key);
  if (top > CYAML_MAX_NESTING_DEPTH) {
    parse_depth_err(ctx);
    return false;
  }
  if (top > ctx->peak_depth) ctx->peak_depth = top;
  return true;
}

// Every byte of input that an error message quotes goes through
// cyaml_err_echo(), so messages stay printable ASCII: No terminal escapes, no
// forged log lines, no invalid UTF-8. Bytes 0x20 to 0x7E stay, any other byte
// becomes <0xNN>, and a long text is cut at a whole byte with "...". The macros
// give each echo its own compound-literal buffer that lives to the end of the
// enclosing block.
#define CYAML_ERR_ECHO_BUF 72

// Writes the printable-ASCII echo of the n bytes at s into out and gives out,
// so every quoted input in an error message is safe to log.
static const char *cyaml_err_echo(const char *s, size_t n, char *out) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    bool plain = c >= 0x20 && c <= 0x7E;
    size_t need = plain ? 1 : 6;
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

// Echo input text into an error message: cyaml_echo_n() takes the n bytes at s,
// cyaml_echo() a NUL-terminated string (NULL gives ""), cyaml_echo_c() one
// character. Each evaluates to the printable-ASCII echo in a compound-literal
// buffer that lives to the end of the enclosing block, so it can go straight
// into a parse_err() argument list.
#define cyaml_echo_n(s, n) \
  cyaml_err_echo((s), (n), (char[CYAML_ERR_ECHO_BUF]){0})
#define cyaml_echo(s) \
  cyaml_echo_n(((s) ? (s) : ""), ((s) ? strlen(s) : (size_t)0))
#define cyaml_echo_c(c) cyaml_echo_n(&(const char){(char)(c)}, 1)

// True once the whole input is read; every scanner's end test.
static inline bool at_end(parse_ctx *ctx) { return ctx->pos >= ctx->len; }

// Gives the current character, or '\0' at the end of the input, so scanners
// can peek without a bounds test of their own.
static inline char cur(parse_ctx *ctx) {
  return at_end(ctx) ? '\0' : ctx->src[ctx->pos];
}

#ifdef RUNNING_UNIT_TESTS
// Test switch that forces the cache-less line-start path. Memory pressure alone
// can't reach the one case where the two paths could differ (a query on the
// first line after a byte-order mark).
bool cyaml_test_force_line_cache_disabled = false;
#endif

// Gives the line start of ctx->pos by scanning back to the last line break,
// never below first_line_start. It's the O(column) fallback of line_start_pos()
// once the cache is disabled.
static size_t line_start_pos_backward_scan(parse_ctx *ctx) {
  size_t p = ctx->pos;
  while (p > ctx->first_line_start && ctx->src[p - 1] != '\n' &&
         ctx->src[p - 1] != '\r')
    p--;
  return p;
}

// Extends the line cache of line_start_pos(): Appends the start of every line
// from line_scan_pos up to, not including, up_to. On OOM it frees the cache and
// sets line_cache_disabled for good.
static void line_starts_extend(parse_ctx *ctx, size_t up_to) {
  size_t p = ctx->line_scan_pos;
  while (p < up_to) {
    char c = ctx->src[p];
    p++;
    if (c == '\n' || c == '\r') {
      if (ctx->line_starts_len == ctx->line_starts_cap) {
        size_t new_cap = ctx->line_starts_cap ? ctx->line_starts_cap * 2 : 64;
        size_t *grown = ccol__mem_realloc(ctx->mp, ctx->line_starts,
                                          new_cap * sizeof(size_t));
        if (!grown) {
          ccol__mem_free(ctx->mp, ctx->line_starts);
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

static size_t line_start_pos_uncached(parse_ctx *ctx);

// Gives the offset where the line holding ctx->pos starts ('\n' and a lone
// '\r' both end a line). A backward scan per call would make long flow lines
// quadratic, so a sorted cache of line starts keeps the total work linear. The
// memo in ctx answers a repeat question; test builds compare it with a fresh
// answer. The first cache allocation happens in parse_common() and its failure
// fails the parse; a later growth failure falls back to the scan.
static size_t line_start_pos(parse_ctx *ctx) {
  if (ctx->pos == ctx->memo_pos) {
#ifdef RUNNING_UNIT_TESTS
    ccol__assert(ctx->memo_line_start == line_start_pos_uncached(ctx));
#endif
    return ctx->memo_line_start;
  }

  size_t result = line_start_pos_uncached(ctx);
  ctx->memo_pos = ctx->pos;
  ctx->memo_line_start = result;
  return result;
}

// Gives the line start of ctx->pos from the line cache, the real lookup behind
// line_start_pos() when the memo misses. After extending to ctx->pos every
// entry is at or below it, so the last entry is the answer (test builds check
// this against the search). After a rewind the cache holds later entries and we
// binary-search instead.
static size_t line_start_pos_uncached(parse_ctx *ctx) {
  if (ctx->line_cache_disabled) return line_start_pos_backward_scan(ctx);

  if (ctx->pos > ctx->line_scan_pos) {
    line_starts_extend(ctx, ctx->pos);
    if (ctx->line_cache_disabled) return line_start_pos_backward_scan(ctx);
    size_t answer = ctx->line_starts[ctx->line_starts_len - 1];
#ifdef RUNNING_UNIT_TESTS
    {
      size_t vlo = 0, vhi = ctx->line_starts_len;
      while (vlo + 1 < vhi) {
        size_t vmid = vlo + (vhi - vlo) / 2;
        if (ctx->line_starts[vmid] <= ctx->pos)
          vlo = vmid;
        else
          vhi = vmid;
      }
      ccol__assert(answer == ctx->line_starts[vlo]);
    }
#endif
    return answer;
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

// Gives the 0-based column of ctx->pos, which the block parsers compare with
// indents to find structure. It's clamped at INT_MAX, since callers only
// compare it with other indents.
CYAML_PARSE_HOT int current_col(parse_ctx *ctx) {
  if (ctx->pos == 0) return 0;
  size_t p = line_start_pos(ctx);
  size_t col = ctx->pos - p;
  return col > (size_t)INT_MAX ? INT_MAX : (int)col;
}

// True when a tab sits anywhere between the line start and ctx->pos. Block
// entries call it, because a block column can't be measured across a tab. The
// whitespace skippers don't refuse tabs themselves: Inside open content a tab
// is data, and their callers need them to always move forward.
static bool line_indent_has_tab(parse_ctx *ctx) {
  size_t p = line_start_pos(ctx);
  return memchr(ctx->src + p, '\t', ctx->pos - p) != NULL;
}

// Gives the indentation of the current line, for the indent comparisons of flow
// continuation lines and block values: The spaces before the first tab when the
// leading whitespace holds one, else current_col(). A tab after the indent is
// separation.
static int line_space_indent(parse_ctx *ctx) {
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

// Skips spaces and tabs without crossing a newline, where block parsing needs
// the column of the next token. It skips tabs on purpose (see
// line_indent_has_tab()); stopping at one would hang the fold loops.
static void skip_inline_ws(parse_ctx *ctx) {
  while (!at_end(ctx)) {
    char c = cur(ctx);
    if (c == ' ' || c == '\t')
      ctx->pos++;
    else
      break;
  }
}

// Skips the rest of the line and its line break (LF, CR or CRLF), past a
// comment or a header the parser is done with.
static void skip_to_eol(parse_ctx *ctx) {
  while (!at_end(ctx) && cur(ctx) != '\n' && cur(ctx) != '\r') ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '\r') ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '\n') ctx->pos++;
}

// True at a line break or the end of the input, the end test of every line
// scan.
static inline bool at_eol(parse_ctx *ctx) {
  return at_end(ctx) || cur(ctx) == '\n' || cur(ctx) == '\r';
}

// True when [start, ctx->pos) holds a line break, for the one-line rule of
// implicit keys and the checks of flow continuation lines. It reads no byte:
// the line start of ctx->pos lies after start exactly when it does. Test builds
// assert the bounds and the equivalence.
static bool span_crosses_newline(parse_ctx *ctx, size_t start) {
#ifdef RUNNING_UNIT_TESTS
  ccol__assert(start >= ctx->first_line_start && start <= ctx->pos);
  ccol__assert((line_start_pos(ctx) > start) ==
               (memchr(ctx->src + start, '\n', ctx->pos - start) != NULL ||
                memchr(ctx->src + start, '\r', ctx->pos - start) != NULL));
#endif
  return line_start_pos(ctx) > start;
}

// True when only inline whitespace and maybe a comment remain on the line.
// Decides whether a value, anchor or tag starts on this line, which a comment
// after the indicator must not fool. ctx->pos doesn't move.
static bool rest_of_line_is_blank(parse_ctx *ctx) {
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

// Skips whitespace, newlines and comments between tokens where indentation
// doesn't matter. A '#' that touches the token before it isn't a comment, so it
// stays for the caller to refuse.
static void skip_ws_comments(parse_ctx *ctx) {
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

// Skips whitespace and comments inside flow content like skip_ws_comments().
// When it crosses a line, the line it lands on must be indented by more than
// indent (the enclosing block indent, -1 at the root), counting the spaces
// before the first tab. A line that starts with ']' or '}' may sit exactly at
// indent, as libyaml allows, to close the collection under its key. false with
// the error set when the check fails.
CYAML_PARSE_HOT bool flow_skip_ws(parse_ctx *ctx, int indent) {
  size_t start = ctx->pos;
  skip_ws_comments(ctx);
  if (!at_end(ctx)) {
    bool crossed_newline = span_crosses_newline(ctx, start);
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

// Reads one line break (CR, LF or CRLF), for the scalar scanners that walk line
// by line.
static void skip_newline(parse_ctx *ctx) {
  if (!at_end(ctx) && cur(ctx) == '\r') ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '\n') ctx->pos++;
}

// True when the rest of the line holds only spaces and tabs. The quoted-scalar
// fold uses it so a blank line with trailing whitespace still folds to a
// newline. Reads nothing.
static bool at_blank_line(parse_ctx *ctx) {
  size_t p = ctx->pos;
  while (p < ctx->len && (ctx->src[p] == ' ' || ctx->src[p] == '\t')) p++;
  return p >= ctx->len || ctx->src[p] == '\r' || ctx->src[p] == '\n';
}

// True for a C0 control other than tab, and for DEL, which no scalar style may
// hold raw (PyYAML refuses DEL in every style too). The serializer uses it to
// double-quote a string holding one and write it as an escape.
static inline bool is_disallowed_control_byte(char c) {
  unsigned char u = (unsigned char)c;
  return (u < 0x20 && u != '\t') || u == 0x7F;
}

// A stream must be UTF-8 made of c-printable characters only. parse_common()
// checks the whole input once before parsing, so no scanner checks again, and a
// raw NUL never reaches a stored token. The check accepts 16 printable ASCII
// bytes with one vector compare and walks any other block one character at a
// time (GCC and Clang vector extensions).
typedef unsigned char cyaml_u8x16 __attribute__((vector_size(16)));
typedef signed char cyaml_s8x16 __attribute__((vector_size(16)));

// True when each of the 16 bytes at p is TAB, LF, CR or 0x20 to 0x7E, the
// vector fast path of cyaml_first_refused_char().
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

// True for an ASCII byte outside c-printable (a C0 control other than TAB, LF
// and CR, or DEL), the slow path of the stream check.
static inline bool cyaml_is_refused_ascii(unsigned char c) {
  return (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) || c == 0x7F;
}

// Gives the offset of the first character of p[0..len) that is ill-formed UTF-8
// or outside c-printable (C1 controls other than NEL, U+FFFE, U+FFFF included),
// or len when there is none. parse_common() runs it on the whole input before
// parsing.
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
      if (c == 0xC2 && p[i + 1] <= 0x9F && p[i + 1] != 0x85) return i;
      if (c == 0xEF && p[i + 1] == 0xBF && p[i + 2] >= 0xBE) return i;
      i += step;
    }
    if (i >= len) return len;
  }
}

// Writes the error for the character at p[at] that cyaml_first_refused_char()
// refused into msg, so the caller learns where and what it is: The defect, the
// bytes as 0xNN, and the line and column counted from 1. The column counts
// characters, and a leading byte-order mark isn't counted. The text comes from
// the UTF-8 helper shared with cjson.
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

  char detail[64];
  const char *what =
      ccol_utf8_describe_ill_formed(p, len, at, detail, sizeof(detail));
  snprintf(msg, msg_size,
           "invalid UTF-8 in a YAML stream (line %zu, column %zu): %s (%s)",
           line, col, what, detail);
}

// True at "---" or "..." in column 0 followed by whitespace or the end, so
// block parsers stop at a document boundary and the stream loop finds the next
// document. "---#x" is no marker, since a comment needs whitespace before it.
CYAML_PARSE_HOT bool at_doc_marker(parse_ctx *ctx) {
  if (current_col(ctx) != 0) return false;
  if (ctx->pos + 3 > ctx->len) return false;
  const char *p = ctx->src + ctx->pos;
  if (memcmp(p, "---", 3) != 0 && memcmp(p, "...", 3) != 0) return false;
  if (ctx->pos + 3 == ctx->len) return true;
  char nx = ctx->src[ctx->pos + 3];
  return nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r';
}

// True at a byte-order mark in column 0. Inside a document it ends the block
// structure like a marker, and the stream loop accepts it only in a document
// prefix.
static inline bool at_col0_bom(parse_ctx *ctx) {
  return ctx->len - ctx->pos >= 3 &&
         (unsigned char)ctx->src[ctx->pos] == 0xEF &&
         (unsigned char)ctx->src[ctx->pos + 1] == 0xBB &&
         (unsigned char)ctx->src[ctx->pos + 2] == 0xBF && current_col(ctx) == 0;
}

// Creates the anchor table on first use. The table points names at the anchored
// nodes, which stay where the document put them, so registering copies nothing;
// an alias resolves to a deep clone, charged to the amplification budget. The
// table owns no node: Each one lives in a tree or in the kept list until
// anchors_destroy(), which runs at the end of every document.
static bool anchors_ensure(parse_ctx *ctx) {
  if (ctx->anchors) return true;
  char *err = NULL;
  ctx->anchors = chashmap_create_mp(CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                    ccol_string, ccol_pointer, ctx->mp, &err);
  if (!ctx->anchors) return false;
  parse_discard_ctx = ctx;
  return true;
}

// Reserves one more kept-list slot for a node anchors_store() registers, so
// parse_keep() never allocates. false on OOM.
static bool anchors_reserve_kept(parse_ctx *ctx) {
  if (ctx->kept_reserved == ctx->kept_cap) {
    size_t new_cap = ctx->kept_cap ? ctx->kept_cap * 2 : 16;
    cyaml_node **grown =
        ccol__mem_realloc(ctx->mp, ctx->kept, new_cap * sizeof(*grown));
    if (!grown) return false;
    ctx->kept = grown;
    ctx->kept_cap = new_cap;
  }
  ctx->kept_reserved++;
  return true;
}

// Appends an entry for name (old is NULL) to the running speculation's log, so
// anchors_store() can record a change a rejection must undo. The entry stays
// valid until the next push; NULL on OOM.
static anchor_undo_entry *anchor_undo_push(parse_ctx *ctx, const char *name) {
  anchor_undo_log *u = ctx->anchor_undo;
  if (u->len == u->cap) {
    size_t new_cap = u->cap * 2;
    anchor_undo_entry *grown;
    if (u->entries == u->inline_entries) {
      grown = ccol__mem_alloc(ctx->mp, new_cap * sizeof(*grown));
      if (grown) memcpy(grown, u->entries, u->len * sizeof(*grown));
    } else {
      grown = ccol__mem_realloc(ctx->mp, u->entries, new_cap * sizeof(*grown));
    }
    if (!grown) return NULL;
    u->entries = grown;
    u->cap = new_cap;
  }
  char *copy = ccol_strdup(ctx->mp, name);
  if (!copy) return NULL;
  anchor_undo_entry *e = &u->entries[u->len++];
  e->name = copy;
  e->old = NULL;
  return e;
}

// Points name at node with one hash and one probe, marks the node anchored and
// reserves a kept slot; a later definition of the name wins. While a
// speculation runs, the change and the previous node go into its log. On OOM it
// sets a parse error (so a later alias doesn't fail with a misleading "unknown
// alias") and gives false, and the caller keeps node.
static bool anchors_store(parse_ctx *ctx, const char *name, cyaml_node *node) {
  if (!anchors_ensure(ctx) || !anchors_reserve_kept(ctx)) {
    parse_err(ctx, "out of memory registering anchor '%s' at position %zu",
              cyaml_echo(name), ctx->pos);
    return false;
  }
  anchor_undo_entry *undo = NULL;
  if (ctx->anchor_undo) {
    undo = anchor_undo_push(ctx, name);
    if (!undo) {
      parse_err(ctx, "out of memory registering anchor '%s' at position %zu",
                cyaml_echo(name), ctx->pos);
      return false;
    }
  }
  cmap_pair kp = {.ptr = (void *)name, .size = strlen(name) + 1};
  cmap_pair vp = {.ptr = &node, .size = sizeof(node)};
  cmap_pair old_vp;
  ccol_retval ins =
      ccol_chmap_insert_or_get_elem(ctx->anchors, &kp, &vp, &old_vp);
  if (ins == ccol_success) {
    node->anchored = true;
    return true;
  }
  if (ins == ccol_key_already_present) {
    if (undo) undo->old = cyaml_read_child(old_vp.ptr);
    memcpy(old_vp.ptr, &node, sizeof(node));
    node->anchored = true;
    return true;
  }
  if (undo) {
    ctx->anchor_undo->len--;
    ccol__mem_free(ctx->mp, undo->name);
  }
  parse_err(ctx, "out of memory registering anchor '%s' at position %zu",
            cyaml_echo(name), ctx->pos);
  return false;
}

// The one speculative parse is a flow collection that may be an implicit block
// key (see try_parse_scalar_dict_key_body()). Its side effects must vanish when
// it's rejected: Stored anchors, kept nodes and budget charges. begin records
// the budgets and the kept length and starts an undo log; reject restores all
// of it without allocating, accept keeps it. A new side effect in a parse
// function needs an undo here too.
typedef struct {
  anchor_undo_log undo;
  anchor_undo_log *outer;
  size_t kept_len;
  size_t node_budget;
  size_t byte_budget;
  size_t amp_node_budget;
  size_t amp_byte_budget;
  bool node_exhausted;
  bool byte_exhausted;
  bool amp_limit_hit;
} anchor_speculation;

// Starts a speculation: Snapshots the budgets and the kept length into sp and
// installs a fresh undo log in ctx.
static void anchor_speculation_begin(parse_ctx *ctx, anchor_speculation *sp) {
  sp->undo.entries = sp->undo.inline_entries;
  sp->undo.len = 0;
  sp->undo.cap = ANCHOR_UNDO_INLINE;
  sp->outer = ctx->anchor_undo;
  sp->kept_len = ctx->kept_len;
  sp->node_budget = parse_node_budget;
  sp->byte_budget = parse_byte_budget;
  sp->amp_node_budget = parse_amp_node_budget;
  sp->amp_byte_budget = parse_amp_byte_budget;
  sp->node_exhausted = parse_node_budget_exhausted;
  sp->byte_exhausted = parse_byte_budget_exhausted;
  sp->amp_limit_hit = parse_amp_limit_hit;
  ctx->anchor_undo = &sp->undo;
}

// Ends a speculation, for both accept and reject: Frees the log's name copies
// and puts the outer log back.
static void anchor_speculation_end(parse_ctx *ctx, anchor_speculation *sp) {
  for (size_t i = 0; i < sp->undo.len; i++)
    ccol__mem_free(ctx->mp, sp->undo.entries[i].name);
  if (sp->undo.entries != sp->undo.inline_entries)
    ccol__mem_free(ctx->mp, sp->undo.entries);
  ctx->anchor_undo = sp->outer;
}

// Accepts a speculation: Ends it and keeps its changes, when the flow
// collection really was a key or the parse fails anyway.
static void anchor_speculation_accept(parse_ctx *ctx, anchor_speculation *sp) {
  anchor_speculation_end(ctx, sp);
}

// Rejects a speculation, so the second reading sees no trace of it: Undoes the
// stores in reverse order (a name the speculation added keeps a slot with NULL,
// so no map delete is needed), frees the nodes it kept and restores the
// budgets. The caller has already destroyed the speculative tree, so the
// restored slots are written but never read through.
static void anchor_speculation_reject(parse_ctx *ctx, anchor_speculation *sp) {
  for (size_t i = sp->undo.len; i-- > 0;) {
    anchor_undo_entry *e = &sp->undo.entries[i];
    cmap_pair kp = {.ptr = e->name, .size = strlen(e->name) + 1};
    cmap_pair vp;
    if (chashmap_get_elem_ref(ctx->anchors, &kp, &vp) == ccol_success)
      memcpy(vp.ptr, &e->old, sizeof(e->old));
  }
  while (ctx->kept_len > sp->kept_len)
    cyaml__destroy((cyaml)ctx->kept[--ctx->kept_len]);
  parse_node_budget = sp->node_budget;
  parse_byte_budget = sp->byte_budget;
  parse_amp_node_budget = sp->amp_node_budget;
  parse_amp_byte_budget = sp->amp_byte_budget;
  parse_node_budget_exhausted = sp->node_exhausted;
  parse_byte_budget_exhausted = sp->byte_exhausted;
  parse_amp_limit_hit = sp->amp_limit_hit;
  anchor_speculation_end(ctx, sp);
}

// Gives the node anchored as name, so an alias can copy it, or NULL; a slot
// holding NULL was taken back by a rejected speculation.
static cyaml_node *anchors_lookup(parse_ctx *ctx, const char *name) {
  if (!ctx->anchors) return NULL;
  cmap_pair kp = {.ptr = (void *)name, .size = strlen(name) + 1};
  cmap_pair vp;
  if (chashmap_get_elem_ref(ctx->anchors, &kp, &vp) != ccol_success)
    return NULL;
  return cyaml_read_child(vp.ptr);
}

// Frees the anchor table and every node of the kept list at the end of each
// document, since anchors don't cross documents. After a failed parse the map
// may name freed nodes, which nothing reads.
static void anchors_destroy(parse_ctx *ctx) {
  if (parse_discard_ctx == ctx) parse_discard_ctx = NULL;
  for (size_t i = 0; i < ctx->kept_len; i++)
    cyaml__destroy((cyaml)ctx->kept[i]);
  ccol__mem_free(ctx->mp, ctx->kept);
  ctx->kept = NULL;
  ctx->kept_len = 0;
  ctx->kept_cap = 0;
  ctx->kept_reserved = 0;
  if (!ctx->anchors) return;
  chashmap__destroy(ctx->anchors);
  ctx->anchors = NULL;
}

// Creates the %TAG table on first use. It lives as long as the anchor table and
// starts empty, since the lookup falls back to the defaults of "!" and "!!"
// itself, so the insert's "already present" answer spots a duplicate %TAG in
// one document.
static bool tag_handles_ensure(parse_ctx *ctx) {
  if (ctx->tag_handles) return true;
  char *err = NULL;
  ctx->tag_handles =
      chashmap_create_mp(CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                         ccol_string, ctx->mp, &err);
  return ctx->tag_handles != NULL;
}

// Sets the prefix of a %TAG handle for the directive parser: ccol_success on a
// first definition, ccol_key_already_present for a handle the document already
// defined, anything else on OOM.
static ccol_retval tag_handles_set(parse_ctx *ctx, const char *handle,
                                   const char *prefix) {
  if (!tag_handles_ensure(ctx)) return ccol_not_enough_memory;
  cmap_pair kp = {.ptr = (void *)handle, .size = strlen(handle) + 1};
  cmap_pair vp = {.ptr = (void *)prefix, .size = strlen(prefix) + 1};
  return chashmap_insert_elem(ctx->tag_handles, &kp, &vp);
}

// Gives the prefix of handle, to resolve a shorthand tag: A %TAG definition,
// else the default for "!" or "!!", else NULL.
static const char *tag_handles_lookup(parse_ctx *ctx, const char *handle) {
  if (ctx->tag_handles) {
    cmap_pair kp = {.ptr = (void *)handle, .size = strlen(handle) + 1};
    cmap_pair vp;
    if (chashmap_get_elem_ref(ctx->tag_handles, &kp, &vp) == ccol_success)
      return (const char *)vp.ptr;
  }
  if (strcmp(handle, "!") == 0) return "!";
  if (strcmp(handle, "!!") == 0) return "tag:yaml.org,2002:";
  return NULL;
}

// Frees the %TAG table at the end of each document, since %TAG doesn't cross
// documents.
static void tag_handles_destroy(parse_ctx *ctx) {
  if (!ctx->tag_handles) return;
  chashmap__destroy(ctx->tag_handles);
  ctx->tag_handles = NULL;
}

// Reads an anchor or alias name after '&' or '*' into *name_out: Everything up
// to whitespace, a flow indicator or '#'. A ':' followed by whitespace ends the
// name too, so "*x: y" uses alias x as a key. On an empty name or OOM it sets
// ctx->error and gives false; the speculative key parse relies on ctx->error to
// tell a real error from "not a key".
static bool parse_anchor_name(parse_ctx *ctx, char **name_out) {
  size_t start = ctx->pos;
  while (!at_end(ctx)) {
    char c = cur(ctx);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' ||
        c == '[' || c == ']' || c == '{' || c == '}' || c == '#')
      break;
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
  char *name = ccol__mem_alloc(ctx->mp, len + 1);
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

// Tells whether s is a core-schema bool (true/True/TRUE, false/False/FALSE) and
// stores it in *out. The type tests are separate functions so an explicit tag
// can force one of them. Each dispatches on the first byte before any
// comparison, because nearly every scalar must be ruled out and a strcmp chain
// costs a call per spelling.
static bool try_parse_bool_scalar(const char *s, bool *out) {
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

// Tells whether s is a core-schema int and stores it in *out, for implicit
// typing and !!int: Decimal with an optional sign, or unsigned 0x hex or 0o
// octal with a lowercase prefix, at most LLONG_MAX. The first byte must be a
// digit or a sign, because strtoll() skips leading whitespace that an escape
// can carry. The digits after 0x or 0o are checked by hand, since strtoull()
// accepts a sign there.
CYAML_PARSE_HOT bool try_parse_int_scalar(const char *s, long long *out) {
  const unsigned char first = (unsigned char)s[0];
  if (!((unsigned)(first - '0') < 10u || first == '+' || first == '-'))
    return false;

  char *endp = NULL;
  long long ival = 0;
  errno = 0;

  const char *p = s;
  if (p[0] == '0' && p[1] == 'x') {
    const char *digits = p + 2;
    if (*digits == '\0') return false;
    for (const char *d = digits; *d; d++) {
      bool is_hex_digit = (*d >= '0' && *d <= '9') ||
                          (*d >= 'a' && *d <= 'f') || (*d >= 'A' && *d <= 'F');
      if (!is_hex_digit) return false;
    }
    unsigned long long uval = strtoull(digits, &endp, 16);
    if (endp == digits || *endp != '\0' || errno == ERANGE) return false;
    if (uval > (unsigned long long)LLONG_MAX) return false;
    ival = (long long)uval;
  } else if (p[0] == '0' && p[1] == 'o') {
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
    ival = strtoll(s, &endp, 10);
    if (endp == s || *endp != '\0' || errno == ERANGE) return false;
  }
  *out = ival;
  return true;
}

// Tells whether s is a core-schema float and stores it in *out, for implicit
// typing and !!float: .inf, -.inf and .nan included, but not "nan", "inf",
// "nan(...)" or any 0x/0o text, which strtod() would accept. The first byte
// must be a digit, a sign or '.'. Overflow gives +-inf and underflow a
// subnormal or 0, as in PyYAML. Callers run in the C locale.
CYAML_PARSE_HOT bool try_parse_float_scalar(const char *s, double *out) {
  const unsigned char first = (unsigned char)s[0];
  if (!((unsigned)(first - '0') < 10u || first == '+' || first == '-' ||
        first == '.'))
    return false;

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
  const char qc = *q;
  if (qc == 'n' || qc == 'N' || qc == 'i' || qc == 'I') {
    if (ccol_ascii_casecmp(q, "nan") == 0 ||
        ccol_ascii_casecmp(q, "inf") == 0 ||
        ccol_ascii_casecmp(q, "infinity") == 0)
      return false;
    if (ccol_ascii_ncasecmp(q, "nan(", 4) == 0) return false;
  }

  if (q[0] == '0' && (q[1] == 'x' || q[1] == 'o' || q[1] == 'X' || q[1] == 'O'))
    return false;

  char *endp = NULL;
  double dval = strtod(s, &endp);
  if (endp == s || *endp != '\0') return false;
  *out = dval;
  return true;
}

// True for "", "~" and the core-schema spellings of null, the first test of
// implicit typing.
static bool is_null_scalar_text(const char *s) {
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

// Makes the node of an untagged plain scalar by core-schema typing (null, bool,
// int, float, else a string); the parser and every plain key go through it.
// NULL on OOM.
static cyaml_node *make_typed_scalar(parse_ctx *ctx, const char *s) {
  ccol_memmgmt_procs *mp = ctx->mp;

  if (is_null_scalar_text(s)) return node_alloc(CYAML_NULL, mp);

  bool bval;
  if (try_parse_bool_scalar(s, &bval)) {
    cyaml_node *n = node_alloc(CYAML_BOOL, mp);
    if (n) n->value.boolean = bval;
    return n;
  }

  long long ival;
  if (try_parse_int_scalar(s, &ival)) {
    cyaml_node *n = node_alloc(CYAML_INTEGER, mp);
    if (n) n->value.integer = ival;
    return n;
  }

  double dval;
  if (try_parse_float_scalar(s, &dval)) {
    cyaml_node *n = node_alloc(CYAML_FLOAT, mp);
    if (n) n->value.number = dval;
    return n;
  }

  cyaml_node *n = node_alloc(CYAML_STRING, mp);
  if (!n) return NULL;
  n->value.string = strdup_charged(mp, s);
  if (!n->value.string) {
    node_free(n);
    return NULL;
  }
  return n;
}

// Removes trailing whitespace and newlines before !!int or !!float parses the
// text, so chomped block scalars match, as PyYAML accepts them. Leading
// whitespace stays and !!bool doesn't trim, again as PyYAML does.
static void trim_trailing_ws_for_numeric_tag(char *text) {
  size_t len = strlen(text);
  while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
                     text[len - 1] == ' ' || text[len - 1] == '\t'))
    len--;
  text[len] = '\0';
}

// Tells whether s is a word that !!bool accepts and stores it in *out. That's
// PyYAML's set: true, false, yes, no, on and off in any case, but not y or n.
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

// Makes a string node that takes over *text from a scanner buffer, for quoted
// and block scalars (*text is NULL on success). The text is shrunk to its
// length and that length is charged to the byte budget, so a short scalar
// doesn't keep a 256-byte buffer. NULL on OOM or a refused charge, without
// setting ctx->error.
static cyaml_node *string_node_adopting_text(parse_ctx *ctx, char **text) {
  size_t size = strlen(*text) + 1;
  size_t charge = size + CYAML_ALLOC_OVERHEAD;
  if (!parse_bytes_charge(charge)) return NULL;
  cyaml_node *n = node_alloc(CYAML_STRING, ctx->mp);
  if (!n) {
    parse_bytes_refund(charge);
    return NULL;
  }
  char *fit = ccol__mem_realloc(ctx->mp, *text, size);
  n->value.string = fit ? fit : *text;
  *text = NULL;
  return n;
}

// Makes the node of a scalar from its text, every scalar style's last step, and
// takes ownership of text. resolved_tag (NULL for none) is borrowed; a core tag
// forces and checks the type, "!" gives a string with no tag, and a custom tag
// is attached to whatever implicit_ok allows (implicit typing for plain scalars
// only). NULL on OOM with ctx->error untouched, or with ctx->error set when tag
// and text disagree.
static cyaml_node *finalize_scalar_node(parse_ctx *ctx, char *text,
                                        const char *resolved_tag,
                                        bool implicit_ok) {
  cyaml_node *n = NULL;

  if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_NON_SPECIFIC) == 0) {
    n = string_node_adopting_text(ctx, &text);
    ccol__mem_free(ctx->mp, text);
    return n;
  }
  if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_NULL) == 0) {
    n = node_alloc(CYAML_NULL, ctx->mp);
  } else if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_BOOL) == 0) {
    bool bval;
    if (!try_parse_bool_scalar_explicit(text, &bval)) {
      parse_err(ctx, "'%s' is not a valid !!bool value", cyaml_echo(text));
      ccol__mem_free(ctx->mp, text);
      return NULL;
    }
    n = node_alloc(CYAML_BOOL, ctx->mp);
    if (n) n->value.boolean = bval;
  } else if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_INT) == 0) {
    trim_trailing_ws_for_numeric_tag(text);
    long long ival;
    if (!try_parse_int_scalar(text, &ival)) {
      parse_err(ctx, "'%s' is not a valid !!int value", cyaml_echo(text));
      ccol__mem_free(ctx->mp, text);
      return NULL;
    }
    n = node_alloc(CYAML_INTEGER, ctx->mp);
    if (n) n->value.integer = ival;
  } else if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_FLOAT) == 0) {
    trim_trailing_ws_for_numeric_tag(text);
    double dval;
    if (!try_parse_float_scalar(text, &dval)) {
      parse_err(ctx, "'%s' is not a valid !!float value", cyaml_echo(text));
      ccol__mem_free(ctx->mp, text);
      return NULL;
    }
    n = node_alloc(CYAML_FLOAT, ctx->mp);
    if (n) n->value.number = dval;
  } else if (resolved_tag && strcmp(resolved_tag, CYAML_TAG_STR) == 0) {
    n = string_node_adopting_text(ctx, &text);
  } else if (resolved_tag && (strcmp(resolved_tag, CYAML_TAG_SEQ) == 0 ||
                              strcmp(resolved_tag, CYAML_TAG_MAP) == 0)) {
    parse_err(ctx, "tag '%s' does not match the node it decorates",
              cyaml_echo(resolved_tag));
    ccol__mem_free(ctx->mp, text);
    return NULL;
  } else if (implicit_ok) {
    n = make_typed_scalar(ctx, text);
  } else {
    n = string_node_adopting_text(ctx, &text);
  }

  ccol__mem_free(ctx->mp, text);

  if (!n) return NULL;

  if (resolved_tag) {
    char *tag_copy = strdup_charged(ctx->mp, resolved_tag);
    if (!tag_copy) {
      cyaml__destroy((cyaml)n);
      return NULL;
    }
    n->tag = tag_copy;
  }

  return n;
}

// Checks resolved_tag (borrowed) against a finished collection and attaches a
// copy, since a collection's type comes from its syntax and a tag can only
// agree with it. A scalar core tag, or !!seq/!!map on the wrong kind, is a
// parse error. Each builder calls it the moment it builds the collection, so an
// anchor above registers the tagged node. Destroys coll and gives NULL on
// failure; "!" and NULL attach nothing.
static cyaml_node *finalize_collection_node(parse_ctx *ctx, cyaml_node *coll,
                                            const char *resolved_tag) {
  if (!resolved_tag || !coll) return coll;
  if (strcmp(resolved_tag, CYAML_TAG_NON_SPECIFIC) == 0) return coll;

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
              cyaml_echo(resolved_tag), ctx->pos);
    cyaml__destroy((cyaml)coll);
    return NULL;
  }

  char *tag_copy = strdup_charged(ctx->mp, resolved_tag);
  if (!tag_copy) {
    cyaml__destroy((cyaml)coll);
    return NULL;
  }
  coll->tag = tag_copy;
  return coll;
}

// Appends cp to b as UTF-8, for the \x, \u and \U escapes of double-quoted
// scalars.
static void encode_utf8(ybuf *b, uint32_t cp) {
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

// Reads the 4 hex digits of a \u escape into a code point and moves past them;
// false on short input or a non-hex digit.
static bool parse_hex4(parse_ctx *ctx, uint32_t *out) {
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
                cyaml_echo_c(c), ctx->pos + i);
      return false;
    }
    v = (v << 4) | d;
  }
  ctx->pos += 4;
  *out = v;
  return true;
}

// Reads the 8 hex digits of a \U escape into a code point, like parse_hex4().
static bool parse_hex8(parse_ctx *ctx, uint32_t *out) {
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
                cyaml_echo_c(c), ctx->pos + i);
      return false;
    }
    v = (v << 4) | d;
  }
  ctx->pos += 8;
  *out = v;
  return true;
}

// Folds a line break inside a quoted scalar, for both quoted styles. It strips
// trailing inline whitespace of b down to strip_floor (the double-quoted style
// passes the end of its last escape, since escaped whitespace is content), then
// folds the break into a space, or into newlines for blank lines that follow. A
// document marker in the fold is an unclosed quote: false with the error set,
// and the caller frees b.
static bool fold_quoted_newline(parse_ctx *ctx, ybuf *b, size_t strip_floor,
                                const char *style_name) {
  yb_trim_trailing_inline_ws(b, strip_floor);
  skip_newline(ctx);
  if (at_doc_marker(ctx)) {
    parse_err(ctx,
              "document marker inside unterminated %s scalar at position %zu",
              style_name, ctx->pos);
    return false;
  }
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

// Parses a double-quoted scalar from its opening quote into a heap string in
// *out, for values and keys. Escapes that decode to U+0000, a lone surrogate or
// a value outside Unicode are refused, since a value can't hold a NUL. An
// escaped line break adds nothing itself, and each blank line after it adds a
// newline. false with ctx->error set on a malformed scalar.
static bool parse_double_quoted(parse_ctx *ctx, char **out) {
  if (at_end(ctx) || cur(ctx) != '"') {
    parse_err(ctx, "expected '\"' at position %zu", ctx->pos);
    return false;
  }
  ctx->pos++;

  ybuf b;
  yb_init(&b, ctx->mp);

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
        case 'N':
          yb_append(&b, "\xC2\x85", 2);
          break;
        case '_':
          yb_append(&b, "\xC2\xA0", 2);
          break;
        case 'L':
          yb_append(&b, "\xE2\x80\xA8", 3);
          break;
        case 'P':
          yb_append(&b, "\xE2\x80\xA9", 3);
          break;
        case 'x': {
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
          if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            parse_err(ctx,
                      "\\U escape at position %zu names 0x%08X, which is "
                      "not a Unicode scalar value (a surrogate, or above "
                      "U+10FFFF)",
                      esc_pos, (unsigned)cp);
            goto fail;
          }
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
                    cyaml_echo_c(esc), ctx->pos - 1);
          goto fail;
      }
      escape_boundary = b.len;
      continue;
    }

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
  ccol__mem_free(b.m_procs, b.buf);
  return false;
}

// Parses a single-quoted scalar ('' is a quote; line breaks fold as in the
// double-quoted style) into a heap string in *out, for values and keys. false
// with ctx->error set when unterminated.
static bool parse_single_quoted(parse_ctx *ctx, char **out) {
  if (at_end(ctx) || cur(ctx) != '\'') {
    parse_err(ctx, "expected \"'\" at position %zu", ctx->pos);
    return false;
  }
  ctx->pos++;

  ybuf b;
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

    if (c == '\r' || c == '\n') {
      if (!fold_quoted_newline(ctx, &b, 0, "single-quoted")) {
        ccol__mem_free(b.m_procs, b.buf);
        return false;
      }
      continue;
    }

    yb_append_c(&b, c);
    ctx->pos++;
  }

  parse_err(ctx, "unterminated single-quoted scalar");
  ccol__mem_free(b.m_procs, b.buf);
  return false;

done:
  if (b.oom) {
    ccol__mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

// The chomping of a block scalar: One final newline, none, or every trailing
// line.
typedef enum { CHOMP_CLIP, CHOMP_STRIP, CHOMP_KEEP } chomp_mode;

// Reads the header of a block scalar after | or >, for both block styles: At
// most one chomp indicator and at most one indent digit (1 to 9), in either
// order, then an optional comment that needs whitespace before its '#'. Sets
// *chomp, and *explicit_indent (0 when no digit is given). false with
// ctx->error set when malformed.
static bool parse_block_scalar_header(parse_ctx *ctx, chomp_mode *chomp,
                                      int *explicit_indent) {
  *chomp = CHOMP_CLIP;
  *explicit_indent = 0;

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

// What scan_block_scalar_line() found. ERROR: ctx->error is set. END: The
// scalar ends before this line, and ctx->pos is back at its start. BLANK: An
// empty line, ctx->pos at its end. CONTENT: A content line, *spaces_out holds
// its leading spaces (at least block_indent) and ctx->pos sits after them.
typedef enum {
  SCAN_BLOCK_LINE_ERROR,
  SCAN_BLOCK_LINE_END,
  SCAN_BLOCK_LINE_BLANK,
  SCAN_BLOCK_LINE_CONTENT
} scan_block_line;

// Measures and classifies one line of a block scalar for both block styles,
// updating indent_determined, block_indent and max_leading_blank_spaces. The
// shared rules live here: A tab as indentation while the indent is unknown is
// an error, no leading blank line may be indented past the first content line,
// the indent is detected from the first content line, and a document marker in
// column 0 always ends the scalar (its auto-detected indent may be 0). A line
// more indented than block_indent is content even when it holds only spaces.
static scan_block_line scan_block_scalar_line(parse_ctx *ctx, int parent_indent,
                                              bool *indent_determined,
                                              int *block_indent,
                                              int *max_leading_blank_spaces,
                                              int *spaces_out) {
  size_t line_start = ctx->pos;
  size_t space_count = 0;
  while (!at_end(ctx) && cur(ctx) == ' ') {
    space_count++;
    ctx->pos++;
  }
  int spaces = space_count > (size_t)INT_MAX ? INT_MAX : (int)space_count;

  if (!*indent_determined && spaces == 0 && !at_end(ctx) && cur(ctx) == '\t') {
    parse_err(ctx,
              "tab cannot be used as block scalar indentation at "
              "position %zu",
              ctx->pos);
    return SCAN_BLOCK_LINE_ERROR;
  }

  if (spaces == 0 && at_doc_marker(ctx)) {
    ctx->pos = line_start;
    return SCAN_BLOCK_LINE_END;
  }

  bool is_blank =
      at_eol(ctx) && (!*indent_determined || spaces <= *block_indent);

  if (is_blank) {
    if (!*indent_determined && spaces > *max_leading_blank_spaces)
      *max_leading_blank_spaces = spaces;
    return SCAN_BLOCK_LINE_BLANK;
  }

  if (!*indent_determined) {
    if (spaces <= parent_indent) {
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
    ctx->pos = line_start;
    return SCAN_BLOCK_LINE_END;
  }
  *spaces_out = spaces;
  return SCAN_BLOCK_LINE_CONTENT;
}

// Reads the content of a literal block scalar into a heap string in *out (free
// it with ctx->mp). parent_indent (-1 at the root) is the base of an explicit
// indent. Trailing lines and the newline of the last content line wait in a
// buffer until more content arrives or the scalar ends, so the chomp mode can
// decide about them; CLIP and KEEP never add a newline the input didn't have.
static bool parse_block_scalar_content(parse_ctx *ctx, int parent_indent,
                                       chomp_mode chomp, int explicit_indent,
                                       char **out) {
  int block_indent = 0;
  bool indent_determined = (explicit_indent > 0);
  if (indent_determined) {
    int effective_parent = parent_indent < 0 ? 0 : parent_indent;
    block_indent = effective_parent + explicit_indent;
  }

  ybuf b;
  yb_init(&b, ctx->mp);

  ybuf pending;
  yb_init(&pending, ctx->mp);
  bool have_content = false;

  bool had_trailing_newline = false;

  bool content_had_trailing_newline = false;

  int max_leading_blank_spaces = -1;

  while (!at_end(ctx)) {
    size_t line_start = ctx->pos;
    int spaces = 0;
    scan_block_line kind = scan_block_scalar_line(
        ctx, parent_indent, &indent_determined, &block_indent,
        &max_leading_blank_spaces, &spaces);

    if (kind == SCAN_BLOCK_LINE_ERROR) {
      ccol__mem_free(b.m_procs, b.buf);
      ccol__mem_free(pending.m_procs, pending.buf);
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

    yb_append_buf(&b, &pending);
    pending.len = 0;
    have_content = true;

    int extra = spaces - block_indent;

    for (int i = 0; i < extra; i++) yb_append_c(&b, ' ');

    while (!at_end(ctx) && !at_eol(ctx)) {
      yb_append_c(&b, cur(ctx));
      ctx->pos++;
    }

    had_trailing_newline = !at_end(ctx);
    content_had_trailing_newline = had_trailing_newline;
    if (had_trailing_newline) yb_append_c(&pending, '\n');
    skip_newline(ctx);
  }

  switch (chomp) {
    case CHOMP_STRIP:
      break;
    case CHOMP_CLIP:
      if (have_content && content_had_trailing_newline) yb_append_c(&b, '\n');
      break;
    case CHOMP_KEEP:
      yb_append_buf(&b, &pending);
      break;
  }

  bool pending_oom = pending.oom;
  ccol__mem_free(pending.m_procs, pending.buf);

  if (b.oom || pending_oom) {
    ccol__mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

// Reads the content of a folded block scalar: It collects the lines as spans
// borrowed from ctx->src (no copy per line), then folds them. A single break
// between two lines becomes a space, a blank line a newline, and a break next
// to a more indented line (spaces or a tab past block_indent) stays a newline,
// plus one more when entering or leaving such a run. Chomping works as in
// parse_block_scalar_content().
static bool parse_folded_scalar_content(parse_ctx *ctx, int parent_indent,
                                        chomp_mode chomp, int explicit_indent,
                                        char **out) {
  int block_indent = 0;
  bool indent_determined = (explicit_indent > 0);
  if (indent_determined) {
    int effective_parent = parent_indent < 0 ? 0 : parent_indent;
    block_indent = effective_parent + explicit_indent;
  }

  typedef struct {
    const char *text;
    size_t text_len;
    int extra;
    bool blank;
    bool spaced;
  } block_line;

  size_t lines_cap = 16, lines_len = 0;
  block_line *lines = ccol__mem_alloc(ctx->mp, lines_cap * sizeof(block_line));
  if (!lines) return false;

  int max_leading_blank_spaces = -1;

  bool had_trailing_newline = false;

  while (!at_end(ctx)) {
    size_t line_start = ctx->pos;
    int spaces = 0;
    scan_block_line kind = scan_block_scalar_line(
        ctx, parent_indent, &indent_determined, &block_indent,
        &max_leading_blank_spaces, &spaces);

    if (kind == SCAN_BLOCK_LINE_ERROR) {
      ccol__mem_free(ctx->mp, lines);
      return false;
    }
    if (kind == SCAN_BLOCK_LINE_END) break;

    bool is_blank = (kind == SCAN_BLOCK_LINE_BLANK);

    if (lines_len == lines_cap) {
      lines_cap *= 2;
      block_line *tmp =
          ccol__mem_realloc(ctx->mp, lines, lines_cap * sizeof(block_line));
      if (!tmp) {
        ccol__mem_free(ctx->mp, lines);
        return false;
      }
      lines = tmp;
    }

    block_line *ln = &lines[lines_len++];
    ln->blank = is_blank;
    ln->extra = is_blank ? 0 : spaces - block_indent;
    ln->spaced =
        !is_blank && (ln->extra > 0 || (!at_end(ctx) && cur(ctx) == '\t'));
    ln->text = NULL;
    ln->text_len = 0;

    if (!is_blank) {
      const char *span_start = ctx->src + (ctx->pos - (size_t)ln->extra);
      while (!at_end(ctx) && !at_eol(ctx)) ctx->pos++;
      ln->text = span_start;
      ln->text_len = (size_t)((ctx->src + ctx->pos) - span_start);
    }
    had_trailing_newline = !at_end(ctx);
    ctx->pos = line_start;
    skip_to_eol(ctx);
  }

  ybuf b;
  yb_init(&b, ctx->mp);

  size_t trailing_blanks = 0;
  bool have_content = false;
  bool prev_extra = false;

  for (size_t i = 0; i < lines_len; i++) {
    block_line *ln = &lines[i];
    if (ln->blank) {
      trailing_blanks++;
      continue;
    }

    if (trailing_blanks > 0) {
      for (size_t j = i - trailing_blanks; j < i; j++) {
        for (int k = 0; k < lines[j].extra; k++) yb_append_c(&b, ' ');
        yb_append_c(&b, '\n');
      }
      if (have_content && (ln->spaced || prev_extra)) yb_append_c(&b, '\n');
    } else if (have_content) {
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

  switch (chomp) {
    case CHOMP_STRIP:
      break;
    case CHOMP_CLIP:
      if (have_content && (trailing_blanks > 0 || had_trailing_newline))
        yb_append_c(&b, '\n');
      break;
    case CHOMP_KEEP:
      if (have_content && (trailing_blanks > 0 || had_trailing_newline))
        yb_append_c(&b, '\n');
      for (size_t j = lines_len - trailing_blanks; j < lines_len; j++) {
        for (int k = 0; k < lines[j].extra; k++) yb_append_c(&b, ' ');
        if (j + 1 < lines_len || had_trailing_newline) yb_append_c(&b, '\n');
      }
      break;
  }

  ccol__mem_free(ctx->mp, lines);

  if (b.oom) {
    ccol__mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

// Appends the plain-scalar text of the current line to *b, shared by the first
// line and every continuation line so both end the same way. true, with
// ctx->pos at the stopper, when a real terminator ends the token (" #", ": ", a
// ':' at the end, or in flow a ':' before a flow indicator, or any flow
// indicator); false at the end of the line or the input. line_start lets a '#'
// in the first position stay content.
CYAML_PARSE_HOT bool scan_plain_scalar_line(parse_ctx *ctx, bool in_flow,
                                            size_t line_start, ybuf *b) {
  while (!at_end(ctx)) {
    char c = cur(ctx);

    if (c == '\n' || c == '\r') return false;

    if (c == '#' && ctx->pos > line_start) {
      char prev = ctx->src[ctx->pos - 1];
      if (prev == ' ' || prev == '\t') return true;
    }

    if (c == ':') {
      if (ctx->pos + 1 >= ctx->len) return true;
      char next = ctx->src[ctx->pos + 1];
      if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
        return true;
      if (in_flow) {
        char safe = ctx->src[ctx->pos + 1];
        if (safe == ',' || safe == ']' || safe == '}' || safe == '[' ||
            safe == '{')
          return true;
      }
    }

    if (in_flow && (c == ',' || c == ']' || c == '}' || c == '[' || c == '{'))
      return true;

    yb_append_c(b, c);
    ctx->pos++;
  }
  return false;
}

// Parses a plain scalar on one line, trailing spaces trimmed. A ':' not
// followed by whitespace (or, in flow, by an indicator) is content, as in a
// URL. *hit_real_terminator (when not NULL) tells whether a terminator ended
// it; only then is no continuation tried. An implicit key never continues.
CYAML_PARSE_HOT bool parse_plain_scalar(parse_ctx *ctx, bool in_flow,
                                        char **out, bool *hit_real_terminator) {
  ybuf b;
  yb_init(&b, ctx->mp);

  size_t start = ctx->pos;
  bool terminated = scan_plain_scalar_line(ctx, in_flow, start, &b);
  if (hit_real_terminator) *hit_real_terminator = terminated;

  yb_trim_trailing_inline_ws(&b, 0);

  if (b.oom) {
    ccol__mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

// Extends the first line of a plain scalar (first_owned, owned from here on)
// with continuation lines: Blank lines and lines indented past indent. Each
// line step folds to one space and each blank line to a newline. It stops at a
// document marker, a line at or below indent, a flow indicator, a comment line,
// a ':' at the start of a line (an implicit key's ':' must stay on the key's
// line), and a line that hits a terminator. The text accumulates in one buffer,
// with a per-line floor for the trailing-space trim.
static bool parse_plain_scalar_multiline(parse_ctx *ctx, bool in_flow,
                                         int indent, char *first_owned,
                                         bool first_terminated, char **out) {
  if (first_terminated || at_end(ctx)) {
    *out = first_owned;
    return true;
  }

  ybuf b;
  yb_init(&b, ctx->mp);
  yb_append_cstr(&b, first_owned);
  ccol__mem_free(ctx->mp, first_owned);

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
    int spaces =
        line_space_count > (size_t)INT_MAX ? INT_MAX : (int)line_space_count;

    if (at_end(ctx)) {
      ctx->pos = save;
      break;
    }
    if (at_blank_line(ctx)) {
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
    while (!at_end(ctx) && (cur(ctx) == ' ' || cur(ctx) == '\t')) ctx->pos++;
    if (in_flow && (cur(ctx) == ']' || cur(ctx) == '}' || cur(ctx) == ',' ||
                    cur(ctx) == '[' || cur(ctx) == '{')) {
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
        ctx->pos = save;
        break;
      }
    }
    if (cur(ctx) == '#') {
      ctx->pos = save;
      break;
    }

    if (blank_count > 0) {
      for (size_t i = 0; i < blank_count; i++) yb_append_c(&b, '\n');
    } else {
      yb_append_c(&b, ' ');
    }
    blank_count = 0;

    size_t line_floor = b.len;
    bool cont_terminated = scan_plain_scalar_line(ctx, in_flow, line_start, &b);
    yb_trim_trailing_inline_ws(&b, line_floor);

    if (cont_terminated) break;
  }

  if (b.oom) {
    ccol__mem_free(b.m_procs, b.buf);
    return false;
  }
  *out = b.buf;
  return true;
}

static char *node_to_dict_key_string(parse_ctx *ctx, cyaml_node *key_node,
                                     const char *context_label);
static char *node_to_dict_key_string_ex(parse_ctx *ctx, cyaml_node *key_node,
                                        const char *context_label,
                                        bool consume);

static char *serialize_flow_canonical(cyaml_node *n, bool *too_long);

static cyaml_node *parse_flow_list(parse_ctx *ctx, int indent);
static cyaml_node *parse_flow_dictionary(parse_ctx *ctx, int indent);

// Tells whether the current position (after the leading whitespace of a line)
// continues the value of a block construct at indent: true when it's indented
// further. seq_ok_at_indent also allows exactly indent for a "- " entry, the
// one exception YAML 1.2 gives the value of a mapping key; pass it only for a
// mapping's indent. Spaces after a tab don't count.
static bool at_block_value_col(parse_ctx *ctx, int indent,
                               bool seq_ok_at_indent) {
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

// True at a '-' followed by whitespace or the end, so the parser can tell a
// block sequence entry from text such as "-1".
static bool at_bare_seq_indicator(parse_ctx *ctx) {
  if (at_end(ctx) || cur(ctx) != '-') return false;
  char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
  return nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0';
}

// True when the current character may start a flow plain scalar, so a lone "-"
// or "?" in flow is refused: Anything but a '-' or '?' that isn't followed by a
// character a flow plain scalar may hold. ':' is left out on purpose, so "[:
// value]" can still read an empty key.
static bool at_valid_flow_plain_scalar_start(parse_ctx *ctx) {
  char c = cur(ctx);
  if (c != '-' && c != '?') return true;
  if (ctx->pos + 1 >= ctx->len) return false;
  char next = ctx->src[ctx->pos + 1];
  return next != ' ' && next != '\t' && next != '\n' && next != '\r' &&
         next != ',' && next != '[' && next != ']' && next != '{' &&
         next != '}';
}

// Skips a tag token at a '!' the caller has seen, for look-aheads that only
// need to get past it: A shorthand ends at whitespace or a flow indicator, and
// a verbatim "!<...>" runs to its '>', commas included.
static void skip_tag_token(parse_ctx *ctx) {
  ctx->pos++;
  if (!at_end(ctx) && cur(ctx) == '<') {
    ctx->pos++;
    while (!at_end(ctx) && cur(ctx) != '>' && cur(ctx) != '\n' &&
           cur(ctx) != '\r')
      ctx->pos++;
    if (!at_end(ctx) && cur(ctx) == '>') ctx->pos++;
    return;
  }
  if (!at_end(ctx) && cur(ctx) == '!') ctx->pos++;
  while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
         cur(ctx) != '\n' && cur(ctx) != '\r' && cur(ctx) != ',' &&
         cur(ctx) != '[' && cur(ctx) != ']' && cur(ctx) != '{' &&
         cur(ctx) != '}')
    ctx->pos++;
}

// True when c may appear unescaped in tag text, so the parser can refuse what
// YAML forbids: ns-uri-char (letters, digits and "-#;/?:@&=+$,_.!~*'()[]%") for
// verbatim content and %TAG prefixes, and with tag_char set the ns-tag-char
// subset (no '!', ',', '[' or ']') for a shorthand suffix. Anything else needs
// a percent escape.
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

// Checks tag text for parse_tag_token_raw() and the %TAG parser: Refuses, as a
// parse error naming it, the first byte that tag_text_char_ok() refuses (start
// is text's offset in the input). true when every byte is allowed.
static bool tag_text_check(parse_ctx *ctx, const char *text, size_t len,
                           size_t start, bool tag_char) {
  for (size_t i = 0; i < len; i++) {
    if (!tag_text_char_ok((unsigned char)text[i], tag_char)) {
      parse_err(ctx,
                "character '%s' at position %zu is not allowed in a tag; "
                "write it as a percent escape",
                cyaml_echo_c(text[i]), start + i);
      return false;
    }
  }
  return true;
}

// Decodes the %XX escapes of tag text in place, for verbatim tags, shorthand
// suffixes and %TAG prefixes alike. A malformed escape, %00 (a tag can't hold a
// NUL) and escapes that decode to ill-formed UTF-8 are parse errors.
static bool percent_decode_tag_inplace(parse_ctx *ctx, char *text,
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
  if (decoded_non_ascii && !ccol_utf8_is_valid(text, (size_t)(w - text))) {
    parse_err(ctx,
              "tag percent-escapes at position %zu decode to bytes that are "
              "not well-formed UTF-8",
              pos_for_err);
    return false;
  }
  return true;
}

// Parses and resolves a tag token at a '!': Verbatim "!<...>", or a shorthand
// with the "!", "!!" or "!name!" handle (a named handle is confirmed by a look
// ahead for its closing '!'). *resolved_out gets the owned resolved tag, or
// NULL for a bare "!". Every failure (bad character, handle with no suffix,
// undefined handle, bad escape, OOM) sets ctx->error, which the speculative key
// parse relies on. A stored tag is never empty.
static bool parse_tag_token_raw(parse_ctx *ctx, char **resolved_out) {
  size_t tag_start = ctx->pos;
  ctx->pos++;

  if (!at_end(ctx) && cur(ctx) == '<') {
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
    ctx->pos++;
    if (content_len == 0) {
      parse_err(ctx, "empty verbatim tag at position %zu", tag_start);
      return false;
    }
    if (!tag_text_check(ctx, ctx->src + content_start, content_len,
                        content_start, false))
      return false;
    char *content = ccol__mem_alloc(ctx->mp, content_len + 1);
    if (!content) {
      parse_err(ctx, "out of memory parsing verbatim tag at position %zu",
                tag_start);
      return false;
    }
    memcpy(content, ctx->src + content_start, content_len);
    content[content_len] = '\0';
    if (!percent_decode_tag_inplace(ctx, content, content_start)) {
      ccol__mem_free(ctx->mp, content);
      return false;
    }
    if (content[0] == '\0') {
      parse_err(ctx, "empty verbatim tag at position %zu", tag_start);
      ccol__mem_free(ctx->mp, content);
      return false;
    }
    *resolved_out = content;
    return true;
  }

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

  char *handle = ccol__mem_alloc(ctx->mp, handle_len + 1);
  if (!handle) {
    parse_err(ctx, "out of memory parsing tag handle at position %zu",
              tag_start);
    return false;
  }
  memcpy(handle, ctx->src + tag_start, handle_len);
  handle[handle_len] = '\0';

  size_t suffix_start = ctx->pos;
  while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
         cur(ctx) != '\n' && cur(ctx) != '\r' && cur(ctx) != ',' &&
         cur(ctx) != '[' && cur(ctx) != ']' && cur(ctx) != '{' &&
         cur(ctx) != '}') {
    ctx->pos++;
  }
  size_t suffix_len = ctx->pos - suffix_start;

  if (suffix_len == 0 && handle_len == 1) {
    ccol__mem_free(ctx->mp, handle);
    *resolved_out = NULL;
    return true;
  }
  if (suffix_len == 0) {
    parse_err(ctx, "tag handle '%s' with no suffix at position %zu",
              cyaml_echo(handle), tag_start);
    ccol__mem_free(ctx->mp, handle);
    return false;
  }
  if (!tag_text_check(ctx, ctx->src + suffix_start, suffix_len, suffix_start,
                      true)) {
    ccol__mem_free(ctx->mp, handle);
    return false;
  }

  const char *prefix = tag_handles_lookup(ctx, handle);
  if (!prefix) {
    parse_err(ctx, "undefined tag handle '%s' at position %zu",
              cyaml_echo(handle), tag_start);
    ccol__mem_free(ctx->mp, handle);
    return false;
  }

  char *suffix = ccol__mem_alloc(ctx->mp, suffix_len + 1);
  if (!suffix) {
    parse_err(ctx, "out of memory parsing tag suffix at position %zu",
              tag_start);
    ccol__mem_free(ctx->mp, handle);
    return false;
  }
  memcpy(suffix, ctx->src + suffix_start, suffix_len);
  suffix[suffix_len] = '\0';
  if (!percent_decode_tag_inplace(ctx, suffix, suffix_start)) {
    ccol__mem_free(ctx->mp, handle);
    ccol__mem_free(ctx->mp, suffix);
    return false;
  }
  size_t decoded_suffix_len = strlen(suffix);

  size_t prefix_len = strlen(prefix);
  char *resolved =
      ccol__mem_alloc(ctx->mp, prefix_len + decoded_suffix_len + 1);
  if (!resolved) {
    parse_err(ctx, "out of memory resolving tag at position %zu", tag_start);
    ccol__mem_free(ctx->mp, handle);
    ccol__mem_free(ctx->mp, suffix);
    return false;
  }
  memcpy(resolved, prefix, prefix_len);
  memcpy(resolved + prefix_len, suffix, decoded_suffix_len);
  resolved[prefix_len + decoded_suffix_len] = '\0';
  ccol__mem_free(ctx->mp, handle);
  ccol__mem_free(ctx->mp, suffix);
  *resolved_out = resolved;
  return true;
}

// Parses a tag token for every place a tag can appear, like
// parse_tag_token_raw(), but a bare "!" comes back as an owned
// CYAML_TAG_NON_SPECIFIC so node construction sees it, and a written tag that
// resolves to "!" (such as "!<!>") is a parse error.
static bool parse_tag_token(parse_ctx *ctx, char **resolved_out) {
  size_t tag_start = ctx->pos;
  char *tag = NULL;
  if (!parse_tag_token_raw(ctx, &tag)) return false;
  if (tag) {
    if (strcmp(tag, CYAML_TAG_NON_SPECIFIC) == 0) {
      ccol__mem_free(ctx->mp, tag);
      parse_err(ctx,
                "tag at position %zu resolves to '!', which is reserved for "
                "the non-specific tag",
                tag_start);
      return false;
    }
    *resolved_out = tag;
    return true;
  }
  tag = strdup_charged(ctx->mp, CYAML_TAG_NON_SPECIFIC);
  if (!tag) {
    parse_err(ctx, "out of memory parsing tag at position %zu", tag_start);
    return false;
  }
  *resolved_out = tag;
  return true;
}

// Registers the anchor of a dictionary key, so a later alias of it resolves;
// every place a key can carry an anchor calls it. It points anchor_name (NULL:
// nothing to do) at anchor_value when the key has a typed form (a flow
// collection, a typed plain scalar, a tagged key), else at a new string node of
// key_str. No tree holds that node, so it goes to the kept list. anchor_name is
// always freed and anchor_value always consumed. false only on OOM, with the
// error set.
static bool register_key_anchor(parse_ctx *ctx, char *anchor_name,
                                const char *key_str, cyaml_node *anchor_value) {
  if (!anchor_name) {
    if (anchor_value) cyaml__destroy((cyaml)anchor_value);
    return true;
  }
  cyaml_node *anchor_val = anchor_value;
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
              cyaml_echo(anchor_name), ctx->pos);
    ccol__mem_free(ctx->mp, anchor_name);
    return false;
  }
  bool stored = anchors_store(ctx, anchor_name, anchor_val);
  if (stored)
    parse_keep(ctx, anchor_val);
  else
    cyaml__destroy((cyaml)anchor_val);
  ccol__mem_free(ctx->mp, anchor_name);
  return stored;
}

// True when, after inline whitespace, ctx->pos is at a ':' followed by
// whitespace or the end, which makes the token before it an implicit key;
// ctx->pos is left on the ':'.
static inline __attribute__((always_inline)) bool at_block_value_indicator(
    parse_ctx *ctx) {
  skip_inline_ws(ctx);
  if (at_end(ctx) || cur(ctx) != ':') return false;
  char after = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
  return after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
         after == '\0';
}

// Tries to read an implicit block key and its ':': A plain, quoted or flow
// collection key, or an alias, with an optional anchor and tag in either order.
// The ':' check runs before any key-only work (cloning an alias, typing a
// scalar, canonical text), because the same text is usually a value and the
// canonical-key limit must never refuse a value. A flow collection is parsed as
// a speculation and rejected when no ':' follows. Every implicit key must fit
// on one line.
//
// On a match ctx->pos is past the ':' and the outputs are set: The owned anchor
// name (NULL for none or an alias key), the owned key text, whether the key is
// a merge key ("<<" plain and untagged, or with the core merge tag; an anchor
// doesn't matter), and the typed node an anchor should name. On no match
// ctx->pos is restored and ctx->error[0] tells a real error from "not a key";
// every OOM sets an error so the caller never mistakes it.
static bool try_parse_scalar_dict_key_body(parse_ctx *ctx,
                                           char **anchor_name_out,
                                           char **key_out,
                                           bool *is_merge_candidate_out,
                                           cyaml_node **anchor_value_out) {
  size_t saved_pos = ctx->pos;
  char *anchor_name = NULL;

  if (!at_end(ctx) && cur(ctx) == '*') {
    const size_t alias_start = ctx->pos;
    ctx->pos++;
    char *alias_name = NULL;
    if (!parse_anchor_name(ctx, &alias_name)) return false;
    cyaml_node *aliased = anchors_lookup(ctx, alias_name);
    if (!aliased) {
      parse_err(ctx, "unknown alias '*%s' at position %zu",
                cyaml_echo(alias_name), alias_start);
      ccol__mem_free(ctx->mp, alias_name);
      return false;
    }
    ccol__mem_free(ctx->mp, alias_name);

    if (!at_block_value_indicator(ctx)) {
      ctx->pos = saved_pos;
      return false;
    }
    ctx->pos++;

    bool too_deep = false;
    size_t height = 0;
    size_t key_levels = ctx->depth + 1 < CYAML_MAX_NESTING_DEPTH
                            ? CYAML_MAX_NESTING_DEPTH - (ctx->depth + 1)
                            : 0;
    cyaml_node *clone =
        parse_clone_levels(aliased, key_levels, &too_deep, &height);
    if (clone && ctx->depth + 1 + height > ctx->peak_depth)
      ctx->peak_depth = ctx->depth + 1 + height;
    if (!clone && too_deep) {
      parse_depth_err(ctx);
      return false;
    }
    if (!clone) {
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

  bool had_tag = false;
  char *key_tag = NULL;
  for (int prop_pass = 0; prop_pass < 2; prop_pass++) {
    if (!at_end(ctx) && cur(ctx) == '&' && !anchor_name) {
      ctx->pos++;
      if (!parse_anchor_name(ctx, &anchor_name)) {
        ccol__mem_free(ctx->mp, key_tag);
        return false;
      }
      skip_inline_ws(ctx);
    } else if (!at_end(ctx) && cur(ctx) == '!' && !had_tag) {
      if (!parse_tag_token(ctx, &key_tag)) {
        ccol__mem_free(ctx->mp, anchor_name);
        return false;
      }
      had_tag = true;
      skip_inline_ws(ctx);
    } else {
      break;
    }
  }
  const bool key_has_merge_tag =
      key_tag && strcmp(key_tag, CYAML_TAG_MERGE) == 0;
  if (!at_end(ctx) && cur(ctx) == '!' && had_tag) {
    parse_err(ctx, "a node cannot carry two tags at position %zu", ctx->pos);
    ccol__mem_free(ctx->mp, key_tag);
    ccol__mem_free(ctx->mp, anchor_name);
    return false;
  }
  if (at_end(ctx) || cur(ctx) == '\n' || cur(ctx) == '\r') {
    ctx->pos = saved_pos;
    ccol__mem_free(ctx->mp, key_tag);
    ccol__mem_free(ctx->mp, anchor_name);
    return false;
  }
  char c = cur(ctx);
  if (c == '|' || c == '>' || c == '&' || c == '*' || c == '#' || c == '%' ||
      c == '@' || c == '`' || c == ']' || c == '}' || c == ',') {
    ctx->pos = saved_pos;
    ccol__mem_free(ctx->mp, key_tag);
    ccol__mem_free(ctx->mp, anchor_name);
    return false;
  }

  char *key_str = NULL;
  cyaml_node *coll = NULL;
  anchor_speculation spec;
  bool parsed;
  if (c == '"' || c == '\'') {
    size_t quote_start = ctx->pos;
    parsed = (c == '"') ? parse_double_quoted(ctx, &key_str)
                        : parse_single_quoted(ctx, &key_str);
    if (parsed && span_crosses_newline(ctx, quote_start)) {
      ccol__mem_free(ctx->mp, key_str);
      ctx->pos = saved_pos;
      ccol__mem_free(ctx->mp, key_tag);
      ccol__mem_free(ctx->mp, anchor_name);
      return false;
    }
  } else if (c == '[' || c == '{') {
    size_t coll_start = ctx->pos;
    anchor_speculation_begin(ctx, &spec);
    coll =
        (c == '[') ? parse_flow_list(ctx, -1) : parse_flow_dictionary(ctx, -1);
    if (!coll) {
      anchor_speculation_accept(ctx, &spec);
      if (!ctx->error[0]) {
        if (parse_node_budget_exhausted)
          parse_err(ctx,
                    "document exceeded the %zu node-allocation limit at "
                    "position %zu",
                    parse_node_limit, coll_start);
        else if (parse_byte_budget_exhausted)
          parse_err(ctx,
                    "document exceeded the %zu-byte memory limit at "
                    "position %zu",
                    parse_byte_limit, coll_start);
        else
          parse_err(ctx,
                    "out of memory parsing flow collection dictionary key "
                    "at position %zu",
                    coll_start);
      }
      ccol__mem_free(ctx->mp, key_tag);
      ccol__mem_free(ctx->mp, anchor_name);
      return false;
    }
    parsed = !span_crosses_newline(ctx, coll_start);
  } else
    parsed = parse_plain_scalar(ctx, false, &key_str, NULL);
  bool from_plain_branch = (c != '"' && c != '\'' && c != '[' && c != '{');
  if (!parsed && !coll) {
    if (!ctx->error[0])
      parse_err(ctx, "out of memory parsing dictionary key at position %zu",
                saved_pos);
    ccol__mem_free(ctx->mp, key_tag);
    ccol__mem_free(ctx->mp, anchor_name);
    return false;
  }

  if (!parsed || !at_block_value_indicator(ctx)) {
    ctx->pos = saved_pos;
    ccol__mem_free(ctx->mp, key_str);
    if (coll) {
      cyaml__destroy((cyaml)coll);
      anchor_speculation_reject(ctx, &spec);
    }
    ccol__mem_free(ctx->mp, key_tag);
    ccol__mem_free(ctx->mp, anchor_name);
    return false;
  }
  if (coll) {
    anchor_speculation_accept(ctx, &spec);
    if (!parse_key_fits_below_mapping(ctx, coll)) {
      cyaml__destroy((cyaml)coll);
      ccol__mem_free(ctx->mp, key_tag);
      ccol__mem_free(ctx->mp, anchor_name);
      return false;
    }
  }
  ctx->pos++;

  cyaml_node *key_node = NULL;
  if (!key_tag && from_plain_branch) {
    key_node = make_typed_scalar(ctx, key_str);
    ccol__mem_free(ctx->mp, key_str);
    key_str = NULL;
  } else if (coll) {
    key_node = finalize_collection_node(ctx, coll, key_tag);
  } else if (key_tag || from_plain_branch) {
    key_node = finalize_scalar_node(ctx, key_str, key_tag, from_plain_branch);
    key_str = NULL;
  }
  if (key_tag) ccol__mem_free(ctx->mp, key_tag);
  if (!key_node && !key_str) {
    if (!ctx->error[0])
      parse_err(ctx, "out of memory typing dictionary key at position %zu",
                saved_pos);
    ccol__mem_free(ctx->mp, anchor_name);
    return false;
  }

  cyaml_node *anchor_value = NULL;
  if (key_node) {
    if (anchor_name) {
      key_str =
          node_to_dict_key_string_ex(ctx, key_node, "block dictionary", false);
      if (!key_str) {
        cyaml__destroy((cyaml)key_node);
        ccol__mem_free(ctx->mp, anchor_name);
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
    cyaml__destroy((cyaml)anchor_value);
  return true;
}

// Tries to read an implicit block key through the body above, for the node
// dispatch and later block-mapping entries. When it declines, ctx->pos and
// ctx->depth must be exactly as on entry, because the caller reads the same
// text again; test builds assert both.
static bool try_parse_scalar_dict_key(parse_ctx *ctx, char **anchor_name_out,
                                      char **key_out,
                                      bool *is_merge_candidate_out,
                                      cyaml_node **anchor_value_out) {
#ifdef RUNNING_UNIT_TESTS
  const size_t saved_pos = ctx->pos;
  const size_t saved_depth = ctx->depth;
#endif
  bool accepted = try_parse_scalar_dict_key_body(
      ctx, anchor_name_out, key_out, is_merge_candidate_out, anchor_value_out);
#ifdef RUNNING_UNIT_TESTS
  ccol__assert(ctx->depth == saved_depth);
  if (!accepted && !ctx->error[0]) ccol__assert(ctx->pos == saved_pos);
#endif
  return accepted;
}

// A parse runs on an explicit stack of frames instead of the C stack, so its
// stack needs don't grow with the nesting. Each kind of frame is one construct
// that can hold another node. A frame either finishes and hands its node to the
// frame below, or asks the driver to push a child and resumes when it's done.
typedef enum {
  PFK_NODE,
  PFK_FLOW_LIST,
  PFK_FLOW_DICT,
  PFK_BLOCK_LIST,
  PFK_BLOCK_DICT
} pframe_kind;

// What a frame asks the driver to push: Exactly the arguments of the call it
// stands for. resolved_tag and first_key are borrowed.
typedef struct {
  pframe_kind kind;
  int indent;
  bool in_flow;
  bool seq_ok_at_indent;
  bool allow_inline_map;
  bool had_anchor;
  bool had_tag;
  const char *resolved_tag;
  const char *first_key;
  bool first_key_colon_consumed;
  bool first_key_is_merge_candidate;
} preq;

// The resume points of each frame kind: Each names the child the frame waits
// for.
enum {
  NS_ENTRY = 0,
  NS_AFTER_ANCHOR_VALUE,
  NS_AFTER_TAG_VALUE,
  NS_AFTER_COLLECTION,
  NS_AFTER_FLOW_COLL
};
enum {
  FLS_ENTRY = 0,
  FLS_AFTER_PAIR_KEY,
  FLS_AFTER_PAIR_VALUE,
  FLS_AFTER_ELEM,
  FLS_AFTER_SHORTHAND_VALUE
};
enum { FDS_ENTRY = 0, FDS_AFTER_KEY, FDS_AFTER_VALUE };
enum { BLS_ENTRY = 0, BLS_AFTER_ELEM };
enum {
  BDS_ENTRY = 0,
  BDS_AFTER_FIRST_KEY_NODE,
  BDS_AFTER_KEY_NODE,
  BDS_AFTER_VALUE
};

// A "<<" entry of a mapping being read: Its value, owned by the frame until the
// mapping ends, and the member count when it appeared, which places the merged
// members. It never enters the map, so merge entries never collide.
typedef struct {
  cyaml_node *val;
  size_t pos;
} cyaml_merge_entry;

// One suspended parser. child is the node the frame above just finished, owned
// here until consumed; result is this frame's finished node. A container under
// construction belongs only to its frame, so a failed parse runs
// pframe_cleanup() on every frame of the stack.
typedef struct {
  pframe_kind kind;
  int state;
  cyaml_node *child;
  cyaml_node *result;
  union {
    struct {
      int indent;
      bool in_flow, seq_ok_at_indent, allow_inline_map, had_anchor, had_tag;
      const char *resolved_tag;
      char *name;
      char *tag;
      char *pending_key;
      // The column of the node and where a flow collection started.
      int col;
      size_t coll_start;
    } n;
    struct {
      int indent;
      cyaml_node *seq;
      char *key_str;
      size_t elem_start;
      bool is_merge_candidate;
    } fl;
    struct {
      int indent;
      cyaml_node *map;
      char *key_str;
      cyaml_merge_entry *merges;
      size_t merge_count, merge_cap;
      bool is_explicit_key_indicator;
      bool explicit_key_is_merge_candidate;
      bool implicit_key_is_merge_candidate;
      bool is_merge_candidate;
    } fd;
    struct {
      int seq_indent;
      cyaml_node *seq;
    } bl;
    struct {
      int map_indent;
      cyaml_node *map;
      const char *key;
      char *key_owned;
      cyaml_merge_entry *merges;
      size_t merge_count, merge_cap;
      bool have_colon, is_explicit;
      bool entry_is_merge_candidate;
    } bd;
  } u;
} pframe;

// What pbmn_begin() decided about a node in a block mapping: NODE (in
// *node_out, NULL meaning OOM), DESCEND (the request is filled in) or ERROR
// (ctx->error is set).
typedef enum { PBMN_NODE, PBMN_DESCEND, PBMN_ERROR } pbmn;

// What one step of a frame decided: DONE (f->result holds the node), DESCEND
// (push the request) or FAIL (ctx->error is set unless it was a bare OOM).
typedef enum { PSTEP_DONE, PSTEP_DESCEND, PSTEP_FAIL } pstep;

// How many parse frames the driver keeps in its locals before moving the stack
// to the heap. A level opens two frames, plus one per anchor or tag, so this
// covers ordinary documents without an allocation.
#define CYAML_PARSE_INLINE_FRAMES 24

static cyaml_node *parse_drive(parse_ctx *ctx, const preq *root_req);

// Parses one node by running the frame driver on a node request. The depth
// guard lives in the collection frames (see pframe_open()), so every container
// counts whatever construct opened it, and every exit gives ctx->depth back.
static cyaml_node *parse_node(parse_ctx *ctx, int indent, bool in_flow,
                              bool seq_ok_at_indent, bool allow_inline_map,
                              bool had_anchor, bool had_tag,
                              const char *resolved_tag) {
  preq rq = {.kind = PFK_NODE,
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
  return parse_drive(ctx, &rq);
}

static bool merge_apply(parse_ctx *ctx, cyaml_node *map,
                        const cyaml_merge_entry *entries, size_t n);
static bool merge_entry_push(parse_ctx *ctx, cyaml_merge_entry **arr,
                             size_t *count, size_t *cap, cyaml_node *val,
                             size_t pos);
static void merge_entries_free(parse_ctx *ctx, cyaml_merge_entry **arr,
                               size_t *count, size_t *cap);
static bool merge_entries_finish(parse_ctx *ctx, cyaml_node *map,
                                 cyaml_merge_entry **arr, size_t *count,
                                 size_t *cap);

// Stores the entry of the one-pair mapping that "[key: value]" or
// "[? key: value]" makes; a confirmed merge key is expanded into the mapping
// instead. It owns key_str and val in every case and destroys pair on failure.
static bool flow_pair_store(parse_ctx *ctx, cyaml_node *pair, char *key_str,
                            cyaml_node *val, bool is_merge_candidate) {
  bool ok;
  if (is_merge_candidate && strcmp(key_str, "<<") == 0) {
    cyaml_merge_entry entry = {.val = val, .pos = 0};
    ok = merge_apply(ctx, pair, &entry, 1);
    parse_discard(val);
  } else {
    ok = dictionary_set_impl((cyaml)pair, key_str, (cyaml)val) == ccol_success;
  }
  ccol__mem_free(ctx->mp, key_str);
  if (!ok) cyaml__destroy((cyaml)pair);
  return ok;
}

static bool explicit_key_peek_is_merge_candidate(parse_ctx *ctx, int indent,
                                                 bool in_flow);
// Fills in a request for one node dispatch, with the arguments of the
// parse_node() call it stands for. Each preq_* builder writes only the fields
// pframe_open() reads for its kind.
static void preq_node(preq *rq, int indent, bool in_flow, bool seq_ok_at_indent,
                      bool allow_inline_map, bool had_anchor, bool had_tag,
                      const char *resolved_tag) {
  rq->kind = PFK_NODE;
  rq->indent = indent;
  rq->in_flow = in_flow;
  rq->seq_ok_at_indent = seq_ok_at_indent;
  rq->allow_inline_map = allow_inline_map;
  rq->had_anchor = had_anchor;
  rq->had_tag = had_tag;
  rq->resolved_tag = resolved_tag;
}

// Fills in a request for a block mapping, for a node that turns out to be its
// first key or a "? " entry. first_key is borrowed; the frame below owns it and
// outlives the new frame.
static void preq_block_dict(preq *rq, int map_indent, const char *first_key,
                            bool first_key_colon_consumed,
                            bool first_key_is_merge_candidate) {
  rq->kind = PFK_BLOCK_DICT;
  rq->indent = map_indent;
  rq->first_key = first_key;
  rq->first_key_colon_consumed = first_key_colon_consumed;
  rq->first_key_is_merge_candidate = first_key_is_merge_candidate;
}

// Fills in a request for a block sequence at seq_indent, for a node that turns
// out to be a "- " entry.
static void preq_block_list(preq *rq, int seq_indent) {
  rq->kind = PFK_BLOCK_LIST;
  rq->indent = seq_indent;
}

// Fills in a request for a flow list ('[') or a flow dictionary ('{'), for a
// node that starts with one.
static void preq_flow(preq *rq, bool is_list, int indent) {
  rq->kind = is_list ? PFK_FLOW_LIST : PFK_FLOW_DICT;
  rq->indent = indent;
}

// Records the finished node of a node frame in f->result, the common exit of
// every node dispatch path. A NULL node gives PSTEP_FAIL.
static pstep pni_finish(pframe *f, cyaml_node *n) {
  f->result = n;
  return n ? PSTEP_DONE : PSTEP_FAIL;
}

// Frees the anchor name a node frame holds across its descent, once the
// resume is done with it. Clearing the field keeps pframe_cleanup()
// idempotent.
static void pni_release_name(parse_ctx *ctx, pframe *f) {
  ccol__mem_free(ctx->mp, f->u.n.name);
  f->u.n.name = NULL;
}

// Frees the tag text a node frame holds across its descent, once the resume
// is done with it, and clears the field as pni_release_name() does.
static void pni_release_tag(parse_ctx *ctx, pframe *f) {
  ccol__mem_free(ctx->mp, f->u.n.tag);
  f->u.n.tag = NULL;
}

// What flow_list_expect_comma_or_close() read: ']' (done), ',' (another
// entry), or an error with ctx->error set.
typedef enum {
  FLOW_LIST_TAIL_CLOSE,
  FLOW_LIST_TAIL_CONTINUE,
  FLOW_LIST_TAIL_ERROR
} flow_list_tail;

// Skips whitespace after a flow-list element and reads the ']' or ',' that
// must follow. Both entry shapes of pfl_step() share it. Anything else is a
// parse error.
static flow_list_tail flow_list_expect_comma_or_close(parse_ctx *ctx,
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

// Runs a flow-list frame, '[' elem (',' elem)* ']', into a CYAML_LIST. The
// element, and the key and value of "[? key: value]", are descents. An empty
// entry is an error. "[key: value]" is a one-entry mapping whose ':' must be on
// the key's line; the merge look-ahead runs before the key is read, and the
// pair holds a nesting level while its value is read. Each stored element is
// marked attached. f->u.fl.indent is the enclosing block indent, or -1.
static pstep pfl_step(parse_ctx *ctx, pframe *f, preq *rq) {
  const int indent = f->u.fl.indent;
  cyaml_node *key_node;
  cyaml_node *val;
  cyaml_node *elem;
  char *key_str;

  switch (f->state) {
    case FLS_AFTER_PAIR_KEY:
      goto after_pair_key;
    case FLS_AFTER_PAIR_VALUE:
      goto after_pair_value;
    case FLS_AFTER_ELEM:
      goto after_elem;
    case FLS_AFTER_SHORTHAND_VALUE:
      goto after_shorthand_value;
    default:
      break;
  }

  ctx->pos++;
  if (!flow_skip_ws(ctx, indent)) return PSTEP_FAIL;

  f->u.fl.seq = (cyaml_node *)cyaml_create_list_interned(ctx->mp);
  if (!f->u.fl.seq) return PSTEP_FAIL;

  if (!at_end(ctx) && cur(ctx) == ']') {
    ctx->pos++;
    goto done;
  }

  while (1) {
    if (!flow_skip_ws(ctx, indent)) return PSTEP_FAIL;
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow list");
      return PSTEP_FAIL;
    }
    if (cur(ctx) == ']') {
      ctx->pos++;
      goto done;
    }
    if (cur(ctx) == ',') {
      parse_err(ctx, "empty flow list entry at position %zu", ctx->pos);
      return PSTEP_FAIL;
    }

    bool is_explicit_pair_indicator = false;
    if (cur(ctx) == '?') {
      char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
      if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
        is_explicit_pair_indicator = true;
    }
    if (is_explicit_pair_indicator) {
      ctx->pos++;
      if (!flow_skip_ws(ctx, indent)) return PSTEP_FAIL;
      f->u.fl.is_merge_candidate =
          explicit_key_peek_is_merge_candidate(ctx, indent, true);
      preq_node(rq, indent, true, false, false, false, false, NULL);
      f->state = FLS_AFTER_PAIR_KEY;
      return PSTEP_DESCEND;
    after_pair_key:
      key_node = f->child;
      f->child = NULL;
      if (!key_node) return PSTEP_FAIL;
      if (!parse_key_fits_below_mapping(ctx, key_node)) {
        cyaml__destroy((cyaml)key_node);
        return PSTEP_FAIL;
      }
      key_str = node_to_dict_key_string(ctx, key_node, "flow sequence entry");
      if (!key_str) return PSTEP_FAIL;

      if (!flow_skip_ws(ctx, indent)) {
        ccol__mem_free(ctx->mp, key_str);
        return PSTEP_FAIL;
      }
      if (!at_end(ctx) && cur(ctx) == ':') {
        ctx->pos++;
        if (!flow_skip_ws(ctx, indent) || !flow_pair_level_take(ctx)) {
          ccol__mem_free(ctx->mp, key_str);
          return PSTEP_FAIL;
        }
        f->u.fl.key_str = key_str;
        preq_node(rq, indent, true, false, false, false, false, NULL);
        f->state = FLS_AFTER_PAIR_VALUE;
        return PSTEP_DESCEND;
      after_pair_value:
        key_str = f->u.fl.key_str;
        f->u.fl.key_str = NULL;
        ctx->depth--;
        val = f->child;
        f->child = NULL;
        if (!val) {
          ccol__mem_free(ctx->mp, key_str);
          return PSTEP_FAIL;
        }
      } else {
        if (ctx->depth >= CYAML_MAX_NESTING_DEPTH) {
          parse_depth_err(ctx);
          ccol__mem_free(ctx->mp, key_str);
          return PSTEP_FAIL;
        }
        if (ctx->depth + 1 > ctx->peak_depth) ctx->peak_depth = ctx->depth + 1;
        val = node_alloc(CYAML_NULL, ctx->mp);
        if (!val) {
          ccol__mem_free(ctx->mp, key_str);
          return PSTEP_FAIL;
        }
      }

      {
        cyaml_node *pair =
            (cyaml_node *)cyaml_create_dictionary_interned(ctx->mp);
        if (!pair) {
          ccol__mem_free(ctx->mp, key_str);
          cyaml__destroy((cyaml)val);
          return PSTEP_FAIL;
        }
        if (!flow_pair_store(ctx, pair, key_str, val,
                             f->u.fl.is_merge_candidate))
          return PSTEP_FAIL;

        cyaml_node *pep = pair;
        if (cvector_push_back(f->u.fl.seq->value.list, &pep) != ccol_success) {
          cyaml__destroy((cyaml)pair);
          return PSTEP_FAIL;
        }
        pep->attached = true;
      }

      switch (flow_list_expect_comma_or_close(ctx, indent)) {
        case FLOW_LIST_TAIL_CLOSE:
          goto done;
        case FLOW_LIST_TAIL_ERROR:
          return PSTEP_FAIL;
        case FLOW_LIST_TAIL_CONTINUE:
          continue;
      }
    }

    f->u.fl.elem_start = ctx->pos;
    f->u.fl.is_merge_candidate =
        explicit_key_peek_is_merge_candidate(ctx, indent, true);
    preq_node(rq, indent, true, false, false, false, false, NULL);
    f->state = FLS_AFTER_ELEM;
    return PSTEP_DESCEND;
  after_elem:
    elem = f->child;
    f->child = NULL;
    if (!elem) return PSTEP_FAIL;

    if (!flow_skip_ws(ctx, indent)) {
      cyaml__destroy((cyaml)elem);
      return PSTEP_FAIL;
    }
    if (!span_crosses_newline(ctx, f->u.fl.elem_start) && !at_end(ctx) &&
        cur(ctx) == ':') {
      if (!parse_key_fits_below_mapping(ctx, elem)) {
        cyaml__destroy((cyaml)elem);
        return PSTEP_FAIL;
      }
      key_str = node_to_dict_key_string(ctx, elem, "flow sequence entry");
      if (!key_str) return PSTEP_FAIL;
      ctx->pos++;
      if (!flow_skip_ws(ctx, indent)) {
        ccol__mem_free(ctx->mp, key_str);
        return PSTEP_FAIL;
      }

      if (!flow_pair_level_take(ctx)) {
        ccol__mem_free(ctx->mp, key_str);
        return PSTEP_FAIL;
      }
      f->u.fl.key_str = key_str;
      preq_node(rq, indent, true, false, false, false, false, NULL);
      f->state = FLS_AFTER_SHORTHAND_VALUE;
      return PSTEP_DESCEND;
    after_shorthand_value:
      key_str = f->u.fl.key_str;
      f->u.fl.key_str = NULL;
      ctx->depth--;
      val = f->child;
      f->child = NULL;
      if (!val) {
        ccol__mem_free(ctx->mp, key_str);
        return PSTEP_FAIL;
      }

      {
        cyaml_node *pair =
            (cyaml_node *)cyaml_create_dictionary_interned(ctx->mp);
        if (!pair) {
          ccol__mem_free(ctx->mp, key_str);
          cyaml__destroy((cyaml)val);
          return PSTEP_FAIL;
        }
        if (!flow_pair_store(ctx, pair, key_str, val,
                             f->u.fl.is_merge_candidate))
          return PSTEP_FAIL;
        elem = pair;
      }
    }

    {
      cyaml_node *ep = elem;
      if (cvector_push_back(f->u.fl.seq->value.list, &ep) != ccol_success) {
        cyaml__destroy((cyaml)elem);
        return PSTEP_FAIL;
      }
      ep->attached = true;
    }

    switch (flow_list_expect_comma_or_close(ctx, indent)) {
      case FLOW_LIST_TAIL_CLOSE:
        goto done;
      case FLOW_LIST_TAIL_ERROR:
        return PSTEP_FAIL;
      case FLOW_LIST_TAIL_CONTINUE:
        continue;
    }
  }

done:
  f->result = f->u.fl.seq;
  f->u.fl.seq = NULL;
  return PSTEP_DONE;
}

// Parses a flow list as an independent walk with its own frame stack. The
// speculative read of a flow-collection key needs it, since that read happens
// outside any node dispatch. It shares ctx, and so the depth budget, with the
// outer parse. Gives the list, or NULL on failure.
static cyaml_node *parse_flow_list(parse_ctx *ctx, int indent) {
  preq rq;
  preq_flow(&rq, true, indent);
  return parse_drive(ctx, &rq);
}

// The buffer size for the canonical text of a float key; the longest text
// ("-%.17g" with an exponent) fits.
#define CYAML_FLOAT_KEY_BUF_LEN 32

// Writes into buf the shortest "%.Ng" text that reads back as exactly the
// finite double v, for float values and float keys: It tries 15, 16 and 17
// digits, and 1 to 14 first for a subnormal (5e-324 is "5e-324").
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

// Writes the canonical key text of a float into buf, so equal numbers make the
// same dictionary key: The shortest round-trip digits with no ".0" (so the keys
// 1.0 and 1 are the same), ".nan", ".inf", "-.inf", and "0" for both zeros.
// Every such text parses back to a value with the same text, which lets the
// serializer write it unquoted. Runs in the C locale.
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

// Gives the canonical key text of key_node, since every dictionary key is a
// string; every key site of the parser calls it. A scalar gives its canonical
// text and a collection its compact canonical flow text, so equal structures
// collide whatever their insertion order. With consume it disposes of key_node
// through parse_discard() and moves a string out instead of copying it (not
// from an anchored node, and only when both use ctx->mp); without consume
// key_node stays as it was. Collection text is limited by
// CYAML_MAX_CANONICAL_KEY_LEN and charged to the amplification budget. Every
// failure sets a parse error.
static char *node_to_dict_key_string_ex(parse_ctx *ctx, cyaml_node *key_node,
                                        const char *context_label,
                                        bool consume) {
  char *key_str = NULL;
  switch (key_node->type) {
    case CYAML_STRING:
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
        parse_amp_enter();
        bool charged =
            parse_bytes_charge(strlen(key_str) + 1 + CYAML_ALLOC_OVERHEAD);
        parse_amp_leave();
        if (!charged) {
          parse_err(ctx,
                    "canonical form of a non-scalar key in %s exceeded the "
                    "%s memory limit at position %zu",
                    context_label,
                    parse_amp_limit_hit ? "alias expansion" : "document",
                    ctx->pos);
          ccol__mem_free(ctx->mp, key_str);
          key_str = NULL;
        }
      }
      break;
    }
  }
  if (!key_str && !ctx->error[0]) {
    parse_err(ctx,
              "out of memory converting a scalar key to a dictionary "
              "key string in %s at position %zu",
              context_label, ctx->pos);
  }
  if (consume) parse_discard(key_node);
  return key_str;
}

// Gives the key text of key_node and disposes of it,
// node_to_dict_key_string_ex() with consume set, for the key sites that don't
// keep the node.
static char *node_to_dict_key_string(parse_ctx *ctx, cyaml_node *key_node,
                                     const char *context_label) {
  return node_to_dict_key_string_ex(ctx, key_node, context_label, true);
}

// Runs a flow-dictionary frame, '{' key ':' value (',' ...)* '}', into a
// CYAML_DICTIONARY. Keys may be any node and are stored as canonical text. The
// key and the value are descents. "{? key: value}" is accepted, "{key}" and
// "{key:}" give null values, and merge entries wait in f->u.fd.merges until the
// mapping ends, so they never replace each other or an ordinary "<<" key.
static pstep pfd_step(parse_ctx *ctx, pframe *f, preq *rq) {
  const int indent = f->u.fd.indent;
  cyaml_node *key_node = NULL;
  cyaml_node *val = NULL;
  char *key_str = NULL;

  switch (f->state) {
    case FDS_AFTER_KEY:
      goto after_key;
    case FDS_AFTER_VALUE:
      goto after_value;
    default:
      break;
  }

  ctx->pos++;
  if (!flow_skip_ws(ctx, indent)) return PSTEP_FAIL;

  f->u.fd.map = (cyaml_node *)cyaml_create_dictionary_interned(ctx->mp);
  if (!f->u.fd.map) return PSTEP_FAIL;

  if (!at_end(ctx) && cur(ctx) == '}') {
    ctx->pos++;
    goto done;
  }

  while (1) {
    if (!flow_skip_ws(ctx, indent)) return PSTEP_FAIL;
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow dictionary");
      return PSTEP_FAIL;
    }
    if (cur(ctx) == '}') {
      ctx->pos++;
      if (f->u.fd.merge_count &&
          !merge_entries_finish(ctx, f->u.fd.map, &f->u.fd.merges,
                                &f->u.fd.merge_count, &f->u.fd.merge_cap))
        return PSTEP_FAIL;
      goto done;
    }

    f->u.fd.is_explicit_key_indicator = false;
    if (cur(ctx) == '?') {
      char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
      if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
        f->u.fd.is_explicit_key_indicator = true;
    }

    f->u.fd.implicit_key_is_merge_candidate = false;
    f->u.fd.explicit_key_is_merge_candidate = false;
    if (f->u.fd.is_explicit_key_indicator) {
      ctx->pos++;
      if (!flow_skip_ws(ctx, indent)) return PSTEP_FAIL;
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
    preq_node(rq, indent, true, false, false, false, false, NULL);
    f->state = FDS_AFTER_KEY;
    return PSTEP_DESCEND;
  after_key:
    key_node = f->child;
    f->child = NULL;
  have_key_node:
    if (!key_node) return PSTEP_FAIL;

    key_str = node_to_dict_key_string(ctx, key_node, "flow dictionary");
    if (!key_str) return PSTEP_FAIL;
    f->u.fd.is_merge_candidate = f->u.fd.is_explicit_key_indicator
                                     ? f->u.fd.explicit_key_is_merge_candidate
                                     : f->u.fd.implicit_key_is_merge_candidate;

    if (!flow_skip_ws(ctx, indent)) {
      ccol__mem_free(ctx->mp, key_str);
      return PSTEP_FAIL;
    }
    if (!at_end(ctx) && cur(ctx) == ':') {
      ctx->pos++;
      if (!flow_skip_ws(ctx, indent)) {
        ccol__mem_free(ctx->mp, key_str);
        return PSTEP_FAIL;
      }
      if (!at_end(ctx) && (cur(ctx) == ',' || cur(ctx) == '}')) {
        val = node_alloc(CYAML_NULL, ctx->mp);
      } else {
        f->u.fd.key_str = key_str;
        preq_node(rq, indent, true, false, false, false, false, NULL);
        f->state = FDS_AFTER_VALUE;
        return PSTEP_DESCEND;
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
      ccol__mem_free(ctx->mp, key_str);
      return PSTEP_FAIL;
    }
    if (!val) {
      ccol__mem_free(ctx->mp, key_str);
      return PSTEP_FAIL;
    }

    if (f->u.fd.is_merge_candidate && strcmp(key_str, "<<") == 0) {
      ccol__mem_free(ctx->mp, key_str);
      if (!merge_entry_push(ctx, &f->u.fd.merges, &f->u.fd.merge_count,
                            &f->u.fd.merge_cap, val,
                            chashmap_size(f->u.fd.map->value.dictionary)))
        return PSTEP_FAIL;
    } else {
      ccol_retval r =
          dictionary_set_impl((cyaml)f->u.fd.map, key_str, (cyaml)val);
      ccol__mem_free(ctx->mp, key_str);
      if (r != ccol_success) return PSTEP_FAIL;
    }

    if (!flow_skip_ws(ctx, indent)) return PSTEP_FAIL;
    if (at_end(ctx)) {
      parse_err(ctx, "unterminated flow dictionary");
      return PSTEP_FAIL;
    }
    if (cur(ctx) == '}') {
      ctx->pos++;
      if (f->u.fd.merge_count &&
          !merge_entries_finish(ctx, f->u.fd.map, &f->u.fd.merges,
                                &f->u.fd.merge_count, &f->u.fd.merge_cap))
        return PSTEP_FAIL;
      goto done;
    }
    if (cur(ctx) != ',') {
      parse_err(ctx, "expected ',' or '}' in flow dictionary at position %zu",
                ctx->pos);
      return PSTEP_FAIL;
    }
    ctx->pos++;
  }

done:
  f->result = f->u.fd.map;
  f->u.fd.map = NULL;
  return PSTEP_DONE;
}

// Parses a flow dictionary as an independent walk, for the same speculative
// key read as parse_flow_list(). Gives the dictionary, or NULL on failure.
static cyaml_node *parse_flow_dictionary(parse_ctx *ctx, int indent) {
  preq rq;
  preq_flow(&rq, false, indent);
  return parse_drive(ctx, &rq);
}

// Dispatches one node: The decision tree the parser runs for every node,
// suspended at its descents (an anchored value, a tagged value, a collection,
// a flow collection that may become a key). It's inlined into the driver, since
// half of all parse steps are node dispatches.
//
// The request carries the context: indent (-1 at the root, in flow, or not
// fixed yet), in_flow, seq_ok_at_indent (indent is a mapping's, so a "- " at
// exactly indent is its value), allow_inline_map (a scalar followed by ':' may
// open a new block mapping here; false only for the same-line value of an
// implicit key, so "a: b: c" is an error), had_anchor and had_tag (an enclosing
// property already decorates this node, so a second one is an error), and
// resolved_tag (the enclosing tag, which decides the type where the content is
// built). The '&', '!' and '*' branches pass these on.
//
// The branches: A document marker or a column-0 BOM is never content. "&a key:"
// and "!t key:" decorate only the key, so they try the key reader first. A
// property alone on its line takes a value only from a more indented line (or a
// "- " at a mapping's indent), else the node is empty and still typed by the
// tag. A collection opener right against a property, a "- " after a property
// on the same line, two anchors or two tags, and a property on an alias are
// errors. An alias is cloned with only the levels left below ctx->depth. A
// flow collection or a quoted scalar followed by ':' on the same line becomes
// the first key of a block mapping. Anchors are stored on the node itself, and
// the tag text is freed here in every case.
static pstep pni_step(parse_ctx *ctx, pframe *f, preq *rq) {
  const int indent = f->u.n.indent;
  const bool in_flow = f->u.n.in_flow;
  const bool seq_ok_at_indent = f->u.n.seq_ok_at_indent;
  const bool allow_inline_map = f->u.n.allow_inline_map;
  const bool had_anchor = f->u.n.had_anchor;
  const bool had_tag = f->u.n.had_tag;
  const char *const resolved_tag = f->u.n.resolved_tag;

  switch (f->state) {
    case NS_AFTER_ANCHOR_VALUE:
      goto after_anchor_value;
    case NS_AFTER_TAG_VALUE:
      goto after_tag_value;
    case NS_AFTER_COLLECTION:
      goto after_collection;
    case NS_AFTER_FLOW_COLL:
      goto after_flow_coll;
    default:
      break;
  }

  {
    if (!in_flow)
      skip_ws_comments(ctx);
    else
      skip_inline_ws(ctx);

    if (at_end(ctx)) {
      if (!in_flow) return pni_finish(f, node_alloc(CYAML_NULL, ctx->mp));
      parse_err(ctx, "unexpected end of input in flow context at position %zu",
                ctx->pos);
      return PSTEP_FAIL;
    }

    if (at_col0_bom(ctx)) {
      parse_err(ctx,
                "byte order mark inside a document at position %zu; a byte "
                "order mark may only precede a document",
                ctx->pos);
      return PSTEP_FAIL;
    }

    if (in_flow && at_doc_marker(ctx)) {
      parse_err(ctx,
                "a document marker cannot appear inside an unclosed "
                "flow collection at position %zu",
                ctx->pos);
      return PSTEP_FAIL;
    }

    char c = cur(ctx);

    if (c == '&') {
      if (!in_flow && allow_inline_map) {
        int col = current_col(ctx);
        char *anchor_name = NULL;
        char *key_str = NULL;
        bool is_merge_candidate = false;
        cyaml_node *anchor_value = NULL;
        if (try_parse_scalar_dict_key(ctx, &anchor_name, &key_str,
                                      &is_merge_candidate, &anchor_value)) {
          if (!register_key_anchor(ctx, anchor_name, key_str, anchor_value)) {
            ccol__mem_free(ctx->mp, key_str);
            return PSTEP_FAIL;
          }
          f->u.n.pending_key = key_str;
          preq_block_dict(rq, col, key_str, true, is_merge_candidate);
          f->state = NS_AFTER_COLLECTION;
          return PSTEP_DESCEND;
        }
        if (ctx->error[0]) return PSTEP_FAIL;
      }

      if (had_anchor) {
        parse_err(ctx, "a node cannot carry two anchors at position %zu",
                  ctx->pos);
        return PSTEP_FAIL;
      }

      ctx->pos++;
      if (!parse_anchor_name(ctx, &f->u.n.name)) return PSTEP_FAIL;
      char *name = f->u.n.name;
      size_t after_name_pos = ctx->pos;
      skip_inline_ws(ctx);
      if (ctx->pos == after_name_pos && !at_end(ctx) &&
          (cur(ctx) == '[' || cur(ctx) == '{' ||
           (!in_flow &&
            (cur(ctx) == ',' || cur(ctx) == ']' || cur(ctx) == '}')))) {
        parse_err(ctx,
                  "unexpected '%s' directly after anchor name at "
                  "position %zu",
                  cyaml_echo_c(cur(ctx)), ctx->pos);
        pni_release_name(ctx, f);
        return PSTEP_FAIL;
      }

      cyaml_node *n;
      if (!in_flow && rest_of_line_is_blank(ctx)) {
        skip_ws_comments(ctx);
        bool is_value_col =
            !at_end(ctx) && at_block_value_col(ctx, indent, seq_ok_at_indent);
        if (at_end(ctx) || !is_value_col || at_doc_marker(ctx)) {
          char *empty = ccol_strdup(ctx->mp, "");
          n = empty ? finalize_scalar_node(ctx, empty, resolved_tag, true)
                    : NULL;
        } else {
          preq_node(rq, indent, in_flow, seq_ok_at_indent, true, true, had_tag,
                    resolved_tag);
          f->state = NS_AFTER_ANCHOR_VALUE;
          return PSTEP_DESCEND;
        }
      } else if (in_flow && rest_of_line_is_blank(ctx)) {
        if (!flow_skip_ws(ctx, indent)) {
          pni_release_name(ctx, f);
          return PSTEP_FAIL;
        }
        preq_node(rq, indent, in_flow, seq_ok_at_indent, allow_inline_map, true,
                  had_tag, resolved_tag);
        f->state = NS_AFTER_ANCHOR_VALUE;
        return PSTEP_DESCEND;
      } else if (!in_flow && at_bare_seq_indicator(ctx)) {
        parse_err(ctx,
                  "a block sequence cannot start on the same line as an "
                  "anchor at position %zu",
                  ctx->pos);
        pni_release_name(ctx, f);
        return PSTEP_FAIL;
      } else if (!in_flow && cur(ctx) == '&') {
        parse_err(ctx, "a node cannot carry two anchors at position %zu",
                  ctx->pos);
        pni_release_name(ctx, f);
        return PSTEP_FAIL;
      } else if (!in_flow && cur(ctx) == '*') {
        parse_err(ctx, "an alias cannot carry an anchor or tag at position %zu",
                  ctx->pos);
        pni_release_name(ctx, f);
        return PSTEP_FAIL;
      } else {
        preq_node(rq, indent, in_flow, seq_ok_at_indent, allow_inline_map, true,
                  had_tag, resolved_tag);
        f->state = NS_AFTER_ANCHOR_VALUE;
        return PSTEP_DESCEND;
      }
      goto anchor_have_value;
    after_anchor_value:
      n = f->child;
      f->child = NULL;
      name = f->u.n.name;
    anchor_have_value:
      if (!n) {
        pni_release_name(ctx, f);
        return PSTEP_FAIL;
      }
      if (!anchors_store(ctx, name, n)) {
        cyaml__destroy((cyaml)n);
        pni_release_name(ctx, f);
        return PSTEP_FAIL;
      }
      pni_release_name(ctx, f);
      return pni_finish(f, n);
    }

    if (c == '*') {
      if (!in_flow && allow_inline_map) {
        int col = current_col(ctx);
        char *anchor_name = NULL;
        char *key_str = NULL;
        if (try_parse_scalar_dict_key(ctx, &anchor_name, &key_str, NULL,
                                      NULL)) {
          ccol__mem_free(ctx->mp, anchor_name);
          f->u.n.pending_key = key_str;
          preq_block_dict(rq, col, key_str, true, false);
          f->state = NS_AFTER_COLLECTION;
          return PSTEP_DESCEND;
        }
        if (ctx->error[0]) return PSTEP_FAIL;
      }

      if (had_anchor || had_tag) {
        parse_err(ctx, "an alias cannot carry an anchor or tag at position %zu",
                  ctx->pos);
        return PSTEP_FAIL;
      }

      const size_t alias_start = ctx->pos;
      ctx->pos++;
      char *name = NULL;
      if (!parse_anchor_name(ctx, &name)) return PSTEP_FAIL;
      cyaml_node *anchored = anchors_lookup(ctx, name);
      if (!anchored) {
        parse_err(ctx, "unknown alias '*%s' at position %zu", cyaml_echo(name),
                  alias_start);
        ccol__mem_free(ctx->mp, name);
        return PSTEP_FAIL;
      }
      bool too_deep = false;
      size_t height = 0;
      cyaml_node *clone = parse_clone_levels(
          anchored, CYAML_MAX_NESTING_DEPTH - ctx->depth, &too_deep, &height);
      if (clone && ctx->depth + height > ctx->peak_depth)
        ctx->peak_depth = ctx->depth + height;
      if (!clone && too_deep)
        parse_err(ctx,
                  "alias '*%s' expansion exceeds the maximum nesting depth "
                  "(%d) at position %zu",
                  cyaml_echo(name), CYAML_MAX_NESTING_DEPTH, ctx->pos);
      else if (!clone && parse_amp_limit_hit)
        parse_err(ctx,
                  "alias '*%s' expansion exceeded the %zu-node, %zu-byte "
                  "alias expansion memory limit at position %zu; check for "
                  "exponential anchor/alias nesting",
                  cyaml_echo(name), (size_t)CYAML_PARSE_NODE_FLOOR,
                  (size_t)CYAML_PARSE_BYTE_FLOOR, ctx->pos);
      else if (!clone && parse_node_budget_exhausted)
        parse_err(ctx,
                  "alias '*%s' expansion exceeded the %zu node-allocation "
                  "limit at position %zu; check for exponential anchor/alias "
                  "nesting",
                  cyaml_echo(name), parse_node_limit, ctx->pos);
      else if (!clone && parse_byte_budget_exhausted)
        parse_err(ctx,
                  "alias '*%s' expansion exceeded the %zu-byte document memory "
                  "limit at position %zu; check for exponential anchor/alias "
                  "nesting",
                  cyaml_echo(name), parse_byte_limit, ctx->pos);
      ccol__mem_free(ctx->mp, name);
      return pni_finish(f, clone);
    }

    if (c == '!') {
      if (!in_flow && allow_inline_map) {
        int col = current_col(ctx);
        char *anchor_name = NULL;
        char *key_str = NULL;
        cyaml_node *anchor_value = NULL;
        bool is_merge_candidate = false;
        if (try_parse_scalar_dict_key(ctx, &anchor_name, &key_str,
                                      &is_merge_candidate, &anchor_value)) {
          if (!register_key_anchor(ctx, anchor_name, key_str, anchor_value)) {
            ccol__mem_free(ctx->mp, key_str);
            return PSTEP_FAIL;
          }
          f->u.n.pending_key = key_str;
          preq_block_dict(rq, col, key_str, true, is_merge_candidate);
          f->state = NS_AFTER_COLLECTION;
          return PSTEP_DESCEND;
        }
        if (ctx->error[0]) return PSTEP_FAIL;
      }

      if (had_tag) {
        parse_err(ctx, "a node cannot carry two tags at position %zu",
                  ctx->pos);
        return PSTEP_FAIL;
      }

      if (!parse_tag_token(ctx, &f->u.n.tag)) return PSTEP_FAIL;
      char *tag = f->u.n.tag;
      size_t after_tag_pos = ctx->pos;
      skip_inline_ws(ctx);
      if (ctx->pos == after_tag_pos && !at_end(ctx) &&
          (cur(ctx) == '[' || cur(ctx) == '{' ||
           (!in_flow &&
            (cur(ctx) == ',' || cur(ctx) == ']' || cur(ctx) == '}')))) {
        parse_err(ctx, "unexpected '%s' directly after tag at position %zu",
                  cyaml_echo_c(cur(ctx)), ctx->pos);
        pni_release_tag(ctx, f);
        return PSTEP_FAIL;
      }
      if (ctx->pos == after_tag_pos && !at_end(ctx) && cur(ctx) != '\n' &&
          cur(ctx) != '\r' && cur(ctx) != '#' && cur(ctx) != '!' &&
          cur(ctx) != '&' && cur(ctx) != '*' &&
          !(in_flow &&
            (cur(ctx) == ',' || cur(ctx) == ']' || cur(ctx) == '}'))) {
        parse_err(ctx,
                  "tag must be separated from its content by whitespace at "
                  "position %zu",
                  ctx->pos);
        pni_release_tag(ctx, f);
        return PSTEP_FAIL;
      }

      cyaml_node *n;
      if (!in_flow && rest_of_line_is_blank(ctx)) {
        skip_ws_comments(ctx);
        bool is_value_col =
            !at_end(ctx) && at_block_value_col(ctx, indent, seq_ok_at_indent);
        if (at_end(ctx) || !is_value_col || at_doc_marker(ctx)) {
          char *empty = ccol_strdup(ctx->mp, "");
          n = empty ? finalize_scalar_node(ctx, empty, tag, true) : NULL;
        } else {
          preq_node(rq, indent, in_flow, seq_ok_at_indent, true, had_anchor,
                    true, tag);
          f->state = NS_AFTER_TAG_VALUE;
          return PSTEP_DESCEND;
        }
      } else if (in_flow && rest_of_line_is_blank(ctx)) {
        if (!flow_skip_ws(ctx, indent)) {
          pni_release_tag(ctx, f);
          return PSTEP_FAIL;
        }
        preq_node(rq, indent, in_flow, seq_ok_at_indent, allow_inline_map,
                  had_anchor, true, tag);
        f->state = NS_AFTER_TAG_VALUE;
        return PSTEP_DESCEND;
      } else if (!in_flow && at_bare_seq_indicator(ctx)) {
        parse_err(ctx,
                  "a block sequence cannot start on the same line as a "
                  "tag at position %zu",
                  ctx->pos);
        pni_release_tag(ctx, f);
        return PSTEP_FAIL;
      } else if (!in_flow && cur(ctx) == '!') {
        parse_err(ctx, "a node cannot carry two tags at position %zu",
                  ctx->pos);
        pni_release_tag(ctx, f);
        return PSTEP_FAIL;
      } else if (!in_flow && cur(ctx) == '*') {
        parse_err(ctx, "an alias cannot carry an anchor or tag at position %zu",
                  ctx->pos);
        pni_release_tag(ctx, f);
        return PSTEP_FAIL;
      } else {
        preq_node(rq, indent, in_flow, seq_ok_at_indent, allow_inline_map,
                  had_anchor, true, tag);
        f->state = NS_AFTER_TAG_VALUE;
        return PSTEP_DESCEND;
      }
      goto tag_have_value;
    after_tag_value:
      n = f->child;
      f->child = NULL;
    tag_have_value:
      if (!n) {
        pni_release_tag(ctx, f);
        return PSTEP_FAIL;
      }

      pni_release_tag(ctx, f);
      return pni_finish(f, n);
    }

    if (c == '[' || c == '{') {
      f->u.n.col = current_col(ctx);
      f->u.n.coll_start = ctx->pos;
      preq_flow(rq, c == '[', indent);
      f->state = NS_AFTER_FLOW_COLL;
      return PSTEP_DESCEND;
    after_flow_coll:;
      cyaml_node *coll = f->child;
      f->child = NULL;
      if (!coll) return PSTEP_FAIL;

      bool spans_multiple_lines = span_crosses_newline(ctx, f->u.n.coll_start);
      if (!in_flow && !spans_multiple_lines) {
        skip_inline_ws(ctx);
        if (!at_end(ctx) && cur(ctx) == ':') {
          char after =
              (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
          if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
              ctx->pos + 1 >= ctx->len) {
            if (!allow_inline_map) {
              parse_err(ctx,
                        "mapping values are not allowed here "
                        "(position %zu)",
                        ctx->pos);
              cyaml__destroy((cyaml)coll);
              return PSTEP_FAIL;
            }
            if (!parse_key_fits_below_mapping(ctx, coll)) {
              cyaml__destroy((cyaml)coll);
              return PSTEP_FAIL;
            }
            char *key_str =
                node_to_dict_key_string(ctx, coll, "block dictionary");
            if (!key_str) return PSTEP_FAIL;
            f->u.n.pending_key = key_str;
            preq_block_dict(rq, f->u.n.col, key_str, false, false);
            f->state = NS_AFTER_COLLECTION;
            return PSTEP_DESCEND;
          }
        }
      }
      return pni_finish(f, finalize_collection_node(ctx, coll, resolved_tag));
    }

    if (c == '|' && !in_flow) {
      ctx->pos++;
      chomp_mode chomp;
      int exp_indent;
      if (!parse_block_scalar_header(ctx, &chomp, &exp_indent))
        return PSTEP_FAIL;
      char *s = NULL;
      if (!parse_block_scalar_content(ctx, indent, chomp, exp_indent, &s))
        return PSTEP_FAIL;
      return pni_finish(f, finalize_scalar_node(ctx, s, resolved_tag, false));
    }

    if (c == '>' && !in_flow) {
      ctx->pos++;
      chomp_mode chomp;
      int exp_indent;
      if (!parse_block_scalar_header(ctx, &chomp, &exp_indent))
        return PSTEP_FAIL;
      char *s = NULL;
      if (!parse_folded_scalar_content(ctx, indent, chomp, exp_indent, &s))
        return PSTEP_FAIL;
      return pni_finish(f, finalize_scalar_node(ctx, s, resolved_tag, false));
    }

    if (c == '-' && !in_flow) {
      if (ctx->pos + 1 < ctx->len) {
        char next = ctx->src[ctx->pos + 1];
        if (next == ' ' || next == '\t' || next == '\n' || next == '\r') {
          preq_block_list(rq, current_col(ctx));
          f->state = NS_AFTER_COLLECTION;
          return PSTEP_DESCEND;
        }
      } else {
        preq_block_list(rq, current_col(ctx));
        f->state = NS_AFTER_COLLECTION;
        return PSTEP_DESCEND;
      }
    }

    if (c == '?' && !in_flow) {
      bool is_explicit_key = false;
      if (ctx->pos + 1 < ctx->len) {
        char next = ctx->src[ctx->pos + 1];
        if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
          is_explicit_key = true;
      } else {
        is_explicit_key = true;
      }
      if (is_explicit_key) {
        if (!allow_inline_map) {
          parse_err(ctx,
                    "mapping values are not allowed here "
                    "(position %zu)",
                    ctx->pos);
          return PSTEP_FAIL;
        }
        preq_block_dict(rq, current_col(ctx), NULL, false, false);
        f->state = NS_AFTER_COLLECTION;
        return PSTEP_DESCEND;
      }
    }

    if (c == '"') {
      int col = current_col(ctx);
      size_t quote_start = ctx->pos;
      char *s = NULL;
      if (!parse_double_quoted(ctx, &s)) return PSTEP_FAIL;
      if (!in_flow) {
        skip_inline_ws(ctx);
        if (!at_end(ctx) && cur(ctx) == ':') {
          char after =
              (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
          if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
              ctx->pos + 1 >= ctx->len) {
            if (span_crosses_newline(ctx, quote_start)) {
              parse_err(ctx,
                        "implicit keys cannot span multiple lines at "
                        "position %zu",
                        ctx->pos);
              ccol__mem_free(ctx->mp, s);
              return PSTEP_FAIL;
            }
            if (!allow_inline_map) {
              parse_err(ctx,
                        "mapping values are not allowed here "
                        "(position %zu)",
                        ctx->pos);
              ccol__mem_free(ctx->mp, s);
              return PSTEP_FAIL;
            }
            f->u.n.pending_key = s;
            preq_block_dict(rq, col, s, false, false);
            f->state = NS_AFTER_COLLECTION;
            return PSTEP_DESCEND;
          }
        }
      }
      return pni_finish(f, finalize_scalar_node(ctx, s, resolved_tag, false));
    }

    if (c == '\'') {
      int col = current_col(ctx);
      size_t quote_start = ctx->pos;
      char *s = NULL;
      if (!parse_single_quoted(ctx, &s)) return PSTEP_FAIL;
      if (!in_flow) {
        skip_inline_ws(ctx);
        if (!at_end(ctx) && cur(ctx) == ':') {
          char after =
              (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
          if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
              ctx->pos + 1 >= ctx->len) {
            if (span_crosses_newline(ctx, quote_start)) {
              parse_err(ctx,
                        "implicit keys cannot span multiple lines at "
                        "position %zu",
                        ctx->pos);
              ccol__mem_free(ctx->mp, s);
              return PSTEP_FAIL;
            }
            if (!allow_inline_map) {
              parse_err(ctx,
                        "mapping values are not allowed here "
                        "(position %zu)",
                        ctx->pos);
              ccol__mem_free(ctx->mp, s);
              return PSTEP_FAIL;
            }
            f->u.n.pending_key = s;
            preq_block_dict(rq, col, s, false, false);
            f->state = NS_AFTER_COLLECTION;
            return PSTEP_DESCEND;
          }
        }
      }
      return pni_finish(f, finalize_scalar_node(ctx, s, resolved_tag, false));
    }

    if (c == '#') {
      parse_err(ctx,
                "unexpected '#' at position %zu (a comment must be "
                "preceded by whitespace)",
                ctx->pos);
      return PSTEP_FAIL;
    }

    if (c == '%' || c == '@' || c == '`') {
      parse_err(ctx,
                "unexpected '%s' at position %zu (not a valid plain scalar "
                "start character)",
                cyaml_echo_c(c), ctx->pos);
      return PSTEP_FAIL;
    }

    if (!in_flow && (c == ']' || c == '}' || c == ',')) {
      parse_err(ctx,
                "unexpected '%s' at position %zu (a flow indicator cannot "
                "start a plain scalar)",
                cyaml_echo_c(c), ctx->pos);
      return PSTEP_FAIL;
    }

    if (in_flow && !at_valid_flow_plain_scalar_start(ctx)) {
      parse_err(ctx,
                "unexpected '%s' at position %zu (not a valid plain "
                "scalar in flow context)",
                cyaml_echo_c(c), ctx->pos);
      return PSTEP_FAIL;
    }

    {
      int col = current_col(ctx);
      char *s = NULL;
      bool hit_real_terminator = false;
      if (!parse_plain_scalar(ctx, in_flow, &s, &hit_real_terminator))
        return PSTEP_FAIL;

      if (!in_flow) {
        skip_inline_ws(ctx);
        if (!at_end(ctx) && cur(ctx) == ':') {
          char after =
              (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
          if (after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
              ctx->pos + 1 >= ctx->len) {
            if (!allow_inline_map) {
              parse_err(ctx,
                        "mapping values are not allowed here "
                        "(position %zu)",
                        ctx->pos);
              ccol__mem_free(ctx->mp, s);
              return PSTEP_FAIL;
            }
            bool is_merge_candidate = strcmp(s, "<<") == 0;
            cyaml_node *typed = make_typed_scalar(ctx, s);
            if (!typed) {
              parse_err(ctx,
                        "out of memory typing dictionary key at position %zu",
                        ctx->pos);
              ccol__mem_free(ctx->mp, s);
              return PSTEP_FAIL;
            }
            char *canon_key =
                node_to_dict_key_string(ctx, typed, "block dictionary");
            ccol__mem_free(ctx->mp, s);
            if (!canon_key) return PSTEP_FAIL;
            f->u.n.pending_key = canon_key;
            preq_block_dict(rq, col, canon_key, false, is_merge_candidate);
            f->state = NS_AFTER_COLLECTION;
            return PSTEP_DESCEND;
          }
        }
      }

      char *full = NULL;
      if (!parse_plain_scalar_multiline(ctx, in_flow, indent, s,
                                        hit_real_terminator, &full))
        return PSTEP_FAIL;

      return pni_finish(f, finalize_scalar_node(ctx, full, resolved_tag, true));
    }
  }

after_collection: {
  cyaml_node *coll = f->child;
  f->child = NULL;
  if (f->u.n.pending_key) {
    ccol__mem_free(ctx->mp, f->u.n.pending_key);
    f->u.n.pending_key = NULL;
  }
  return pni_finish(f, finalize_collection_node(ctx, coll, resolved_tag));
}
}

// Runs a block-sequence frame: '-' entries at column f->u.bl.seq_indent, each
// value a descent. A '-' followed only by a comment, or whose next content line
// isn't more indented, is a null entry. A tab after the '-' is separation, but
// a block collection after it fails. The value gets seq_indent as its indent,
// so nested block scalars use it as parent_indent, and a compact "- key: value"
// mapping is allowed. Each stored element is marked attached.
static pstep pbl_step(parse_ctx *ctx, pframe *f, preq *rq) {
  const int seq_indent = f->u.bl.seq_indent;
  cyaml_node *elem = NULL;

  if (f->state == BLS_AFTER_ELEM) goto after_elem;

  f->u.bl.seq = (cyaml_node *)cyaml_create_list_interned(ctx->mp);
  if (!f->u.bl.seq) return PSTEP_FAIL;

  while (!at_end(ctx)) {
    skip_ws_comments(ctx);
    if (at_end(ctx)) break;

    if (line_indent_has_tab(ctx)) {
      parse_err(ctx,
                "tab cannot be used as block sequence indentation at "
                "position %zu",
                ctx->pos);
      return PSTEP_FAIL;
    }
    if (current_col(ctx) != seq_indent) break;
    if (cur(ctx) != '-') break;

    {
      bool is_seq_entry = false;
      if (ctx->pos + 1 < ctx->len) {
        char next = ctx->src[ctx->pos + 1];
        if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
          is_seq_entry = true;
      } else {
        is_seq_entry = true;
      }

      if (!is_seq_entry) break;
    }

    ctx->pos++;

    if (!at_end(ctx) && cur(ctx) == ' ') ctx->pos++;

    if (rest_of_line_is_blank(ctx) || at_end(ctx)) {
      skip_ws_comments(ctx);
      if (at_end(ctx) || line_space_indent(ctx) <= seq_indent) {
        elem = node_alloc(CYAML_NULL, ctx->mp);
        goto have_elem;
      }
    }
    preq_node(rq, seq_indent, false, false, true, false, false, NULL);
    f->state = BLS_AFTER_ELEM;
    return PSTEP_DESCEND;
  after_elem:
    elem = f->child;
    f->child = NULL;
  have_elem:
    if (!elem) return PSTEP_FAIL;

    {
      cyaml_node *ep = elem;
      if (cvector_push_back(f->u.bl.seq->value.list, &ep) != ccol_success) {
        cyaml__destroy((cyaml)elem);
        return PSTEP_FAIL;
      }
      ep->attached = true;
    }
  }

  f->result = f->u.bl.seq;
  f->u.bl.seq = NULL;
  return PSTEP_DONE;
}

// Starts the node in a block mapping's key or value position, shared by the
// explicit '?' key, the explicit ':' value and an implicit value. Content may
// follow on the same line or on a more indented later line, else the node is an
// implicit null. allow_inline_seq (true only for explicit-style content) lets a
// "- " sequence or a chained "k: v" mapping start on the same line, which an
// implicit value's grammar doesn't allow. *is_merge_candidate_out (key
// position only, may be NULL) gets the merge look-ahead, taken here because
// only here is the start of the key's content known in both cases.
CYAML_PARSE_HOT pbmn pbmn_begin(parse_ctx *ctx, int map_indent,
                                bool allow_inline_seq,
                                bool *is_merge_candidate_out,
                                cyaml_node **node_out, preq *rq) {
  if (is_merge_candidate_out) *is_merge_candidate_out = false;
  if (rest_of_line_is_blank(ctx)) {
    skip_ws_comments(ctx);
    bool is_value_col =
        !at_end(ctx) && at_block_value_col(ctx, map_indent, true);
    if (at_end(ctx) || !is_value_col || at_doc_marker(ctx)) {
      *node_out = node_alloc(CYAML_NULL, ctx->mp);
      return PBMN_NODE;
    }
    if (is_merge_candidate_out)
      *is_merge_candidate_out =
          explicit_key_peek_is_merge_candidate(ctx, map_indent, false);
    preq_node(rq, map_indent, false, true, true, false, false, NULL);
    return PBMN_DESCEND;
  }
  if (!allow_inline_seq && at_bare_seq_indicator(ctx)) {
    parse_err(ctx,
              "a block sequence cannot start on the same line as its "
              "mapping key at position %zu",
              ctx->pos);
    return PBMN_ERROR;
  }
  if (is_merge_candidate_out)
    *is_merge_candidate_out =
        explicit_key_peek_is_merge_candidate(ctx, map_indent, false);
  preq_node(rq, map_indent, false, true, allow_inline_seq, false, false, NULL);
  return PBMN_DESCEND;
}

// Looks ahead at the key starting at ctx->pos and tells whether it's a real
// merge key, restoring ctx->pos. Every key form (implicit and explicit, block
// and flow) uses it before reading the key. It walks the whole anchor and tag
// chain, since "&y !!str <<" has a tag after its anchor: An untagged plain "<<"
// counts, and so does a plain or quoted "<<" with the core merge tag. in_flow
// picks flow_skip_ws() (which reads indent) or skip_inline_ws(). Whitespace
// after
// "<<" is over-approximated, which is safe because every caller checks the real
// key text; the ':' rule matches scan_plain_scalar_line() exactly. A failure in
// the look-ahead gives false and clears ctx->error, since the real parse
// reports it and a stale message would hide a later one.
static bool explicit_key_peek_is_merge_candidate(parse_ctx *ctx, int indent,
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
        ctx->error[0] = '\0';
        ctx->pos = saved_pos;
        return false;
      }
      ccol__mem_free(ctx->mp, tmp_anchor);
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
      ctx->error[0] = '\0';
      ctx->pos = saved_pos;
      return false;
    }
  }
  bool merge_tag = false;
  if (had_tag && !at_end(ctx) &&
      (cur(ctx) == '<' || cur(ctx) == '"' || cur(ctx) == '\'')) {
    size_t key_pos = ctx->pos;
    char *tag = NULL;
    ctx->pos = tag_pos;
    if (parse_tag_token(ctx, &tag)) {
      merge_tag = tag && strcmp(tag, CYAML_TAG_MERGE) == 0;
      ccol__mem_free(ctx->mp, tag);
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
    char *text = NULL;
    bool ok = cur(ctx) == '"' ? parse_double_quoted(ctx, &text)
                              : parse_single_quoted(ctx, &text);
    is_candidate = ok && strcmp(text, "<<") == 0;
    if (ok) ccol__mem_free(ctx->mp, text);
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

// Finishes an explicit "? key" entry once its key node is in: Builds the
// canonical key text, then looks for the ':' on a later line at exactly
// map_indent (no tab before it, and a ':' that isn't followed by whitespace
// starts a plain key instead). The key node is a descent, so pode_begin() and
// this run on either side of it. Gives 1 on success, -1 on error.
static int pode_finish(parse_ctx *ctx, int map_indent, cyaml_node *key_node,
                       bool is_merge_candidate, char **key_out,
                       bool *have_colon_out, bool *is_explicit_out,
                       bool *is_merge_candidate_out) {
  if (!key_node) return -1;
  char *key_str = node_to_dict_key_string(ctx, key_node, "block dictionary");
  if (!key_str) return -1;

  skip_ws_comments(ctx);
  bool have_colon = false;
  if (!at_end(ctx) && !at_doc_marker(ctx)) {
    if (line_indent_has_tab(ctx)) {
      parse_err(ctx,
                "tab cannot be used as block mapping indentation at "
                "position %zu",
                ctx->pos);
      ccol__mem_free(ctx->mp, key_str);
      return -1;
    }
    if (current_col(ctx) == map_indent && cur(ctx) == ':') {
      if (ctx->pos + 1 >= ctx->len) {
        have_colon = true;
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

// Reads the key of one block-mapping entry. Gives 1 for a key, 0 when the
// position isn't a key (the mapping ends), -1 on error, and 2 for an explicit
// "? key" whose node must be read first; the request is filled in and the
// caller resumes through pode_finish(). A '?' or '-' followed by text starts a
// plain key, and an implicit key may carry an anchor and a tag (read through
// try_parse_scalar_dict_key()).
static int pode_begin(parse_ctx *ctx, int map_indent, char **key_out,
                      bool *have_colon_out, bool *is_explicit_out,
                      bool *is_merge_candidate_out, preq *rq) {
  char c = cur(ctx);
  *is_merge_candidate_out = false;

  bool is_explicit_key_indicator = false;
  if (c == '?') {
    if (ctx->pos + 1 < ctx->len) {
      char next = ctx->src[ctx->pos + 1];
      if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
        is_explicit_key_indicator = true;
    } else {
      is_explicit_key_indicator = true;
    }
  }

  if (is_explicit_key_indicator) {
    ctx->pos++;

    if (!at_end(ctx) && cur(ctx) == ' ') ctx->pos++;

    cyaml_node *key_node = NULL;
    switch (pbmn_begin(ctx, map_indent, true, is_merge_candidate_out, &key_node,
                       rq)) {
      case PBMN_ERROR:
        return -1;
      case PBMN_DESCEND:
        return 2;
      case PBMN_NODE:
        break;
    }
    return pode_finish(ctx, map_indent, key_node, *is_merge_candidate_out,
                       key_out, have_colon_out, is_explicit_out,
                       is_merge_candidate_out);
  }

  if (c == '-') {
    char nx = (ctx->pos + 1 < ctx->len) ? ctx->src[ctx->pos + 1] : '\0';
    if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0')
      return 0;
  } else if (c == '|' || c == '>' || c == '#') {
    return 0;
  }

  {
    char *anchor_name = NULL;
    char *key_str = NULL;
    cyaml_node *anchor_value = NULL;
    if (!try_parse_scalar_dict_key(ctx, &anchor_name, &key_str,
                                   is_merge_candidate_out, &anchor_value))
      return ctx->error[0] ? -1 : 0;

    if (!register_key_anchor(ctx, anchor_name, key_str, anchor_value)) {
      ccol__mem_free(ctx->mp, key_str);
      return -1;
    }

    *key_out = key_str;
    *have_colon_out = true;
    *is_explicit_out = false;
  }
  return 1;
}

// Inserts a copy of val into target under a key target doesn't hold, for a
// merged member. The copy and its entry are amplification, so both are charged
// to the amplification budget. false with ctx->error set on failure.
static bool merge_insert_copy(parse_ctx *ctx, cyaml_node *target,
                              const char *key, cyaml_node *val) {
  parse_amp_enter();
  cyaml clone = cyaml_clone((cyaml)val);
  bool ok =
      clone && dictionary_set_impl((cyaml)target, key, clone) == ccol_success;
  parse_amp_leave();
  if (ok) return true;
  if (parse_amp_limit_hit)
    parse_err(ctx,
              "merge key expansion exceeded the %zu-node, %zu-byte alias "
              "expansion memory limit at position %zu",
              (size_t)CYAML_PARSE_NODE_FLOOR, (size_t)CYAML_PARSE_BYTE_FLOOR,
              ctx->pos);
  else if (parse_node_budget_exhausted)
    parse_err(ctx,
              "merge key expansion exceeded the %zu node-allocation "
              "limit at position %zu",
              parse_node_limit, ctx->pos);
  else if (parse_byte_budget_exhausted)
    parse_err(ctx,
              "merge key expansion exceeded the %zu-byte document memory "
              "limit at position %zu",
              parse_byte_limit, ctx->pos);
  else
    parse_err(ctx, "out of memory expanding merge key at position %zu",
              ctx->pos);
  return false;
}

// Merges the members of one source mapping into target in the source's order,
// for merge_apply(), skipping keys target already holds. Values are copied,
// since they belong to the source's tree. false with ctx->error set on failure.
static bool merge_one_source_into(parse_ctx *ctx, cyaml_node *target,
                                  cyaml_node *source) {
  for (const ccol_chmap_entry_ref *e = dict_first_entry(source); e;) {
    const cmap_pair *kp, *vp;
    e = ccol_chmap_entry_read(e, &kp, &vp);
    const char *key = (const char *)kp->ptr;
    if (cyaml_dictionary_get((cyaml)target, key)) continue;
    if (!merge_insert_copy(ctx, target, key, cyaml_read_child(vp->ptr)))
      return false;
  }
  return true;
}

// Moves count members of map, starting at first, to the end of the member order
// by deleting and inserting each again; merge_reorder() needs it to place
// merged members. The key is copied first (stack buffer, or one allocation for
// a long key), since the delete frees it. A member the insert refuses is
// destroyed and the parse fails.
static bool merge_move_to_end(parse_ctx *ctx, cyaml_node *map,
                              const ccol_chmap_entry_ref *first, size_t count) {
  const ccol_chmap_entry_ref *e = first;
  for (size_t i = 0; i < count && e; i++) {
    const cmap_pair *kp, *vp;
    const ccol_chmap_entry_ref *next = ccol_chmap_entry_read(e, &kp, &vp);
    cyaml_node *child = cyaml_read_child(vp->ptr);
    char stack_key[256];
    char *key = stack_key;
    size_t key_size = kp->size;
    if (key_size > sizeof(stack_key)) {
      key = ccol__mem_alloc(ctx->mp, key_size);
      if (!key) {
        parse_err(ctx, "out of memory expanding merge key at position %zu",
                  ctx->pos);
        return false;
      }
    }
    memcpy(key, kp->ptr, key_size);
    cmap_pair key_pair = {.ptr = key, .size = key_size};
    cmap_pair val_pair = {.ptr = &child, .size = sizeof(child)};
    ccol_retval r = chashmap_remove_elem(map->value.dictionary, &key_pair);
    if (r == ccol_success)
      r = chashmap_insert_elem(map->value.dictionary, &key_pair, &val_pair);
    if (key != stack_key) ccol__mem_free(ctx->mp, key);
    if (r != ccol_success) {
      cyaml__destroy((cyaml)child);
      parse_err(ctx, "out of memory expanding merge key at position %zu",
                ctx->pos);
      return false;
    }
    e = next;
  }
  return true;
}

// Gives how many sources one merge entry names (the length of its sequence, or
// 1 for a single mapping), so merge_apply() can treat both shapes alike.
static size_t merge_source_count(const cyaml_node *val) {
  return val->type == CYAML_LIST ? cvector_size(val->value.list) : 1;
}

// Gives source i of one merge entry, the counterpart of merge_source_count().
static cyaml_node *merge_source_at(cyaml_node *val, size_t i) {
  if (val->type != CYAML_LIST) return val;
  return *(cyaml_node **)cvector_at(val->value.list, i);
}

// One segment that merge_reorder() moves: The first explicit member after a
// merge entry, and the first member and count of what that entry placed.
typedef struct {
  const ccol_chmap_entry_ref *explicit_first;
  const ccol_chmap_entry_ref *merged_first;
  size_t merged_count;
} cyaml_merge_seg;

// Puts the members of map in their final order after merge_apply() placed them.
// On entry the explicit members come first, then what each merge entry placed;
// afterwards the members of entry i sit between the explicit members before and
// after that entry. Only out-of-place segments move, found in one walk (entries
// keep their addresses while other members move). false with ctx->error set on
// OOM.
static bool merge_reorder(parse_ctx *ctx, cyaml_node *map,
                          const cyaml_merge_entry *entries,
                          cyaml_merge_seg *seg, size_t n,
                          size_t explicit_count) {
  size_t k = 0;
  while (k < n) {
    size_t end = k + 1 < n ? entries[k + 1].pos : explicit_count;
    if (end > entries[k].pos) break;
    k++;
  }
  if (k == n) return true;

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

// Expands the merge entries of a mapping once it's read; the frames keep every
// "<<" entry aside, so map holds only explicit members. The caller keeps the
// values. Precedence follows PyYAML: Explicit members win, a later merge entry
// beats an earlier one, and inside one entry an earlier source wins. With
// several entries, a temporary map of winning values is built first and the
// members are placed in document order after. A source mapping's own merges
// were expanded when it was read, so nesting needs nothing special. false with
// ctx->error set for a source that isn't a mapping, or on OOM.
static bool merge_apply(parse_ctx *ctx, cyaml_node *map,
                        const cyaml_merge_entry *entries, size_t n) {
  for (size_t i = 0; i < n; i++) {
    cyaml_node *val = entries[i].val;
    size_t cnt = merge_source_count(val);
    for (size_t j = 0; j < cnt; j++) {
      cyaml_node *src = merge_source_at(val, j);
      if (!src || src->type != CYAML_DICTIONARY) {
        parse_err(ctx, "merge key source is not a mapping at position %zu",
                  ctx->pos);
        return false;
      }
    }
  }
  size_t explicit_count = chashmap_size(map->value.dictionary);

  if (n == 1) {
    size_t cnt = merge_source_count(entries[0].val);
    for (size_t j = 0; j < cnt; j++)
      if (!merge_one_source_into(ctx, map, merge_source_at(entries[0].val, j)))
        return false;
    cyaml_merge_seg seg = {
        .merged_count = chashmap_size(map->value.dictionary) - explicit_count};
    return merge_reorder(ctx, map, entries, &seg, 1, explicit_count);
  }

  char *err = NULL;
  chmap winners =
      ccol_chmap_create_compact(ccol_string, ccol_pointer, ctx->mp, &err);
  cyaml_merge_seg *seg = ccol__mem_alloc(ctx->mp, n * sizeof(*seg));
  bool ok = winners && seg;
  if (!ok)
    parse_err(ctx, "out of memory expanding merge key at position %zu",
              ctx->pos);
  for (size_t i = n; ok && i-- > 0;) {
    cyaml_node *val = entries[i].val;
    size_t cnt = merge_source_count(val);
    for (size_t j = 0; ok && j < cnt; j++) {
      cyaml_node *src = merge_source_at(val, j);
      for (const ccol_chmap_entry_ref *e = dict_first_entry(src); e;) {
        const cmap_pair *kp, *vp;
        cmap_pair slot;
        e = ccol_chmap_entry_read(e, &kp, &vp);
        if (chashmap_get_elem_ref(map->value.dictionary, kp, &slot) ==
            ccol_success)
          continue;
        cyaml_node *child = cyaml_read_child(vp->ptr);
        cmap_pair val_pair = {.ptr = &child, .size = sizeof(child)};
        ccol_retval r =
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
    cyaml_node *val = entries[i].val;
    size_t cnt = merge_source_count(val);
    size_t before = chashmap_size(map->value.dictionary);
    for (size_t j = 0; ok && j < cnt; j++) {
      cyaml_node *src = merge_source_at(val, j);
      for (const ccol_chmap_entry_ref *e = dict_first_entry(src); e;) {
        const cmap_pair *kp, *vp;
        cmap_pair slot;
        e = ccol_chmap_entry_read(e, &kp, &vp);
        cyaml_node *none = NULL;
        cmap_pair none_pair = {.ptr = &none, .size = sizeof(none)};
        ccol_retval r =
            ccol_chmap_insert_or_get_elem(winners, kp, &none_pair, &slot);
        if (r == ccol_success) continue;
        if (r != ccol_key_already_present) {
          parse_err(ctx, "out of memory expanding merge key at position %zu",
                    ctx->pos);
          ok = false;
          break;
        }
        cyaml_node *win = cyaml_read_child(slot.ptr);
        if (!win) continue;
        memcpy(slot.ptr, &none, sizeof(none));
        if (!merge_insert_copy(ctx, map, (const char *)kp->ptr, win)) {
          ok = false;
          break;
        }
      }
    }
    if (ok) seg[i].merged_count = chashmap_size(map->value.dictionary) - before;
  }
  if (ok) ok = merge_reorder(ctx, map, entries, seg, n, explicit_count);
  if (winners) chashmap__destroy(winners);
  ccol__mem_free(ctx->mp, seg);
  return ok;
}

// Keeps one merge entry of a mapping aside until the mapping ends, so merge
// entries never replace each other in the map. It takes ownership of val; a
// failure destroys val and sets ctx->error.
static bool merge_entry_push(parse_ctx *ctx, cyaml_merge_entry **arr,
                             size_t *count, size_t *cap, cyaml_node *val,
                             size_t pos) {
  if (*count == *cap) {
    size_t new_cap = *cap ? *cap * 2 : 2;
    cyaml_merge_entry *grown =
        ccol__mem_realloc(ctx->mp, *arr, new_cap * sizeof(*grown));
    if (!grown) {
      cyaml__destroy((cyaml)val);
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

// Destroys the merge entries a frame kept aside and the array, when the
// mapping fails. A value may hold anchored nodes ("<<: &b {x: 1}"), so it goes
// through parse_discard().
static void merge_entries_free(parse_ctx *ctx, cyaml_merge_entry **arr,
                               size_t *count, size_t *cap) {
  for (size_t i = 0; i < *count; i++) parse_discard((*arr)[i].val);
  ccol__mem_free(ctx->mp, *arr);
  *arr = NULL;
  *count = 0;
  *cap = 0;
}

// Expands the merge entries a frame kept aside into map when the mapping ends,
// then destroys them whatever the result. Gives what merge_apply() gives.
static bool merge_entries_finish(parse_ctx *ctx, cyaml_node *map,
                                 cyaml_merge_entry **arr, size_t *count,
                                 size_t *cap) {
  bool ok = merge_apply(ctx, map, *arr, *count);
  merge_entries_free(ctx, arr, count, cap);
  return ok;
}

// Runs a block-mapping frame. The request gives map_indent and maybe a first
// key that the requesting frame already read (it owns that key and stays below
// this frame); first_key_colon_consumed says whether that key's ':' is read
// too, and first_key_is_merge_candidate whether it's a merge key. Without a
// first key the mapping starts with a "? key" entry. Explicit keys and every
// value are descents. A tab before a key on its line is refused, since the
// column would be undefined; an explicit key without ':' has a null value;
// merge entries wait in f->u.bd.merges until the mapping ends; each value is
// stored with dictionary_set_impl().
static pstep pbd_step(parse_ctx *ctx, pframe *f, preq *rq) {
  const int map_indent = f->u.bd.map_indent;
  cyaml_node *val = NULL;
  int r = 0;

  switch (f->state) {
    case BDS_AFTER_FIRST_KEY_NODE:
      goto after_first_key_node;
    case BDS_AFTER_KEY_NODE:
      goto after_key_node;
    case BDS_AFTER_VALUE:
      goto after_value;
    default:
      break;
  }

  {
    size_t ls = line_start_pos(ctx);
    size_t span = ctx->pos - ls;
    if (map_indent >= 0 && (size_t)map_indent < span) span = (size_t)map_indent;
    if (memchr(ctx->src + ls, '\t', span) != NULL) {
      parse_err(ctx,
                "tab cannot be used as block mapping indentation at "
                "position %zu",
                ls + span);
      return PSTEP_FAIL;
    }
  }

  f->u.bd.map = (cyaml_node *)cyaml_create_dictionary_interned(ctx->mp);
  if (!f->u.bd.map) return PSTEP_FAIL;

  if (f->u.bd.key != NULL) {
    if (f->u.bd.have_colon) {
    } else if (at_end(ctx) || cur(ctx) != ':') {
      parse_err(ctx, "expected ':' after dictionary key at position %zu",
                ctx->pos);
      return PSTEP_FAIL;
    } else {
      ctx->pos++;
      f->u.bd.have_colon = true;
    }
  } else {
    r = pode_begin(ctx, map_indent, &f->u.bd.key_owned, &f->u.bd.have_colon,
                   &f->u.bd.is_explicit, &f->u.bd.entry_is_merge_candidate, rq);
    if (r == 2) {
      f->state = BDS_AFTER_FIRST_KEY_NODE;
      return PSTEP_DESCEND;
    }
    goto first_key_done;
  after_first_key_node:
    r = pode_finish(ctx, map_indent, f->child, f->u.bd.entry_is_merge_candidate,
                    &f->u.bd.key_owned, &f->u.bd.have_colon,
                    &f->u.bd.is_explicit, &f->u.bd.entry_is_merge_candidate);
    f->child = NULL;
  first_key_done:
    if (r <= 0) {
      if (r == 0)
        parse_err(ctx, "expected block dictionary key at position %zu",
                  ctx->pos);
      return PSTEP_FAIL;
    }
    f->u.bd.key = f->u.bd.key_owned;
  }

  while (1) {
    if (f->u.bd.have_colon && !at_end(ctx) && cur(ctx) == ' ') ctx->pos++;

    if (f->u.bd.have_colon) {
      switch (
          pbmn_begin(ctx, map_indent, f->u.bd.is_explicit, NULL, &val, rq)) {
        case PBMN_ERROR:
          val = NULL;
          goto have_value;
        case PBMN_NODE:
          goto have_value;
        case PBMN_DESCEND:
          f->state = BDS_AFTER_VALUE;
          return PSTEP_DESCEND;
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
      if (!ctx->error[0])
        parse_err(ctx, "out of memory parsing dictionary value at position %zu",
                  ctx->pos);
      return PSTEP_FAIL;
    }

    if (f->u.bd.entry_is_merge_candidate && strcmp(f->u.bd.key, "<<") == 0) {
      if (!merge_entry_push(ctx, &f->u.bd.merges, &f->u.bd.merge_count,
                            &f->u.bd.merge_cap, val,
                            chashmap_size(f->u.bd.map->value.dictionary)))
        return PSTEP_FAIL;
    } else if (dictionary_set_impl((cyaml)f->u.bd.map, f->u.bd.key,
                                   (cyaml)val) != ccol_success) {
      return PSTEP_FAIL;
    }
    ccol__mem_free(ctx->mp, f->u.bd.key_owned);
    f->u.bd.key_owned = NULL;
    f->u.bd.key = NULL;

    skip_ws_comments(ctx);
    if (at_end(ctx)) break;
    if (at_doc_marker(ctx)) break;
    if (at_col0_bom(ctx)) break;
    if (line_indent_has_tab(ctx)) {
      parse_err(ctx,
                "tab cannot be used as block mapping indentation at "
                "position %zu",
                ctx->pos);
      return PSTEP_FAIL;
    }
    if (current_col(ctx) != map_indent) break;

    r = pode_begin(ctx, map_indent, &f->u.bd.key_owned, &f->u.bd.have_colon,
                   &f->u.bd.is_explicit, &f->u.bd.entry_is_merge_candidate, rq);
    if (r == 2) {
      f->state = BDS_AFTER_KEY_NODE;
      return PSTEP_DESCEND;
    }
    goto next_key_done;
  after_key_node:
    r = pode_finish(ctx, map_indent, f->child, f->u.bd.entry_is_merge_candidate,
                    &f->u.bd.key_owned, &f->u.bd.have_colon,
                    &f->u.bd.is_explicit, &f->u.bd.entry_is_merge_candidate);
    f->child = NULL;
  next_key_done:
    if (r < 0) return PSTEP_FAIL;
    if (r == 0) break;
    f->u.bd.key = f->u.bd.key_owned;
  }

  if (f->u.bd.merge_count &&
      !merge_entries_finish(ctx, f->u.bd.map, &f->u.bd.merges,
                            &f->u.bd.merge_count, &f->u.bd.merge_cap))
    return PSTEP_FAIL;

  f->result = f->u.bd.map;
  f->u.bd.map = NULL;
  return PSTEP_DONE;
}

// Initializes a frame from the request that asked for it, as the driver pushes
// it. A collection frame takes one ctx->depth level for as long as it's on the
// stack, which is where the nesting limit is enforced; a node frame takes none.
// false, with the error set and nothing pushed, when the level would pass the
// limit. Only the fields a kind reads before writing are cleared.
static bool pframe_open(parse_ctx *ctx, pframe *f, const preq *rq) {
  f->kind = rq->kind;
  f->state = 0;
  f->child = NULL;
  f->result = NULL;
  if (rq->kind == PFK_NODE) {
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
  if (ctx->depth >= CYAML_MAX_NESTING_DEPTH) {
    parse_depth_err(ctx);
    return false;
  }
  if (++ctx->depth > ctx->peak_depth) ctx->peak_depth = ctx->depth;
  switch (rq->kind) {
    case PFK_NODE:
      break;
    case PFK_FLOW_LIST:
      f->u.fl.indent = rq->indent;
      f->u.fl.seq = NULL;
      f->u.fl.key_str = NULL;
      break;
    case PFK_FLOW_DICT:
      f->u.fd.indent = rq->indent;
      f->u.fd.map = NULL;
      f->u.fd.key_str = NULL;
      f->u.fd.merges = NULL;
      f->u.fd.merge_count = 0;
      f->u.fd.merge_cap = 0;
      break;
    case PFK_BLOCK_LIST:
      f->u.bl.seq_indent = rq->indent;
      f->u.bl.seq = NULL;
      break;
    case PFK_BLOCK_DICT:
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

// Frees everything a frame still owns, for a failed parse: The driver sweeps
// every frame left on the stack, since a container under construction belongs
// only to its frame. It gives back the depth level of a collection frame, and
// of a flow pair whose value was being read.
static void pframe_cleanup(parse_ctx *ctx, pframe *f) {
  if (f->child) {
    cyaml__destroy((cyaml)f->child);
    f->child = NULL;
  }
  switch (f->kind) {
    case PFK_NODE:
      ccol__mem_free(ctx->mp, f->u.n.name);
      ccol__mem_free(ctx->mp, f->u.n.tag);
      ccol__mem_free(ctx->mp, f->u.n.pending_key);
      f->u.n.name = NULL;
      f->u.n.tag = NULL;
      f->u.n.pending_key = NULL;
      return;
    case PFK_FLOW_LIST:
      cyaml__destroy((cyaml)f->u.fl.seq);
      f->u.fl.seq = NULL;
      if (f->u.fl.key_str) {
        ccol__mem_free(ctx->mp, f->u.fl.key_str);
        f->u.fl.key_str = NULL;
        ctx->depth--;
      }
      break;
    case PFK_FLOW_DICT:
      cyaml__destroy((cyaml)f->u.fd.map);
      f->u.fd.map = NULL;
      ccol__mem_free(ctx->mp, f->u.fd.key_str);
      f->u.fd.key_str = NULL;
      merge_entries_free(ctx, &f->u.fd.merges, &f->u.fd.merge_count,
                         &f->u.fd.merge_cap);
      break;
    case PFK_BLOCK_LIST:
      cyaml__destroy((cyaml)f->u.bl.seq);
      f->u.bl.seq = NULL;
      break;
    case PFK_BLOCK_DICT:
      cyaml__destroy((cyaml)f->u.bd.map);
      f->u.bd.map = NULL;
      ccol__mem_free(ctx->mp, f->u.bd.key_owned);
      f->u.bd.key_owned = NULL;
      f->u.bd.key = NULL;
      merge_entries_free(ctx, &f->u.bd.merges, &f->u.bd.merge_count,
                         &f->u.bd.merge_cap);
      break;
  }
  ctx->depth--;
}

// Releases a finished frame, which has handed over its node and owns nothing
// else, by giving back a collection frame's depth level. Test builds assert
// that the frame holds nothing.
static void pframe_release(parse_ctx *ctx, pframe *f) {
#ifdef RUNNING_UNIT_TESTS
  ccol__assert(f->child == NULL);
  switch (f->kind) {
    case PFK_NODE:
      ccol__assert(f->u.n.name == NULL && f->u.n.tag == NULL &&
                   f->u.n.pending_key == NULL);
      break;
    case PFK_FLOW_LIST:
      ccol__assert(f->u.fl.seq == NULL && f->u.fl.key_str == NULL);
      break;
    case PFK_FLOW_DICT:
      ccol__assert(f->u.fd.map == NULL && f->u.fd.key_str == NULL &&
                   f->u.fd.merges == NULL);
      break;
    case PFK_BLOCK_LIST:
      ccol__assert(f->u.bl.seq == NULL);
      break;
    case PFK_BLOCK_DICT:
      ccol__assert(f->u.bd.map == NULL && f->u.bd.key_owned == NULL &&
                   f->u.bd.merges == NULL);
      break;
  }
#endif
  if (f->kind != PFK_NODE) ctx->depth--;
}

// Runs one step of the frame's state machine, picked by its kind; node frames
// are tested first since they're half of all steps.
static pstep pframe_step(parse_ctx *ctx, pframe *f, preq *rq) {
  if (f->kind == PFK_NODE) return pni_step(ctx, f, rq);
  switch (f->kind) {
    case PFK_NODE:
      break;
    case PFK_FLOW_LIST:
      return pfl_step(ctx, f, rq);
    case PFK_FLOW_DICT:
      return pfd_step(ctx, f, rq);
    case PFK_BLOCK_LIST:
      return pbl_step(ctx, f, rq);
    case PFK_BLOCK_DICT:
      return pbd_step(ctx, f, rq);
  }
  return PSTEP_FAIL;
}

// Runs one complete walk from root_req and gives the node it made, or NULL on
// failure (ctx->error is set unless it was a bare OOM). Every parse of a node
// goes through here. The frame stack starts in locals and moves to the heap
// past CYAML_PARSE_INLINE_FRAMES, never past CYAML_PARSE_MAX_FRAMES. A finished
// node goes straight into the child slot of the frame below, so on failure only
// the frames need sweeping. The walk leaves ctx->depth as it found it, which
// lets a speculative key parse run its own walk; test builds check that.
static cyaml_node *parse_drive(parse_ctx *ctx, const preq *root_req) {
  pframe inline_frames[CYAML_PARSE_INLINE_FRAMES];
  pframe *stack = inline_frames;
  size_t cap = CYAML_PARSE_INLINE_FRAMES;
  size_t sp = 0;
  cyaml_node *result = NULL;
  bool failed = true;
  preq req = *root_req;
#ifdef RUNNING_UNIT_TESTS
  const size_t entry_depth = ctx->depth;
#endif

  if (pframe_open(ctx, &stack[0], &req)) {
    sp = 1;
    failed = false;
    pframe *f = stack;
    for (;;) {
      pstep r = pframe_step(ctx, f, &req);
      if (r == PSTEP_DESCEND) {
        if (sp == cap) {
          if (cap >= CYAML_PARSE_MAX_FRAMES) {
            parse_depth_err(ctx);
            failed = true;
            break;
          }
          size_t want = cap * 2 < CYAML_PARSE_MAX_FRAMES
                            ? cap * 2
                            : (size_t)CYAML_PARSE_MAX_FRAMES;
          pframe *grown = walk_stack_grow_to(stack, &cap, want, inline_frames,
                                             sizeof(*stack), ctx->mp);
          if (!grown) {
            failed = true;
            break;
          }
          stack = grown;
          f = stack + (sp - 1);
        }
        if (!pframe_open(ctx, f + 1, &req)) {
          failed = true;
          break;
        }
        sp++;
        f++;
        continue;
      }
      if (r == PSTEP_FAIL) {
        failed = true;
        break;
      }
      {
        cyaml_node *done_node = f->result;
        f->result = NULL;
        pframe_release(ctx, f);
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
    while (sp > 0) {
      sp--;
      pframe_cleanup(ctx, &stack[sp]);
    }
    result = NULL;
  }
  if (stack != inline_frames) ccol__mem_free(ctx->mp, stack);

#ifdef RUNNING_UNIT_TESTS
  if (cap > cyaml_test_peak_parse_frame_cap)
    cyaml_test_peak_parse_frame_cap = cap;
  ccol__assert(ctx->depth == entry_depth);
#endif
  return result;
}

// Reads one directive line from its '%'. A directive needs a name; unknown
// names are accepted and ignored. "%YAML" needs exactly one MAJOR.MINOR with
// major 1, and "%TAG" a valid handle and a prefix of ns-uri-char (a '#' is part
// of it, and escapes are decoded); after either only whitespace and a comment
// may follow, and a handle defined twice is an error. *is_yaml_directive_out
// (may be NULL) tells the caller it was "%YAML", so the caller can refuse a
// second one in the same document. false with ctx->error set on a malformed
// line.
static bool parse_directive_line(parse_ctx *ctx, bool *is_yaml_directive_out) {
  size_t start = ctx->pos;
  ctx->pos++;
  size_t name_start = ctx->pos;
  while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
         cur(ctx) != '\n' && cur(ctx) != '\r')
    ctx->pos++;
  size_t name_len = ctx->pos - name_start;
  if (name_len == 0) {
    parse_err(ctx, "directive with no name at position %zu", start);
    return false;
  }
  bool is_yaml = name_len == 4 && memcmp(ctx->src + name_start, "YAML", 4) == 0;
  bool is_tag = name_len == 3 && memcmp(ctx->src + name_start, "TAG", 3) == 0;
  if (is_yaml_directive_out) *is_yaml_directive_out = is_yaml;

  if (is_tag) {
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
    char *handle = ccol__mem_alloc(ctx->mp, handle_len + 1);
    if (!handle) return false;
    memcpy(handle, ctx->src + handle_start, handle_len);
    handle[handle_len] = '\0';

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
                cyaml_echo(handle), start);
      ccol__mem_free(ctx->mp, handle);
      return false;
    }

    if (at_end(ctx) || (cur(ctx) != ' ' && cur(ctx) != '\t')) {
      parse_err(ctx, "malformed %%TAG directive at position %zu", start);
      ccol__mem_free(ctx->mp, handle);
      return false;
    }
    skip_inline_ws(ctx);

    size_t prefix_start = ctx->pos;
    while (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
           cur(ctx) != '\n' && cur(ctx) != '\r') {
      ctx->pos++;
    }
    size_t prefix_len = ctx->pos - prefix_start;
    if (prefix_len == 0) {
      parse_err(ctx, "malformed %%TAG prefix at position %zu", start);
      ccol__mem_free(ctx->mp, handle);
      return false;
    }
    char *prefix = ccol__mem_alloc(ctx->mp, prefix_len + 1);
    if (!prefix) {
      ccol__mem_free(ctx->mp, handle);
      return false;
    }
    memcpy(prefix, ctx->src + prefix_start, prefix_len);
    prefix[prefix_len] = '\0';
    if (prefix[0] == ',' || prefix[0] == '[' || prefix[0] == ']') {
      parse_err(ctx,
                "character '%s' at position %zu cannot start a %%TAG prefix",
                cyaml_echo_c(prefix[0]), prefix_start);
      ccol__mem_free(ctx->mp, handle);
      ccol__mem_free(ctx->mp, prefix);
      return false;
    }
    if (!tag_text_check(ctx, prefix, prefix_len, prefix_start, false) ||
        !percent_decode_tag_inplace(ctx, prefix, prefix_start)) {
      ccol__mem_free(ctx->mp, handle);
      ccol__mem_free(ctx->mp, prefix);
      return false;
    }

    if (!at_end(ctx) && cur(ctx) != ' ' && cur(ctx) != '\t' &&
        cur(ctx) != '\n' && cur(ctx) != '\r' && cur(ctx) != '#') {
      parse_err(ctx, "extra content after %%TAG prefix at position %zu",
                ctx->pos);
      ccol__mem_free(ctx->mp, handle);
      ccol__mem_free(ctx->mp, prefix);
      return false;
    }
    skip_inline_ws(ctx);
    if (!at_end(ctx) && cur(ctx) != '\n' && cur(ctx) != '\r' &&
        cur(ctx) != '#') {
      parse_err(ctx, "extra content after %%TAG prefix at position %zu",
                ctx->pos);
      ccol__mem_free(ctx->mp, handle);
      ccol__mem_free(ctx->mp, prefix);
      return false;
    }

    ccol_retval set_rv = tag_handles_set(ctx, handle, prefix);
    ccol__mem_free(ctx->mp, handle);
    ccol__mem_free(ctx->mp, prefix);
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
    if (at_end(ctx) || (cur(ctx) != ' ' && cur(ctx) != '\t')) {
      parse_err(ctx, "malformed %%YAML directive at position %zu", start);
      return false;
    }
    skip_inline_ws(ctx);
    size_t ver_start = ctx->pos;
    while (!at_end(ctx) && cur(ctx) >= '0' && cur(ctx) <= '9') ctx->pos++;
    bool has_major = ctx->pos > ver_start;
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

// Reads a "---" the caller has confirmed, with the separator whitespace (tabs
// allowed) and newline after it, and tells through *same_line_content_out
// whether content follows on the same line.
static void consume_doc_start_marker(parse_ctx *ctx,
                                     bool *same_line_content_out) {
  ctx->pos += 3;
  skip_inline_ws(ctx);
  *same_line_content_out = !rest_of_line_is_blank(ctx);
  if (!at_end(ctx) && at_eol(ctx)) skip_newline(ctx);
  skip_ws_comments(ctx);
}

// Skips a byte-order mark at the start of a line between documents, which
// YAML 1.2 allows before every document (concatenated files). The mark isn't
// content, so first_line_start, the line cache and the memo restart after it;
// otherwise every column on the line would be off by 3. Gives true when it
// skipped one.
static bool skip_document_prefix_bom(parse_ctx *ctx) {
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

// Parses one document: Optional directives (each needs a "---" after it), an
// optional "---", one root node, and an optional "...". Content on the same
// line as "---" may only be a flow node, and a '%' after this document's own
// "---" belongs to the next document. On success ctx->pos is past the document
// and *consumed_end_marker tells whether a "..." was read, which the next
// document needs to leave out its own "---". The anchor and %TAG tables are
// always destroyed before it returns. NULL with ctx->error set on failure.
static cyaml_node *parse_one_document(parse_ctx *ctx,
                                      bool *consumed_end_marker) {
  bool doc_marker_same_line_content = false;

  bool consumed_leading_dashes = false;

  if (at_doc_marker(ctx) && memcmp(ctx->src + ctx->pos, "---", 3) == 0) {
    consume_doc_start_marker(ctx, &doc_marker_same_line_content);
    consumed_leading_dashes = true;
  }

  bool saw_directive = false;
  bool saw_yaml_directive = false;
  bool consumed_doc_start_after_directive = false;
  while (!consumed_leading_dashes && !at_end(ctx) && cur(ctx) == '%') {
    saw_directive = true;
    bool is_yaml_directive = false;
    if (!parse_directive_line(ctx, &is_yaml_directive)) {
      anchors_destroy(ctx);
      tag_handles_destroy(ctx);
      return NULL;
    }
    if (is_yaml_directive) {
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

  cyaml_node *root;
  if (at_end(ctx) || at_doc_marker(ctx) ||
      (current_col(ctx) == 0 && cur(ctx) == '%')) {
    root = node_alloc(CYAML_NULL, ctx->mp);
  } else if (doc_marker_same_line_content && at_bare_seq_indicator(ctx)) {
    parse_err(ctx,
              "a block sequence cannot start on the same line as "
              "its document start marker at position %zu",
              ctx->pos);
    root = NULL;
  } else {
    root = parse_node(ctx, -1, false, false, !doc_marker_same_line_content,
                      false, false, NULL);
  }

  anchors_destroy(ctx);
  tag_handles_destroy(ctx);

  if (!root) return NULL;

  skip_ws_comments(ctx);
  *consumed_end_marker = false;
  while (at_doc_marker(ctx) && memcmp(ctx->src + ctx->pos, "...", 3) == 0) {
    ctx->pos += 3;
    skip_inline_ws(ctx);
    if (!at_end(ctx) && !at_eol(ctx) && cur(ctx) != '#') {
      parse_err(ctx,
                "unexpected content after '...' document end marker "
                "at position %zu",
                ctx->pos);
      cyaml__destroy((cyaml)root);
      return NULL;
    }
    if (!at_end(ctx) && at_eol(ctx)) skip_newline(ctx);
    *consumed_end_marker = true;
    skip_ws_comments(ctx);
  }

  return root;
}

// The size of the per-thread buffer behind err_str. The err out-parameter of
// every module has one rule (the library owns the string, the caller never
// frees it), and this parser's messages are built at run time, so they live in
// a heap buffer per thread instead of being allocated per message. The text
// stays valid until the next failing parse on the same thread. A __thread array
// would make the TLS block too large for dlopen(). 512 is the size of
// parse_ctx.error, where every message is built.
#define CYAML_ERR_BUF_LEN 512
// The message used when the thread can't keep a buffer (OOM, or the pool key
// can't be armed so nothing would free it).
static const char cyaml_err_unstored[] =
    "parse failed; the error message could not be stored";

// Gives the calling thread's message buffer, allocated on first use, for
// cyaml_report_err(). NULL when the allocation fails or the pool key can't be
// armed, since only the key's thread-exit drain frees the buffer.
static char *cyaml_err_storage(void) {
#if defined(CCOL__EMULATE_DARWIN_TLS)
  ccol__call_once(pool_key_once, do_pool_key_init);
  cyaml_tls *t = cyaml_tls_get();
  if (!t) return NULL;
  if (!t->err_buf) t->err_buf = malloc(CYAML_ERR_BUF_LEN);
  return t->err_buf;
#else
  if (!cyaml_err_buf) {
    char *buf = malloc(CYAML_ERR_BUF_LEN);
    if (!buf) return NULL;
    ccol__call_once(pool_key_once, do_pool_key_init);
    pool_key_arm();
    if (!pool_key_armed) {
      free(buf);
      return NULL;
    }
    cyaml_err_buf = buf;
  }
  return cyaml_err_buf;
#endif
}

#ifdef RUNNING_UNIT_TESTS
// Test builds only: Ends the process when a parse message isn't printable
// ASCII, right where the message is reported, so the tests and fuzz targets
// catch an echo that was missed.
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

// Copies msg into the per-thread buffer and points *err_str at it, or at the
// fixed message when there is no buffer. Every failing parse reports through
// it. Does nothing for a NULL err_str.
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

// The body of every parse: Checks the whole input for c-printable UTF-8, arms
// the budgets, allocates the line cache (its failure fails the parse), skips
// BOMs of the first document prefix and parses each document. One document
// gives its root; several give a list of roots, where each document after the
// first needs "---" unless a "..." ended the previous one, and each starts at
// depth 1 (a first document at the limit is refused once a second follows). An
// empty input gives a null node. On failure *err_str gets the message (an error
// with no message is reported as a named budget limit or as OOM), every
// finished document is destroyed and NULL comes back. *err_str is NULL only on
// success. The budgets are disarmed on every exit.
static cyaml parse_common(const char *src, size_t len, char **err_str,
                          ccol_memmgmt_procs *mp) {
  if (!src) {
    cyaml_report_err(err_str, "null input");
    return NULL;
  }
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
  parse_ctx ctx = {.src = src,
                   .pos = 0,
                   .len = len,
                   .error = "",
                   .mp = mp,
                   .anchors = NULL,
                   .memo_pos = SIZE_MAX};
  parse_node_budget_arm(len);

  ctx.line_starts = ccol__mem_alloc(mp, 64 * sizeof(size_t));
  if (!ctx.line_starts) {
    cyaml_report_err(err_str, "out of memory");
    parse_node_budget_disarm();
    return NULL;
  }
  ctx.line_starts_cap = 64;
  ctx.line_starts[0] = 0;
  ctx.line_starts_len = 1;

  while (skip_document_prefix_bom(&ctx)) {
  }

#ifdef RUNNING_UNIT_TESTS
  if (cyaml_test_force_line_cache_disabled) ctx.line_cache_disabled = true;
#endif

  skip_ws_comments(&ctx);
  while (skip_document_prefix_bom(&ctx)) skip_ws_comments(&ctx);

  cyaml_node **docs = NULL;
  size_t ndocs = 0;
  size_t dcap = 0;
  bool prev_consumed_end_marker = false;
  size_t bom_pos = SIZE_MAX;

  size_t first_doc_peak = 0;

  while (!at_end(&ctx)) {
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
    cyaml_node *root = parse_one_document(&ctx, &consumed_end_marker);
    if (ndocs == 0) first_doc_peak = ctx.peak_depth;
    if (!root) {
      if (ctx.error[0]) {
        cyaml_report_err(err_str, ctx.error);
      } else {
        char buf[128];
        if (parse_node_budget_exhausted)
          snprintf(buf, sizeof(buf),
                   "document exceeded the %zu node-allocation limit at "
                   "position %zu",
                   parse_node_limit, ctx.pos);
        else if (parse_byte_budget_exhausted)
          snprintf(buf, sizeof(buf),
                   "document exceeded the %zu-byte memory limit at "
                   "position %zu",
                   parse_byte_limit, ctx.pos);
        else
          snprintf(buf, sizeof(buf),
                   "out of memory (unreported allocation failure) at "
                   "position %zu",
                   ctx.pos);
        cyaml_report_err(err_str, buf);
      }
      goto fail;
    }

    if (ndocs == dcap) {
      size_t new_cap = dcap ? dcap * 2 : 4;
      cyaml_node **nd =
          ccol__mem_realloc(mp, docs, new_cap * sizeof(cyaml_node *));
      if (!nd) {
        cyaml__destroy((cyaml)root);
        cyaml_report_err(err_str, "out of memory");
        goto fail;
      }
      docs = nd;
      dcap = new_cap;
    }
    docs[ndocs++] = root;
    prev_consumed_end_marker = consumed_end_marker;

    skip_ws_comments(&ctx);
    bom_pos = ctx.pos;
    if (skip_document_prefix_bom(&ctx)) {
      skip_ws_comments(&ctx);
      while (skip_document_prefix_bom(&ctx)) skip_ws_comments(&ctx);
    } else {
      bom_pos = SIZE_MAX;
    }
  }

  if (ndocs == 0) {
    ccol__mem_free(mp, docs);
    cyaml_node *empty = node_alloc(CYAML_NULL, mp);
    if (!empty) {
      cyaml_report_err(err_str, "out of memory");
      parse_node_budget_disarm();
      ccol__mem_free(mp, ctx.line_starts);
      return NULL;
    }
    if (err_str) *err_str = NULL;
    parse_node_budget_disarm();
    ccol__mem_free(mp, ctx.line_starts);
    return (cyaml)empty;
  }

  if (ndocs == 1) {
    cyaml_node *result = docs[0];
    ccol__mem_free(mp, docs);
    if (err_str) *err_str = NULL;
    parse_node_budget_disarm();
    ccol__mem_free(mp, ctx.line_starts);
    return (cyaml)result;
  }

  cyaml_node *list = (cyaml_node *)cyaml_create_list_interned(mp);
  if (!list) {
    cyaml_report_err(err_str, "out of memory");
    goto fail;
  }
  for (size_t i = 0; i < ndocs; i++) {
    cyaml_node *elem = docs[i];
    if (cvector_push_back(list->value.list, &elem) != ccol_success) {
      for (size_t j = i; j < ndocs; j++) cyaml__destroy((cyaml)docs[j]);
      ccol__mem_free(mp, docs);
      cyaml__destroy((cyaml)list);
      cyaml_report_err(err_str, "out of memory");
      parse_node_budget_disarm();
      ccol__mem_free(mp, ctx.line_starts);
      return NULL;
    }
    elem->attached = true;
  }
  ccol__mem_free(mp, docs);
  if (err_str) *err_str = NULL;
  parse_node_budget_disarm();
  ccol__mem_free(mp, ctx.line_starts);
  return (cyaml)list;

fail:
  parse_node_budget_disarm();
  for (size_t i = 0; i < ndocs; i++) cyaml__destroy((cyaml)docs[i]);
  ccol__mem_free(mp, docs);
  ccol__mem_free(mp, ctx.line_starts);
  return NULL;
}

// Runs parse_common() inside one C locale scope, so a float always reads with
// '.' whatever LC_NUMERIC the thread uses. Two uselocale() calls per parse and
// nothing per scalar.
static cyaml parse_in_c_locale(const char *src, size_t len, char **err_str,
                               ccol_memmgmt_procs *mp) {
  ccol_c_locale_scope scope = ccol_c_locale_enter();
  cyaml root = parse_common(src, len, err_str, mp);
  ccol_c_locale_leave(scope);
  return root;
}

// Parses a NUL-terminated YAML string with allocator mp (NULL for the default),
// the public entry point. mp is interned first. On failure *err_str (when not
// NULL) points at the per-thread message, which the caller never frees.
cyaml cyaml_parse_mp(const char *yaml_str, char **err_str,
                     ccol_memmgmt_procs *mp) {
  if (!yaml_str) return parse_common(NULL, 0, err_str, mp);
  return parse_in_c_locale(yaml_str, strlen(yaml_str), err_str, mp);
}

// Parses exactly len bytes of YAML, so the input needn't end with a NUL. The
// rest is as for cyaml_parse_mp().
cyaml cyaml_parse_n_mp(const char *yaml_str, size_t len, char **err_str,
                       ccol_memmgmt_procs *mp) {
  return parse_in_c_locale(yaml_str, len, err_str, mp);
}

// True when n's tag is a core tag that names n's type, so the serializer can
// leave it out: The output already reads back as that type. A mismatched core
// tag, which no public call can make, would go out verbatim instead of
// vanishing.
static bool tag_matches_node_type(const cyaml_node *n) {
  if (!n->tag) return false;
  cyaml_node_type expected;
  if (!core_tag_expected_type(n->tag, &expected)) return false;
  return n->type == expected;
}

// The bytes needs_quoting() inspects more closely: NUL, the refused controls
// and tab, line breaks, the indicators "?,[]{}:#", and the UTF-8 lead bytes
// 0xC2, 0xE2 and 0xEF. Every other byte costs one table load.
static const unsigned char cyaml_nq_class[256] = {
    [0x00] = 1, [0x01] = 1, [0x02] = 1, [0x03] = 1, [0x04] = 1, [0x05] = 1,
    [0x06] = 1, [0x07] = 1, [0x08] = 1, [0x09] = 1, [0x0A] = 1, [0x0B] = 1,
    [0x0C] = 1, [0x0D] = 1, [0x0E] = 1, [0x0F] = 1, [0x10] = 1, [0x11] = 1,
    [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 1, [0x16] = 1, [0x17] = 1,
    [0x18] = 1, [0x19] = 1, [0x1A] = 1, [0x1B] = 1, [0x1C] = 1, [0x1D] = 1,
    [0x1E] = 1, [0x1F] = 1, [0x7F] = 1, ['?'] = 1,  [','] = 1,  ['['] = 1,
    [']'] = 1,  ['{'] = 1,  ['}'] = 1,  [':'] = 1,  ['#'] = 1,  [0xC2] = 1,
    [0xE2] = 1, [0xEF] = 1};

// Tells whether the serializer must quote s instead of writing a plain scalar.
// Quoted are: "", a leading U+FEFF (a parse would drop it as a BOM), every
// spelling a parse reads as another type (null, bools, special floats, "<<",
// "=", and the YAML 1.1 booleans such as "yes" and "no", which 1.1 readers
// would misread), a leading indicator, a number-like start ("+.N" included),
// and text holding a line break, tab, '?', flow indicator, " #", a C0 or C1
// control, NEL/LS/PS, U+FFFE/U+FFFF, or trailing whitespace. The ambiguous
// words are matched with a switch on the first byte and constant-size memcmp.
// Runs in the C locale.
static bool needs_quoting(const char *s) {
  if (!s || s[0] == '\0') return true;

  if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
      (unsigned char)s[2] == 0xBF)
    return true;

  size_t len = 0;
  while (len < 6 && s[len] != '\0') len++;
#define CYAML_IS(lit) \
  (len == sizeof(lit) - 1 && memcmp(s, (lit), sizeof(lit) - 1) == 0)
  bool ambiguous = false;
  switch (s[0]) {
    case '~':
      ambiguous = CYAML_IS("~");
      break;
    case 'n':
      ambiguous = CYAML_IS("null") || CYAML_IS("n") || CYAML_IS("no");
      break;
    case 'N':
      ambiguous = CYAML_IS("Null") || CYAML_IS("NULL") || CYAML_IS("N") ||
                  CYAML_IS("No") || CYAML_IS("NO");
      break;
    case 't':
      ambiguous = CYAML_IS("true");
      break;
    case 'T':
      ambiguous = CYAML_IS("True") || CYAML_IS("TRUE");
      break;
    case 'f':
      ambiguous = CYAML_IS("false");
      break;
    case 'F':
      ambiguous = CYAML_IS("False") || CYAML_IS("FALSE");
      break;
    case '.':
      ambiguous = CYAML_IS(".inf") || CYAML_IS(".Inf") || CYAML_IS(".INF") ||
                  CYAML_IS(".nan") || CYAML_IS(".NaN") || CYAML_IS(".NAN");
      break;
    case '+':
      ambiguous = CYAML_IS("+.inf") || CYAML_IS("+.Inf") || CYAML_IS("+.INF");
      break;
    case '-':
      ambiguous = CYAML_IS("-.inf") || CYAML_IS("-.Inf") || CYAML_IS("-.INF");
      break;
    case 'y':
      ambiguous = CYAML_IS("y") || CYAML_IS("yes");
      break;
    case 'Y':
      ambiguous = CYAML_IS("Y") || CYAML_IS("Yes") || CYAML_IS("YES");
      break;
    case 'o':
      ambiguous = CYAML_IS("on") || CYAML_IS("off");
      break;
    case 'O':
      ambiguous = CYAML_IS("On") || CYAML_IS("ON") || CYAML_IS("Off") ||
                  CYAML_IS("OFF");
      break;
    case '<':
      ambiguous = CYAML_IS("<<");
      break;
    case '=':
      ambiguous = len == 1;
      break;
    default:
      break;
  }
#undef CYAML_IS
  if (ambiguous) return true;

  const char *p = s;

  char fc = *p;
  if (fc == '-' || fc == '?' || fc == ':' || fc == ',' || fc == '[' ||
      fc == ']' || fc == '{' || fc == '}' || fc == '#' || fc == '&' ||
      fc == '*' || fc == '!' || fc == '|' || fc == '>' || fc == '\'' ||
      fc == '"' || fc == '%' || fc == '@' || fc == '`' || fc == ' ' ||
      fc == '\t')
    return true;

  if (fc >= '0' && fc <= '9') return true;
  if (fc == '+' && (s[1] >= '0' && s[1] <= '9')) return true;
  if (fc == '+' && s[1] == '.') {
    char *endp = NULL;
    (void)strtod(s, &endp);
    if (endp != s && *endp == '\0') return true;
  }
  if (fc == '.') return true;

  for (;; p++) {
    char c = *p;
    if (__builtin_expect(!cyaml_nq_class[(unsigned char)c], 1)) continue;
    if (c == '\0') break;
    if (c == '\n' || c == '\r') return true;
    if (c == '\t') return true;
    if (c == '?') return true;
    if ((unsigned char)c == 0xE2 && (unsigned char)p[1] == 0x80 &&
        ((unsigned char)p[2] == 0xA8 || (unsigned char)p[2] == 0xA9))
      return true;
    if ((unsigned char)c == 0xC2 && (unsigned char)p[1] >= 0x80 &&
        (unsigned char)p[1] <= 0x9F)
      return true;
    if ((unsigned char)c == 0xEF && (unsigned char)p[1] == 0xBF &&
        ((unsigned char)p[2] == 0xBE || (unsigned char)p[2] == 0xBF))
      return true;
    if (is_disallowed_control_byte(c)) return true;
    if (c == ',' || c == '[' || c == ']' || c == '{' || c == '}') return true;
    if (c == ':') {
      char nx = *(p + 1);
      if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '\0' ||
          nx == ',' || nx == ']' || nx == '}')
        return true;
    }
    if (c == '#' && (*(p - 1) == ' ' || *(p - 1) == '\t')) return true;
  }
  if (p > s && (*(p - 1) == ' ' || *(p - 1) == '\t')) return true;
  return false;
}

static int lltoa_y(char *buf, long long v);

// Tells whether the key s is exactly the canonical text of an int or float key,
// which the serializer writes unquoted so readers keep the numeric type; a
// parse turns it back into the same key. Other spellings ("010", "0x10") stay
// quoted.
static bool key_is_canonical_number(const char *s) {
  if (!((s[0] >= '0' && s[0] <= '9') || s[0] == '-' || s[0] == '.'))
    return false;
  char canon[CYAML_FLOAT_KEY_BUF_LEN];
  long long ival;
  double dval;
  if (try_parse_int_scalar(s, &ival)) {
    lltoa_y(canon, ival);
  } else if (try_parse_float_scalar(s, &dval)) {
    format_float_key(dval, canon);
  } else {
    return false;
  }
  return strcmp(s, canon) == 0;
}

// Tells whether the serializer must quote the dictionary key s: Canonical
// numbers go out plain, everything else follows needs_quoting().
static bool key_needs_quoting(const char *s) {
  if (s && key_is_canonical_number(s)) return false;
  return needs_quoting(s);
}

// Appends n bytes of d to b, but never lets b grow past limit + 1 bytes; later
// calls add nothing. The canonical key builder relies on it to stop at the
// first byte past CYAML_MAX_CANONICAL_KEY_LEN. Callers without a limit pass
// SIZE_MAX as a constant, so the test folds away.
static inline __attribute__((always_inline)) void yb_put(ybuf *b, const char *d,
                                                         size_t n,
                                                         size_t limit) {
  if (limit != SIZE_MAX) {
    if (b->oom || b->len > limit) return;
    size_t room = limit - b->len;
    if (n > room) n = room + 1;
  }
  yb_append(b, d, n);
}

// True once a capped build has gone past its limit, so the canonical key walk
// can stop.
static inline __attribute__((always_inline)) bool yb_over(const ybuf *b,
                                                          size_t limit) {
  return limit != SIZE_MAX && b->len > limit;
}

// Hex digits for the "\xNN" escapes of the double-quoted writer.
static const char cyaml_hex_lower[] = "0123456789abcdef";

// Nonzero for a byte that ends a run a double-quoted scalar holds as it is:
// every C0 control (NUL included, so no end test is needed), '"', '\\', DEL,
// and the lead bytes 0xC2, 0xE2 and 0xEF of sequences that may need an escape.
static const unsigned char cyaml_dq_stop[256] = {
    [0x00 ... 0x1F] = 1, ['"'] = 1,  ['\\'] = 1, [0x7F] = 1,
    [0xC2] = 1,          [0xE2] = 1, [0xEF] = 1};

// Writes s as a double-quoted scalar into b, within limit + 1 bytes (see
// yb_put()). Runs that need no escape go out in one append and escapes come
// from tables. NEL, LS and PS become \N, \L and \P (1.1 readers fold them raw),
// C1 controls and U+FFFE/U+FFFF get escapes since readers refuse them raw.
static inline __attribute__((always_inline)) void yb_dquoted_body(
    ybuf *b, const char *s, size_t limit) {
  yb_put(b, "\"", 1, limit);
  if (!s) {
    yb_put(b, "\"", 1, limit);
    return;
  }
  const unsigned char *p = (const unsigned char *)s;
  const unsigned char *run = p;
  for (;;) {
    while (!cyaml_dq_stop[*p]) p++;
    unsigned char c = *p;
    if (c == 0xC2 || c == 0xE2 || c == 0xEF) {
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
        p++;
        if (*p == 0x85) {
          esc[1] = 'N';
        } else {
          esc[1] = 'x';
          esc[2] = cyaml_hex_lower[*p >> 4];
          esc[3] = cyaml_hex_lower[*p & 0x0F];
          out_len = 4;
        }
        break;
      case 0xE2:
        p += 2;
        esc[1] = *p == 0xA8 ? 'L' : 'P';
        break;
      case 0xEF:
        p += 2;
        out = *p == 0xBE ? "\\uFFFE" : "\\uFFFF";
        out_len = 6;
        break;
      default:
        esc[1] = 'x';
        esc[2] = cyaml_hex_lower[c >> 4];
        esc[3] = cyaml_hex_lower[c & 0x0F];
        out_len = 4;
        break;
    }
    yb_put(b, out, out_len, limit);
    if (yb_over(b, limit)) return;
    run = ++p;
  }
  yb_put(b, "\"", 1, limit);
}

// Appends s as a double-quoted scalar with no limit, for the serializers'
// quoted strings and keys.
static void yb_append_yaml_dquoted(ybuf *b, const char *s) {
  yb_dquoted_body(b, s, SIZE_MAX);
}

// Appends s as a double-quoted scalar within yb_put()'s limit, for the
// canonical key text.
static void yb_append_yaml_dquoted_capped(ybuf *b, const char *s,
                                          size_t limit) {
  yb_dquoted_body(b, s, limit);
}

// The longest rendered key the serializer writes as an implicit "key: value";
// YAML 1.2 allows 1024 characters there and PyYAML and libyaml refuse more. A
// longer key goes out in the explicit "? key" form. Counting bytes only errs
// toward the explicit form.
#define CYAML_MAX_IMPLICIT_KEY_LEN 1024

// Appends the text of a dictionary key, plain when that reads back as the same
// key and double-quoted otherwise, for both serializers. The quoted form
// escapes line breaks, so a key always stays on one line.
static inline void yb_append_key_text(ybuf *b, const char *key) {
  if (key_needs_quoting(key))
    yb_append_yaml_dquoted(b, key);
  else
    yb_append_cstr(b, key);
}

// Appends a key's text like yb_append_key_text(), within yb_put()'s limit, for
// the flow writer that builds canonical keys.
static inline __attribute__((always_inline)) void yb_append_key_text_lim(
    ybuf *b, const char *key, size_t limit) {
  if (limit == SIZE_MAX) {
    yb_append_key_text(b, key);
  } else if (key_needs_quoting(key)) {
    yb_append_yaml_dquoted_capped(b, key, limit);
  } else {
    yb_put(b, key, strlen(key), limit);
  }
}

// Appends two spaces per level of depth for the block writer, one append per
// 128 bytes from a constant block of spaces.
static void yb_indent(ybuf *b, int depth) {
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

// Gives the YAML text of a double for float values, written into buf: ".nan",
// ".inf", "-.inf", or the shortest round-trip digits with a '.' in the mantissa
// ("1.0", "1.0e+20"), since without it YAML 1.1 readers read "1e+20" as a
// string and "1" would come back as an int. Runs in the C locale.
static const char *yaml_double_text(double v,
                                    char buf[CYAML_FLOAT_KEY_BUF_LEN]) {
  if (__builtin_isnan(v)) return ".nan";
  if (__builtin_isinf(v)) return v > 0 ? ".inf" : "-.inf";
  format_shortest_double(v, buf);
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

// Appends the YAML text of a double, for float scalars in both serializers.
static void yb_append_double(ybuf *b, double v) {
  char buf[CYAML_FLOAT_KEY_BUF_LEN];
  yb_append_cstr(b, yaml_double_text(v, buf));
}

// Writes v in decimal into buf without snprintf, for integer scalars and the
// canonical-number check. Gives the length.
static int lltoa_y(char *buf, long long v) {
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

static void serialize_block(ybuf *b, cyaml_node *n, int depth, int max_depth);

// Writes a string scalar for the block writer: Double-quoted when a parse would
// read it as another type or it holds a character that needs it
// (needs_quoting()), plain otherwise.
static void serialize_block_scalar(ybuf *b, cyaml_node *n) {
  const char *s = n->value.string;
  if (needs_quoting(s)) {
    yb_append_yaml_dquoted(b, s);
  } else {
    yb_append_cstr(b, s);
  }
}

// Tells whether c may appear literally in a written verbatim tag: ns-uri-char
// without '%' (it starts an escape) and without '#' (libyaml and PyYAML refuse
// it literally; "%23" means the same).
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

// Writes tag as "!<...>" within yb_put()'s limit, percent-escaping every byte
// that may not appear literally, so any conforming reader accepts the token and
// parsing gives back the same tag. The caller writes the separator after it.
static inline __attribute__((always_inline)) void yb_verbatim_tag_body(
    ybuf *b, const char *tag, size_t limit) {
  static const char hex_digits[] = "0123456789ABCDEF";
  yb_put(b, "!<", 2, limit);
  const unsigned char *p = (const unsigned char *)tag;
  for (;;) {
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

// Writes a verbatim tag with no limit, for custom tags in the block and flow
// writers.
static void yb_append_verbatim_tag(ybuf *b, const char *tag) {
  yb_verbatim_tag_body(b, tag, SIZE_MAX);
}

// Writes a verbatim tag within yb_put()'s limit, for canonical key text.
static void yb_append_verbatim_tag_capped(ybuf *b, const char *tag,
                                          size_t limit) {
  yb_verbatim_tag_body(b, tag, limit);
}

// What a serialize_*_head() call found: The node is fully written, or it's a
// non-empty list or dictionary whose children the walk must visit.
typedef enum {
  CYAML_EMIT_DONE,
  CYAML_EMIT_LIST,
  CYAML_EMIT_DICT
} cyaml_emit_head;

// One level of the block writer's walk: A list walks n by idx up to count, a
// dictionary walks through next in insertion order. depth is the indentation
// level.
typedef struct {
  cyaml_node *n;
  size_t idx;
  size_t count;
  const ccol_chmap_entry_ref *next;
  int depth;
} sblock_frame;

#ifdef RUNNING_UNIT_TESTS
// Counts the nodes a serializer visits after its buffer went OOM, so a test can
// check that the walk stops at once. CYAML_COUNT_OOM_VISIT() bumps it in test
// builds and evaluates to nothing otherwise.
__thread size_t cyaml__serialize_oom_visits_for_tests;
#define CYAML_COUNT_OOM_VISIT() (cyaml__serialize_oom_visits_for_tests++)
#else
#define CYAML_COUNT_OOM_VISIT() ((void)0)
#endif

// Writes everything about n that comes before its children, for the block
// writer: '~' for a NULL pointer, a verbatim tag the type doesn't imply (on its
// own line for a collection, as a prefix for a scalar), and the whole of a
// scalar or an empty container. depth sets the indentation (2 spaces a level).
// A container at max_depth or deeper sets the buffer's oom flag like an
// allocation failure, and a set flag writes nothing. Gives what is left to
// walk, with the child count in *count_out. Inlined, since it runs per node.
static inline __attribute__((always_inline)) cyaml_emit_head
serialize_block_head(ybuf *b, cyaml_node *n, int depth, size_t *count_out,
                     int max_depth) {
  if (b->oom) {
    CYAML_COUNT_OOM_VISIT();
    return CYAML_EMIT_DONE;
  }
  if (!n) {
    yb_append_cstr(b, "~");
    return CYAML_EMIT_DONE;
  }

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
      int len = lltoa_y(buf, n->value.integer);
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
        CYAML_COUNT_OOM_VISIT();
        b->oom = true;
        break;
      }
      *count_out = cvector_size(n->value.list);
      if (*count_out == 0) {
        yb_indent(b, depth);
        yb_append_cstr(b, "[]\n");
        break;
      }
      return CYAML_EMIT_LIST;
    case CYAML_DICTIONARY:
      if (depth >= max_depth) {
        CYAML_COUNT_OOM_VISIT();
        b->oom = true;
        break;
      }
      *count_out = chashmap_size(n->value.dictionary);
      if (*count_out == 0) {
        yb_indent(b, depth);
        yb_append_cstr(b, "{}\n");
        break;
      }
      return CYAML_EMIT_DICT;
  }
  return CYAML_EMIT_DONE;
}

// Starts the walk of the children of the non-empty container n, for
// serialize_block(); inlined, since it runs once per container.
static inline __attribute__((always_inline)) void sblock_frame_open(
    sblock_frame *f, cyaml_node *n, int depth, cyaml_emit_head kind,
    size_t count) {
  f->n = n;
  f->depth = depth;
  f->idx = 0;
  f->count = count;
  f->next = (kind == CYAML_EMIT_DICT) ? dict_first_entry(n) : NULL;
}

// Writes block-style YAML for the subtree n, the body of cyaml_serialize() and
// of each stream document. Each child goes on its own indented line, a key too
// long for an implicit key uses the "? key" form, and a container child gets a
// bare "-" or "key:" so no line holds only whitespace. An explicit frame stack
// drives the walk; it stops as soon as the buffer's oom flag is set, which a
// tree nesting past max_depth also sets.
static void serialize_block(ybuf *b, cyaml_node *n, int depth, int max_depth) {
  size_t count = 0;
  cyaml_emit_head kind = serialize_block_head(b, n, depth, &count, max_depth);
  if (kind == CYAML_EMIT_DONE) return;

  sblock_frame inline_frames[CYAML_WALK_INLINE_FRAMES];
  sblock_frame *stack = inline_frames;
  size_t cap = CYAML_WALK_INLINE_FRAMES;
  size_t sp = 1;
  sblock_frame_open(&stack[0], n, depth, kind, count);

  while (sp > 0 && !b->oom) {
    sblock_frame *f = &stack[sp - 1];
    cyaml_node *child;

    if (f->n->type == CYAML_LIST) {
      if (f->idx >= f->count) {
        sp--;
        continue;
      }
      child = *(cyaml_node **)cvector_at(f->n->value.list, f->idx);
      f->idx++;
      yb_indent(b, f->depth);
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
      const cmap_pair *kp, *vp;
      f->next = ccol_chmap_entry_read(f->next, &kp, &vp);
      const char *key = (const char *)kp->ptr;
      child = cyaml_read_child(vp->ptr);

      yb_indent(b, f->depth);
      size_t key_start = b->len;
      yb_append_key_text(b, key);
      if (!b->oom && b->len - key_start > CYAML_MAX_IMPLICIT_KEY_LEN) {
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

    if (kind != CYAML_EMIT_DONE) {
      int child_depth = f->depth + 1;
      if (sp == cap) {
        sblock_frame *grown = walk_stack_grow(stack, &cap, inline_frames,
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

  if (stack != inline_frames) ccol__mem_free(b->m_procs, stack);
}

// Serializes node to block YAML that always ends with a newline, the public
// entry point. It runs in one C locale scope, since needs_quoting() and the
// float writer follow LC_NUMERIC. Gives a string from the node's allocator, or
// NULL on OOM or a tree that is too deep.
char *cyaml_serialize(cyaml node) {
  cyaml_node *n = (cyaml_node *)node;
  ccol_memmgmt_procs *mp = n ? n->m_procs : NULL;
  ybuf b;
  yb_init(&b, mp);
  ccol_c_locale_scope scope = ccol_c_locale_enter();
  serialize_block(&b, n, 0, CYAML_MAX_NESTING_DEPTH);
  ccol_c_locale_leave(scope);
  if (b.len > 0 && !yb_ends_with_char(&b, '\n')) yb_append_c(&b, '\n');
  if (b.oom) {
    ccol__mem_free(b.m_procs, b.buf);
    return NULL;
  }
  return b.buf;
}

// Writes one document of a stream: "---" on its own line, then the block YAML
// of root (a root tag goes on the next line). max_depth is one less for an
// element of a list of documents, as when parsing a stream.
static void serialize_stream_document(ybuf *b, cyaml_node *root,
                                      int max_depth) {
  yb_append_cstr(b, "---\n");
  size_t start = b->len;
  serialize_block(b, root, 0, max_depth);
  if (b->len > start && !yb_ends_with_char(b, '\n')) yb_append_c(b, '\n');
}

// Serializes the elements of a list as a stream of "---" documents, or a node
// that isn't a list as one document, in one C locale scope. Gives the string,
// "" for an empty list, or NULL on OOM or a tree that is too deep.
char *cyaml_serialize_stream(cyaml node) {
  cyaml_node *n = (cyaml_node *)node;
  ccol_memmgmt_procs *mp = n ? n->m_procs : NULL;
  ybuf b;
  yb_init(&b, mp);
  ccol_c_locale_scope scope = ccol_c_locale_enter();
  if (n && n->type == CYAML_LIST) {
    size_t count = cyaml_list_size(node);
    for (size_t i = 0; i < count && !b.oom; i++)
      serialize_stream_document(&b, (cyaml_node *)cyaml_list_get(node, i),
                                CYAML_MAX_NESTING_DEPTH - 1);
  } else {
    serialize_stream_document(&b, n, CYAML_MAX_NESTING_DEPTH);
  }
  ccol_c_locale_leave(scope);
  if (b.oom) {
    ccol__mem_free(b.m_procs, b.buf);
    return NULL;
  }
  return b.buf;
}

// A key and value pair copied out of a dictionary so canonical mode can sort
// it. key is borrowed from the dictionary's storage.
typedef struct {
  const char *key;
  cyaml_node *val;
} flow_dict_entry;

// Orders flow_dict_entry items by key with strcmp, for the qsort of canonical
// mode.
static int flow_dict_entry_cmp(const void *a, const void *b) {
  const flow_dict_entry *ea = (const flow_dict_entry *)a;
  const flow_dict_entry *eb = (const flow_dict_entry *)b;
  return strcmp(ea->key, eb->key);
}

// One level of the flow writer's walk: A list walks by index, a dictionary
// walks through the sorted entries copy of canonical mode or through next in
// insertion order.
typedef struct {
  cyaml_node *n;
  flow_dict_entry *entries;
  const ccol_chmap_entry_ref *next;
  size_t idx;
  size_t count;
  int depth;
} sflow_frame;

// Writes everything about n that comes before its children, for the flow
// writer: '~' for NULL, a tag prefix the type doesn't imply, the whole of a
// scalar, and the opening bracket of a container (both brackets when empty).
// depth is the containers around n; one at the nesting limit or deeper sets the
// oom flag, and a set flag writes nothing. All output goes through yb_put()
// under limit. Gives what is left to walk.
static inline __attribute__((always_inline)) cyaml_emit_head
serialize_flow_head(ybuf *b, cyaml_node *n, int depth, size_t *count_out,
                    size_t limit) {
  if (b->oom) {
    CYAML_COUNT_OOM_VISIT();
    return CYAML_EMIT_DONE;
  }
  if (!n) {
    yb_put(b, "~", 1, limit);
    return CYAML_EMIT_DONE;
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
      int len = lltoa_y(buf, n->value.integer);
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
        CYAML_COUNT_OOM_VISIT();
        b->oom = true;
        break;
      }
      *count_out = cvector_size(n->value.list);
      if (*count_out == 0) {
        yb_put(b, "[]", 2, limit);
        break;
      }
      yb_put(b, "[", 1, limit);
      return CYAML_EMIT_LIST;
    case CYAML_DICTIONARY:
      if (depth >= CYAML_MAX_NESTING_DEPTH) {
        CYAML_COUNT_OOM_VISIT();
        b->oom = true;
        break;
      }
      *count_out = chashmap_size(n->value.dictionary);
      if (*count_out == 0) {
        yb_put(b, "{}", 2, limit);
        break;
      }
      yb_put(b, "{", 1, limit);
      return CYAML_EMIT_DICT;
  }
  return CYAML_EMIT_DONE;
}

// Frees the sorted entries copy of a flow frame, if canonical mode made one,
// when the walk leaves the frame or fails.
static void sflow_frame_close(ybuf *b, sflow_frame *f) {
  f->next = NULL;
  if (f->entries) {
    ccol__mem_free(b->m_procs, f->entries);
    f->entries = NULL;
  }
}

// Starts the walk of the children of the non-empty container n. In canonical
// mode a dictionary of two or more entries is copied into an array and sorted
// by key, so the text doesn't depend on insertion order. false, with the oom
// flag set, when that array can't be allocated.
static inline __attribute__((always_inline)) bool sflow_frame_open(
    ybuf *b, sflow_frame *f, cyaml_node *n, int depth, bool canonical,
    cyaml_emit_head kind, size_t count) {
  f->n = n;
  f->depth = depth;
  f->idx = 0;
  f->count = count;
  f->next = NULL;
  f->entries = NULL;
  if (kind == CYAML_EMIT_LIST) return true;
  if (!canonical || f->count <= 1) {
    f->next = dict_first_entry(n);
    return true;
  }

  flow_dict_entry *entries =
      ccol__mem_calloc(b->m_procs, f->count, sizeof(*entries));
  if (!entries) {
    b->oom = true;
    return false;
  }
  size_t idx = 0;
  for (const ccol_chmap_entry_ref *e = dict_first_entry(n); e; idx++) {
    const cmap_pair *kp, *vp;
    e = ccol_chmap_entry_read(e, &kp, &vp);
    entries[idx].key = (const char *)kp->ptr;
    entries[idx].val = cyaml_read_child(vp->ptr);
  }
  qsort(entries, f->count, sizeof(*entries), flow_dict_entry_cmp);
  f->entries = entries;
  return true;
}

// Writes one flow dictionary key and its ": ", or an explicit "? key: " entry
// for a key longer than CYAML_MAX_IMPLICIT_KEY_LEN. Inlined, since it runs per
// entry.
static inline __attribute__((always_inline)) void sflow_emit_key(
    ybuf *b, const char *key, size_t limit) {
  size_t key_start = b->len;
  yb_append_key_text_lim(b, key, limit);
  if (!b->oom && b->len - key_start > CYAML_MAX_IMPLICIT_KEY_LEN) {
    b->len = key_start;
    yb_put(b, "? ", 2, limit);
    yb_append_key_text_lim(b, key, limit);
  }
  yb_put(b, ": ", 2, limit);
}

// Writes compact flow YAML for the subtree n, quoting strings by the same
// rules as the block writer, for cyaml_serialize_flow() and the canonical key
// builder. canonical sorts the entries of every dictionary by key (lists keep
// their order). limit bounds the text at limit + 1 bytes and ends the walk once
// it's passed (SIZE_MAX for none). Each caller passes constants, so the public
// path carries no limit tests. An explicit frame stack drives the walk, and it
// stops as soon as the oom flag is set.
static inline __attribute__((always_inline)) void serialize_flow(
    ybuf *b, cyaml_node *n, bool canonical, int depth, size_t limit) {
  size_t count = 0;
  cyaml_emit_head kind = serialize_flow_head(b, n, depth, &count, limit);
  if (kind == CYAML_EMIT_DONE) return;

  sflow_frame inline_frames[CYAML_WALK_INLINE_FRAMES];
  sflow_frame *stack = inline_frames;
  size_t cap = CYAML_WALK_INLINE_FRAMES;
  size_t sp =
      sflow_frame_open(b, &stack[0], n, depth, canonical, kind, count) ? 1 : 0;

  while (sp > 0 && !b->oom && !yb_over(b, limit)) {
    sflow_frame *f = &stack[sp - 1];
    cyaml_node *child;

    if (f->n->type == CYAML_LIST) {
      if (f->idx >= f->count) {
        yb_put(b, "]", 1, limit);
        sflow_frame_close(b, f);
        sp--;
        continue;
      }
      if (f->idx > 0) yb_put(b, ", ", 2, limit);
      child = *(cyaml_node **)cvector_at(f->n->value.list, f->idx);
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
      const cmap_pair *kp, *vp;
      f->next = ccol_chmap_entry_read(f->next, &kp, &vp);
      child = cyaml_read_child(vp->ptr);
      f->idx++;
      sflow_emit_key(b, (const char *)kp->ptr, limit);
    }

    int child_depth = f->depth + 1;
    kind = serialize_flow_head(b, child, child_depth, &count, limit);
    if (kind != CYAML_EMIT_DONE) {
      if (sp == cap) {
        sflow_frame *grown = walk_stack_grow(stack, &cap, inline_frames,
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
  if (stack != inline_frames) ccol__mem_free(b->m_procs, stack);
}

// Serializes node to one-line flow YAML, the public entry point, in one C
// locale scope. Gives a string from the node's allocator, or NULL on OOM or a
// tree that is too deep.
char *cyaml_serialize_flow(cyaml node) {
  cyaml_node *n = (cyaml_node *)node;
  ccol_memmgmt_procs *mp = n ? n->m_procs : NULL;
  ybuf b;
  yb_init(&b, mp);
  ccol_c_locale_scope scope = ccol_c_locale_enter();
  serialize_flow(&b, n, false, 0, SIZE_MAX);
  ccol_c_locale_leave(scope);
  if (b.oom) {
    ccol__mem_free(b.m_procs, b.buf);
    return NULL;
  }
  return b.buf;
}

#ifdef RUNNING_UNIT_TESTS
// The largest buffer serialize_flow_canonical() allocated on this thread; a
// test resets it and checks it against the key limit.
__thread size_t cyaml__canonical_key_peak_bytes_for_tests;
#endif
// Builds the canonical flow text of a collection used as a key, for
// node_to_dict_key_string(): Every dictionary sorted by key, so equal
// structures give equal text. It stops at the first byte past
// CYAML_MAX_CANONICAL_KEY_LEN, so a refused key costs at most that much work.
// NULL with *too_long set when the text is too long, NULL with *too_long false
// on OOM.
static char *serialize_flow_canonical(cyaml_node *n, bool *too_long) {
  ccol_memmgmt_procs *mp = n ? n->m_procs : NULL;
  ybuf b;
  yb_init(&b, mp);
  serialize_flow(&b, n, true, 0, CYAML_MAX_CANONICAL_KEY_LEN);
#ifdef RUNNING_UNIT_TESTS
  if (b.cap > cyaml__canonical_key_peak_bytes_for_tests)
    cyaml__canonical_key_peak_bytes_for_tests = b.cap;
#endif
  *too_long = !b.oom && b.len > CYAML_MAX_CANONICAL_KEY_LEN;
  if (b.oom || *too_long) {
    ccol__mem_free(b.m_procs, b.buf);
    return NULL;
  }
  return b.buf;
}

// Frees a string from one of the serializers through mp (NULL for the default
// allocator); s may be NULL. A parse's *err_str must never come here, since the
// library owns it.
void cyaml_serialize_free_mp(char *s, ccol_memmgmt_procs *mp) {
  ccol__mem_free(mp, s);
}

// Gives the first unescaped '.' in s, or NULL; a path splits there. In a path
// "\." is a literal dot and "\\" a literal backslash, and any other backslash
// passes through.
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

// Gives the last unescaped '.' in s, or NULL, so set and delete can split the
// parent path from the leaf.
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

// Resolves the escapes of one path component in place, before it's used as a
// key; it never gets longer.
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

// Parses the digits of a "#N" component (digits points past the '#'), strictly:
// "#", "#+3" and "#3x" are malformed. An index too large for size_t saturates
// at SIZE_MAX, so a well-formed index past the list is absent on every width.
// false, with *idx_out untouched, when malformed.
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

// Why navigate_y() stopped, so set and delete can tell a malformed path
// (ccol_invalid_args: Wrong parent type, bad "#N", empty component, bad UTF-8,
// at any position) from an absent well-formed component (ccol_key_not_found).
// The UTF-8 test runs only from the first component that doesn't resolve.
typedef enum {
  CYAML_NAV_NOT_FOUND = 0,
  CYAML_NAV_WRONG_TYPE,
  CYAML_NAV_MALFORMED_INDEX,
  CYAML_NAV_EMPTY_COMPONENT,
  CYAML_NAV_BAD_UTF8,
} cyaml_nav_fail;

// Walks a dot-separated path from root and gives the node at its end, or NULL,
// for every path operation. path_copy is writable: Each unescaped dot becomes a
// NUL and each component is unescaped in place. "#N" indexes a list. After an
// absent component it still scans the rest for an empty or non-UTF-8
// component, since that makes the whole path malformed. *fail_reason_out (may
// be NULL) is written only when NULL comes back.
static cyaml navigate_y(cyaml root, char *path_copy,
                        cyaml_nav_fail *fail_reason_out) {
  cyaml node = root;
  char *p = path_copy;
  cyaml_nav_fail fail_reason = CYAML_NAV_NOT_FOUND;
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

    cyaml_node *n = (cyaml_node *)node;

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
      if (chashmap_get_elem_ref(n->value.dictionary, &kp, &vp) !=
          ccol_success) {
        fail_reason = ccol_utf8_is_valid(p, kp.size - 1) ? CYAML_NAV_NOT_FOUND
                                                         : CYAML_NAV_BAD_UTF8;
        node = NULL;
      } else {
        node = cyaml_read_child(vp.ptr);
      }
    } else {
      fail_reason = CYAML_NAV_WRONG_TYPE;
      node = NULL;
    }

    if (!dot) break;
    p = dot + 1;
  }
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

// Maps a navigate_y() failure to the code set and delete report for a parent
// they couldn't reach: ccol_invalid_args for a malformed path (a leaf that
// isn't UTF-8 included), ccol_key_not_found for an absent component.
static ccol_retval nav_fail_to_retval(cyaml_nav_fail reason, const char *leaf) {
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

// A path shorter than this is copied to the stack, so cyaml__get() allocates
// nothing and can't fail for lack of memory.
#define CYAML_GET_STACK_PATH 256
// Gives the node at path, the body of cyaml_get(). It walks a copy of path, so
// the caller's string never changes; only a long path is copied to the heap,
// with root's allocator. NULL or "" gives root.
cyaml cyaml__get(cyaml root, const char *path) {
  if (!root) return NULL;
  if (!path || path[0] == '\0') return root;
  ccol_memmgmt_procs *mp = ((cyaml_node *)root)->m_procs;
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
  if (copy != stack_copy) ccol__mem_free(mp, copy);
  return result;
}

// Writes a scalar to the leaf at path, the body of cyaml_set(). The path splits
// at its last dot; the leaf is copied before the parent copy is freed, since it
// lives inside it. An empty leaf, a bad payload and a key that isn't UTF-8 are
// refused before anything changes. An existing scalar leaf is updated in place
// (borrowed handles and custom tags survive) and a stale core tag is dropped. A
// new dictionary key is inserted with a NULL placeholder in one probe and
// removed again if building the child fails. An absent list index is
// ccol_key_not_found, since a list doesn't grow to an index; no parent is ever
// created.
ccol_retval cyaml__set_typed(cyaml root, const char *path, cyaml_node_type type,
                             void *raw, size_t raw_size, bool is_signed) {
  if (!root || !path || path[0] == '\0') return ccol_invalid_args;

  ccol_memmgmt_procs *mp = ((cyaml_node *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return ccol_not_enough_memory;

  char *last_dot = path_find_last_unescaped_dot(copy);
  char *leaf_copy;
  cyaml parent;

  if (!last_dot) {
    leaf_copy = ccol_strdup(mp, path);
    parent = root;
    ccol__mem_free(mp, copy);
  } else {
    *last_dot = '\0';
    if (last_dot[1] == '\0') {
      ccol__mem_free(mp, copy);
      return ccol_invalid_args;
    }
    cyaml_nav_fail fail_reason = CYAML_NAV_NOT_FOUND;
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    parent = navigate_y(root, copy, &fail_reason);
    ccol__mem_free(mp, copy);
    if (!leaf_copy) return ccol_not_enough_memory;
    if (!parent) {
      ccol_retval nav_rv = nav_fail_to_retval(fail_reason, leaf_copy);
      ccol__mem_free(mp, leaf_copy);
      return nav_rv;
    }
  }

  if (!leaf_copy) return ccol_not_enough_memory;
  path_unescape_component(leaf_copy);
  const char *leaf_comp = leaf_copy;
  if (leaf_comp[0] == '\0') {
    ccol__mem_free(mp, leaf_copy);
    return ccol_invalid_args;
  }

  cyaml_node *pn = (cyaml_node *)parent;
  ccol_retval ret;

  if (pn->type == CYAML_DICTIONARY) {
    cmap_pair kp = {.ptr = (void *)leaf_comp, .size = strlen(leaf_comp) + 1};
    ret = ccol_utf8_is_valid(leaf_comp, kp.size - 1)
              ? scalar_payload_check(type, raw, raw_size)
              : ccol_invalid_args;
    if (ret == ccol_success) {
      cyaml_node *placeholder = NULL;
      cmap_pair vp = {.ptr = &placeholder, .size = sizeof(placeholder)};
      cmap_pair slot;
      ccol_retval r =
          ccol_chmap_insert_or_get_elem(pn->value.dictionary, &kp, &vp, &slot);
      if (r == ccol_key_already_present) {
        cyaml_node *existing = cyaml_read_child(slot.ptr);
        ret = node_reinit_scalar_checked(existing, type, raw, raw_size,
                                         is_signed);
      } else if (r == ccol_success) {
        cyaml_node *new_node = NULL;
        ret = node_make_scalar(type, raw, raw_size, is_signed, pn->m_procs,
                               &new_node);
        if (ret == ccol_success) {
          memcpy(slot.ptr, &new_node, sizeof(new_node));
          new_node->attached = true;
        } else {
          chashmap_remove_elem(pn->value.dictionary, &kp);
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
          ret = ccol_key_not_found;
        } else {
          cyaml_node *existing = *(cyaml_node **)slot;
          ret = node_reinit_scalar(existing, type, raw, raw_size, is_signed);
        }
      }
    }
  } else {
    ret = ccol_invalid_args;
  }

  ccol__mem_free(mp, leaf_copy);
  return ret;
}

// Removes and deep-frees the node at path, the body of cyaml_remove(). The path
// splits at its last unescaped dot (no dot: root is the parent), and the leaf
// is unescaped. ccol_invalid_args for NULL or "" arguments, an empty or
// non-UTF-8 leaf, a malformed component or a parent of the wrong type;
// ccol_key_not_found for an absent key or index; ccol_not_enough_memory when a
// copy fails.
ccol_retval cyaml__remove(cyaml root, const char *path) {
  if (!root || !path || path[0] == '\0') return ccol_invalid_args;

  ccol_memmgmt_procs *mp = ((cyaml_node *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return ccol_not_enough_memory;

  char *last_dot = path_find_last_unescaped_dot(copy);
  char *leaf_copy;
  cyaml parent;

  if (!last_dot) {
    leaf_copy = ccol_strdup(mp, path);
    parent = root;
    ccol__mem_free(mp, copy);
  } else {
    *last_dot = '\0';
    if (last_dot[1] == '\0') {
      ccol__mem_free(mp, copy);
      return ccol_invalid_args;
    }
    cyaml_nav_fail fail_reason = CYAML_NAV_NOT_FOUND;
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    parent = navigate_y(root, copy, &fail_reason);
    ccol__mem_free(mp, copy);
    if (!leaf_copy) return ccol_not_enough_memory;
    if (!parent) {
      ccol_retval nav_rv = nav_fail_to_retval(fail_reason, leaf_copy);
      ccol__mem_free(mp, leaf_copy);
      return nav_rv;
    }
  }

  if (!leaf_copy) return ccol_not_enough_memory;
  path_unescape_component(leaf_copy);
  const char *leaf_comp = leaf_copy;

  if (leaf_comp[0] == '\0') {
    ccol__mem_free(mp, leaf_copy);
    return ccol_invalid_args;
  }

  cyaml_node *pn = (cyaml_node *)parent;
  ccol_retval ret;

  if (pn->type == CYAML_DICTIONARY) {
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
      } else if (idx >= cvector_size(pn->value.list)) {
        ret = ccol_key_not_found;
      } else {
        ret = cyaml_list_remove(parent, idx);
      }
    }
  } else {
    ret = ccol_invalid_args;
  }

  ccol__mem_free(mp, leaf_copy);
  return ret;
}
