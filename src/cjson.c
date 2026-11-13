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

// The JSON parser, serializer and DOM of cjson.h. Lists are cvectors and
// dictionaries are chmaps keyed by string. Every string that a node can reach
// (a value or a key) is well-formed UTF-8; each place where bytes enter the DOM
// refuses anything else, which is what lets the serializer copy bytes >= 0x20
// straight through. Parse, clone and serialize walk the nesting with explicit
// stacks on the heap and destroy uses a worklist, so no function's native stack
// depends on how deep a tree is.
#include <chashmap.h>
#include <cjson.h>
#include <cvector.h>
#include <errno.h>
#include <float.h>
#include <internal/cgrowbuf.h>
#include <internal/chashinsert.h>
#include <internal/cnumlocale.h>
#include <internal/cprocsintern.h>
#include <internal/cstrutil.h>
#include <internal/ctlsmodel.h>
#include <internal/cutf8.h>
#include <internal/test_helpers/cjson.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>

// The node behind the opaque cjson handle. m_procs is NULL for the default
// allocator or an interned copy from ccol_procs_intern(), never the caller's
// pointer, and every node of a tree carries the same value. attached becomes
// true once a push or a set puts the node into a container and is never
// cleared; it's how cjson_list_push() and cjson_dictionary_set() refuse a child
// that already has a parent (two owners would mean a double free). A list or
// dictionary store stays NULL until the first child arrives, so "[]" and "{}"
// cost one node.
typedef struct cjson_node {
  cjson_node_type type;
  bool attached;
  ccol_memmgmt_procs *m_procs;
  union {
    bool boolean;
    long long integer;
    double number;
    char *string;
    cvec list;
    chmap dictionary;
  } value;
} cjson_node;

// The serializer's output buffer, a ccol_growbuf with short sb_* names. Every
// operation does nothing once oom is set, so a serialization checks for an
// error once, at the end.
typedef ccol_growbuf sbuf;

// The sb_* wrappers (init, append n bytes, append a byte, append a C string)
// forward to ccol_growbuf, so the serializer and the string decoder have a
// short vocabulary of their own.
static inline void sb_init(sbuf *sb, ccol_memmgmt_procs *mp) {
  ccol_growbuf_init(sb, mp);
}

static inline void sb_append(sbuf *sb, const char *data, size_t n) {
  ccol_growbuf_append(sb, data, n);
}

static inline void sb_append_c(sbuf *sb, char c) {
  ccol_growbuf_append_c(sb, c);
}

static inline void sb_append_cstr(sbuf *sb, const char *s) {
  ccol_growbuf_append_cstr(sb, s);
}

// Like sb_init(), with a starting capacity of at least 64 bytes; the string
// decoder uses it because it knows the length of the literal.
static inline void sb_init_hint(sbuf *sb, ccol_memmgmt_procs *mp, size_t hint) {
  ccol_growbuf_init_hint(sb, mp, hint);
}

// Test counters: C strings checked by utf8_cstr_is_valid() and nodes that the
// cycle search of node_reaches() took off its stack, over all threads.
#ifdef RUNNING_UNIT_TESTS
atomic_ulong cjson__utf8_cstr_checks_for_tests;

atomic_ulong cjson__cycle_search_nodes_for_tests;
#endif

// Tells whether a NUL-terminated string is well-formed UTF-8. Every API call
// that takes a C string for the DOM (a string value, a key, a path leaf) uses
// it, so no ill-formed byte gets in. A NULL s passes, so a caller can check an
// optional string. Counts calls in cjson__utf8_cstr_checks_for_tests.
static bool utf8_cstr_is_valid(const char *s) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&cjson__utf8_cstr_checks_for_tests, 1,
                            memory_order_relaxed);
#endif
  return !s || ccol_utf8_is_valid(s, strlen(s));
}

// Converts nptr with strtod() in the "C" locale. The number parser needs it
// because JSON always writes '.' as the decimal point while strtod() follows
// the LC_NUMERIC of the calling thread.
static double cjson_strtod_c(const char *nptr) {
  ccol_c_locale_scope scope = ccol_c_locale_enter();
  double v = strtod(nptr, NULL);
  ccol_c_locale_leave(scope);
  return v;
}

// Writes into buf (cap bytes) the shortest "%.Ng" text that reads back as
// exactly val, in the "C" locale. The serializer uses it so a double round
// trips with no noise digits and '.' as the decimal point in every locale. A
// normal double tries 15, 16 and 17 digits (anything with at most DBL_DIG
// digits survives, so 1/3 is 0.3333333333333333); a subnormal has fewer bits
// and tries every precision from 1 upward (5e-324 stays 5e-324).
static void cjson_format_shortest_c(char *buf, size_t cap, double val) {
  ccol_c_locale_scope scope = ccol_c_locale_enter();
  int prec = (val != 0.0 && fabs(val) < DBL_MIN) ? 1 : 15;
  for (; prec < 17; prec++) {
    snprintf(buf, cap, "%.*g", prec, val);
    if (strtod(buf, NULL) == val) break;
  }
  if (prec == 17) snprintf(buf, cap, "%.17g", val);
  ccol_c_locale_leave(scope);
}

// Reads the child pointer stored at src. Containers keep child pointers in
// cvector and chmap slots, and every read of one goes through here; the memcpy
// is a second line of defence against an unaligned slot.
static inline cjson_node *cjson_read_child(const void *src) {
  cjson_node *p;
  memcpy(&p, src, sizeof(p));
  return p;
}

// Gives the number of elements of list n. This and the accessors below read a
// container's store, which stays NULL until the first child (so an empty
// container costs one node); they treat a NULL store as empty, and every insert
// goes through list_store() or dict_store() to create it.
static inline size_t list_len_of(const cjson_node *n) {
  return n->value.list ? cvector_elem_count(n->value.list) : 0;
}

// Gives element index of list n, or NULL when index is out of range; used by
// the getters, the path walk and the clone.
static inline cjson_node *list_child_at(const cjson_node *n, size_t index) {
  if (!n->value.list) return NULL;
  void *slot = cvector_at(n->value.list, index);
  return slot ? *(cjson_node **)slot : NULL;
}

// Gives the number of members of dictionary n, for cjson_dictionary_size().
static inline size_t dict_size_of(const cjson_node *n) {
  return n->value.dictionary ? chashmap_elem_count(n->value.dictionary) : 0;
}

// Looks key_pair up in dictionary n, for every read of a member by key. On
// ccol_success *vp describes the stored value; any other code means the key is
// absent (and *vp is cleared when there is no store).
static inline ccol_retval dict_lookup(const cjson_node *n,
                                      const cmap_pair *key_pair,
                                      cmap_pair *vp) {
  if (!n->value.dictionary) {
    vp->ptr = NULL;
    vp->size = 0;
    return ccol_key_not_found;
  }
  return chashmap_get_elem_ref(n->value.dictionary, key_pair, vp);
}

// Gives the store of list n, creating it with n's allocator on the first call,
// so every insert has a store to push into. NULL when the creation fails, with
// n unchanged.
static cvec list_store(cjson_node *n) {
  if (!n->value.list)
    n->value.list = cvector_create_full(sizeof(cjson_node *), n->m_procs, NULL);
  return n->value.list;
}

// Gives the store of dictionary n, creating a compact chmap with n's allocator
// on the first call, so every insert has a store. NULL when the creation
// fails, with n unchanged.
static chmap dict_store(cjson_node *n) {
  if (!n->value.dictionary) {
    char *err = NULL;
    n->value.dictionary =
        ccol_chmap_create_compact(ccol_string, ccol_pointer, n->m_procs, &err);
  }
  return n->value.dictionary;
}

// Gives the oldest member of dictionary n, or NULL. Parse, clone, the
// serializers, the cycle check and the public iterator walk members from here
// in insertion order, without allocating. A new key goes last, a replace keeps
// its place and a delete takes it out.
static inline const ccol_chmap_entry_ref *dict_first_entry(cjson_node *n) {
  return n->value.dictionary ? ccol_chmap_oldest_entry(n->value.dictionary)
                             : NULL;
}

// A heap worklist that tears a subtree down without recursion. The public API
// can build a tree of any depth, and destruction can't fail, so a depth cap
// would only turn a crash into a silent leak.
typedef struct destroy_worklist {
  cjson_node **items;
  size_t cap;
  size_t len;
} destroy_worklist;

// Queues child on wl for destruction, so a teardown never recurses per level.
// A NULL child is ignored. The worklist grows with the default allocator,
// since it's scratch state unrelated to any node's m_procs. When it can't
// grow, the child is destroyed at once by a recursive cjson__destroy(): Stack
// use then follows the depth of that one subtree, but only when the tree is
// deep and memory has run out together.
static void destroy_worklist_push(destroy_worklist *wl, cjson_node *child) {
  if (!child) return;
  if (wl->len == wl->cap) {
    size_t new_cap = wl->cap == 0 ? 32 : wl->cap * 2;
    cjson_node **grown =
        ccol__default_mem_realloc(wl->items, new_cap * sizeof(*wl->items));
    if (!grown) {
      cjson__destroy((cjson)child);
      return;
    }
    wl->items = grown;
    wl->cap = new_cap;
  }
  wl->items[wl->len++] = child;
}

// Queues the child of one dictionary entry on the worklist in dtor_ctx. It's
// the value destructor that node_clear_value() hands to
// chashmap_destroy_with_dtor(), which walks the map's storage directly and so
// reaches every entry without an allocation that could fail and leak the rest.
static void cjson_enqueue_dict_child(const cmap_pair *val_pair,
                                     void *dtor_ctx) {
  cjson_node *child = cjson_read_child(val_pair->ptr);
  destroy_worklist_push((destroy_worklist *)dtor_ctx, child);
}

// Pushes item onto the search stack of node_reaches(). *stack starts at the
// caller's inline_stack (inline_cap entries) and moves to the default heap once
// it outgrows it, so attaching a small subtree allocates nothing. *cap and *len
// describe the stack. Returns false, with nothing changed, when the growth
// fails.
static bool cycle_stack_push(cjson_node ***stack, cjson_node **inline_stack,
                             size_t inline_cap, size_t *cap, size_t *len,
                             cjson_node *item) {
  if (*len == *cap) {
    size_t new_cap = *cap * 2;
    cjson_node **grown;
    if (*stack == inline_stack) {
      grown = ccol__default_mem_alloc(new_cap * sizeof(**stack));
      if (!grown) return false;
      memcpy(grown, inline_stack, inline_cap * sizeof(**stack));
    } else {
      grown = ccol__default_mem_realloc(*stack, new_cap * sizeof(**stack));
      if (!grown) return false;
    }
    *stack = grown;
    *cap = new_cap;
  }
  (*stack)[(*len)++] = item;
  return true;
}

// Tells whether needle is haystack or lies somewhere below it.
// cjson_list_push() and cjson_dictionary_set() use it to refuse a child whose
// subtree reaches the target: The attach would close a cycle, and the teardown
// would then free a node twice. A needle that was never attached has no parent,
// so nothing can reach it; that answer costs O(1) and keeps a tree built bottom
// up linear. The search uses an explicit stack, because the subtree can be
// deeper than the native stack allows. *incomplete is set when the stack can't
// grow before needle is found; the caller must treat that as out of memory,
// never as safe. Counts visited nodes in cjson__cycle_search_nodes_for_tests.
static bool node_reaches(cjson_node *haystack, const cjson_node *needle,
                         bool *incomplete) {
  *incomplete = false;
  if ((const cjson_node *)haystack == needle) return true;
  if (!needle->attached) return false;
  if (haystack->type != CJSON_LIST && haystack->type != CJSON_DICTIONARY)
    return false;

  cjson_node *inline_stack[32];
  cjson_node **stack = inline_stack;
  size_t cap = sizeof(inline_stack) / sizeof(inline_stack[0]);
  size_t len = 0;
  bool found = false;

  if (!cycle_stack_push(&stack, inline_stack, cap, &cap, &len, haystack)) {
    *incomplete = true;
    return false;
  }

  while (len > 0 && !found) {
    cjson_node *n = stack[--len];
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add_explicit(&cjson__cycle_search_nodes_for_tests, 1,
                              memory_order_relaxed);
#endif
    if ((const cjson_node *)n == needle) {
      found = true;
      break;
    }
    if (n->type == CJSON_LIST) {
      size_t cnt = list_len_of(n);
      for (size_t i = 0; i < cnt; i++) {
        cjson_node *c = *(cjson_node **)cvector_at(n->value.list, i);
        if (!cycle_stack_push(&stack, inline_stack,
                              sizeof(inline_stack) / sizeof(*inline_stack),
                              &cap, &len, c)) {
          *incomplete = true;
          goto done;
        }
      }
    } else if (n->type == CJSON_DICTIONARY) {
      const cmap_pair *kp, *vp;
      for (const ccol_chmap_entry_ref *e = dict_first_entry(n); e;) {
        e = ccol_chmap_entry_read(e, &kp, &vp);
        cjson_node *c = cjson_read_child(vp->ptr);
        if (!cycle_stack_push(&stack, inline_stack,
                              sizeof(inline_stack) / sizeof(*inline_stack),
                              &cap, &len, c)) {
          *incomplete = true;
          goto done;
        }
      }
    }
  }

done:
  if (stack != inline_stack) ccol__default_mem_free(stack);
  return found;
}

// The thread-local node pool. It holds only nodes of the default allocator
// (m_procs == NULL), so node_pool_drain() frees them with plain free(). A
// pooled node keeps the next pointer of the free list in its own bytes, read
// back with memcpy. NODE_POOL_CAP bounds the list.
#define NODE_POOL_CAP 512U
#if defined(CCOL__EMULATE_DARWIN_TLS)
// With CCOL__EMULATE_DARWIN_TLS, one thread's pool and error buffer live in a
// heap block whose pointer is the value of the pool key, so node_pool_drain()
// gets it as its argument and reads no __thread variable. A thread has a value
// on the key exactly as long as it has a block.
typedef struct {
  cjson_node *head;
  unsigned sz;
  char *err_buf;
} cjson_tls;
static cjson_tls *cjson_tls_peek(void);
static cjson_tls *cjson_tls_get(void);
#else
// Elsewhere the pool is plain __thread state. pool_key_armed records that this
// thread has set its value on the key, which needs to happen once per thread,
// so the lock is taken once per thread and not on every refill.
static __thread cjson_node *node_pool_head = NULL;
static __thread unsigned node_pool_sz = 0;
static __thread bool pool_key_armed = false;
#endif
// Test counters: Arms of the pool key that took the lock, and the nodes that
// the pools of all threads hold.
#ifdef RUNNING_UNIT_TESTS
atomic_ulong cjson__pool_key_lock_count_for_tests;
atomic_long cjson__pool_population_for_tests;
#endif

// Allocates a zeroed node of the given type with allocator mp; every node of
// the module is born here. With the default allocator (mp NULL) it reuses a
// node from this thread's pool when there is one, which saves a calloc and a
// free per node when documents are parsed and destroyed in a loop. Returns NULL
// when the allocation fails. Takes a node off the thread's pool and lowers
// cjson__pool_population_for_tests.
static cjson_node *node_alloc(cjson_node_type type, ccol_memmgmt_procs *mp) {
  cjson_node *n;
#if defined(CCOL__EMULATE_DARWIN_TLS)
  cjson_tls *t = mp == NULL ? cjson_tls_peek() : NULL;
  if (t && t->head) {
    n = t->head;
    cjson_node *next;
    memcpy(&next, (cjson_node **)n, sizeof(next));
    t->head = next;
    t->sz--;
#else
  if (mp == NULL && node_pool_head) {
    n = node_pool_head;
    cjson_node *next;
    memcpy(&next, (cjson_node **)n, sizeof(next));
    node_pool_head = next;
    node_pool_sz--;
#endif
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_sub_explicit(&cjson__pool_population_for_tests, 1,
                              memory_order_relaxed);
#endif
    memset(n, 0, sizeof(*n));
  } else {
    n = ccol__mem_calloc(mp, 1, sizeof(*n));
    if (!n) return NULL;
  }
  n->type = type;
  n->attached = false;
  n->m_procs = mp;
  return n;
}

// The key whose destructor (node_pool_drain) empties a thread's pool and frees
// its error buffer at thread exit.
static ccol__thread_ls_key pool_pthread_key;

// True once do_pool_key_init() has created the key and the rwlock, false again
// after pool_key_fini() deletes the key. It stays false when either init call
// fails, so check it before using the key or the lock.
static atomic_bool pool_key_live = false;

// True once the shared object's destructor has run. A later first use (from an
// application destructor, say) must not create a key that nothing would delete
// or drain, so the pool stays unarmed and nodes go straight to the allocator.
static atomic_bool pool_key_finalized = false;

// Held around the check of pool_key_live and the ccol__thread_ls_set() that
// acts on it (read side), and around the delete of the key in pool_key_fini()
// (write side), so a set never runs on a key that is being deleted.
static ccol__rw_lock pool_key_rwlock;

static void node_pool_drain(void *);

// Runs do_pool_key_init() once, lazily, on the first node_free() or parse error
// that needs the pool key.
static ccol__once_flag pool_key_once = CCOL__ONCE_INIT;

// Creates pool_key_rwlock and pool_pthread_key, the two primitives the pool and
// the error buffer need, and only then sets pool_key_live. It runs once under
// pool_key_once and does nothing once pool_key_finalized is set.
static void do_pool_key_init(void) {
  if (atomic_load(&pool_key_finalized)) return;
  if (ccol__rw_lock_init(pool_key_rwlock) != 0) return;
  if (ccol__thread_ls_key_create(pool_pthread_key, node_pool_drain) != 0)
    return;
  atomic_store(&pool_key_live, true);
}

// Tears the pool key down when the shared object is unloaded (dlclose or exit),
// so repeated dlopen and dlclose cycles don't use up PTHREAD_KEYS_MAX and the
// calling thread's pool is freed while the code is still mapped. It sets
// pool_key_finalized, drains the calling thread's pool, and then clears
// pool_key_live and deletes the key under the write lock of pool_key_rwlock,
// so no node_free() can be inside ccol__thread_ls_set() on the key meanwhile.
// Without a live key it does nothing more (the lock was never initialized).
__attribute__((destructor)) static void pool_key_fini(void) {
  atomic_store(&pool_key_finalized, true);
  if (!atomic_load(&pool_key_live)) return;
#if defined(CCOL__EMULATE_DARWIN_TLS)
  cjson_tls *t = cjson_tls_peek();
  if (t) {
    (void)ccol__thread_ls_set(pool_pthread_key, NULL);
    node_pool_drain(t);
  }
#else
  node_pool_drain(NULL);
#endif
  ccol__rw_lock_wrlock(pool_key_rwlock);
  atomic_store(&pool_key_live, false);
  ccol__thread_ls_key_delete(pool_pthread_key);
  ccol__rw_lock_unlock(pool_key_rwlock);
}

#if defined(CCOL__EMULATE_DARWIN_TLS)
// Gives the calling thread's pool block, or NULL when it has none or the key
// isn't live; node_alloc() uses it because it must not create a block.
static cjson_tls *cjson_tls_peek(void) {
  if (!atomic_load_explicit(&pool_key_live, memory_order_acquire)) return NULL;
  return (cjson_tls *)ccol__thread_ls_get(pool_pthread_key);
}

// Gives the calling thread's pool block, creating it and setting it on the pool
// key when it has none; node_free() and cjson_report_err() need a block to keep
// nodes or a message in. NULL when the key isn't live, the set fails or memory
// runs out. Takes the read side of pool_key_rwlock for the set. The caller must
// have run the once-guard.
static cjson_tls *cjson_tls_get(void) {
  cjson_tls *t = cjson_tls_peek();
  if (t || !atomic_load(&pool_key_live)) return t;
  t = (cjson_tls *)calloc(1, sizeof(*t));
  if (!t) return NULL;
  bool set = false;
  ccol__rw_lock_rdlock(pool_key_rwlock);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&cjson__pool_key_lock_count_for_tests, 1,
                            memory_order_relaxed);
#endif
  if (atomic_load(&pool_key_live) &&
      ccol__thread_ls_set(pool_pthread_key, t) == 0)
    set = true;
  ccol__rw_lock_unlock(pool_key_rwlock);
  if (!set) {
    free(t);
    return NULL;
  }
  return t;
}
#else
// Sets this thread's value on the pool key, once per thread, so that
// node_pool_drain() runs at thread exit; node_free() and cjson_report_err()
// need that before they keep anything per thread. It takes the read side of
// pool_key_rwlock, checks pool_key_live again under it and sets pool_key_armed
// on success. The caller must have run the once-guard. Always inlined so
// node_free() keeps its shape.
static inline __attribute__((always_inline)) void pool_key_arm(void) {
  if (!pool_key_armed && atomic_load(&pool_key_live)) {
    ccol__rw_lock_rdlock(pool_key_rwlock);
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add_explicit(&cjson__pool_key_lock_count_for_tests, 1,
                              memory_order_relaxed);
#endif
    if (atomic_load(&pool_key_live) &&
        ccol__thread_ls_set(pool_pthread_key, (void *)1) == 0)
      pool_key_armed = true;
    ccol__rw_lock_unlock(pool_key_rwlock);
  }
}

// This thread's message buffer for a failed parse (see cjson_report_err());
// node_pool_drain() frees it.
static __thread char *cjson_err_buf;
#endif

// Frees a node; every node of the module dies here. A custom allocator frees it
// directly. With the default allocator the node goes back on this thread's pool
// when the pool has room, so node_alloc() can reuse it. A thread whose key
// can't be armed (no key left, a failed set, or a call after pool_key_fini())
// pools nothing, because nothing would ever drain it. The unlocked check of
// pool_key_live only skips a rwlock that was never initialized; the check under
// the lock is what makes the race with pool_key_fini() safe. Changes the
// thread's pool and cjson__pool_population_for_tests.
static void node_free(cjson_node *n) {
  if (n->m_procs != NULL) {
    ccol__mem_free(n->m_procs, n);
    return;
  }
#if defined(CCOL__EMULATE_DARWIN_TLS)
  ccol__call_once(pool_key_once, do_pool_key_init);
  cjson_tls *t = cjson_tls_get();
  if (!t || t->sz >= NODE_POOL_CAP) {
    free(n);
    return;
  }
  memcpy((cjson_node **)n, &t->head, sizeof(t->head));
  t->head = n;
  t->sz++;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&cjson__pool_population_for_tests, 1,
                            memory_order_relaxed);
#endif
#else
  if (node_pool_sz >= NODE_POOL_CAP) {
    free(n);
    return;
  }
  ccol__call_once(pool_key_once, do_pool_key_init);
  if (node_pool_sz == 0) {
    pool_key_arm();
    if (!pool_key_armed) {
      free(n);
      return;
    }
  }
  memcpy((cjson_node **)n, &node_pool_head, sizeof(node_pool_head));
  node_pool_head = n;
  node_pool_sz++;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&cjson__pool_population_for_tests, 1,
                            memory_order_relaxed);
#endif
#endif
}

// Frees every pooled node of the calling thread and its error buffer. It's the
// destructor of the pool key, so a thread's pool doesn't leak at thread exit,
// and pool_key_fini() calls it for the unloading thread. With
// CCOL__EMULATE_DARWIN_TLS it frees the block that arg names. Otherwise it
// resets the __thread pool and clears pool_key_armed: The C library has already
// cleared the key's value, and if a later key destructor refills the pool, the
// refill must set the value again so the C library runs this destructor once
// more. Lowers cjson__pool_population_for_tests.
#if defined(CCOL__EMULATE_DARWIN_TLS)
static void node_pool_drain(void *arg) {
  cjson_tls *t = (cjson_tls *)arg;
  if (!t) return;
  cjson_node *n = t->head;
  while (n) {
    cjson_node *next;
    memcpy(&next, (cjson_node **)n, sizeof(next));
    free(n);
    n = next;
  }
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_sub_explicit(&cjson__pool_population_for_tests, (long)t->sz,
                            memory_order_relaxed);
#endif
  free(t->err_buf);
  free(t);
}
#else
static void node_pool_drain(void *arg) {
  (void)arg;
  cjson_node *n = node_pool_head;
  while (n) {
    cjson_node *next;
    memcpy(&next, (cjson_node **)n, sizeof(next));
    free(n);
    n = next;
  }
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_sub_explicit(&cjson__pool_population_for_tests,
                            (long)node_pool_sz, memory_order_relaxed);
#endif
  node_pool_head = NULL;
  node_pool_sz = 0;
  free(cjson_err_buf);
  cjson_err_buf = NULL;
  pool_key_armed = false;
}
#endif

// Gives the size of the calling thread's pool, so the white-box tests can check
// the cap and that threads don't share a pool.
#ifdef RUNNING_UNIT_TESTS
#if defined(CCOL__EMULATE_DARWIN_TLS)
size_t cjson_debug_pool_size(void) {
  cjson_tls *t = cjson_tls_peek();
  return t ? (size_t)t->sz : 0;
}
#else
size_t cjson_debug_pool_size(void) { return (size_t)node_pool_sz; }
#endif
#endif

// Frees the payload of n (string bytes or a container store) and pushes every
// direct child onto wl instead of recursing; node_clear(), cjson__destroy() and
// the worklist drain build every teardown on it. n->type and n itself stay.
static void node_clear_value(cjson_node *n, destroy_worklist *wl) {
  switch (n->type) {
    case CJSON_STRING:
      ccol__mem_free(n->m_procs, n->value.string);
      n->value.string = NULL;
      break;
    case CJSON_LIST: {
      size_t cnt = list_len_of(n);
      for (size_t i = 0; i < cnt; i++) {
        cjson_node *child = *(cjson_node **)cvector_at(n->value.list, i);
        destroy_worklist_push(wl, child);
      }
      cvector__destroy(n->value.list);
      n->value.list = NULL;
      break;
    }
    case CJSON_DICTIONARY: {
      chashmap_destroy_with_dtor(n->value.dictionary, cjson_enqueue_dict_child,
                                 wl);
      n->value.dictionary = NULL;
      break;
    }
    default:
      break;
  }
}

// Destroys every node on wl, structs included, to finish a teardown that
// node_clear_value() started. Each drained node adds its own children, so the
// loop ends when the whole subtree is gone.
static void destroy_worklist_drain(destroy_worklist *wl) {
  while (wl->len > 0) {
    cjson_node *n = wl->items[--wl->len];
    node_clear_value(n, wl);
    node_free(n);
  }
}

// Frees everything the value of n owns, without freeing n.
// node_reinit_scalar_checked() uses it to drop an old value before it writes a
// new scalar in place. The old value can be a tree of any depth, so it goes
// through the worklist.
static void node_clear(cjson_node *n) {
  destroy_worklist wl = {NULL, 0, 0};
  node_clear_value(n, &wl);
  destroy_worklist_drain(&wl);
  ccol__default_mem_free(wl.items);
}

// Checks a scalar payload (type, raw, raw_size) from cjson_set() or a direct
// cjson__set_typed() call, touching no node, so a set can refuse before
// anything changes. Gives ccol_invalid_args for a container or unknown type
// (which includes CJSON__TYPE_UNSUPPORTED), a NULL raw for anything but
// CJSON_NULL, a non-NULL void *, a raw_size that doesn't fit the type (a direct
// call can pass a narrower buffer), a non-finite float and a string that isn't
// UTF-8. After ccol_success the store can only fail for lack of memory.
static ccol_retval scalar_payload_check(cjson_node_type type, void *raw,
                                        size_t raw_size) {
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

  if (type != CJSON_NULL && !raw) return ccol_invalid_args;

  if (type == CJSON_NULL && raw && raw_size == sizeof(void *) &&
      *(void **)raw != NULL)
    return ccol_invalid_args;

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

  if (type == CJSON_BOOL && raw_size != sizeof(bool)) return ccol_invalid_args;

  if (type == CJSON_STRING && raw_size != sizeof(const char *))
    return ccol_invalid_args;

  if (type == CJSON_STRING && !utf8_cstr_is_valid(*(const char **)raw))
    return ccol_invalid_args;
  return ccol_success;
}

// Overwrites n with a scalar payload that scalar_payload_check() accepted and
// frees whatever the old value owned; cjson_set() uses it to update a leaf in
// place, so a borrowed handle to the leaf stays valid. is_signed says how to
// widen an integer. The string copy (with n->m_procs) happens first, so
// ccol_not_enough_memory leaves n intact; nothing after node_clear() can fail.
// Integers are read through memcpy into a fixed-width type, and an unsigned
// value above LLONG_MAX becomes the nearest CJSON_FLOAT, as the parser reads
// the same literal. A NULL C string becomes CJSON_NULL.
static ccol_retval node_reinit_scalar_checked(cjson_node *n,
                                              cjson_node_type type, void *raw,
                                              size_t raw_size, bool is_signed) {
  double pre_d = 0.0;
  if (type == CJSON_FLOAT)
    pre_d = raw_size == sizeof(float) ? (double)*(float *)raw : *(double *)raw;

  char *new_str = NULL;
  if (type == CJSON_STRING) {
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
    case CJSON_NULL:
      break;
    case CJSON_BOOL:
      n->value.boolean = *(bool *)raw;
      break;
    case CJSON_INTEGER: {
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
              n->type = CJSON_FLOAT;
              n->value.number = (double)u;
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
    case CJSON_FLOAT:
      n->value.number = pre_d;
      break;
    case CJSON_STRING:
      if (!new_str)
        n->type = CJSON_NULL;
      else
        n->value.string = new_str;
      break;
    default:
      break;
  }
  return ccol_success;
}

// Checks a payload and then stores it in n, for a set of an existing list
// element. Every check runs before n changes, so any failure leaves n intact.
// Gives the codes of the two steps.
static ccol_retval node_reinit_scalar(cjson_node *n, cjson_node_type type,
                                      void *raw, size_t raw_size,
                                      bool is_signed) {
  ccol_retval r = scalar_payload_check(type, raw, raw_size);
  if (r != ccol_success) return r;
  return node_reinit_scalar_checked(n, type, raw, raw_size, is_signed);
}

// Allocates a scalar node with mp from a payload that scalar_payload_check()
// accepted, for a set that creates a new dictionary key. Gives ccol_success
// with the node in *out, or ccol_not_enough_memory with *out NULL.
static ccol_retval node_make_scalar(cjson_node_type type, void *raw,
                                    size_t raw_size, bool is_signed,
                                    ccol_memmgmt_procs *mp, cjson_node **out) {
  *out = NULL;
  cjson_node *n = node_alloc(CJSON_NULL, mp);
  if (!n) return ccol_not_enough_memory;
  ccol_retval r = node_reinit_scalar_checked(n, type, raw, raw_size, is_signed);
  if (r != ccol_success) {
    node_free(n);
    return r;
  }
  *out = n;
  return ccol_success;
}

// Replaces *mp with its interned copy, so no node keeps the caller's pointer
// and the caller may free its struct; every public entry point that takes an
// allocator calls it first. Gives false when the intern table is full.
static inline bool cjson_intern_procs(ccol_memmgmt_procs **mp) {
  if (*mp == NULL) return true;
  return ccol_procs_intern(*mp, mp) == ccol_success;
}

// The factories behind the public ones and the parser and clone, for an mp
// that is already interned (or NULL). Each gives a new node or NULL. A
// non-finite double gives NULL, a NULL string gives a CJSON_NULL node and a
// string that isn't UTF-8 gives NULL. A new container has no store yet.
static cjson cjson_create_null_interned(ccol_memmgmt_procs *mp) {
  return (cjson)node_alloc(CJSON_NULL, mp);
}

static cjson cjson_create_bool_interned(bool val, ccol_memmgmt_procs *mp) {
  cjson_node *n = node_alloc(CJSON_BOOL, mp);
  if (n) n->value.boolean = val;
  return (cjson)n;
}

static cjson cjson_create_int_interned(long long val, ccol_memmgmt_procs *mp) {
  cjson_node *n = node_alloc(CJSON_INTEGER, mp);
  if (n) n->value.integer = val;
  return (cjson)n;
}

static cjson cjson_create_double_interned(double val, ccol_memmgmt_procs *mp) {
  if (!isfinite(val)) return NULL;
  cjson_node *n = node_alloc(CJSON_FLOAT, mp);
  if (n) n->value.number = val;
  return (cjson)n;
}

static cjson cjson_create_string_interned(const char *val,
                                          ccol_memmgmt_procs *mp) {
  if (!val) return cjson_create_null_interned(mp);
  if (!utf8_cstr_is_valid(val)) return NULL;
  cjson_node *n = node_alloc(CJSON_STRING, mp);
  if (!n) return NULL;
  n->value.string = ccol_strdup(mp, val);
  if (!n->value.string) {
    node_free(n);
    return NULL;
  }
  return (cjson)n;
}

static cjson cjson_create_list_interned(ccol_memmgmt_procs *mp) {
  return (cjson)node_alloc(CJSON_LIST, mp);
}

static cjson cjson_create_dictionary_interned(ccol_memmgmt_procs *mp) {
  return (cjson)node_alloc(CJSON_DICTIONARY, mp);
}

// The public factories: Each creates one node with allocator mp (NULL for the
// default) and gives it to the caller, or NULL. They intern mp first, because
// nodes must never point at the caller's struct, and fail with NULL when the
// intern table is full.
cjson cjson_create_null_mp(ccol_memmgmt_procs *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_null_interned(mp);
}

cjson cjson_create_bool_mp(bool val, ccol_memmgmt_procs *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_bool_interned(val, mp);
}

cjson cjson_create_int_mp(long long val, ccol_memmgmt_procs *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_int_interned(val, mp);
}

cjson cjson_create_double_mp(double val, ccol_memmgmt_procs *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_double_interned(val, mp);
}

cjson cjson_create_string_mp(const char *val, ccol_memmgmt_procs *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_string_interned(val, mp);
}

cjson cjson_create_list_mp(ccol_memmgmt_procs *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_list_interned(mp);
}

cjson cjson_create_dictionary_mp(ccol_memmgmt_procs *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_dictionary_interned(mp);
}

// Frees node and every descendant; it's the body of cjson_destroy(), and the
// rest of the module uses it to drop subtrees. node itself is freed directly
// and never pushed on the worklist: A push that can't grow falls back to
// cjson__destroy() on what it pushes, which would be this same node.
void cjson__destroy(cjson node) {
  if (!node) return;
  cjson_node *n = (cjson_node *)node;

  destroy_worklist wl = {NULL, 0, 0};
  node_clear_value(n, &wl);
  node_free(n);

  destroy_worklist_drain(&wl);
  ccol__default_mem_free(wl.items);
}

// Gives the type tag of node, or CJSON_NULL for a NULL handle, so a caller can
// check a node before it calls a typed getter.
cjson_node_type cjson_type(cjson node) {
  if (!node) return CJSON_NULL;
  return ((cjson_node *)node)->type;
}

// The typed getters: Each gives the value of a leaf (or the length of a
// container) for the caller. They call ccol__fatal_err() for a NULL node or
// another type, because reading the wrong member of the value union is a bug.
bool cjson_bool_val(cjson node) {
  cjson_node *n = (cjson_node *)node;
  if (!n || n->type != CJSON_BOOL)
    ccol__fatal_err("cjson_bool_val: node is %s, expected CJSON_BOOL",
                    n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.boolean;
}

long long cjson_int_val(cjson node) {
  cjson_node *n = (cjson_node *)node;
  if (!n || n->type != CJSON_INTEGER)
    ccol__fatal_err("cjson_int_val: node is %s, expected CJSON_INTEGER",
                    n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.integer;
}

double cjson_double_val(cjson node) {
  cjson_node *n = (cjson_node *)node;
  if (!n || n->type != CJSON_FLOAT)
    ccol__fatal_err("cjson_double_val: node is %s, expected CJSON_FLOAT",
                    n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.number;
}

const char *cjson_str_val(cjson node) {
  cjson_node *n = (cjson_node *)node;
  if (!n || n->type != CJSON_STRING)
    ccol__fatal_err("cjson_str_val: node is %s, expected CJSON_STRING",
                    n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.string;
}

size_t cjson_list_len(cjson node) {
  cjson_node *n = (cjson_node *)node;
  if (!n || n->type != CJSON_LIST)
    ccol__fatal_err("cjson_list_len: node is %s, expected CJSON_LIST",
                    n ? cjson_type_str((cjson)n) : "NULL");
  return list_len_of(n);
}

size_t cjson_dictionary_size(cjson node) {
  cjson_node *n = (cjson_node *)node;
  if (!n || n->type != CJSON_DICTIONARY)
    ccol__fatal_err(
        "cjson_dictionary_size: node is %s, expected CJSON_DICTIONARY",
        n ? cjson_type_str((cjson)n) : "NULL");
  return dict_size_of(n);
}

// Appends child to the list arr, which takes ownership on success; it's how a
// caller builds a list. The ownership split of the header holds on every path:
// ccol_invalid_args touches nothing, every other failure destroys child. The
// refusals that leave child alone (already attached, child == arr) are decided
// before the type check of arr, so passing one non-list handle as both
// arguments can't free a node the caller still owns. A cycle check that runs
// out of memory moves ownership like a failed insert, so ccol_not_enough_memory
// means one thing. Sets child->attached.
ccol_retval cjson_list_push(cjson arr, cjson child) {
  if (!child) return ccol_invalid_args;
  cjson_node *c = (cjson_node *)child;

  if (c->attached || (const void *)c == (const void *)arr) {
    return ccol_invalid_args;
  }

  cjson_node *n = (arr && ((cjson_node *)arr)->type == CJSON_LIST)
                      ? (cjson_node *)arr
                      : NULL;
  if (!n) {
    return ccol_invalid_args;
  }
  bool cycle_incomplete;
  if (node_reaches(c, (const cjson_node *)n, &cycle_incomplete)) {
    return ccol_invalid_args;
  }
  if (cycle_incomplete) {
    cjson__destroy(child);
    return ccol_not_enough_memory;
  }
  cvec store = list_store(n);
  ccol_retval r = store ? cvector_push_back(store, &c) : ccol_not_enough_memory;
  if (r != ccol_success) {
    cjson__destroy(child);
    return r;
  }
  c->attached = true;
  return ccol_success;
}

// Gives element index of list arr as a borrowed reference, for a caller that
// reads a list; NULL for a bad index, a NULL arr or a non-list.
cjson cjson_list_get(cjson arr, size_t index) {
  if (!arr) return NULL;
  cjson_node *n = (cjson_node *)arr;
  if (n->type != CJSON_LIST) return NULL;
  return (cjson)list_child_at(n, index);
}

// Destroys the old child of a dictionary slot and stores child there, for a set
// or a parsed duplicate key that replaces a value. slot_vp is the slot that
// ccol_chmap_insert_or_get_elem() found. Freeing the old child doesn't change
// the map, so slot_vp stays valid. Sets child->attached.
static void dict_slot_replace_child(const cmap_pair *slot_vp,
                                    cjson_node *child) {
  cjson_node *old_child = cjson_read_child(slot_vp->ptr);
  cjson__destroy((cjson)old_child);
  memcpy((void *)slot_vp->ptr, &child, sizeof(child));
  child->attached = true;
}

// Inserts key with child in obj, or replaces its value; it's how a caller fills
// a dictionary. Ownership splits as in cjson_list_push(). Only the self-attach
// is decided before the type check; the refusal of an attached child comes
// after the lookup, because an attached child may be the value key already
// holds, which is a no-op. A replace frees the old child and stores the new one
// with nothing in between that can fail. Sets child->attached.
ccol_retval cjson_dictionary_set(cjson obj, const char *key, cjson child) {
  if (!child) return ccol_invalid_args;
  cjson_node *c = (cjson_node *)child;

  if ((const void *)c == (const void *)obj) return ccol_invalid_args;

  cjson_node *n = (obj && ((cjson_node *)obj)->type == CJSON_DICTIONARY)
                      ? (cjson_node *)obj
                      : NULL;
  if (!n || !key || !utf8_cstr_is_valid(key)) {
    return ccol_invalid_args;
  }

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};

  if (c->attached) {
    cmap_pair held_vp;
    if (dict_lookup(n, &kp, &held_vp) == ccol_success &&
        cjson_read_child(held_vp.ptr) == c)
      return ccol_success;
    return ccol_invalid_args;
  }

  bool cycle_incomplete;
  if (node_reaches(c, (const cjson_node *)n, &cycle_incomplete)) {
    return ccol_invalid_args;
  }
  if (cycle_incomplete) {
    cjson__destroy(child);
    return ccol_not_enough_memory;
  }

  cmap_pair vp = {.ptr = &c, .size = sizeof(c)};
  cmap_pair old_vp;
  chmap store = dict_store(n);
  ccol_retval r = store
                      ? ccol_chmap_insert_or_get_elem(store, &kp, &vp, &old_vp)
                      : ccol_not_enough_memory;
  if (r == ccol_success) {
    c->attached = true;
    return ccol_success;
  }
  if (r == ccol_key_already_present) {
    dict_slot_replace_child(&old_vp, c);
    return ccol_success;
  }
  cjson__destroy((cjson)child);
  return r;
}

// Gives the value of key in obj as a borrowed reference, for a caller that
// reads a dictionary; NULL for a missing key or bad arguments.
cjson cjson_dictionary_get(cjson obj, const char *key) {
  if (!obj || !key) return NULL;
  cjson_node *n = (cjson_node *)obj;
  if (n->type != CJSON_DICTIONARY) return NULL;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair vp;
  if (dict_lookup(n, &kp, &vp) != ccol_success) return NULL;
  return cjson_read_child(vp.ptr);
}

// Removes and destroys element index of arr, for callers and for
// cjson__delete(). The later elements move left, O(n) in the elements after
// index. ccol_invalid_args for a NULL or non-list arr or a bad index.
ccol_retval cjson_list_remove(cjson arr, size_t index) {
  if (!arr) return ccol_invalid_args;
  cjson_node *n = (cjson_node *)arr;
  if (n->type != CJSON_LIST) return ccol_invalid_args;
  size_t cnt = list_len_of(n);
  if (index >= cnt) return ccol_invalid_args;

  cjson_node *child = *(cjson_node **)cvector_at(n->value.list, index);
  cjson__destroy((cjson)child);

  for (size_t i = index; i + 1 < cnt; i++) {
    cjson_node **dst = (cjson_node **)cvector_at(n->value.list, i);
    cjson_node **src = (cjson_node **)cvector_at(n->value.list, i + 1);
    *dst = *src;
  }
  cjson_node *tmp = NULL;
  cvector_pop_back(n->value.list, &tmp);
  return ccol_success;
}

// Removes the entry of key from obj and destroys its value, for callers and for
// cjson__delete(). It reads the child before the delete and frees it only
// after, so the map never points at freed memory. ccol_invalid_args for bad
// arguments, ccol_key_not_found when the key is absent.
ccol_retval cjson_dictionary_remove(cjson obj, const char *key) {
  if (!obj || !key) return ccol_invalid_args;
  cjson_node *n = (cjson_node *)obj;
  if (n->type != CJSON_DICTIONARY) return ccol_invalid_args;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair vp;
  if (dict_lookup(n, &kp, &vp) != ccol_success) return ccol_key_not_found;
  cjson_node *child = cjson_read_child(vp.ptr);
  ccol_retval r = chashmap_delete_elem(n->value.dictionary, &kp);
  if (r == ccol_success) cjson__destroy((cjson)child);
  return r;
}

// Steps the cursor it onto the member it holds, so a caller can walk a
// dictionary in insertion order, and stores that member's successor. Gives
// false, with key and value cleared, at the end. Because the successor is read
// on each step, removing the current member leaves the cursor valid.
bool cjson_dictionary_next(cjson_dictionary_iter *it) {
  if (!it) return false;
  const ccol_chmap_entry_ref *e = (const ccol_chmap_entry_ref *)it->cjson__next;
  if (!e) {
    it->key = NULL;
    it->value = NULL;
    return false;
  }
  const cmap_pair *kp, *vp;
  it->cjson__next = ccol_chmap_entry_read(e, &kp, &vp);
  it->key = (const char *)kp->ptr;
  it->value = (cjson)cjson_read_child(vp->ptr);
  return true;
}

// Starts the cursor it on dict and steps onto the oldest member. Gives false
// for an empty, NULL or non-dictionary dict.
bool cjson_dictionary_first(cjson dict, cjson_dictionary_iter *it) {
  if (!it) return false;
  cjson_node *n = (cjson_node *)dict;
  it->cjson__next =
      (n && n->type == CJSON_DICTIONARY) ? dict_first_entry(n) : NULL;
  return cjson_dictionary_next(it);
}

// The deepest nesting cjson_clone() accepts, in containers. A tree from
// cjson_list_push() and cjson_dictionary_set() can be of any depth; this has
// the parser's value, so whatever parses can be cloned. The clone walks with a
// heap stack, so the limit is a policy, not a stack bound.
#define CJSON_CLONE_MAX_DEPTH 500

// One container on the explicit stack of clone_tree(). dst is the copy, already
// attached to its parent; depth is its nesting level. taken counts the copied
// elements of a list, and next is the next member of a dictionary to copy, in
// insertion order.
typedef struct {
  cjson_node *src;
  cjson_node *dst;
  unsigned int depth;
  size_t taken;
  const ccol_chmap_entry_ref *next;
} clone_frame;

// Pushes a frame onto the stack of clone_tree(), growing it like
// serialize_stack_push() does. Gives false when the growth fails.
static bool clone_stack_push(clone_frame **frames, clone_frame *inline_frames,
                             size_t inline_cap, size_t *cap, size_t *len,
                             const clone_frame *frame) {
  if (*len == *cap) {
    size_t new_cap = *cap * 2;
    clone_frame *grown;
    if (*frames == inline_frames) {
      grown = ccol__default_mem_alloc(new_cap * sizeof(**frames));
      if (!grown) return false;
      memcpy(grown, inline_frames, inline_cap * sizeof(**frames));
    } else {
      grown = ccol__default_mem_realloc(*frames, new_cap * sizeof(**frames));
      if (!grown) return false;
    }
    *frames = grown;
    *cap = new_cap;
  }
  (*frames)[(*len)++] = *frame;
  return true;
}

// Copies one node without its children, a scalar whole and a container empty;
// clone_tree() builds every node of the copy with it. Gives NULL when an
// allocation fails. The source string is already valid UTF-8, so the copy
// needs no check.
static cjson_node *clone_shallow(cjson_node *src) {
  ccol_memmgmt_procs *mp = src->m_procs;
  switch (src->type) {
    case CJSON_NULL:
      return (cjson_node *)cjson_create_null_interned(mp);
    case CJSON_BOOL:
      return (cjson_node *)cjson_create_bool_interned(src->value.boolean, mp);
    case CJSON_INTEGER:
      return (cjson_node *)cjson_create_int_interned(src->value.integer, mp);
    case CJSON_FLOAT:
      return (cjson_node *)cjson_create_double_interned(src->value.number, mp);
    case CJSON_STRING: {
      cjson_node *n = node_alloc(CJSON_STRING, mp);
      if (!n) return NULL;
      size_t slen = strlen(src->value.string);
      n->value.string = ccol__mem_alloc(mp, slen + 1);
      if (!n->value.string) {
        node_free(n);
        return NULL;
      }
      memcpy(n->value.string, src->value.string, slen + 1);
      return n;
    }
    case CJSON_LIST:
      return (cjson_node *)cjson_create_list_interned(mp);
    case CJSON_DICTIONARY:
      return (cjson_node *)cjson_create_dictionary_interned(mp);
  }
  return NULL;
}

// Makes a deep copy of src with the same allocator, for cjson_clone(). Gives
// NULL (after destroying the half-built copy) when an allocation fails or the
// depth passes CJSON_CLONE_MAX_DEPTH. Each container is created empty, attached
// to its parent and only then filled, which is what lets the walk be a loop
// over an explicit stack. Children go straight into the store without the
// checks of cjson_list_push() and cjson_dictionary_set(): The copy is new, and
// each key comes from the source map, so it's unique and valid UTF-8. Members
// keep their insertion order.
static cjson_node *clone_tree(cjson_node *src) {
  clone_frame inline_frames[32];
  clone_frame *frames = inline_frames;
  size_t cap = sizeof(inline_frames) / sizeof(inline_frames[0]);
  size_t len = 0;

  cjson_node *root = NULL;
  cjson_node *cur = src;
  cjson_node *parent_dst = NULL;
  const char *parent_key = NULL;
  size_t parent_key_size = 0;
  unsigned int cur_depth = 0;

  for (;;) {
    if (cur_depth >= CJSON_CLONE_MAX_DEPTH &&
        (cur->type == CJSON_LIST || cur->type == CJSON_DICTIONARY))
      goto fail;

    cjson_node *copy = (cjson_node *)clone_shallow(cur);
    if (!copy) goto fail;

    if (!parent_dst) {
      root = copy;
    } else if (parent_key) {
      cmap_pair kp = {.ptr = (void *)parent_key, .size = parent_key_size};
      cmap_pair vp = {.ptr = &copy, .size = sizeof(copy)};
      chmap store = dict_store(parent_dst);
      if (!store || chashmap_insert_elem(store, &kp, &vp) != ccol_success) {
        cjson__destroy((cjson)copy);
        goto fail;
      }
      copy->attached = true;
    } else {
      cvec store = list_store(parent_dst);
      if (!store || cvector_push_back(store, &copy) != ccol_success) {
        cjson__destroy((cjson)copy);
        goto fail;
      }
      copy->attached = true;
    }

    if (cur->type == CJSON_LIST || cur->type == CJSON_DICTIONARY) {
      clone_frame f = {.src = cur,
                       .dst = copy,

                       .depth = cur_depth,
                       .taken = 0,
                       .next = NULL};
      if (cur->type == CJSON_DICTIONARY) {
        f.next = dict_first_entry(cur);
      }
      if (!clone_stack_push(&frames, inline_frames,
                            sizeof(inline_frames) / sizeof(*inline_frames),
                            &cap, &len, &f)) {
        goto fail;
      }
    }

    for (;;) {
      if (len == 0) goto done;
      clone_frame *f = &frames[len - 1];

      if (f->src->type == CJSON_LIST) {
        if (f->taken < list_len_of(f->src)) {
          cur = *(cjson_node **)cvector_at(f->src->value.list, f->taken);
          f->taken++;
          parent_dst = f->dst;
          parent_key = NULL;
          cur_depth = f->depth + 1;
          break;
        }
        len--;
        continue;
      }

      if (f->next) {
        const cmap_pair *kp, *vp;
        f->next = ccol_chmap_entry_read(f->next, &kp, &vp);
        cur = cjson_read_child(vp->ptr);
        parent_dst = f->dst;
        parent_key = (const char *)kp->ptr;
        parent_key_size = kp->size;
        cur_depth = f->depth + 1;
        break;
      }
      len--;
    }
  }

fail:
  if (frames != inline_frames) ccol__default_mem_free(frames);
  cjson__destroy((cjson)root);
  return NULL;

done:
  if (frames != inline_frames) ccol__default_mem_free(frames);
  return root;
}

// Gives the caller a deep copy of node through clone_tree(), or NULL for a NULL
// node or a failure.
cjson cjson_clone(cjson node) {
  if (!node) return NULL;
  return (cjson)clone_tree((cjson_node *)node);
}

// The state every parse function receives: The input and the current offset,
// the message that parse_err() fills (meaningful only after a failure), and
// the allocator for every node and string. The parser reads src directly, with
// no token stream.
typedef struct {
  const char *src;
  size_t pos;
  size_t len;
  char error[512];
  ccol_memmgmt_procs *mp;
} parse_ctx;

// The deepest nesting a parse accepts, in containers; one more container is a
// parse error that names the limit. parse_value() walks with a heap stack, so
// the limit doesn't depend on the native stack.
#define CJSON_MAX_PARSE_DEPTH 500

static void parse_err(parse_ctx *ctx, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
// Formats a parse error into ctx->error, replacing any earlier one; every
// syntax error of the parser reports through it. The format attribute on the
// declaration lets the compiler check every call site.
static void parse_err(parse_ctx *ctx, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
  va_end(ap);
}

// Tells whether an input byte may appear as it is in an error message. The
// messages print any other byte as 0xNN, so they stay printable ASCII whatever
// the input holds.
static bool parse_err_byte_is_printable(unsigned char b) {
  return b >= 0x20 && b <= 0x7E;
}

// Moves ctx->pos past JSON whitespace (space, tab, CR, LF) between tokens.
static void skip_ws(parse_ctx *ctx) {
  while (ctx->pos < ctx->len) {
    char c = ctx->src[ctx->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      ctx->pos++;
    else
      break;
  }
}

// Skips whitespace and stores the next byte in *out without consuming it, so
// the parser can pick what comes next. Gives false at the end of the input.
static bool peek(parse_ctx *ctx, char *out) {
  skip_ws(ctx);
  if (ctx->pos >= ctx->len) return false;
  *out = ctx->src[ctx->pos];
  return true;
}

// Consumes the next non-whitespace byte when it is expected (a ':' after a
// key); otherwise reports an error and gives false.
static bool expect_char(parse_ctx *ctx, char expected) {
  skip_ws(ctx);
  if (ctx->pos >= ctx->len || ctx->src[ctx->pos] != expected) {
    parse_err(ctx, "expected '%c' at position %zu", expected, ctx->pos);
    return false;
  }
  ctx->pos++;
  return true;
}

// Parses the literal null into a node; NULL with an error otherwise.
static cjson_node *parse_null(parse_ctx *ctx) {
  if (ctx->pos + 4 > ctx->len || memcmp(ctx->src + ctx->pos, "null", 4) != 0) {
    parse_err(ctx, "expected 'null' at position %zu", ctx->pos);
    return NULL;
  }
  ctx->pos += 4;
  return node_alloc(CJSON_NULL, ctx->mp);
}

// Parses true or false into a node; NULL with an error otherwise.
static cjson_node *parse_bool(parse_ctx *ctx) {
  if (ctx->pos + 4 <= ctx->len && memcmp(ctx->src + ctx->pos, "true", 4) == 0) {
    ctx->pos += 4;
    cjson_node *n = node_alloc(CJSON_BOOL, ctx->mp);
    if (n) n->value.boolean = true;
    return n;
  }
  if (ctx->pos + 5 <= ctx->len &&
      memcmp(ctx->src + ctx->pos, "false", 5) == 0) {
    ctx->pos += 5;
    cjson_node *n = node_alloc(CJSON_BOOL, ctx->mp);
    if (n) n->value.boolean = false;
    return n;
  }
  parse_err(ctx, "expected 'true' or 'false' at position %zu", ctx->pos);
  return NULL;
}

// Parses a number at ctx->pos into a node, with the grammar of RFC 8259 sec. 6
// (no digits after a leading zero). An integer that fits a long long is a
// CJSON_INTEGER; everything else, an overflowing integer included, is a
// CJSON_FLOAT, and a value that would be infinite is refused. The token is
// copied into a 360-byte stack buffer, enough for any real number, or into an
// exact heap buffer for a longer (still valid) literal. NULL on an error.
static cjson_node *parse_number(parse_ctx *ctx) {
  size_t start = ctx->pos;
  bool is_float = false;

  if (ctx->pos < ctx->len && ctx->src[ctx->pos] == '-') ctx->pos++;

  if (ctx->pos >= ctx->len) {
    parse_err(ctx, "unexpected end of number at position %zu", ctx->pos);
    return NULL;
  }

  if (ctx->src[ctx->pos] == '0') {
    ctx->pos++;
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

  size_t tok_len = ctx->pos - start;
  char stack_tok[360];
  char *tok = stack_tok;
  char *heap_tok = NULL;
  if (tok_len >= sizeof(stack_tok)) {
    heap_tok = ccol__mem_alloc(ctx->mp, tok_len + 1);
    if (!heap_tok) return NULL;
    tok = heap_tok;
  }
  memcpy(tok, ctx->src + start, tok_len);
  tok[tok_len] = '\0';

  cjson_node *result = NULL;
  if (is_float) {
    double dval = cjson_strtod_c(tok);
    if (isinf(dval)) {
      parse_err(ctx, "number out of range at position %zu", start);
    } else {
      cjson_node *n = node_alloc(CJSON_FLOAT, ctx->mp);
      if (n) n->value.number = dval;
      result = n;
    }
  } else {
    char *endp;
    errno = 0;
    long long ival = strtoll(tok, &endp, 10);
    if (*endp == '\0' && errno != ERANGE) {
      cjson_node *n = node_alloc(CJSON_INTEGER, ctx->mp);
      if (n) n->value.integer = ival;
      result = n;
    } else {
      double dval = cjson_strtod_c(tok);
      if (isinf(dval)) {
        parse_err(ctx, "number out of range at position %zu", start);
      } else {
        cjson_node *n = node_alloc(CJSON_FLOAT, ctx->mp);
        if (n) n->value.number = dval;
        result = n;
      }
    }
  }
  ccol__mem_free(ctx->mp, heap_tok);
  return result;
}

// Appends code point cp to sb as UTF-8, for a decoded \uXXXX escape.
static void encode_utf8(sbuf *sb, uint32_t cp) {
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

// Reads the 4 hex digits of a \uXXXX escape into *out and moves past them;
// reports an error and gives false for a short or bad escape.
static bool parse_hex4(parse_ctx *ctx, uint32_t *out) {
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
      if (parse_err_byte_is_printable((unsigned char)c))
        parse_err(ctx, "invalid hex digit '%c' in \\uXXXX at position %zu", c,
                  ctx->pos + i);
      else
        parse_err(ctx,
                  "invalid hex digit byte 0x%02X in \\uXXXX at position %zu",
                  (unsigned char)c, ctx->pos + i);
      return false;
    }
    v = (v << 4) | d;
  }
  ctx->pos += 4;
  *out = v;
  return true;
}

// Checks that src[start..end) of a string literal is well-formed UTF-8, so the
// parser never stores ill-formed bytes. On a defect it reports the bytes and
// their position and gives false.
static bool string_bytes_are_utf8(parse_ctx *ctx, size_t start, size_t end) {
  size_t bad =
      start + ccol_utf8_first_ill_formed(ctx->src + start, end - start);
  if (bad == end) return true;
  char detail[96];
  const char *what = ccol_utf8_describe_ill_formed(
      (const unsigned char *)ctx->src, ctx->len, bad, detail, sizeof(detail));
  parse_err(ctx, "invalid UTF-8 in string at position %zu: %s (%s)", bad, what,
            detail);
  return false;
}

// Makes the first pass over a string literal, so the common literal can be
// copied without decoding. ctx->pos must be at the opening quote; it moves past
// it. *end gets the position of the closing quote, *need_slow_out whether the
// literal holds an escape or a control byte, and *has_non_ascii_out whether it
// holds a byte above 0x7F (an ASCII literal then skips the UTF-8 check). An
// unterminated literal is an error.
static bool parse_string_scan(parse_ctx *ctx, size_t *end, bool *need_slow_out,
                              bool *has_non_ascii_out) {
  if (ctx->pos >= ctx->len || ctx->src[ctx->pos] != '"') {
    parse_err(ctx, "expected '\"' at position %zu", ctx->pos);
    return false;
  }
  ctx->pos++;

  size_t scan = ctx->pos;
  bool need_slow = false;
  bool has_non_ascii = false;
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
    } else if (sc >= 0x80) {
      has_non_ascii = true;
    }
    scan++;
  }

  if (scan >= ctx->len) {
    parse_err(ctx, "unterminated string starting before position %zu",
              ctx->pos - 1);
    return false;
  }
  *end = scan;
  *need_slow_out = need_slow;
  *has_non_ascii_out = has_non_ascii;
  return true;
}

// Decodes a string literal that holds an escape or a control byte, from
// ctx->pos up to the closing quote at scan, into a heap string (ctx->mp) in
// *out with its length in *out_len. Plain runs are copied in one go and checked
// for UTF-8 in order, so the first defect is the one reported. A surrogate
// escape that isn't half of a high-low pair, \u0000 (a C string can't hold it),
// an unknown escape and a raw control byte are errors.
static bool parse_string_slow(parse_ctx *ctx, size_t scan, bool has_non_ascii,
                              char **out, size_t *out_len) {
  sbuf sb;
  sb_init_hint(&sb, ctx->mp, scan - ctx->pos);

  while (ctx->pos < ctx->len) {
    size_t chunk_start = ctx->pos;
    while (ctx->pos < ctx->len) {
      unsigned char c2 = (unsigned char)ctx->src[ctx->pos];
      if (c2 == '"' || c2 == '\\' || c2 < 0x20) break;
      ctx->pos++;
    }
    if (ctx->pos > chunk_start) {
      if (has_non_ascii && !string_bytes_are_utf8(ctx, chunk_start, ctx->pos)) {
        ccol__mem_free(sb.m_procs, sb.buf);
        return false;
      }
      sb_append(&sb, ctx->src + chunk_start, ctx->pos - chunk_start);
    }

    if (ctx->pos >= ctx->len) break;
    char c = ctx->src[ctx->pos];

    if (c == '"') {
      ctx->pos++;
      if (sb.oom) {
        ccol__mem_free(sb.m_procs, sb.buf);
        return false;
      }
      *out = sb.buf;
      *out_len = sb.len;
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
            ccol__mem_free(sb.m_procs, sb.buf);
            return false;
          }
          size_t esc_pos = ctx->pos - 6;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t low = 0;
            if (ctx->pos + 1 < ctx->len && ctx->src[ctx->pos] == '\\' &&
                ctx->src[ctx->pos + 1] == 'u') {
              ctx->pos += 2;
              if (!parse_hex4(ctx, &low)) {
                ccol__mem_free(sb.m_procs, sb.buf);
                return false;
              }
            }
            if (low < 0xDC00 || low > 0xDFFF) {
              parse_err(ctx,
                        "high surrogate escape \\u%04X at position %zu is "
                        "not followed by a low surrogate escape",
                        (unsigned)cp, esc_pos);
              ccol__mem_free(sb.m_procs, sb.buf);
              return false;
            }
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            parse_err(ctx,
                      "low surrogate escape \\u%04X at position %zu does not "
                      "follow a high surrogate escape",
                      (unsigned)cp, esc_pos);
            ccol__mem_free(sb.m_procs, sb.buf);
            return false;
          }
          if (cp == 0) {
            parse_err(ctx, "\\u0000 not supported at position %zu", esc_pos);
            ccol__mem_free(sb.m_procs, sb.buf);
            return false;
          }
          encode_utf8(&sb, cp);
          break;
        }
        default:
          if (parse_err_byte_is_printable((unsigned char)esc))
            parse_err(ctx, "unknown escape '\\%c' at position %zu", esc,
                      ctx->pos - 1);
          else
            parse_err(ctx,
                      "unknown escape: '\\' followed by byte 0x%02X at "
                      "position %zu",
                      (unsigned char)esc, ctx->pos - 1);
          ccol__mem_free(sb.m_procs, sb.buf);
          return false;
      }
    } else {
      parse_err(ctx,
                "unescaped control character 0x%02x in string at position %zu",
                (unsigned char)c, ctx->pos);
      ccol__mem_free(sb.m_procs, sb.buf);
      return false;
    }
  }

  parse_err(ctx, "unterminated string starting before position %zu", ctx->pos);
  ccol__mem_free(sb.m_procs, sb.buf);
  return false;
}

// Parses a string literal into a heap string (ctx->mp) in *out, for string
// values. Without escapes or control bytes the string is its raw bytes, so one
// UTF-8 check, one allocation and one memcpy are enough.
static bool parse_string_raw(parse_ctx *ctx, char **out) {
  *out = NULL;
  size_t scan;
  bool need_slow, has_non_ascii;
  if (!parse_string_scan(ctx, &scan, &need_slow, &has_non_ascii)) return false;

  if (!need_slow) {
    size_t slen = scan - ctx->pos;
    if (has_non_ascii && !string_bytes_are_utf8(ctx, ctx->pos, scan))
      return false;
    char *s = ccol__mem_alloc(ctx->mp, slen + 1);
    if (!s) return false;
    memcpy(s, ctx->src + ctx->pos, slen);
    s[slen] = '\0';
    ctx->pos = scan + 1;
    *out = s;
    return true;
  }

  size_t slen;
  return parse_string_slow(ctx, scan, has_non_ascii, out, &slen);
}

// The keys of the dictionaries that parse_value() has open, back to back, each
// with its '\0'. A key lives only while its value is parsed and nested values
// end first, so keys come and go in LIFO order. The buffer starts inline on
// the stack and moves to ctx->mp's heap, doubling, only for a longer total.
#define CJSON_PARSE_KEY_INLINE 256
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  char inline_buf[CJSON_PARSE_KEY_INLINE];
} parse_keys;

// Makes room for n more bytes in the key buffer, moving it to the heap the
// first time it outgrows the inline array. Gives false when that fails.
static bool parse_keys_reserve(parse_ctx *ctx, parse_keys *keys, size_t n) {
  if (n <= keys->cap - keys->len) return true;
  if (n > SIZE_MAX / 2 - keys->len) return false;
  size_t new_cap = keys->cap * 2;
  if (new_cap < keys->len + n) new_cap = keys->len + n;
  char *grown;
  if (keys->buf == keys->inline_buf) {
    grown = ccol__mem_alloc(ctx->mp, new_cap);
    if (!grown) return false;
    memcpy(grown, keys->inline_buf, keys->len);
  } else {
    grown = ccol__mem_realloc(ctx->mp, keys->buf, new_cap);
    if (!grown) return false;
  }
  keys->buf = grown;
  keys->cap = new_cap;
  return true;
}

// Parses a dictionary key and pushes it, '\0' included, onto the key buffer, so
// a key costs no allocation of its own. *off gets its offset and *klen its
// length. A key without escapes is copied straight from the input; one with
// escapes is decoded on the heap first.
static bool parse_key(parse_ctx *ctx, parse_keys *keys, size_t *off,
                      size_t *klen) {
  size_t scan;
  bool need_slow, has_non_ascii;
  if (!parse_string_scan(ctx, &scan, &need_slow, &has_non_ascii)) return false;

  const char *bytes;
  size_t slen;
  char *decoded = NULL;
  if (!need_slow) {
    slen = scan - ctx->pos;
    if (has_non_ascii && !string_bytes_are_utf8(ctx, ctx->pos, scan))
      return false;
    bytes = ctx->src + ctx->pos;
  } else {
    if (!parse_string_slow(ctx, scan, has_non_ascii, &decoded, &slen))
      return false;
    bytes = decoded;
  }
  bool ok = parse_keys_reserve(ctx, keys, slen + 1);
  if (ok) {
    memcpy(keys->buf + keys->len, bytes, slen);
    keys->buf[keys->len + slen] = '\0';
    *off = keys->len;
    *klen = slen;
    keys->len += slen + 1;
  }
  if (decoded) {
    ccol__mem_free(ctx->mp, decoded);
  } else {
    ctx->pos = scan + 1;
  }
  return ok;
}

// Parses a string literal into a CJSON_STRING node that owns it, for a string
// value; NULL on an error.
static cjson_node *parse_string(parse_ctx *ctx) {
  char *s = NULL;
  if (!parse_string_raw(ctx, &s)) return NULL;
  cjson_node *n = node_alloc(CJSON_STRING, ctx->mp);
  if (!n) {
    ccol__mem_free(ctx->mp, s);
    return NULL;
  }
  n->value.string = s;
  return n;
}

// One open '[' or '{' on the explicit stack of parse_value(). key_off and
// key_len locate, in the key buffer, the key whose value is being read.
// started tells "a value may follow the bracket" apart from "a ',' must come
// first". A recursive descent would let the sender of a document pick the
// stack depth, which crashes a small thread stack long before the depth cap.
typedef struct {
  cjson_node *node;
  size_t key_off;
  size_t key_len;
  bool is_dict;
  bool started;
} parse_frame;

// Pushes a frame onto the stack of parse_value(), growing it like
// serialize_stack_push() does. Gives false when the growth fails.
static bool parse_stack_push(parse_frame **frames, parse_frame *inline_frames,
                             size_t inline_cap, size_t *cap, size_t *len,
                             const parse_frame *frame) {
  if (*len == *cap) {
    size_t new_cap = *cap * 2;
    parse_frame *grown;
    if (*frames == inline_frames) {
      grown = ccol__default_mem_alloc(new_cap * sizeof(**frames));
      if (!grown) return false;
      memcpy(grown, inline_frames, inline_cap * sizeof(**frames));
    } else {
      grown = ccol__default_mem_realloc(*frames, new_cap * sizeof(**frames));
      if (!grown) return false;
    }
    *frames = grown;
    *cap = new_cap;
  }
  (*frames)[(*len)++] = *frame;
  return true;
}

// Parses a value that isn't a container, picking the sub-parser from its first
// byte c (which peek() already found); parse_value() takes containers itself.
static cjson_node *parse_scalar_value(parse_ctx *ctx, char c) {
  switch (c) {
    case 'n':
      return parse_null(ctx);
    case 't':
    case 'f':
      return parse_bool(ctx);
    case '"':
      return parse_string(ctx);
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
      if (parse_err_byte_is_printable((unsigned char)c))
        parse_err(ctx, "unexpected character '%c' at position %zu", c,
                  ctx->pos);
      else
        parse_err(ctx, "unexpected byte 0x%02X at position %zu",
                  (unsigned char)c, ctx->pos);
      return NULL;
  }
}

// Parses one complete value and everything inside it, for parse_common().
// Gives the root, or NULL with ctx->error set and every node built so far
// freed. It walks with an explicit stack in four steps: Parse a value, hand a
// complete value to the innermost open container, decide what comes next in
// that container, and read a key. A container goes to its parent only after
// its closing bracket, so on failure every frame still owns its container and
// each one is destroyed. A repeated key keeps its first place and the last
// value (RFC 8259 sec. 4 allows it).
static cjson_node *parse_value(parse_ctx *ctx) {
  parse_frame inline_frames[32];
  parse_frame *frames = inline_frames;
  size_t cap = sizeof(inline_frames) / sizeof(inline_frames[0]);
  size_t len = 0;
  cjson_node *pending = NULL;
  char c;
  parse_keys keys;
  keys.buf = keys.inline_buf;
  keys.len = 0;
  keys.cap = sizeof(keys.inline_buf);

value_step:
  if (!peek(ctx, &c)) {
    parse_err(ctx, "unexpected end of input at position %zu", ctx->pos);
    goto fail;
  }
  if (c == '[' || c == '{') {
    if (len >= CJSON_MAX_PARSE_DEPTH) {
      parse_err(ctx, "maximum nesting depth (%u) exceeded at position %zu",
                CJSON_MAX_PARSE_DEPTH, ctx->pos);
      goto fail;
    }
    parse_frame f = {.node = NULL,
                     .key_off = 0,
                     .key_len = 0,
                     .is_dict = (c == '{'),
                     .started = false};
    ctx->pos++;
    f.node = f.is_dict ? (cjson_node *)cjson_create_dictionary_interned(ctx->mp)
                       : (cjson_node *)cjson_create_list_interned(ctx->mp);
    if (!f.node) goto fail;
    if (!parse_stack_push(&frames, inline_frames,
                          sizeof(inline_frames) / sizeof(*inline_frames), &cap,
                          &len, &f)) {
      cjson__destroy((cjson)f.node);
      goto fail;
    }
    goto next_step;
  }
  pending = parse_scalar_value(ctx, c);
  if (!pending) goto fail;

deliver_step:
  if (len == 0) goto done;
  {
    parse_frame *f = &frames[len - 1];
    if (f->is_dict) {
      cmap_pair kp = {.ptr = keys.buf + f->key_off, .size = f->key_len + 1};

      cmap_pair vp = {.ptr = &pending, .size = sizeof(pending)};
      cmap_pair existing_vp;
      chmap store = dict_store(f->node);
      ccol_retval r =
          store ? ccol_chmap_insert_or_get_elem(store, &kp, &vp, &existing_vp)
                : ccol_not_enough_memory;
      keys.len = f->key_off;
      if (r == ccol_key_already_present) {
        dict_slot_replace_child(&existing_vp, pending);
      } else if (r != ccol_success) {
        cjson__destroy((cjson)pending);
        goto fail;
      } else {
        pending->attached = true;
      }
    } else {
      cvec store = list_store(f->node);
      if (!store || cvector_push_back(store, &pending) != ccol_success) {
        cjson__destroy((cjson)pending);
        goto fail;
      }
      pending->attached = true;
    }
  }

next_step: {
  parse_frame *f = &frames[len - 1];
  if (!peek(ctx, &c)) {
    if (f->is_dict)
      parse_err(ctx, "unterminated dictionary");
    else
      parse_err(ctx, "unterminated list");
    goto fail;
  }
  if (c == (f->is_dict ? '}' : ']')) {
    ctx->pos++;
    pending = f->node;
    len--;
    goto deliver_step;
  }
  if (f->started) {
    if (c != ',') {
      if (f->is_dict)
        parse_err(ctx, "expected ',' or '}' in dictionary at position %zu",
                  ctx->pos);
      else
        parse_err(ctx, "expected ',' or ']' in list at position %zu", ctx->pos);
      goto fail;
    }
    ctx->pos++;
  }
  f->started = true;
  if (!f->is_dict) {
    skip_ws(ctx);
    goto value_step;
  }
  goto key_step;
}

key_step: {
  parse_frame *f = &frames[len - 1];
  skip_ws(ctx);
  if (ctx->pos >= ctx->len || ctx->src[ctx->pos] != '"') {
    parse_err(ctx, "expected string key at position %zu", ctx->pos);
    goto fail;
  }
  if (!parse_key(ctx, &keys, &f->key_off, &f->key_len)) goto fail;
  if (!expect_char(ctx, ':')) goto fail;
  skip_ws(ctx);
  goto value_step;
}

fail:
  for (size_t i = 0; i < len; i++) {
    cjson__destroy((cjson)frames[i].node);
  }
  pending = NULL;

done:
  if (frames != inline_frames) ccol__default_mem_free(frames);
  if (keys.buf != keys.inline_buf) ccol__mem_free(ctx->mp, keys.buf);
  return pending;
}

// The per-thread buffer behind err_str. The message names a position, so it
// can't be a literal, and a heap string the caller frees would give err_str a
// rule opposite to every other module's. So the library owns it, and it lives
// until the next failing parse on the same thread. The buffer is on the heap,
// allocated by the thread's first failing parse and freed by the thread-exit
// drain; a thread-local array would take room in the TLS block that dlopen()
// must fit. 512 is the size of parse_ctx.error.
#define CJSON_ERR_BUF_LEN 512
// The message a failed parse reports when the thread can't keep a buffer.
static const char cjson_err_unstored[] =
    "parse failed; the error message could not be stored";

// Copies msg into this thread's message buffer and points *err_str at it, so a
// failed parse can hand out a message the caller never frees. Does nothing for
// a NULL err_str. It allocates the buffer on first use and arms the pool key,
// whose drain frees it at thread exit; a thread whose key can't be armed keeps
// no buffer and gets cjson_err_unstored. Changes cjson_err_buf (or the thread's
// block).
static void cjson_report_err(char **err_str, const char *msg) {
  if (!err_str) return;
#if defined(CCOL__EMULATE_DARWIN_TLS)
  ccol__call_once(pool_key_once, do_pool_key_init);
  cjson_tls *t = cjson_tls_get();
  if (t && !t->err_buf) t->err_buf = malloc(CJSON_ERR_BUF_LEN);
  if (!t || !t->err_buf) {
    *err_str = (char *)cjson_err_unstored;
    return;
  }
  snprintf(t->err_buf, CJSON_ERR_BUF_LEN, "%s", msg);
  *err_str = t->err_buf;
#else
  if (!cjson_err_buf) {
    char *buf = malloc(CJSON_ERR_BUF_LEN);
    if (!buf) {
      *err_str = (char *)cjson_err_unstored;
      return;
    }
    ccol__call_once(pool_key_once, do_pool_key_init);
    pool_key_arm();
    if (!pool_key_armed) {
      free(buf);
      *err_str = (char *)cjson_err_unstored;
      return;
    }
    cjson_err_buf = buf;
  }
  snprintf(cjson_err_buf, CJSON_ERR_BUF_LEN, "%s", msg);
  *err_str = cjson_err_buf;
#endif
}

// Parses len bytes of src with allocator mp; both public parse functions share
// it. It interns mp and refuses anything but whitespace after the root value.
// Gives the root, or NULL. On success *err_str becomes NULL; on failure it
// points at the per-thread message of cjson_report_err(). Every syntax error
// sets its own message, so an empty ctx.error can only mean an allocation
// failed, and the message says so.
static cjson parse_common(const char *src, size_t len, char **err_str,
                          ccol_memmgmt_procs *mp) {
  if (!src) {
    cjson_report_err(err_str, "null input");
    return NULL;
  }
  if (!cjson_intern_procs(&mp)) {
    cjson_report_err(err_str,
                     "too many distinct allocator procs in this process");
    return NULL;
  }
  parse_ctx ctx = {.src = src, .pos = 0, .len = len, .error = "", .mp = mp};
  cjson_node *root = parse_value(&ctx);
  if (!root) {
    if (err_str) {
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
      cjson_report_err(err_str, msg);
    }
    return NULL;
  }
  skip_ws(&ctx);
  if (ctx.pos != ctx.len) {
    if (err_str) {
      char buf[64];
      snprintf(buf, sizeof(buf), "trailing garbage at position %zu", ctx.pos);
      cjson_report_err(err_str, buf);
    }
    cjson__destroy((cjson)root);
    return NULL;
  }
  if (err_str) *err_str = NULL;
  return (cjson)root;
}

// Parses a NUL-terminated JSON string for the caller through parse_common();
// a NULL json_str is reported as an error.
cjson cjson_parse_mp(const char *json_str, char **err_str,
                     ccol_memmgmt_procs *mp) {
  if (!json_str) return parse_common(NULL, 0, err_str, mp);
  return parse_common(json_str, strlen(json_str), err_str, mp);
}

// Parses len bytes of json_str, which needn't be NUL-terminated, for a caller
// whose JSON sits inside a larger buffer.
cjson cjson_parse_n_mp(const char *json_str, size_t len, char **err_str,
                       ccol_memmgmt_procs *mp) {
  return parse_common(json_str, len, err_str, mp);
}

// Writes val into buf (cap bytes) in the shortest form that reads back the
// same, for the serializer. It adds ".0" to a whole number so it parses back as
// a CJSON_FLOAT; the longest such case, a signed 17-digit number, fits the
// caller's 32 bytes. A non-finite value (which the API never stores) gives
// null.
static void format_double(char *buf, size_t cap, double val) {
  if (!isfinite(val)) {
    snprintf(buf, cap, "null");
    return;
  }
  cjson_format_shortest_c(buf, cap, val);
  if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
    size_t len = strlen(buf);
    if (len + 2 < cap) {
      buf[len] = '.';
      buf[len + 1] = '0';
      buf[len + 2] = '\0';
    }
  }
}

// Appends a newline and depth * indent spaces, for the pretty serializer. It
// copies from a constant block, one append per 128 spaces, and does nothing in
// compact mode (indent 0). A width that doesn't fit a size_t sets sb->oom.
static void sb_append_indent(sbuf *sb, unsigned int indent,
                             unsigned int depth) {
  static const char nl_spaces[130] =
      "\n"
      "                                                                "
      "                                                                ";
  if (!indent) return;
  if (depth != 0 && indent > SIZE_MAX / depth) {
    sb->oom = true;
    return;
  }
  size_t width = (size_t)depth * indent;
  size_t chunk = width < 128 ? width : 128;
  sb_append(sb, nl_spaces, chunk + 1);
  width -= chunk;
  while (width > 0) {
    chunk = width < 128 ? width : 128;
    sb_append(sb, nl_spaces + 1, chunk);
    width -= chunk;
  }
}

// Appends s as a quoted JSON string with escapes, for string values and keys.
// Bytes >= 0x20 other than '"' and '\\' are copied as they are, which is
// correct because every DOM string is valid UTF-8.
static void sb_append_json_str(sbuf *sb, const char *s) {
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

// Writes v in decimal into buf (at least 21 bytes) and gives its length, for
// integer values; it's cheaper than snprintf.
static int lltoa(char *buf, long long v) {
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

// The deepest nesting the serializers accept, in containers. A tree from the
// public API can be of any depth; this has the parser's value, so whatever
// parses can be written. serialize_node() uses a heap stack, so the limit is a
// policy, not a stack bound.
#define CJSON_MAX_SERIALIZE_DEPTH 500

// One open container on the explicit stack of serialize_node(). emitted counts
// the children written (for the commas and the closing indent), count is the
// length of a list, next is the next member of a dictionary in insertion
// order, and depth gives the indent of the children.
typedef struct {
  cjson_node *node;
  size_t emitted;
  size_t count;
  const ccol_chmap_entry_ref *next;
  unsigned int depth;
  bool is_dict;
} serialize_frame;

// Pushes a frame onto the stack of serialize_node(), growing it when it's full.
// *frames starts at the caller's inline_frames, so an ordinary document needs
// no scratch allocation; *cap and *len describe the stack. Gives false, with
// the stack unchanged, when the growth fails.
static bool serialize_stack_push(serialize_frame **frames,
                                 serialize_frame *inline_frames,
                                 size_t inline_cap, size_t *cap, size_t *len,
                                 const serialize_frame *frame) {
  if (*len == *cap) {
    size_t new_cap = *cap * 2;
    serialize_frame *grown;
    if (*frames == inline_frames) {
      grown = ccol__default_mem_alloc(new_cap * sizeof(**frames));
      if (!grown) return false;
      memcpy(grown, inline_frames, inline_cap * sizeof(**frames));
    } else {
      grown = ccol__default_mem_realloc(*frames, new_cap * sizeof(**frames));
      if (!grown) return false;
    }
    *frames = grown;
    *cap = new_cap;
  }
  (*frames)[(*len)++] = *frame;
  return true;
}

// Writes a node that isn't a container into sb, for serialize_node(); a NULL
// node is written as null. Each case is listed so -Wswitch reports a new node
// kind.
static void serialize_scalar(sbuf *sb, cjson_node *n) {
  if (!n) {
    sb_append_cstr(sb, "null");
    return;
  }
  switch (n->type) {
    case CJSON_BOOL:
      sb_append_cstr(sb, n->value.boolean ? "true" : "false");
      break;
    case CJSON_INTEGER: {
      char buf[24];
      int len = lltoa(buf, n->value.integer);
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
    case CJSON_NULL:
    case CJSON_LIST:
    case CJSON_DICTIONARY:
      sb_append_cstr(sb, "null");
      break;
  }
}

// Writes the subtree at n into sb, for both public serializers, with indent
// spaces per level (0 for compact) from nesting level depth. It walks with an
// explicit stack, and every failure, the depth cap included, sets sb->oom.
static void serialize_node(sbuf *sb, cjson_node *n, unsigned int indent,
                           unsigned int depth) {
  serialize_frame inline_frames[32];
  serialize_frame *frames = inline_frames;
  size_t cap = sizeof(inline_frames) / sizeof(inline_frames[0]);
  size_t len = 0;

  cjson_node *cur = n;
  unsigned int cur_depth = depth;

  for (;;) {
    if (sb->oom) goto done;
    if (cur && (cur->type == CJSON_LIST || cur->type == CJSON_DICTIONARY)) {
      if (cur_depth >= CJSON_MAX_SERIALIZE_DEPTH) {
        sb->oom = true;
        goto done;
      }
      serialize_frame f = {.node = cur,
                           .emitted = 0,
                           .count = 0,
                           .next = NULL,
                           .depth = cur_depth,
                           .is_dict = (cur->type == CJSON_DICTIONARY)};
      if (cur->type == CJSON_LIST) {
        f.count = list_len_of(cur);
        sb_append_c(sb, '[');
      } else {
        f.next = dict_first_entry(cur);
        sb_append_c(sb, '{');
      }
      if (!serialize_stack_push(&frames, inline_frames,
                                sizeof(inline_frames) / sizeof(*inline_frames),
                                &cap, &len, &f)) {
        sb->oom = true;
        goto done;
      }
    } else {
      serialize_scalar(sb, cur);
    }

    for (;;) {
      if (len == 0) goto done;
      serialize_frame *f = &frames[len - 1];

      if (!f->is_dict) {
        if (f->emitted < f->count) {
          if (f->emitted > 0) sb_append_c(sb, ',');
          if (indent) sb_append_indent(sb, indent, f->depth + 1);
          cur = *(cjson_node **)cvector_at(f->node->value.list, f->emitted);
          f->emitted++;
          cur_depth = f->depth + 1;
          break;
        }
        if (indent && f->count > 0) sb_append_indent(sb, indent, f->depth);
        sb_append_c(sb, ']');
        len--;
        continue;
      }

      if (f->next) {
        const cmap_pair *kp, *vp;
        f->next = ccol_chmap_entry_read(f->next, &kp, &vp);
        if (f->emitted > 0) sb_append_c(sb, ',');
        if (indent) sb_append_indent(sb, indent, f->depth + 1);
        sb_append_json_str(sb, (const char *)kp->ptr);
        sb_append_c(sb, ':');
        if (indent) sb_append_c(sb, ' ');
        cur = cjson_read_child(vp->ptr);
        f->emitted++;
        cur_depth = f->depth + 1;
        break;
      }
      if (indent && f->emitted > 0) sb_append_indent(sb, indent, f->depth);
      sb_append_c(sb, '}');
      len--;
    }
  }

done:
  if (frames != inline_frames) ccol__default_mem_free(frames);
}

// Writes node as compact JSON for the caller, into a buffer from node's
// allocator. Gives NULL when an allocation fails or the tree is too deep.
char *cjson_serialize(cjson node) {
  cjson_node *n = (cjson_node *)node;
  ccol_memmgmt_procs *mp = n ? n->m_procs : NULL;
  sbuf sb;
  sb_init(&sb, mp);
  serialize_node(&sb, n, 0, 0);
  if (sb.oom) {
    ccol__mem_free(sb.m_procs, sb.buf);
    return NULL;
  }
  return sb.buf;
}

// Writes node as indented JSON for the caller (indent 0 means 4), into a
// buffer from node's allocator. Gives NULL on failure.
char *cjson_serialize_pretty(cjson node, unsigned int indent) {
  cjson_node *n = (cjson_node *)node;
  ccol_memmgmt_procs *mp = n ? n->m_procs : NULL;
  sbuf sb;
  sb_init(&sb, mp);
  serialize_node(&sb, n, indent ? indent : 4, 0);
  if (sb.oom) {
    ccol__mem_free(sb.m_procs, sb.buf);
    return NULL;
  }
  return sb.buf;
}

// Frees a serializer buffer with mp (NULL for the default allocator), since
// only the caller knows which allocator built the tree.
void cjson_serialize_free_mp(char *s, ccol_memmgmt_procs *mp) {
  ccol__mem_free(mp, s);
}

// Gives the first unescaped '.' in s, or NULL, so navigate() can split a path
// into components. In a path "\." is a literal dot and "\\" a literal
// backslash; a backslash before anything else is kept. A component's escapes
// are resolved after the path is split.
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

// Gives the last unescaped '.' in s, or NULL, so a set or a delete can split a
// path into the parent path and the leaf.
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

// Resolves the escapes of a path component s in place before it's used as a
// key; s never gets longer.
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

// Parses the digits after the '#' of a "#N" component into *out, for every
// path function that indexes a list. Only one or more ASCII digits are
// accepted (no sign, no whitespace, no bare '#'), so it gives false for
// anything else. The digits are read by hand rather than with strtol(), and a
// value too big for size_t saturates at SIZE_MAX, so an index of any length is
// just "not found".
static bool parse_list_index_component(const char *digits, size_t *out) {
  if (digits[0] < '0' || digits[0] > '9') return false;
  size_t idx = 0;
  const char *p = digits;
  for (; *p >= '0' && *p <= '9'; p++) {
    size_t d = (size_t)(*p - '0');
    idx = (idx > (SIZE_MAX - d) / 10) ? SIZE_MAX : idx * 10 + d;
  }
  if (*p != '\0') return false;
  *out = idx;
  return true;
}

// Walks a dot-separated path from root and gives the node at its end, or NULL;
// cjson__get() and the parent walk of set and delete use it. path_copy must be
// writable: Each separator becomes a '\0' in turn and each component is
// unescaped in place. When invalid isn't NULL it is set for an error in the
// path, at any position: An empty component, a component that isn't UTF-8, a
// malformed #N on a list, or a shape that can't apply (a scalar in the middle,
// a name on a list). It stays false when a well-formed component is simply
// absent, which lets the callers tell ccol_invalid_args from
// ccol_key_not_found. The scan goes on past an absent component, so a later
// syntax error is still reported; the UTF-8 test runs only for a component
// that doesn't resolve, so a lookup that succeeds pays nothing.
static cjson navigate(cjson root, char *path_copy, bool *invalid) {
  if (invalid) *invalid = false;
  cjson cur = root;
  char *p = path_copy;

  while (1) {
    char *dot = path_find_unescaped_dot(p);
    if (dot) *dot = '\0';

    if (p[0] == '\0') {
      if (invalid) *invalid = true;
      cur = NULL;
      break;
    }

    path_unescape_component(p);

    if (cur) {
      cjson_node *n = (cjson_node *)cur;

      if (n->type == CJSON_LIST && p[0] == '#') {
        size_t idx;
        if (!parse_list_index_component(p + 1, &idx)) {
          if (invalid) {
            *invalid = true;
            cur = NULL;
            break;
          }
          cur = NULL;
        } else {
          cur = (cjson)list_child_at(n, idx);
        }
      } else if (n->type == CJSON_DICTIONARY) {
        cmap_pair kp = {.ptr = p, .size = strlen(p) + 1};
        cmap_pair vp;
        if (dict_lookup(n, &kp, &vp) != ccol_success)
          cur = NULL;
        else
          cur = cjson_read_child(vp.ptr);
      } else {
        if (invalid) {
          *invalid = true;
          cur = NULL;
          break;
        }
        cur = NULL;
      }
    }

    if (!cur && invalid && !utf8_cstr_is_valid(p)) {
      *invalid = true;
      break;
    }

    if (!dot) break;
    p = dot + 1;
  }

  return cur;
}

// Paths shorter than this are copied to the stack, so cjson_get() of such a
// path allocates nothing.
#define CJSON_GET_STACK_PATH 256
// Gives the node at path, root for an empty path, or NULL; it's the body of
// cjson_get(). It copies the path (to the stack or, when long, with root's
// allocator) so navigate() can write into it and the caller's string stays
// unchanged.
cjson cjson__get(cjson root, const char *path) {
  if (!root) return NULL;
  if (!path || path[0] == '\0') return root;

  ccol_memmgmt_procs *mp = ((cjson_node *)root)->m_procs;
  char stack_copy[CJSON_GET_STACK_PATH];
  size_t len = strlen(path);
  char *copy = stack_copy;
  if (len < sizeof(stack_copy)) {
    memcpy(stack_copy, path, len + 1);
  } else {
    copy = ccol_strdup(mp, path);
    if (!copy) return NULL;
  }
  cjson result = navigate(root, copy, NULL);
  if (copy != stack_copy) ccol__mem_free(mp, copy);
  return result;
}

// Writes a scalar to the leaf at path; it's the body of cjson_set(). type, raw,
// raw_size and is_signed describe the C value. The path is split on its last
// unescaped dot into the parent path and the leaf, and the leaf is checked (not
// empty, valid UTF-8, since it may become a key) before the parent is walked,
// so a syntax error never hides behind a missing parent. The payload is
// checked before the map changes. On a dictionary one insert either finds the
// leaf, which is updated in place so a borrowed handle stays valid, or adds the
// key with a NULL placeholder that is filled (or removed again when building
// the child fails). On a list the leaf must be an existing #N; an out-of-range
// index is ccol_key_not_found, since a list can't grow to fit.
ccol_retval cjson__set_typed(cjson root, const char *path, cjson_node_type type,
                             void *raw, size_t raw_size, bool is_signed) {
  if (!root || !path || path[0] == '\0') return ccol_invalid_args;

  ccol_memmgmt_procs *mp = ((cjson_node *)root)->m_procs;

  char *copy = ccol_strdup(mp, path);
  if (!copy) return ccol_not_enough_memory;

  char *last_dot = path_find_last_unescaped_dot(copy);
  char *leaf_copy;

  if (!last_dot) {
    leaf_copy = copy;
  } else {
    *last_dot = '\0';
    if (copy[0] == '\0') {
      ccol__mem_free(mp, copy);
      return ccol_invalid_args;
    }
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    if (!leaf_copy) {
      ccol__mem_free(mp, copy);
      return ccol_not_enough_memory;
    }
  }

  path_unescape_component(leaf_copy);
  if (leaf_copy[0] == '\0' || !utf8_cstr_is_valid(leaf_copy)) {
    if (leaf_copy != copy) ccol__mem_free(mp, leaf_copy);
    ccol__mem_free(mp, copy);
    return ccol_invalid_args;
  }
  const char *leaf_comp = leaf_copy;

  cjson parent;
  if (!last_dot) {
    parent = root;
  } else {
    bool invalid = false;
    parent = navigate(root, copy, &invalid);
    ccol__mem_free(mp, copy);
    if (!parent) {
      ccol__mem_free(mp, leaf_copy);
      return invalid ? ccol_invalid_args : ccol_key_not_found;
    }
  }

  cjson_node *pn = (cjson_node *)parent;
  ccol_retval ret;

  if (pn->type == CJSON_DICTIONARY) {
    cmap_pair kp = {.ptr = (void *)leaf_comp, .size = strlen(leaf_comp) + 1};
    ret = scalar_payload_check(type, raw, raw_size);
    if (ret == ccol_success) {
      cjson_node *placeholder = NULL;
      cmap_pair vp = {.ptr = &placeholder, .size = sizeof(placeholder)};
      cmap_pair slot;
      chmap store = dict_store(pn);
      ccol_retval r =
          store ? ccol_chmap_insert_or_get_elem(store, &kp, &vp, &slot)
                : ccol_not_enough_memory;
      if (r == ccol_key_already_present) {
        cjson_node *existing = cjson_read_child(slot.ptr);
        ret = node_reinit_scalar_checked(existing, type, raw, raw_size,
                                         is_signed);
      } else if (r == ccol_success) {
        cjson_node *new_node;
        ret = node_make_scalar(type, raw, raw_size, is_signed, pn->m_procs,
                               &new_node);
        if (ret == ccol_success) {
          memcpy(slot.ptr, &new_node, sizeof(new_node));
          new_node->attached = true;
        } else {
          chashmap_delete_elem(pn->value.dictionary, &kp);
        }
      } else {
        ret = r;
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
        cjson_node *existing = list_child_at(pn, idx);
        if (!existing) {
          ret = ccol_key_not_found;
        } else {
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

// Removes and destroys the node at path; it's the body of cjson_delete(). It
// splits the path like cjson__set_typed(), checks the leaf before walking the
// parent, and then removes through cjson_dictionary_remove() or
// cjson_list_remove(). An absent list index is checked here, so it gives
// ccol_key_not_found and not the ccol_invalid_args that cjson_list_remove()
// would.
ccol_retval cjson__delete(cjson root, const char *path) {
  if (!root || !path || path[0] == '\0') return ccol_invalid_args;

  ccol_memmgmt_procs *mp = ((cjson_node *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return ccol_not_enough_memory;

  char *last_dot = path_find_last_unescaped_dot(copy);
  char *leaf_copy;

  if (!last_dot) {
    leaf_copy = copy;
  } else {
    *last_dot = '\0';
    if (copy[0] == '\0') {
      ccol__mem_free(mp, copy);
      return ccol_invalid_args;
    }
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    if (!leaf_copy) {
      ccol__mem_free(mp, copy);
      return ccol_not_enough_memory;
    }
  }

  path_unescape_component(leaf_copy);
  if (leaf_copy[0] == '\0') {
    if (leaf_copy != copy) ccol__mem_free(mp, leaf_copy);
    ccol__mem_free(mp, copy);
    return ccol_invalid_args;
  }
  const char *leaf_comp = leaf_copy;

  cjson parent;
  if (!last_dot) {
    parent = root;
  } else {
    bool invalid = false;
    parent = navigate(root, copy, &invalid);
    ccol__mem_free(mp, copy);
    if (!parent) {
      ccol__mem_free(mp, leaf_copy);
      return invalid ? ccol_invalid_args : ccol_key_not_found;
    }
  }

  cjson_node *pn = (cjson_node *)parent;
  ccol_retval ret;

  if (pn->type == CJSON_DICTIONARY) {
    ret = cjson_dictionary_remove(parent, leaf_comp);
  } else if (pn->type == CJSON_LIST) {
    if (leaf_comp[0] != '#') {
      ret = ccol_invalid_args;
    } else {
      size_t idx;
      if (!parse_list_index_component(leaf_comp + 1, &idx)) {
        ret = ccol_invalid_args;
      } else if (idx >= list_len_of(pn)) {
        ret = ccol_key_not_found;
      } else {
        ret = cjson_list_remove(parent, idx);
      }
    }
  } else {
    ret = ccol_invalid_args;
  }

  ccol__mem_free(mp, leaf_copy);
  return ret;
}
