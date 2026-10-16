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

/* ========================================================================== */
/*                         INTERNAL DOM NODE                                  */
/* ========================================================================== */

/*
 * The full definition of the opaque struct that cjson.h declares.
 *
 * m_procs is the allocator pointer of one node. node_alloc() sets it and
 * nothing changes it afterwards. NULL means the default
 * malloc/free/calloc/realloc, and any other value is an interned copy from
 * ccol_procs_intern(), never the pointer the caller gave us, because the
 * caller's struct can go out of scope while the tree is in use while the
 * interned copy lives as long as the process. Every public entry point that
 * takes a caller's procs interns it before any node exists. All nodes of a
 * tree carry the same m_procs value, and the pool's fast path is only there
 * when m_procs == NULL.
 *
 * Layout:
 *  - type     : one of the seven cjson_node_type_t values
 *  - attached : true once a push or a set puts this node into a CJSON_LIST
 *               or CJSON_DICTIONARY parent (see cjson_list_push() and
 *               cjson_dictionary_set() below). In a tree built through the
 *               public API exactly one parent slot can reach each node, and
 *               this flag is how those two functions refuse a child that
 *               already has a parent instead of quietly giving the node a
 *               second owner. Without the guard two container slots point at
 *               the same node and each one destroys it when its own parent
 *               goes away, which is a double free. Under the default
 *               allocator that corrupts the free-list of the thread-local
 *               node pool into a cycle pointing at itself, which hangs the
 *               teardown in __attribute__((destructor)); under a custom
 *               allocator it segfaults right away. A new node from
 *               node_alloc() starts with attached == false, and so does one
 *               from the pool's fast path, after its own memset.
 *  - m_procs  : the allocator for this node and for the strings it owns
 *  - value    : a union as large as its largest member, which is 8 bytes on
 *               a 64-bit target
 *     boolean : bool        -> CJSON_BOOL
 *     integer : long long   -> CJSON_INTEGER
 *     number  : double      -> CJSON_FLOAT
 *     string  : char *      -> CJSON_STRING  (heap-allocated, owned)
 *     list   : cvec         -> CJSON_LIST  (cvec of cjson_node_t *, NULL
 *                               until the first element; see list_store())
 *     dictionary : chmap    -> CJSON_DICTIONARY (chmap char* -> cjson_node_t *,
 *                               NULL until the first member; see
 *                               dict_store())
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
 * A dynamic string buffer used only for the output of the JSON serializer.
 * It sits on the ccol_growbuf_t of cgrowbuf.h, the same growable byte buffer
 * that the ybuf_t of cyaml.c uses, and the sb_* functions below are thin
 * wrappers around it so the serialization code of this file has its own
 * short vocabulary. Every sb_* operation does nothing once oom is set, so a
 * caller can check for an error once at the end of a serialization pass
 * instead of after each append.
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

/* Initializes with a capacity given in advance, at least 64 bytes. */
static inline void sb_init_hint(sbuf_t *sb, ccol_memmgmt_procs_t *mp,
                                size_t hint) {
  ccol_growbuf_init_hint(sb, mp, hint);
}

/* ========================================================================== */
/*                         UTF-8 WELL-FORMEDNESS                              */
/* ========================================================================== */

/*
 * RFC 8259 sec. 8.1 requires JSON text in UTF-8, and cjson.h documents
 * CJSON_STRING as UTF-8, so this module keeps one invariant: every string
 * that a DOM node can reach, a CJSON_STRING value or a dictionary key, is a
 * well-formed UTF-8 encoding of a sequence of Unicode scalar values. Every
 * place where bytes enter the DOM establishes it, and it's what makes the
 * serializer's raw pass-through of any byte >= 0x20 correct, so the
 * serializer needs no second check over its output. Without it, a document
 * read from an untrusted peer and serialized again would pass invalid or
 * overlong sequences straight through to consumers that reject them, and
 * cjson_str_val() would hand application code a buffer that the
 * documentation calls UTF-8 but that isn't.
 *
 * Both kinds of entry point refuse what is not UTF-8, and neither rewrites
 * it. A loud error is the better choice here than a quiet repair, for the
 * reasons below:
 *
 * - The parser decodes text that a peer gives. A raw byte sequence in a
 *   string that isn't well-formed UTF-8 (truncated, a stray continuation
 *   byte, overlong, an encoded surrogate, or above U+10FFFF) is a parse error
 *   that names the bytes and their position, and so is a \uXXXX escape for a
 *   surrogate that isn't one half of a high-low pair. A repair, such as a
 *   U+FFFD in place of the defect, would let two keys that differ only in
 *   their ill-formed bytes collapse into one, and our reading of the document
 *   would differ from that of a second parser that refuses it.
 * - The calling program hands a C string to the API, which builds a node from
 *   it or changes one. A string that isn't valid UTF-8 there is a bug in that
 *   program, so the API reports it with NULL or with ccol_invalid_args,
 *   exactly as it treats a non-finite double.
 */

#ifdef RUNNING_UNIT_TESTS
/* How many strings utf8_cstr_is_valid() has checked, over all threads. */
atomic_ulong _cjson_utf8_cstr_checks_for_tests;
#endif /* RUNNING_UNIT_TESTS */

/* True when the whole NUL-terminated string is well-formed UTF-8. A NULL s is
 * accepted, so a caller can pass a string that is itself optional. */
static bool utf8_cstr_is_valid(const char *s) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&_cjson_utf8_cstr_checks_for_tests, 1,
                            memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
  return !s || ccol_utf8_is_valid(s, strlen(s));
}

/* ========================================================================== */
/*                    LOCALE-INDEPENDENT NUMBER I/O                           */
/* ========================================================================== */

/*
 * JSON (RFC 8259 sec. 6) always writes the decimal point as an ASCII '.'
 * whatever the locale of the host process is, but strtod() and the "%g"
 * conversions of the printf family follow LC_NUMERIC. So every strtod() and
 * every "%g"-style snprintf() in this file goes through _cjson_strtod_c() or
 * _cjson_format_shortest_c() below, and both run their conversions inside a
 * ccol_c_locale_enter() scope (internal/cnumlocale.h has the mechanism and
 * its guarantees). Without that, parse_number() reads "3.14" as 3 under a
 * locale whose decimal point is ',', and format_double() writes text that is
 * not valid JSON.
 */

/* A strtod() that ignores the locale: it always reads '.' as the decimal
 * point, whatever the LC_NUMERIC of the calling thread is. */
static double _cjson_strtod_c(const char *nptr) {
  ccol_c_locale_scope_t scope = ccol_c_locale_enter();
  double v = strtod(nptr, NULL);
  ccol_c_locale_leave(scope);
  return v;
}

/* Writes into buf the shortest "%.Ng" text that strtod() reads back as exactly
 * the finite double val, with '.' as the decimal point whatever the
 * LC_NUMERIC of the calling thread is. All the snprintf() and strtod() calls
 * run inside one C locale scope.
 *
 * For a normal double we try the precisions 15, 16 and 17 in that order.
 * Every decimal with at most DBL_DIG (15) significant digits survives a trip
 * through a double, so when the shortest form has 15 digits or fewer "%.15g"
 * gives it with its trailing zeros dropped, and 1.0 / 3.0 comes out as
 * "0.3333333333333333" instead of the 17-digit "0.33333333333333331". A
 * subnormal double has fewer than 53 bits of precision, so the DBL_DIG
 * guarantee doesn't hold for it and "%.15g" can write more digits than
 * needed (4.94065645841247e-324 for 5e-324); for a subnormal we try every
 * precision from 1 upward. 17 digits always read back, so the last try
 * needs no test.
 *
 * The format argument "%.*g" is a literal, so we need no
 * -Wformat-nonliteral suppression. */
static void _cjson_format_shortest_c(char *buf, size_t cap, double val) {
  ccol_c_locale_scope_t scope = ccol_c_locale_enter();
  int prec = (val != 0.0 && fabs(val) < DBL_MIN) ? 1 : 15;
  for (; prec < 17; prec++) {
    snprintf(buf, cap, "%.*g", prec, val);
    if (strtod(buf, NULL) == val) break;
  }
  if (prec == 17) snprintf(buf, cap, "%.17g", val);
  ccol_c_locale_leave(scope);
}

/* ========================================================================== */
/*                         INTERNAL HELPERS                                   */
/* ========================================================================== */

/*
 * The SSO storage of a chmap_entry is already aligned by the way it is
 * built, so this memcpy is a second line of defence and not something the
 * alignment needs today. It costs nothing, and it follows the pattern that
 * cyaml, clrucache, cthreadcomm and chttpclient use for their own pointer
 * storage in a chmap.
 */
static inline cjson_node_t *_cjson_read_child(const void *src) {
  cjson_node_t *p;
  memcpy(&p, src, sizeof(p));
  return p;
}

/*
 * A container node owns no backing store until it gets its first child:
 * value.list or value.dictionary stays NULL as long as the container has
 * never held anything. So an empty container costs one node and nothing else,
 * and "[]" and "{}" in a parsed document allocate no cvec and no chmap, whose
 * empty forms cost several times as much as the node. Every read of a
 * container goes through the accessors below, which treat a NULL store as
 * empty, and every insert first calls list_store() or dict_store(), which
 * create the store on demand. Once created, a store stays until the node is
 * cleared or destroyed, even after its last child is gone.
 */
static inline size_t list_len_of(const cjson_node_t *n) {
  return n->value.list ? cvector_elem_count(n->value.list) : 0;
}

/* The element at index of a list, or NULL when index is out of range. */
static inline cjson_node_t *list_child_at(const cjson_node_t *n, size_t index) {
  if (!n->value.list) return NULL;
  void *slot = cvector_at(n->value.list, index);
  return slot ? *(cjson_node_t **)slot : NULL;
}

static inline size_t dict_size_of(const cjson_node_t *n) {
  return n->value.dictionary ? chmap_elem_count(n->value.dictionary) : 0;
}

/* Looks key_pair up in a dictionary. Gives ccol_success with the location and
 * size of the value in *vp, or some other code when the key is absent. */
static inline ccol_retval_t dict_lookup(const cjson_node_t *n,
                                        const cmap_pair *key_pair,
                                        cmap_pair *vp) {
  if (!n->value.dictionary) {
    vp->ptr = NULL;
    vp->size = 0;
    return ccol_key_not_found;
  }
  return chmap_get_elem_ref(n->value.dictionary, key_pair, vp);
}

/* The backing cvec of a list, created on the first call. NULL when the
 * allocation fails, and the node is left unchanged then. */
static cvec list_store(cjson_node_t *n) {
  if (!n->value.list)
    n->value.list =
        cvector_create_full(sizeof(cjson_node_t *), n->m_procs, NULL);
  return n->value.list;
}

/* The backing chmap of a dictionary, created on the first call. NULL when
 * the allocation fails, and the node is left unchanged then. */
static chmap dict_store(cjson_node_t *n) {
  if (!n->value.dictionary) {
    char *err = NULL;
    n->value.dictionary =
        ccol_chmap_create_compact(ccol_string, ccol_pointer, n->m_procs, &err);
  }
  return n->value.dictionary;
}

/*
 * The members of a dictionary, in insertion order. The dictionary map keeps
 * every live entry on one list in that order: a new key goes after all the
 * others, replacing the value of a key keeps its place, and a delete takes it
 * out. That's why parsing, cjson_dictionary_set(), cjson_clone(), the
 * serializers and cjson_dictionary_first() all see the members in the order
 * in which their keys first arrived. A walk allocates nothing and can't fail.
 * The first call gives the oldest member and each later call gives the member
 * after the one the previous call read.
 */
static inline const ccol_chmap_entry_ref *dict_first_entry(cjson_node_t *n) {
  return n->value.dictionary ? ccol_chmap_oldest_entry(n->value.dictionary)
                             : NULL;
}

/*
 * An explicit worklist on the heap. node_clear_value(), node_clear() and
 * __cjson_destroy() below use it to tear down a whole subtree instead of
 * recursing once per level of nesting. A tree that reaches them doesn't have
 * to come from cjson_parse(): CJSON_MAX_PARSE_DEPTH bounds that function on
 * its own (see the PARSER section further down), but the public
 * cjson_list_push() and cjson_dictionary_set() can build a tree of any depth
 * directly. Destruction has no way to fail cleanly either, since node_clear()
 * and __cjson_destroy() are both void and a caller can't get back a
 * half-freed tree that it still owns and try again. So recursion with a depth
 * cap isn't an option here, even though cjson_clone() and serialize_node()
 * return an error past such a cap: here it would only swap an immediate crash
 * for a silent, permanent leak of every node past the cap. The worklist
 * avoids both, and it keeps the native call stack at O(1) depth whatever the
 * real depth and width of the tree are.
 */
typedef struct destroy_worklist {
  cjson_node_t **items;
  size_t cap;
  size_t len;
} destroy_worklist_t;

/* Pushes child onto wl, growing wl when it is full. The growth uses a plain
 * realloc and free, because the worklist is short-lived scratch state with
 * nothing to do with the m_procs of any node. A NULL child is silently
 * ignored, as everywhere else in this file that destroys a child pointer.
 *
 * When the worklist can't grow, we tear the child down at once with an
 * ordinary recursive __cjson_destroy() call instead of deferring it. That
 * recursion can use stack in proportion to the depth of that one child's
 * subtree, but only when BOTH things hold at the same time: the tree is deep
 * enough to matter AND the allocator can't grow a small scratch array. We
 * accept that pair of failures as a rare, graceful fallback to a recursive
 * teardown of one subtree and don't engineer around it any further. It is
 * much narrower than the stack overflow the worklist prevents, which happens
 * on depth alone with no memory pressure at all. */
static void destroy_worklist_push(destroy_worklist_t *wl, cjson_node_t *child) {
  if (!child) return;
  if (wl->len == wl->cap) {
    size_t new_cap = wl->cap == 0 ? 32 : wl->cap * 2;
    cjson_node_t **grown =
        _ccol_default_mem_realloc(wl->items, new_cap * sizeof(*wl->items));
    if (!grown) {
      __cjson_destroy((cjson)child);
      return;
    }
    wl->items = grown;
    wl->cap = new_cap;
  }
  wl->items[wl->len++] = child;
}

/* The destructor callback for chmap_destroy_with_dtor(), used by the
 * CJSON_DICTIONARY case of node_clear_value() below. It puts the child node
 * of one dictionary entry onto the worklist that dtor_ctx carries instead of
 * recursing into it, so dictionary values drain through the same iterative
 * worklist as everything else. The doc comment of chmap_destroy_with_dtor in
 * chashmap.h explains why this is how a teardown reaches every child without
 * an allocation. The alternative, walking the dictionary with
 * chashmap_begin_iter() first, needs its own small allocation, which can fail
 * under sustained memory pressure, and the teardown would then silently leak
 * every child it could no longer reach. */
static void _cjson_enqueue_dict_child(const cmap_pair *val_pair,
                                      void *dtor_ctx) {
  cjson_node_t *child = _cjson_read_child(val_pair->ptr);
  destroy_worklist_push((destroy_worklist_t *)dtor_ctx, child);
}

/* Pushes item onto the growable stack array whose current length and
 * capacity are len and cap. The growth uses a plain realloc because this is
 * short-lived scratch state with nothing to do with the m_procs of any node,
 * the same as in destroy_worklist_push above. Returns false when the
 * allocation fails, leaving stack, cap and len all unchanged.
 *
 * inline_stack is the caller's small array on its own stack, and *stack
 * points at it until the search outgrows it. A tree that a caller builds top
 * down attaches a new container at every level, so each such attach searches
 * a one-node subtree and allocates nothing; without the inline array each
 * attach would pay one malloc and one free. */
static bool cycle_stack_push(cjson_node_t ***stack, cjson_node_t **inline_stack,
                             size_t inline_cap, size_t *cap, size_t *len,
                             cjson_node_t *item) {
  if (*len == *cap) {
    size_t new_cap = *cap * 2;
    cjson_node_t **grown;
    if (*stack == inline_stack) {
      grown = _ccol_default_mem_alloc(new_cap * sizeof(**stack));
      if (!grown) return false;
      memcpy(grown, inline_stack, inline_cap * sizeof(**stack));
    } else {
      grown = _ccol_default_mem_realloc(*stack, new_cap * sizeof(**stack));
      if (!grown) return false;
    }
    *stack = grown;
    *cap = new_cap;
  }
  (*stack)[(*len)++] = item;
  return true;
}

/*
 * Returns true when the subtree of haystack (haystack included) can reach
 * needle, i.e. haystack == needle or needle is a descendant somewhere in its
 * list or dictionary contents.
 *
 * cjson_list_push() and cjson_dictionary_set() use this to reject a push or
 * a set that would create a cycle. Attaching child as a new descendant of
 * target is only safe when a walk down from child can't ALREADY reach
 * target. If it can, target becomes a new ancestor of child through the edge
 * we are about to add while it stays a descendant of child through the
 * existing subtree, and that's a real cycle in the graph. The "attached"
 * field rejects a child that already has SOME parent (see the doc comment at
 * the top of this file), and also a child that is the target itself, but it
 * can't reject a node that isn't attached yet. The root of a whole tree that
 * the caller still holds and never pushed anywhere is one example: the caller
 * can push it into one of its own descendants, and this function closes that
 * gap. Once such a cycle exists, the worklist teardown of __cjson_destroy()
 * reaches the same node twice and frees it twice (see the doc comment of
 * destroy_worklist_t), because freeing an ancestor doesn't make it
 * unreachable when the cycle has its own back edge. A cycle of just two nodes
 * is enough to make glibc abort on a double free inside __cjson_destroy().
 *
 * The search is iterative, with an explicit stack on the heap, for the same
 * reason as destroy_worklist_t: the public API can build the subtree of child
 * to any depth, far past what the native stack can take, so a recursive
 * search here would bring back exactly the stack overflow that
 * destroy_worklist_t prevents.
 *
 * *incomplete is set to true when the search can't run to the end and hasn't
 * found needle yet, which only happens when the stack can't grow (the walk
 * over dictionary members allocates nothing). The caller must treat that
 * like any other allocation failure, report ccol_not_enough_memory and refuse
 * the operation. It must not read "not found yet" as "safe", since a real
 * cycle past the point where the search gave up would then go undetected.
 *
 * needle->attached cuts the whole search to O(1). A walk from another node
 * can only reach a node that some call has inserted as a child at least once,
 * and that insert is the only place that sets "attached" to true, which
 * nothing ever clears (see the doc comment of the struct). So
 * needle->attached == false proves that needle has no parent at all yet, no
 * subtree of ANY node (haystack's included) can reach it, and we know that
 * without walking a single byte of haystack. This isn't a heuristic. It keeps
 * the most common pattern at O(1) per call, where a caller wraps a subtree it
 * already built into a new outer container that isn't attached yet, for
 * example a deeply nested structure built bottom up with one
 * cjson_list_push() per level. Without the short-circuit that ordinary,
 * documented use costs O(size of the subtree so far) per call, which is
 * O(n^2) for n calls; with it each call is O(1) and n calls are O(n).
 */
static bool node_reaches(cjson_node_t *haystack, const cjson_node_t *needle,
                         bool *incomplete) {
  *incomplete = false;
  if ((const cjson_node_t *)haystack == needle) return true;
  if (!needle->attached) return false;
  if (haystack->type != CJSON_LIST && haystack->type != CJSON_DICTIONARY)
    return false;

  cjson_node_t *inline_stack[32];
  cjson_node_t **stack = inline_stack;
  size_t cap = sizeof(inline_stack) / sizeof(inline_stack[0]);
  size_t len = 0;
  bool found = false;

  if (!cycle_stack_push(&stack, inline_stack, cap, &cap, &len, haystack)) {
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
      size_t cnt = list_len_of(n);
      for (size_t i = 0; i < cnt; i++) {
        cjson_node_t *c = *(cjson_node_t **)cvector_at(n->value.list, i);
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
        cjson_node_t *c = _cjson_read_child(vp->ptr);
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
  if (stack != inline_stack) _ccol_default_mem_free(stack);
  return found;
}

/*
 * Thread-local free-list pool for cjson_node_t.
 *
 * The pool only holds nodes with m_procs == NULL, the default allocator. A
 * node with a custom allocator never goes into the pool; its own m_procs
 * allocates and frees it directly. Because of that we never record
 * which allocator a node came from: _node_pool_drain always calls a plain
 * free(), since calloc() allocated every node in the pool.
 *
 * Every DOM node would otherwise cost one calloc and one free, and in a
 * workload that parses and destroys documents over and over that round trip
 * to the allocator is most of the cost. A capped free-list lets us reuse a
 * node for nothing, so for that workload the pool pays for its extra code.
 * When a node goes back to the pool we store the old head of the list
 * inside the node's own memory, which is safe because
 * sizeof(cjson_node_t) >= sizeof(void *), and the next allocation gets that
 * head back with a memcpy so that strict aliasing gives no undefined
 * behaviour.
 */
#define _NODE_POOL_CAP 512U
#if defined(_CCOL_EMULATE_DARWIN_TLS)
/* One thread's pool and its error buffer. The pointer to this block is the
 * value of the pool key, so _node_pool_drain gets it as its argument and
 * reads no __thread variable (see ctlsmodel.h). A thread gets a block with
 * its first pooled node or its first stored error message, and it has a
 * value on the key exactly as long as it has a block. */
typedef struct {
  cjson_node_t *head;
  unsigned sz;
  char *err_buf;
} _cjson_tls_t;
static _cjson_tls_t *_cjson_tls_peek(void);
static _cjson_tls_t *_cjson_tls_get(void);
#else
static __thread cjson_node_t *_node_pool_head = NULL;
static __thread unsigned _node_pool_sz = 0;
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
atomic_ulong _cjson_pool_key_lock_count_for_tests;
/* How many nodes the pools of all threads hold together. A pool that a thread
 * leaves behind at exit stays in this count. */
atomic_long _cjson_pool_population_for_tests;
#endif /* RUNNING_UNIT_TESTS */

static cjson_node_t *node_alloc(cjson_node_type_t type,
                                ccol_memmgmt_procs_t *mp) {
  cjson_node_t *n;
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  _cjson_tls_t *t = mp == NULL ? _cjson_tls_peek() : NULL;
  if (t && t->head) {
    n = t->head;
    cjson_node_t *next;
    memcpy(&next, (cjson_node_t **)n, sizeof(next));
    t->head = next;
    t->sz--;
#else
  if (mp == NULL && _node_pool_head) {
    /* Default allocator and the pool has a node, so we reuse it. */
    n = _node_pool_head;
    cjson_node_t *next;
    memcpy(&next, (cjson_node_t **)n, sizeof(next));
    _node_pool_head = next;
    _node_pool_sz--;
#endif
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_sub_explicit(&_cjson_pool_population_for_tests, 1,
                              memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
    memset(n, 0, sizeof(*n));
  } else {
    n = _ccol_mem_calloc(mp, 1, sizeof(*n));
    if (!n) return NULL;
  }
  n->type = type;
  n->attached = false;
  n->m_procs = mp;
  return n;
}

/* Thread-local storage key whose only job is to run _node_pool_drain when a
 * thread exits. It wraps a pthread_key_t with the thread_ls_* macros of
 * common.h, because every pthread_* use in library code must go through a
 * wrapper in common.h and nothing calls the raw API directly. */
static _ccol_thread_ls_key_t _pool_pthread_key;

/* True once _do_pool_key_init() has created _pool_pthread_key and
 * initialized _pool_key_rwlock below, and false again after _pool_key_fini()
 * deletes the key. The cold path of node_free guards its _ccol_thread_ls_set
 * call with this flag under _pool_key_rwlock (see the doc comment of that
 * lock), so it never calls into a key that was deleted while a background
 * thread is still running during a concurrent dlclose. The flag is also the
 * only sign that the one-time init worked: it keeps its static initial value
 * false when either pthread call in _do_pool_key_init() fails, for example
 * from a real resource shortage in a process that has already reached
 * PTHREAD_KEYS_MAX. So check this flag before treating the rwlock or the key
 * as valid. */
static atomic_bool _pool_key_live = false;

/* True once the shared object's destructor has run. A thread whose first
 * pooled call comes after that, such as an application destructor that
 * destroys a tree, must not create the key, since nothing would delete it or
 * drain the pool it arms. _do_pool_key_init() then leaves the pool unarmed
 * and every node goes straight back to the allocator. */
static atomic_bool _pool_key_finalized = false;

/* Guards the load-and-act pair in the cold path of node_free against
 * _pool_key_fini, which flips the flag and then deletes the key.
 * _pool_key_fini is the shared object's destructor below and the only
 * writer. A bare atomic_load followed by acting on _pool_key_live isn't
 * enough, because the destructor can run between node_free's load and its
 * _ccol_thread_ls_set call and delete the key under a pthread_setspecific()
 * that is already in flight, which POSIX calls undefined behaviour. The lock
 * is held across the load and the act so the two exclude each other:
 * _ccol_thread_ls_set either completes before the key goes away or doesn't
 * run at all. _do_pool_key_init() initializes the lock on first use under the
 * _ccol_once_flag_t below, since this codebase never gives a lock a static or
 * constant initializer. */
static _ccol_rw_lock_t _pool_key_rwlock;

static void _node_pool_drain(void *);

/* The key init runs exactly once, lazily, the first time node_free() needs
 * the pool. When either pthread call fails it leaves _pool_key_live false and
 * _pool_key_rwlock uninitialized, so the read side of node_free never touches
 * either primitive before it has confirmed that the init worked. */
static _ccol_once_flag_t _pool_key_once = _CCOL_ONCE_INIT;

static void _do_pool_key_init(void) {
  if (atomic_load(&_pool_key_finalized)) return;
  if (_ccol_rw_lock_init(_pool_key_rwlock) != 0) return;
  if (_ccol_thread_ls_key_create(_pool_pthread_key, _node_pool_drain) != 0)
    return;
  atomic_store(&_pool_key_live, true);
}

/* The loader calls this when it unloads the shared object, on a dlclose or at
 * process exit.
 * - It drains the calling thread's pool first, so the pool is freed before
 *   the code mapping of the shared object goes away.
 * - It flips the liveness flag and deletes the key under the write lock, so
 *   this can never interleave with a concurrent node_free() that uses the key
 *   under the read lock (see the doc comment of _pool_key_rwlock):
 *   _ccol_thread_ls_set either completes before the key goes away or doesn't
 *   run at all. A bare atomic flag would only make the race window smaller
 *   without closing it.
 * - It deletes the key, because without that repeated dlopen and dlclose
 *   cycles use up PTHREAD_KEYS_MAX.
 * Apart from setting _pool_key_finalized it does nothing when
 * _do_pool_key_init() never ran, which is the case when no thread ever called
 * a cjson function that touches the pool; _pool_key_rwlock isn't validly
 * initialized then. */
__attribute__((destructor)) static void _pool_key_fini(void) {
  atomic_store(&_pool_key_finalized, true);
  if (!atomic_load(&_pool_key_live)) return;
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  _cjson_tls_t *t = _cjson_tls_peek();
  if (t) {
    (void)_ccol_thread_ls_set(_pool_pthread_key, NULL);
    _node_pool_drain(t);
  }
#else
  _node_pool_drain(NULL);
#endif
  _ccol_rw_lock_wrlock(_pool_key_rwlock);
  atomic_store(&_pool_key_live, false);
  _ccol_thread_ls_key_delete(_pool_pthread_key);
  _ccol_rw_lock_unlock(_pool_key_rwlock);
}

#if defined(_CCOL_EMULATE_DARWIN_TLS)
/* The calling thread's block, or NULL when the thread has none. */
static _cjson_tls_t *_cjson_tls_peek(void) {
  if (!atomic_load_explicit(&_pool_key_live, memory_order_acquire)) return NULL;
  return (_cjson_tls_t *)_ccol_thread_ls_get(_pool_pthread_key);
}

/* The calling thread's block. When the thread has none we make one and set it
 * as the value of the pool key. Gives NULL when the key doesn't exist or
 * memory runs out. The caller must already have run the key's once-guard. */
static _cjson_tls_t *_cjson_tls_get(void) {
  _cjson_tls_t *t = _cjson_tls_peek();
  if (t || !atomic_load(&_pool_key_live)) return t;
  t = (_cjson_tls_t *)calloc(1, sizeof(*t));
  if (!t) return NULL;
  bool set = false;
  _ccol_rw_lock_rdlock(_pool_key_rwlock);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&_cjson_pool_key_lock_count_for_tests, 1,
                            memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
  if (atomic_load(&_pool_key_live) &&
      _ccol_thread_ls_set(_pool_pthread_key, t) == 0)
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
 * that _node_pool_drain runs when the thread exits. The caller must already
 * have run the key's once-guard. It is always inlined so node_free() keeps the
 * same shape as with the code written in place. */
static inline __attribute__((always_inline)) void _pool_key_arm(void) {
  if (!_pool_key_armed && atomic_load(&_pool_key_live)) {
    _ccol_rw_lock_rdlock(_pool_key_rwlock);
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add_explicit(&_cjson_pool_key_lock_count_for_tests, 1,
                              memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
    if (atomic_load(&_pool_key_live) &&
        _ccol_thread_ls_set(_pool_pthread_key, (void *)1) == 0)
      _pool_key_armed = true;
    _ccol_rw_lock_unlock(_pool_key_rwlock);
  }
}

/* The per-thread message buffer of a failed parse (see cjson_report_err()).
 * The thread-exit drain frees it together with the node pool. */
static __thread char *cjson_err_buf;
#endif /* _CCOL_EMULATE_DARWIN_TLS */

/* Gives a node back to the thread-local pool, or frees it directly through
 * its own allocator. It goes back to the pool when m_procs == NULL and the
 * pool isn't full. The first call from a thread that uses the pool registers
 * a pthread destructor, which drains the pool when the thread exits. */
static void node_free(cjson_node_t *n) {
  if (n->m_procs != NULL) {
    /* A custom allocator, so free the node directly and never touch the
     * default pool. */
    _ccol_mem_free(n->m_procs, n);
    return;
  }
  /* The default allocator (m_procs == NULL), so try to give the node back
   * to the pool. */
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  _ccol_call_once(_pool_key_once, _do_pool_key_init);
  _cjson_tls_t *t = _cjson_tls_get();
  if (!t || t->sz >= _NODE_POOL_CAP) {
    free(n);
    return;
  }
  memcpy((cjson_node_t **)n, &t->head, sizeof(t->head));
  t->head = n;
  t->sz++;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&_cjson_pool_population_for_tests, 1,
                            memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
#else
  if (_node_pool_sz >= _NODE_POOL_CAP) {
    free(n);
    return;
  }
  /* Any function that touches _pool_key_rwlock directly runs the once-guard
   * itself, as with every pthread primitive and every _ccol_once_flag_t in
   * this codebase. Don't drop it because the call graph says some other
   * function already ran it; that argument breaks silently as soon as a new
   * call path shows up. */
  _ccol_call_once(_pool_key_once, _do_pool_key_init);
  if (_node_pool_sz == 0) {
    /* The cold path, which runs once per thread each time its pool has become
     * empty. The doc comment of _pool_key_rwlock explains why the
     * load-and-act needs a lock and why a bare atomic check isn't enough.
     * The outer check here takes no lock; it only skips _pool_key_rwlock
     * entirely when _do_pool_key_init() never completed (a real shortage of
     * pthread resources, for example), because the rwlock is then never
     * validly initialized and locking it is undefined behaviour. That outer
     * check doesn't reopen the race that the inner lock closes.
     * _pool_key_live only goes from false to true inside _do_pool_key_init()
     * itself, and the _ccol_call_once() above has already run that function
     * to the end with the usual happens-before guarantee of pthread_once. So
     * a true here means the rwlock is fully initialized, and it stays valid
     * memory for the rest of the process even if a concurrent unload of the
     * shared object flips the flag back to false right afterwards. The inner
     * check under the lock is what makes racing against that later move to
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
  memcpy((cjson_node_t **)n, &_node_pool_head, sizeof(_node_pool_head));
  _node_pool_head = n;
  _node_pool_sz++;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&_cjson_pool_population_for_tests, 1,
                            memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
#endif /* _CCOL_EMULATE_DARWIN_TLS */
}

/* Drains the calling thread's node pool. Every node in the pool has
 * m_procs == NULL, so a plain free() is always right here.
 *
 * The drain also disarms the key for this thread. At thread exit the C
 * library clears the key's value before it calls this destructor, so the
 * thread holds no value at that point. A destructor of some other key can run
 * after this one and destroy a tree, and node_free() then refills the pool.
 * With the flag cleared, that refill sets the value again and the C library
 * calls this destructor once more in its next round over the keys; if we left
 * the flag set, the refill would never arm the key and every node in it would
 * leak. */
#if defined(_CCOL_EMULATE_DARWIN_TLS)
/* Under _CCOL_EMULATE_DARWIN_TLS the drain frees the block that arg names.
 * The C library has already cleared the key's value, so a node that another
 * key's destructor frees later makes a new block and sets the value again,
 * and the C library calls this destructor once more for it. */
static void _node_pool_drain(void *arg) {
  _cjson_tls_t *t = (_cjson_tls_t *)arg;
  if (!t) return;
  cjson_node_t *n = t->head;
  while (n) {
    cjson_node_t *next;
    memcpy(&next, (cjson_node_t **)n, sizeof(next));
    free(n);
    n = next;
  }
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_sub_explicit(&_cjson_pool_population_for_tests, (long)t->sz,
                            memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
  free(t->err_buf);
  free(t);
}
#else
static void _node_pool_drain(void *arg) {
  (void)arg;
  cjson_node_t *n = _node_pool_head;
  while (n) {
    cjson_node_t *next;
    memcpy(&next, (cjson_node_t **)n, sizeof(next));
    free(n);
    n = next;
  }
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_sub_explicit(&_cjson_pool_population_for_tests,
                            (long)_node_pool_sz, memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
  _node_pool_head = NULL;
  _node_pool_sz = 0;
  free(cjson_err_buf);
  cjson_err_buf = NULL;
  _pool_key_armed = false;
}
#endif /* _CCOL_EMULATE_DARWIN_TLS */

#ifdef RUNNING_UNIT_TESTS
/* The size of the free-list in the calling thread's node pool. White-box unit
 * tests use it to check how the pool evicts a node at its cap
 * (_NODE_POOL_CAP) and that one thread doesn't see another thread's pool. It
 * isn't part of the public API. */
#if defined(_CCOL_EMULATE_DARWIN_TLS)
size_t cjson_debug_pool_size(void) {
  _cjson_tls_t *t = _cjson_tls_peek();
  return t ? (size_t)t->sz : 0;
}
#else
size_t cjson_debug_pool_size(void) { return (size_t)_node_pool_sz; }
#endif
#endif

/* Deep-frees the value payload of n: the bytes of a string, or a list or
 * dictionary container. Every direct child is pushed onto wl instead of being
 * recursed into, and n->type and n itself are left alone. */
static void node_clear_value(cjson_node_t *n, destroy_worklist_t *wl) {
  switch (n->type) {
    case CJSON_STRING:
      _ccol_mem_free(n->m_procs, n->value.string);
      n->value.string = NULL;
      break;
    case CJSON_LIST: {
      size_t cnt = list_len_of(n);
      for (size_t i = 0; i < cnt; i++) {
        cjson_node_t *child = *(cjson_node_t **)cvector_at(n->value.list, i);
        destroy_worklist_push(wl, child);
      }
      __cvector_destroy(n->value.list);
      n->value.list = NULL;
      break;
    }
    case CJSON_DICTIONARY: {
      /* We use chmap_destroy_with_dtor from chashmap.h here and not a walk with
       * chashmap_begin_iter followed by the destroy. That helper allocates
       * its own small iterator struct, which can fail under sustained memory
       * pressure, and then there is no way left to reach the remaining
       * entries and the teardown silently leaks every child past that point.
       * chmap_destroy_with_dtor walks the map's internal storage directly, so
       * it can always reach a live entry. */
      chmap_destroy_with_dtor(n->value.dictionary, _cjson_enqueue_dict_child,
                              wl);
      n->value.dictionary = NULL;
      break;
    }
    default:
      break;
  }
}

/* Drains wl until it is empty, fully destroying every node in it, the node
 * struct included. node_clear_value() above pushes the children of each
 * drained node onto the worklist, so it keeps getting work until the whole
 * subtree of the initial contents of wl is gone. It's a plain loop and not
 * recursion, and that is what keeps the stack use of node_clear() and
 * __cjson_destroy() independent of the depth of the tree. */
static void destroy_worklist_drain(destroy_worklist_t *wl) {
  while (wl->len > 0) {
    cjson_node_t *n = wl->items[--wl->len];
    node_clear_value(n, wl);
    node_free(n);
  }
}

/* Deep-frees the resources of a node's value without freeing the node
 * itself. node_reinit_scalar() uses it to throw away the old list or
 * dictionary value of a node before writing a new scalar into that node in
 * place, for a cjson_set() for example. The old value can be a tree of any
 * depth (see the doc comment of destroy_worklist_t above), so every
 * descendant below the direct children of n goes through the same iterative
 * worklist as __cjson_destroy() uses and nothing here recurses. */
static void node_clear(cjson_node_t *n) {
  destroy_worklist_t wl = {NULL, 0, 0};
  node_clear_value(n, &wl);
  destroy_worklist_drain(&wl);
  _ccol_default_mem_free(wl.items);
}

/*
 * Checks a scalar payload described by cjson_set() or by a direct
 * _cjson_set_typed() call, without touching any node. Returns ccol_success
 * when node_reinit_scalar_checked() can store the payload, which can then
 * only fail for lack of memory. Returns ccol_invalid_args for a wrong type, a
 * wrong size, a non-finite float, a void * that isn't NULL, and a string that
 * isn't valid UTF-8.
 */
static ccol_retval_t scalar_payload_check(cjson_node_type_t type, void *raw,
                                          size_t raw_size) {
  /* Rejects the composite types CJSON_LIST and CJSON_DICTIONARY. The switch
   * that assigns the value after the clear only covers scalars, and landing
   * in its default branch after node_clear would leave the node in an
   * inconsistent state. This is also where the list of types that cjson_set()
   * documents is enforced: _cjson_type_of() in cjson.h doesn't know every C
   * type, and a value of a type it doesn't know arrives here as
   * _CJSON_TYPE_UNSUPPORTED. That falls into the default branch too, so the
   * switch rejects it before anything touches the node instead of the library
   * silently writing it as CJSON_NULL. */
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

  /* Every accepted type except CJSON_NULL carries a payload that we read
   * through raw. A NULL raw has nothing to read, and every size check below
   * would dereference it. Only CJSON_NULL accepts raw == NULL, which is how a
   * direct _cjson_set_typed() call says "no payload". */
  if (type != CJSON_NULL && !raw) return ccol_invalid_args;

  /* The _Generic dispatch of cjson_set() (_cjson_type_of() in cjson.h) maps
   * ANY C expression of a void pointer type to CJSON_NULL, not just the
   * literal NULL. So a caller can pass a live, non-NULL void pointer by
   * mistake instead of one of the documented types (bool, integer, float,
   * double, char pointer, const char pointer and NULL). Without this check
   * the library would silently drop that pointer, write the leaf as a JSON
   * null and report nothing. We reject such a pointer instead, which matches
   * the list of accepted types in the documentation of cjson_set(). The check
   * only runs when raw carries a full pointer-sized payload (raw_size ==
   * sizeof(void*)), which is exactly what the macro always passes for this
   * type. A direct _cjson_set_typed() call can pass raw == NULL and
   * raw_size == 0 for a real null with no payload, and that is left alone. */
  if (type == CJSON_NULL && raw && raw_size == sizeof(void *) &&
      *(void **)raw != NULL)
    return ccol_invalid_args;

  /* The size of the float, and whether it is finite. */
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

  /* The raw_size of the integer, before anything touches the node. */
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

  /* The raw_size of the bool, before anything touches the node. Only a direct
   * call to _cjson_set_typed() that bypasses the macro can get here, because
   * cjson_set() always passes sizeof(bool) for a bool expression, so a
   * mismatch can only come from a caller of the public back-end function
   * itself. Without this check *(bool *)raw reads past a narrower raw buffer,
   * or reads a garbage byte out of a wider one, and stores whatever bit
   * pattern it read into a _Bool, which is a trap representation unless the
   * byte is exactly 0 or 1. */
  if (type == CJSON_BOOL && raw_size != sizeof(bool)) return ccol_invalid_args;

  /* The raw_size of the string, before anything touches the node, for the same
   * reason as the bool check above and reached the same way. cjson_set()
   * always turns a string into a pointer-sized payload because it copies its
   * argument into a local of the decayed type (a char array arrives as a
   * pointer to its first character), so a mismatch can only come from a
   * direct call to the public back-end function. Without this check
   * *(const char **)raw reads sizeof(const char *) bytes out of the caller's
   * narrower buffer and then copies a string through whatever garbage pointer
   * those bytes make up. */
  if (type == CJSON_STRING && raw_size != sizeof(const char *))
    return ccol_invalid_args;

  /* Same rule as cjson_create_string_mp(): a caller's string that isn't valid
   * UTF-8 has no JSON form, so we report it instead of storing it. */
  if (type == CJSON_STRING && !utf8_cstr_is_valid(*(const char **)raw))
    return ccol_invalid_args;
  return ccol_success;
}

/*
 * Overwrites the content of an existing node with a payload that
 * scalar_payload_check() has already accepted, and deep-frees everything the
 * old content owned. A string is allocated with n->m_procs.
 *
 * Returns ccol_success, or ccol_not_enough_memory when copying the string
 * fails. The copy happens before node_clear(), so that failure leaves the
 * original node completely intact.
 */
static ccol_retval_t node_reinit_scalar_checked(cjson_node_t *n,
                                                cjson_node_type_t type,
                                                void *raw, size_t raw_size,
                                                bool is_signed) {
  double pre_d = 0.0;
  if (type == CJSON_FLOAT)
    pre_d = raw_size == sizeof(float) ? (double)*(float *)raw : *(double *)raw;

  /* Allocate the new string first, so a failed allocation leaves the existing
   * content untouched. */
  char *new_str = NULL;
  if (type == CJSON_STRING) {
    const char *s = *(const char **)raw;
    if (s) {
      new_str = ccol_strdup(n->m_procs, s);
      if (!new_str) return ccol_not_enough_memory;
    }
  }

  /* === The change starts here. Every check has passed and nothing below can
   * fail. === */
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
            /* An unsigned 64-bit value above LLONG_MAX has no CJSON_INTEGER
             * form, so we store the nearest CJSON_FLOAT, which is what
             * cjson_parse() makes of the same decimal literal. Converting to
             * long long would store a negative number and report success. */
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
        n->type = CJSON_NULL; /* a NULL C string becomes a JSON null */
      else
        n->value.string = new_str;
      break;
    default:
      break; /* Unreachable, since the cases above cover every scalar
              * type. */
  }
  return ccol_success;
}

/* Overwrites the content of an existing node with a new scalar value. Every
 * check runs before anything touches the node, so a failure leaves it
 * completely intact. The return codes are those of scalar_payload_check() and
 * node_reinit_scalar_checked(). */
static ccol_retval_t node_reinit_scalar(cjson_node_t *n, cjson_node_type_t type,
                                        void *raw, size_t raw_size,
                                        bool is_signed) {
  ccol_retval_t r = scalar_payload_check(type, raw, raw_size);
  if (r != ccol_success) return r;
  return node_reinit_scalar_checked(n, type, raw, raw_size, is_signed);
}

/* Allocates a new scalar node with the caller's allocator, from a payload
 * that scalar_payload_check() has already accepted. The only possible failure
 * is ccol_not_enough_memory, for the node or for the copy of a string. */
static ccol_retval_t node_make_scalar(cjson_node_type_t type, void *raw,
                                      size_t raw_size, bool is_signed,
                                      ccol_memmgmt_procs_t *mp,
                                      cjson_node_t **out) {
  *out = NULL;
  cjson_node_t *n = node_alloc(CJSON_NULL, mp);
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
static inline bool cjson_intern_procs(ccol_memmgmt_procs_t **mp) {
  if (*mp == NULL) return true;
  return ccol_procs_intern(*mp, mp) == ccol_success;
}

/*
 * The factory functions, one per node type. mp can be NULL, and then the
 * default malloc, calloc and free are used. All of them return NULL when an
 * allocation fails.
 *
 * cjson_create_double_mp also rejects a non-finite value (an Inf or a NaN),
 * since JSON has no way to write one.
 *
 * cjson_create_string_mp with val == NULL gives a CJSON_NULL node, the same
 * as cjson_set does for a char * variable that is NULL.
 *
 * cjson_create_list_mp and cjson_create_dictionary_mp give empty containers,
 * which the caller fills with cjson_list_push and cjson_dictionary_set.
 */
static cjson cjson_create_null_interned(ccol_memmgmt_procs_t *mp) {
  return (cjson)node_alloc(CJSON_NULL, mp);
}

static cjson cjson_create_bool_interned(bool val, ccol_memmgmt_procs_t *mp) {
  cjson_node_t *n = node_alloc(CJSON_BOOL, mp);
  if (n) n->value.boolean = val;
  return (cjson)n;
}

static cjson cjson_create_int_interned(long long val,
                                       ccol_memmgmt_procs_t *mp) {
  cjson_node_t *n = node_alloc(CJSON_INTEGER, mp);
  if (n) n->value.integer = val;
  return (cjson)n;
}

static cjson cjson_create_double_interned(double val,
                                          ccol_memmgmt_procs_t *mp) {
  if (!isfinite(val)) return NULL;
  cjson_node_t *n = node_alloc(CJSON_FLOAT, mp);
  if (n) n->value.number = val;
  return (cjson)n;
}

static cjson cjson_create_string_interned(const char *val,
                                          ccol_memmgmt_procs_t *mp) {
  if (!val) return cjson_create_null_interned(mp);
  /* JSON text is UTF-8 (RFC 8259 sec. 8.1), so a string that isn't valid UTF-8
   * has no JSON form and we refuse it instead of silently rewriting it, the
   * same way cjson_create_double_mp() treats a non-finite double. Without
   * this check the bytes reach the serializer, which writes them raw, and the
   * output is text that a consumer following the RFC rejects. */
  if (!utf8_cstr_is_valid(val)) return NULL;
  cjson_node_t *n = node_alloc(CJSON_STRING, mp);
  if (!n) return NULL;
  n->value.string = ccol_strdup(mp, val);
  if (!n->value.string) {
    node_free(n);
    return NULL;
  }
  return (cjson)n;
}

/* A new container has no backing store yet; list_store() and dict_store()
 * create it on the first insert. */
static cjson cjson_create_list_interned(ccol_memmgmt_procs_t *mp) {
  return (cjson)node_alloc(CJSON_LIST, mp);
}

static cjson cjson_create_dictionary_interned(ccol_memmgmt_procs_t *mp) {
  return (cjson)node_alloc(CJSON_DICTIONARY, mp);
}

cjson cjson_create_null_mp(ccol_memmgmt_procs_t *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_null_interned(mp);
}

cjson cjson_create_bool_mp(bool val, ccol_memmgmt_procs_t *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_bool_interned(val, mp);
}

cjson cjson_create_int_mp(long long val, ccol_memmgmt_procs_t *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_int_interned(val, mp);
}

cjson cjson_create_double_mp(double val, ccol_memmgmt_procs_t *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_double_interned(val, mp);
}

cjson cjson_create_string_mp(const char *val, ccol_memmgmt_procs_t *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_string_interned(val, mp);
}

cjson cjson_create_list_mp(ccol_memmgmt_procs_t *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_list_interned(mp);
}

cjson cjson_create_dictionary_mp(ccol_memmgmt_procs_t *mp) {
  if (!cjson_intern_procs(&mp)) return NULL;
  return cjson_create_dictionary_interned(mp);
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/* Frees a node and all its descendants iteratively (the doc comment of
 * destroy_worklist_t explains why). A tree that gets here doesn't have to come
 * from cjson_parse(), and when a caller builds it directly through the public
 * API its depth has no bound. Calling it on NULL is safe. It does NOT set the
 * caller's pointer to NULL; the cjson_destroy() macro does that.
 *
 * n itself is cleared and freed directly and never goes through
 * destroy_worklist_push(). When that function can't allocate it falls back to
 * a recursive __cjson_destroy() call on the item it couldn't queue, and n is
 * the argument of this very call, so pushing n there could make us call
 * ourselves on the same node while the allocator is out of memory. Real
 * descendants aren't affected: the worklist drains every one of them, the
 * same way node_clear() uses it. */
void __cjson_destroy(cjson node) {
  if (!node) return;
  cjson_node_t *n = (cjson_node_t *)node;

  destroy_worklist_t wl = {NULL, 0, 0};
  node_clear_value(n, &wl);
  node_free(n);

  destroy_worklist_drain(&wl);
  _ccol_default_mem_free(wl.items);
}

/* ========================================================================== */
/*                         LEAF VALUE ACCESS                                  */
/* ========================================================================== */

/* The type tag of the node, or CJSON_NULL for a NULL handle. */
cjson_node_type_t cjson_type(cjson node) {
  if (!node) return CJSON_NULL;
  return ((cjson_node_t *)node)->type;
}

/*
 * Typed value readers. Each one calls _ccol_fatal_err() on a type mismatch
 * and on a NULL handle, so guard the call with cjson_type() when the call
 * site doesn't know the type at compile time.
 */
bool cjson_bool_val(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_BOOL)
    _ccol_fatal_err("cjson_bool_val: node is %s, expected CJSON_BOOL",
                    n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.boolean;
}

long long cjson_int_val(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_INTEGER)
    _ccol_fatal_err("cjson_int_val: node is %s, expected CJSON_INTEGER",
                    n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.integer;
}

double cjson_double_val(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_FLOAT)
    _ccol_fatal_err("cjson_double_val: node is %s, expected CJSON_FLOAT",
                    n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.number;
}

const char *cjson_str_val(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_STRING)
    _ccol_fatal_err("cjson_str_val: node is %s, expected CJSON_STRING",
                    n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.string;
}

/* The number of elements of a list, or the number of keys of a dictionary.
 * Both call _ccol_fatal_err() when the node is NULL or doesn't have the type
 * they need.
 */
size_t cjson_list_len(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_LIST)
    _ccol_fatal_err("cjson_list_len: node is %s, expected CJSON_LIST",
                    n ? cjson_type_str((cjson)n) : "NULL");
  return list_len_of(n);
}

size_t cjson_dictionary_size(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_DICTIONARY)
    _ccol_fatal_err(
        "cjson_dictionary_size: node is %s, expected CJSON_DICTIONARY",
        n ? cjson_type_str((cjson)n) : "NULL");
  return dict_size_of(n);
}

/* ========================================================================== */
/*                       LIST / DICTIONARY MANIPULATION                       */
/* ========================================================================== */

/*
 * Appends child to the element list of arr.
 *
 * Each return code carries exactly one ownership rule. ccol_invalid_args
 * ALWAYS means that the call rejected its arguments and touched nothing:
 * child is still owned by the caller, or by the container it was already
 * attached to, and that owner still needs to destroy it. Every other
 * non-success code (ccol_not_enough_memory or ccol_container_full) ALWAYS
 * means that ownership moved and the call has already destroyed child, so a
 * second free of child is a double free. Without this split a caller couldn't
 * act on a code at all, since a free on ccol_invalid_args would double-free
 * the rejections and skipping the free on ccol_not_enough_memory would leak
 * the transfers. Any new failure path added here has to keep this split,
 * because it's the whole ownership contract of the function.
 *
 * child must not already be attached to a list or dictionary parent, and that
 * includes arr itself and the chain of arr's ancestors: child must not
 * contain arr anywhere in its own subtree, which node_reaches() below checks.
 * Two parent slots that reach one node each destroy that node in their own
 * teardown, which is a double free that corrupts the heap, and under the
 * default allocator with its thread-local node pool it also builds a
 * free-list cycle pointing at itself. A cycle in the graph, where child can
 * already reach arr, corrupts the heap the same way at teardown, because
 * freeing one node of the cycle doesn't make it unreachable when the cycle
 * has its own back edge.
 *
 * So five conditions give ccol_invalid_args: child is NULL; child is already
 * attached; child is arr itself; arr is NULL or not a CJSON_LIST; or attaching
 * child would close a cycle that the check finds. Two outcomes move
 * ownership: the cycle check runs out of memory, or the insert itself fails.
 */
ccol_retval_t cjson_list_push(cjson arr, cjson child) {
  if (!child) return ccol_invalid_args;
  cjson_node_t *c = (cjson_node_t *)child;

  /* Every rejection that must leave child completely untouched is decided
   * BEFORE the container kind check below, because that check takes
   * ownership of an unattached child and deep-frees it. Both conditions can
   * hold at once when a caller passes one handle as both arr and child and
   * that handle isn't a CJSON_LIST, and answering the destroying branch first
   * would free a node the caller still owns while returning the very code
   * that the documentation calls "left completely untouched". We compare
   * against arr and not n, so the check holds even when arr is the wrong kind
   * of node. */
  if (c->attached || (const void *)c == (const void *)arr) {
    /* Something else already owns child, or child is arr itself. Accepting
     * either one would give the node two owners, each of which would free it
     * when its own parent is destroyed. */
    return ccol_invalid_args;
  }

  cjson_node_t *n = (arr && ((cjson_node_t *)arr)->type == CJSON_LIST)
                        ? (cjson_node_t *)arr
                        : NULL;
  if (!n) {
    /* arr is NULL or isn't a CJSON_LIST. That's an argument rejection, so child
     * stays exactly as it was. A destroy here would make ccol_invalid_args
     * mean "untouched" on one path and "already freed" on another, and a
     * caller acting on the code would then double-free or leak depending on
     * which path it hit. */
    return ccol_invalid_args;
  }
  bool cycle_incomplete;
  if (node_reaches(c, (const cjson_node_t *)n, &cycle_incomplete)) {
    /* child's subtree can already reach arr, so attaching child here would make
     * arr a new ancestor of child while arr stays a descendant of child, a
     * real cycle. We reject the call and touch neither node (see the doc
     * comment of node_reaches() above). */
    return ccol_invalid_args;
  }
  if (cycle_incomplete) {
    /* The cycle check couldn't run to the end because it ran out of memory,
     * so we refuse the call, since carrying on could let an undetected cycle
     * through. child is certainly not attached here. For this function
     * ccol_not_enough_memory is an outcome that moves ownership, so the call
     * destroys child, exactly as when the insert below fails for lack of
     * memory. One return code must carry one ownership rule: if this path
     * left child alive while the insert path freed it, no caller could act
     * correctly on ccol_not_enough_memory, because freeing would double-free
     * one case and not freeing would leak the other. */
    __cjson_destroy(child);
    return ccol_not_enough_memory;
  }
  cvec store = list_store(n);
  ccol_retval_t r =
      store ? cvector_push_back(store, &c) : ccol_not_enough_memory;
  if (r != ccol_success) {
    /* A failed insert moves ownership, so the caller need not keep track of
     * child across this error path. Every code that can get here moves
     * ownership: the insert can only report ccol_invalid_args for a NULL
     * element pointer, and what it gets is the address of a local variable.
     * A store that can't be created counts as a failed insert. */
    __cjson_destroy(child);
    return r;
  }
  c->attached = true;
  return ccol_success;
}

/* The element at position index, as a borrowed reference, so don't destroy it
 * apart from its parent list. NULL when the index is out of bounds, and also
 * when arr is NULL or not a CJSON_LIST node. */
cjson cjson_list_get(cjson arr, size_t index) {
  if (!arr) return NULL;
  cjson_node_t *n = (cjson_node_t *)arr;
  if (n->type != CJSON_LIST) return NULL;
  return (cjson)list_child_at(n, index);
}

/* Replaces the child that the dictionary slot slot_vp describes with child.
 * slot_vp is the pair that ccol_chmap_insert_or_get_elem() filled for a key
 * that was already present. The old child is freed first and then the new
 * pointer goes into the slot, so the slot never names freed memory once this
 * returns. Freeing the old child changes no map that holds the slot, so
 * slot_vp->ptr stays valid across the free. child must have no parent, and
 * it becomes attached here. */
static void dict_slot_replace_child(const cmap_pair *slot_vp,
                                    cjson_node_t *child) {
  cjson_node_t *old_child = _cjson_read_child(slot_vp->ptr);
  __cjson_destroy((cjson)old_child);
  memcpy((void *)slot_vp->ptr, &child, sizeof(child));
  child->attached = true;
}

/*
 * Inserts or replaces the value for key in obj. Ownership splits exactly as
 * in cjson_list_push: ccol_invalid_args ALWAYS means that the arguments were
 * rejected and child is untouched, and every other non-success code ALWAYS
 * means that ownership moved and child has already been destroyed. When the
 * key already exists, the old child is freed and the slot pointed at child
 * with nothing in between that can fail, so the slot never holds a dangling
 * pointer once we return. A replaced member keeps its place in the insertion
 * order, and a new key goes after every other member.
 *
 * child must not already be attached to a list or dictionary parent, and that
 * includes obj itself and the chain of obj's ancestors: attaching child would
 * make obj a new ancestor of child while obj stays its descendant, which is a
 * cycle. The one exception is that child can be exactly the value key already
 * holds. The doc comment of cjson_list_push explains why an attached child is
 * otherwise rejected: accepting it would give one node two owners, each
 * destroying it on its own, which corrupts the heap. A cycle corrupts the
 * heap the same way, because freeing an ancestor doesn't make it unreachable
 * through the back edge of the cycle. We accept the case where key already
 * holds child as a harmless no-op on purpose, so that
 * `cjson_dictionary_set(obj, k, cjson_dictionary_get(obj, k))` succeeds; that
 * call sets a key to the value it already has and isn't an illegal move of a
 * child to a new parent.
 */
ccol_retval_t cjson_dictionary_set(cjson obj, const char *key, cjson child) {
  if (!child) return ccol_invalid_args;
  cjson_node_t *c = (cjson_node_t *)child;

  /* The self-attach is rejected without touching child, and that must be
   * decided BEFORE the container kind check below, because that check takes
   * ownership of an unattached child and deep-frees it. Both conditions hold
   * at once when a caller passes one handle as both obj and child and that
   * handle isn't a CJSON_DICTIONARY, and answering the destroying branch first
   * would free a node the caller still owns while returning the very code
   * that the documentation calls "left completely untouched". We compare
   * against obj and not n, so the check holds even when obj is the wrong kind
   * of node. Only the self-check sits up here. The rejection of an
   * already-attached child must stay below the c == old_child no-op,
   * because only an attached child can reach that no-op (see the comment of
   * that check). */
  if ((const void *)c == (const void *)obj) return ccol_invalid_args;

  cjson_node_t *n = (obj && ((cjson_node_t *)obj)->type == CJSON_DICTIONARY)
                        ? (cjson_node_t *)obj
                        : NULL;
  if (!n || !key || !utf8_cstr_is_valid(key)) {
    /* obj is NULL or not a CJSON_DICTIONARY, or key is NULL or not valid UTF-8.
     * That's an argument rejection, so child stays exactly as it was, whether
     * it is a free-standing tree the caller still owns or a node that another
     * container already owns. A key that isn't valid UTF-8 has no JSON form
     * and the serializer would write it raw, so it is refused for the same
     * reason as a string value (see cjson_create_string_mp()). Destroying
     * child here would make ccol_invalid_args mean "untouched" on one path and
     * "already freed" on another, and a caller acting on the code would then
     * double-free or leak depending on which path it hit. */
    return ccol_invalid_args;
  }

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};

  if (c->attached) {
    /* Only an attached child can be the node key already holds, so only this
     * branch needs to look the key up before deciding. Setting a key to the
     * value it already holds is a harmless no-op. Any other attached child
     * belongs to some other parent, so we reject the call and touch neither
     * child nor the slot. child == obj was already rejected at the top of the
     * function, before the container kind check. */
    cmap_pair held_vp;
    if (dict_lookup(n, &kp, &held_vp) == ccol_success &&
        _cjson_read_child(held_vp.ptr) == c)
      return ccol_success;
    return ccol_invalid_args;
  }

  bool cycle_incomplete;
  if (node_reaches(c, (const cjson_node_t *)n, &cycle_incomplete)) {
    /* child's subtree can already reach obj, so attaching child here would make
     * obj a new ancestor of child while obj stays a descendant of child, a
     * real cycle. We reject the call and touch neither node (see the doc
     * comment of node_reaches()). */
    return ccol_invalid_args;
  }
  if (cycle_incomplete) {
    /* The cycle check couldn't run to the end because it ran out of memory,
     * so we refuse the call, since carrying on could let an undetected cycle
     * through. child is certainly not attached here. For this function
     * ccol_not_enough_memory is an outcome that moves ownership, so the call
     * destroys child, exactly as when the insert below fails for lack of
     * memory. One return code must carry one ownership rule: if this path
     * left child alive while the insert path freed it, no caller could act
     * correctly on ccol_not_enough_memory, because freeing would double-free
     * one case and not freeing would leak the other. */
    __cjson_destroy(child);
    return ccol_not_enough_memory;
  }

  /* Every refusal has been decided above, so nothing below can leave child
   * untouched. With one hash and one probe we either insert child under a
   * new key or get back the slot of the key that is already there. */
  cmap_pair vp = {.ptr = &c, .size = sizeof(c)};
  cmap_pair old_vp;
  chmap store = dict_store(n);
  ccol_retval_t r =
      store ? ccol_chmap_insert_or_get_elem(store, &kp, &vp, &old_vp)
            : ccol_not_enough_memory;
  if (r == ccol_success) {
    c->attached = true;
    return ccol_success;
  }
  if (r == ccol_key_already_present) {
    /* The map is unchanged. Free the old child, whose subtree can't hold child
     * since child has no parent, and then point the slot at child. */
    dict_slot_replace_child(&old_vp, c);
    return ccol_success;
  }
  /* A failed insert moves ownership, as in cjson_list_push. Every code that can
   * get here moves ownership: the map insert can only report
   * ccol_invalid_args for a key or value pair that is NULL or has size zero,
   * and we build both pairs right here from a non-empty key and the address
   * of a local variable. A store that can't be created counts as a failed
   * insert. */
  __cjson_destroy((cjson)child);
  return r;
}

/* Looks key up in obj and returns its child as a borrowed reference. NULL
 * when the key isn't found, when obj is NULL, or when obj isn't a
 * CJSON_DICTIONARY node. */
cjson cjson_dictionary_get(cjson obj, const char *key) {
  if (!obj || !key) return NULL;
  cjson_node_t *n = (cjson_node_t *)obj;
  if (n->type != CJSON_DICTIONARY) return NULL;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair vp;
  if (dict_lookup(n, &kp, &vp) != ccol_success) return NULL;
  return _cjson_read_child(vp.ptr);
}

/*
 * Removes and deep-frees the element at position index of a list.
 *
 * The element at that index is destroyed and every element after it moves
 * one position to the left, which is O(n) in the number of elements after the
 * index; cvector_pop_back then makes the vector one element shorter.
 */
ccol_retval_t cjson_list_remove(cjson arr, size_t index) {
  if (!arr) return ccol_invalid_args;
  cjson_node_t *n = (cjson_node_t *)arr;
  if (n->type != CJSON_LIST) return ccol_invalid_args;
  size_t cnt = list_len_of(n);
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
 * Removes and deep-frees the entry with the given key from a dictionary.
 *
 * We read the child pointer out before deleting the map entry, so the subtree
 * is only freed once the hash table no longer points at it.
 */
ccol_retval_t cjson_dictionary_remove(cjson obj, const char *key) {
  if (!obj || !key) return ccol_invalid_args;
  cjson_node_t *n = (cjson_node_t *)obj;
  if (n->type != CJSON_DICTIONARY) return ccol_invalid_args;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair vp;
  if (dict_lookup(n, &kp, &vp) != ccol_success) return ccol_key_not_found;
  cjson_node_t *child = _cjson_read_child(vp.ptr);
  ccol_retval_t r = chmap_delete_elem(n->value.dictionary, &kp);
  if (r == ccol_success) __cjson_destroy((cjson)child);
  return r;
}

/* ========================================================================== */
/*                         DICTIONARY ITERATION                               */
/* ========================================================================== */

/* The cursor reads a member's successor at the moment it steps onto that
 * member. Removing the current member frees only that member's entry, so the
 * successor the cursor holds stays valid. */
bool cjson_dictionary_next(cjson_dictionary_iter *it) {
  if (!it) return false;
  const ccol_chmap_entry_ref *e = (const ccol_chmap_entry_ref *)it->_cjson_next;
  if (!e) {
    it->key = NULL;
    it->value = NULL;
    return false;
  }
  const cmap_pair *kp, *vp;
  it->_cjson_next = ccol_chmap_entry_read(e, &kp, &vp);
  it->key = (const char *)kp->ptr;
  it->value = (cjson)_cjson_read_child(vp->ptr);
  return true;
}

bool cjson_dictionary_first(cjson dict, cjson_dictionary_iter *it) {
  if (!it) return false;
  cjson_node_t *n = (cjson_node_t *)dict;
  it->_cjson_next =
      (n && n->type == CJSON_DICTIONARY) ? dict_first_entry(n) : NULL;
  return cjson_dictionary_next(it);
}

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

/*
 * The deepest nesting cjson_clone() accepts, counted in containers: a tree
 * may hold this many lists and dictionaries inside one another, with any
 * value inside the innermost one. The tree given to cjson_clone() doesn't
 * have to come from cjson_parse(), because the public cjson_list_push() and
 * cjson_dictionary_set() can build one directly to any depth, so
 * CJSON_MAX_PARSE_DEPTH says nothing about it; that constant only bounds what
 * the parser accepts. This one has the same value, so a tree exactly at the
 * parser's limit can be cloned instead of failing for no good reason. It is a
 * separate constant only because CJSON_MAX_PARSE_DEPTH is defined further
 * down in this file.
 *
 * It's a policy limit and has nothing to do with the stack: the clone below
 * walks the tree with an explicit stack of frames on the heap and uses a
 * fixed, small amount of native stack at any depth.
 */
#define CJSON_CLONE_MAX_DEPTH 500

/*
 * One container that the explicit stack of clone_tree() is filling.
 *
 * src   : the source container. Its children are not copied yet.
 * dst   : the copy this frame fills. It is already attached to its own
 *         parent.
 * depth : the nesting depth of dst, so the cap below applies to exactly the
 *         same nodes that a recursive clone would apply it to.
 * taken : how many list elements the frame has copied so far. Only a list
 *         uses this.
 * next  : for a dictionary, the next member of src to copy, in insertion
 *         order. NULL once every member is copied, and when src is empty.
 *
 * Recursing once per level of nesting would make the peak native stack use
 * depend on the depth of the source tree, and for a tree built through the
 * public API the caller controls that depth and nothing bounds it.
 */
typedef struct {
  cjson_node_t *src;
  cjson_node_t *dst;
  unsigned int depth;
  size_t taken;
  const ccol_chmap_entry_ref *next;
} clone_frame_t;

/* Pushes one frame, growing the stack when it is full. It works the same way
 * as serialize_stack_push(), whose doc comment explains it. */
static bool clone_stack_push(clone_frame_t **frames,
                             clone_frame_t *inline_frames, size_t inline_cap,
                             size_t *cap, size_t *len,
                             const clone_frame_t *frame) {
  if (*len == *cap) {
    size_t new_cap = *cap * 2;
    clone_frame_t *grown;
    if (*frames == inline_frames) {
      grown = _ccol_default_mem_alloc(new_cap * sizeof(**frames));
      if (!grown) return false;
      memcpy(grown, inline_frames, inline_cap * sizeof(**frames));
    } else {
      grown = _ccol_default_mem_realloc(*frames, new_cap * sizeof(**frames));
      if (!grown) return false;
    }
    *frames = grown;
    *cap = new_cap;
  }
  (*frames)[(*len)++] = *frame;
  return true;
}

/* Copies the value of one node without its children: a scalar is copied
 * completely, and a container is created empty for the caller to fill.
 * Returns NULL when an allocation fails. */
static cjson_node_t *clone_shallow(cjson_node_t *src) {
  ccol_memmgmt_procs_t *mp = src->m_procs;
  switch (src->type) {
    case CJSON_NULL:
      return (cjson_node_t *)cjson_create_null_interned(mp);
    case CJSON_BOOL:
      return (cjson_node_t *)cjson_create_bool_interned(src->value.boolean, mp);
    case CJSON_INTEGER:
      return (cjson_node_t *)cjson_create_int_interned(src->value.integer, mp);
    case CJSON_FLOAT:
      return (cjson_node_t *)cjson_create_double_interned(src->value.number,
                                                          mp);
    case CJSON_STRING: {
      /* The source string is valid UTF-8 like every string of the DOM, so the
       * copy needs no check and just one strlen. */
      cjson_node_t *n = node_alloc(CJSON_STRING, mp);
      if (!n) return NULL;
      size_t slen = strlen(src->value.string);
      n->value.string = _ccol_mem_alloc(mp, slen + 1);
      if (!n->value.string) {
        node_free(n);
        return NULL;
      }
      memcpy(n->value.string, src->value.string, slen + 1);
      return n;
    }
    case CJSON_LIST:
      return (cjson_node_t *)cjson_create_list_interned(mp);
    case CJSON_DICTIONARY:
      return (cjson_node_t *)cjson_create_dictionary_interned(mp);
  }
  return NULL;
}

/*
 * Makes an independent deep copy of the subtree rooted at src, with the same
 * allocator (the m_procs of the source). Returns NULL when an allocation
 * fails, and also when the depth goes past CJSON_CLONE_MAX_DEPTH, and in both
 * cases any half-built copy is destroyed before returning.
 *
 * Each container is created empty, attached to its own parent, and only then
 * filled. That order is what lets the walk be a loop over an explicit stack
 * of frames, with no native frame per level of nesting.
 *
 * A child goes straight into its parent's container instead of through
 * cjson_list_push() or cjson_dictionary_set(). The parser does the same for
 * the same reason: every check those two run is already settled here. The
 * copy is new and unattached, so it can't contain its own parent; the
 * destination is a container this call has just built; and the key comes out
 * of the source map, so it differs from every key inserted so far and is
 * already valid UTF-8. The members of a source dictionary are copied in
 * insertion order, so the copy keeps its source's order.
 */
static cjson_node_t *clone_tree(cjson_node_t *src) {
  clone_frame_t inline_frames[32];
  clone_frame_t *frames = inline_frames;
  size_t cap = sizeof(inline_frames) / sizeof(inline_frames[0]);
  size_t len = 0;

  cjson_node_t *root = NULL;
  cjson_node_t *cur = src;
  cjson_node_t *parent_dst = NULL; /* the container for the copy of cur */
  const char *parent_key = NULL;   /* the key for it, in a dictionary only */
  size_t parent_key_size = 0;      /* the size of that key, '\0' included */
  unsigned int cur_depth = 0;

  for (;;) {
    /* cur_depth counts the containers around cur, so a container at cur_depth
     * sits at level cur_depth + 1. Only containers add levels. */
    if (cur_depth >= CJSON_CLONE_MAX_DEPTH &&
        (cur->type == CJSON_LIST || cur->type == CJSON_DICTIONARY))
      goto fail;

    cjson_node_t *copy = (cjson_node_t *)clone_shallow(cur);
    if (!copy) goto fail;

    if (!parent_dst) {
      root = copy;
    } else if (parent_key) {
      cmap_pair kp = {.ptr = (void *)parent_key, .size = parent_key_size};
      cmap_pair vp = {.ptr = &copy, .size = sizeof(copy)};
      chmap store = dict_store(parent_dst);
      if (!store || chmap_insert_elem(store, &kp, &vp) != ccol_success) {
        __cjson_destroy((cjson)copy);
        goto fail;
      }
      copy->attached = true;
    } else {
      cvec store = list_store(parent_dst);
      if (!store || cvector_push_back(store, &copy) != ccol_success) {
        __cjson_destroy((cjson)copy);
        goto fail;
      }
      copy->attached = true;
    }

    if (cur->type == CJSON_LIST || cur->type == CJSON_DICTIONARY) {
      clone_frame_t f = {.src = cur,
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

    /* Move on to the next source child of the innermost open container, and
     * pop that container once its copy is complete. */
    for (;;) {
      if (len == 0) goto done;
      clone_frame_t *f = &frames[len - 1];

      if (f->src->type == CJSON_LIST) {
        if (f->taken < list_len_of(f->src)) {
          cur = *(cjson_node_t **)cvector_at(f->src->value.list, f->taken);
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
        /* parent_key points at the key that the source map stores. The source
         * doesn't change during the clone, so the key stays valid for the
         * attach above. */
        const cmap_pair *kp, *vp;
        f->next = ccol_chmap_entry_read(f->next, &kp, &vp);
        cur = _cjson_read_child(vp->ptr);
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
  if (frames != inline_frames) _ccol_default_mem_free(frames);
  __cjson_destroy((cjson)root);
  return NULL;

done:
  if (frames != inline_frames) _ccol_default_mem_free(frames);
  return root;
}

/* Makes an independent deep copy of the whole subtree rooted at node (see the
 * doc comment of CJSON_CLONE_MAX_DEPTH above). A tree nested more than
 * CJSON_CLONE_MAX_DEPTH levels deep is reported like a failed allocation,
 * with a NULL, the same as every other failure of a clone. */
cjson cjson_clone(cjson node) {
  if (!node) return NULL;
  return (cjson)clone_tree((cjson_node_t *)node);
}

/* ========================================================================== */
/*                         PARSER                                             */
/* ========================================================================== */

/*
 * The state that every parse function of the recursive descent receives.
 *
 * src / pos / len : the source text and the current byte offset.
 * error           : a human-readable message that parse_err() fills. It only
 *                   means something after a parse function has returned NULL
 *                   or false.
 * mp              : the allocator for every node and every string.
 *
 * The parser covers the whole JSON grammar of RFC 8259 in one pass, with no
 * separate token stream: each sub-parser reads src directly through pos. It
 * walks the nesting with an explicit stack of frames instead of recursion
 * (see parse_frame_t below), so the depth of a document doesn't decide how
 * much native stack a parse needs.
 */
typedef struct {
  const char *src;
  size_t pos;
  size_t len;
  char error[512];
  ccol_memmgmt_procs_t *mp;
} parse_ctx_t;

/*
 * The deepest nesting that cjson_parse() and its siblings accept. A document
 * may hold this many '[' or '{' containers inside one another, with any value
 * (a scalar or an empty container) inside the innermost one; one more
 * container inside that is a parse error that names the limit. 500 covers any
 * real JSON document with plenty of room, whether a person or a machine
 * wrote it.
 *
 * It's a policy limit on the documents we accept and the native stack has
 * nothing to do with it: parse_value() walks the nesting with an explicit
 * stack of frames on the heap, so a document at the cap costs the same fixed,
 * small amount of native stack as a flat one.
 */
#define CJSON_MAX_PARSE_DEPTH 500

/* Formats a human-readable parse error into ctx->error. Only the message of
 * the last call stays; an earlier one is silently overwritten.
 *
 * The declaration carries __attribute__((format(printf, 2, 3))) and nothing
 * suppresses -Wformat-nonliteral at the vsnprintf call. Every call in this
 * file passes a string literal, so with the attribute the compiler checks the
 * format string of each call site against its arguments, and that catches a
 * real class of bug like a "%d" for a size_t. A suppression would only hide
 * the warning. */
static void parse_err(parse_ctx_t *ctx, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void parse_err(parse_ctx_t *ctx, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
  va_end(ap);
}

/* True when an input byte may appear as it is inside an error message. Every
 * message that quotes an input byte prints any other byte as 0xNN, so the
 * message stays printable ASCII, and therefore valid UTF-8, whatever the
 * document holds. A raw byte of 0x80 or above would make the message invalid
 * UTF-8, which a JSON or syslog log sink rejects or mangles, and a raw
 * control byte, or a NUL that cjson_parse_n() can carry, would corrupt or
 * truncate it. */
static bool parse_err_byte_is_printable(unsigned char b) {
  return b >= 0x20 && b <= 0x7E;
}

/* Moves ctx->pos past any JSON whitespace: space, tab, CR or LF. */
static void skip_ws(parse_ctx_t *ctx) {
  while (ctx->pos < ctx->len) {
    char c = ctx->src[ctx->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      ctx->pos++;
    else
      break;
  }
}

/* Skips whitespace and stores the next character in *out without consuming
 * it. Returns false at the end of the input. */
static bool peek(parse_ctx_t *ctx, char *out) {
  skip_ws(ctx);
  if (ctx->pos >= ctx->len) return false;
  *out = ctx->src[ctx->pos];
  return true;
}

/* Consumes the next non-whitespace character and returns true when it is the
 * expected one. */
static bool expect_char(parse_ctx_t *ctx, char expected) {
  skip_ws(ctx);
  if (ctx->pos >= ctx->len || ctx->src[ctx->pos] != expected) {
    parse_err(ctx, "expected '%c' at position %zu", expected, ctx->pos);
    return false;
  }
  ctx->pos++;
  return true;
}

/* null */
/* Consumes the literal "null" token and returns a CJSON_NULL node. */
static cjson_node_t *parse_null(parse_ctx_t *ctx) {
  if (ctx->pos + 4 > ctx->len || memcmp(ctx->src + ctx->pos, "null", 4) != 0) {
    parse_err(ctx, "expected 'null' at position %zu", ctx->pos);
    return NULL;
  }
  ctx->pos += 4;
  return node_alloc(CJSON_NULL, ctx->mp);
}

/* bool */
/* Consumes "true" or "false" and returns the matching CJSON_BOOL node. */
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

/* number */
/*
 * Parses a JSON number as RFC 8259 section 6 describes it.
 *
 * An integer has no decimal point and no exponent. An integer that fits in a
 * long long is stored as a CJSON_INTEGER and everything else becomes a
 * CJSON_FLOAT. A leading zero can't be followed by more digits. An integer
 * literal that overflows a long long falls back to a CJSON_FLOAT through
 * strtod, and a value that would give an infinite double is rejected.
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
    /* RFC 8259 sec. 6: no more digits are allowed after a leading zero. */
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
   * Builds a null-terminated token for strtoll or strtod. A 360-byte stack
   * buffer handles every real JSON number without allocating: it holds the
   * full decimal form of any IEEE 754 double, which is about 328 characters
   * at its longest, and also LLONG_MIN, which is 20. RFC 8259 sec. 6 puts no
   * limit on the length of a number literal, so a syntactically correct token
   * can be much longer, for example an ordinary finite value with lots of
   * unneeded leading or trailing zeros. We accept such a token instead of
   * rejecting it and allocate a heap buffer of exactly its size.
   */
  size_t tok_len = ctx->pos - start;
  char stack_tok[360];
  char *tok = stack_tok;
  char *heap_tok = NULL;
  if (tok_len >= sizeof(stack_tok)) {
    heap_tok = _ccol_mem_alloc(ctx->mp, tok_len + 1);
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
    /* Try an integer first, and fall back to a double when the value is out of
     * range. */
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
  _ccol_mem_free(ctx->mp, heap_tok);
  return result;
}

/* Encodes a Unicode code point as UTF-8 into an sbuf_t. */
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

/* Reads exactly 4 hex digits and gives the value of the code point. */
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

/* Checks that the raw bytes src[start..end) of a string are well-formed
 * UTF-8. On a defect we report it with parse_err(), naming the bytes as 0xNN
 * and the position of the first one, and return false. */
static bool string_bytes_are_utf8(parse_ctx_t *ctx, size_t start, size_t end) {
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

/*
 * The first pass over a JSON string literal. ctx->pos must be at the
 * opening '"'. On success ctx->pos moves past that quote, *end gets the
 * position of the closing '"', and we report whether the literal holds an
 * escape or a control character (*need_slow) and whether it holds a non-ASCII
 * byte (*has_non_ascii). An unterminated literal is a parse error.
 */
static bool parse_string_scan(parse_ctx_t *ctx, size_t *end,
                              bool *need_slow_out, bool *has_non_ascii_out) {
  if (ctx->pos >= ctx->len || ctx->src[ctx->pos] != '"') {
    parse_err(ctx, "expected '\"' at position %zu", ctx->pos);
    return false;
  }
  ctx->pos++; /* skip the opening quote */

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
      /* Only a non-ASCII byte can start an ill-formed UTF-8 sequence, so a
       * pure ASCII literal, the common case, skips the checks below
       * entirely. */
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

/*
 * The second pass over a JSON string literal that holds an escape or a
 * control character. It decodes from ctx->pos, one byte past the opening
 * quote, up to the closing '"' at scan, and stores the result in *out as a
 * heap C string allocated with ctx->mp, with its length in *out_len.
 */
static bool parse_string_slow(parse_ctx_t *ctx, size_t scan, bool has_non_ascii,
                              char **out, size_t *out_len) {
  sbuf_t sb;
  sb_init_hint(&sb, ctx->mp, scan - ctx->pos);

  while (ctx->pos < ctx->len) {
    /* Copy a run of plain characters in one go, up to the next special one. */
    size_t chunk_start = ctx->pos;
    while (ctx->pos < ctx->len) {
      unsigned char c2 = (unsigned char)ctx->src[ctx->pos];
      if (c2 == '"' || c2 == '\\' || c2 < 0x20) break;
      ctx->pos++;
    }
    if (ctx->pos > chunk_start) {
      /* Escapes and control characters are ASCII, so a run between two of them
       * never splits a multi-byte sequence, and checking the runs in order
       * reports the first defect of the string whatever kind it is. */
      if (has_non_ascii && !string_bytes_are_utf8(ctx, chunk_start, ctx->pos)) {
        _ccol_mem_free(sb.m_procs, sb.buf);
        return false;
      }
      sb_append(&sb, ctx->src + chunk_start, ctx->pos - chunk_start);
    }

    if (ctx->pos >= ctx->len) break;
    char c = ctx->src[ctx->pos];

    if (c == '"') {
      ctx->pos++;
      if (sb.oom) {
        _ccol_mem_free(sb.m_procs, sb.buf);
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
            _ccol_mem_free(sb.m_procs, sb.buf);
            return false;
          }
          /* A surrogate is only a code point as one half of a pair, a high
           * surrogate escape immediately followed by a low surrogate escape.
           * Any other surrogate escape names no Unicode scalar value, so we
           * refuse it instead of storing a replacement that would make two
           * different keys equal. */
          size_t esc_pos = ctx->pos - 6; /* the backslash of this escape */
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t low = 0;
            if (ctx->pos + 1 < ctx->len && ctx->src[ctx->pos] == '\\' &&
                ctx->src[ctx->pos + 1] == 'u') {
              ctx->pos += 2;
              if (!parse_hex4(ctx, &low)) {
                _ccol_mem_free(sb.m_procs, sb.buf);
                return false;
              }
            }
            if (low < 0xDC00 || low > 0xDFFF) {
              parse_err(ctx,
                        "high surrogate escape \\u%04X at position %zu is "
                        "not followed by a low surrogate escape",
                        (unsigned)cp, esc_pos);
              _ccol_mem_free(sb.m_procs, sb.buf);
              return false;
            }
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            parse_err(ctx,
                      "low surrogate escape \\u%04X at position %zu does not "
                      "follow a high surrogate escape",
                      (unsigned)cp, esc_pos);
            _ccol_mem_free(sb.m_procs, sb.buf);
            return false;
          }
          /* \u0000 gives a null byte, which a null-terminated C string can't
           * hold, so we reject it instead of silently cutting the string at
           * that null. */
          if (cp == 0) {
            parse_err(ctx, "\\u0000 not supported at position %zu", esc_pos);
            _ccol_mem_free(sb.m_procs, sb.buf);
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
          _ccol_mem_free(sb.m_procs, sb.buf);
          return false;
      }
    } else {
      /* A control character, i.e. a byte below 0x20. */
      parse_err(ctx,
                "unescaped control character 0x%02x in string at position %zu",
                (unsigned char)c, ctx->pos);
      _ccol_mem_free(sb.m_procs, sb.buf);
      return false;
    }
  }

  parse_err(ctx, "unterminated string starting before position %zu", ctx->pos);
  _ccol_mem_free(sb.m_procs, sb.buf);
  return false;
}

/*
 * Parses a JSON string from the opening '"' to the closing '"' and stores the
 * result in *out as a heap C string allocated with ctx->mp. Raw bytes that
 * aren't well-formed UTF-8, and a \uXXXX escape for a surrogate that isn't one
 * half of a high-low pair, are parse errors.
 */
static bool parse_string_raw(parse_ctx_t *ctx, char **out) {
  *out = NULL;
  size_t scan;
  bool need_slow, has_non_ascii;
  if (!parse_string_scan(ctx, &scan, &need_slow, &has_non_ascii)) return false;

  if (!need_slow) {
    /* The fast path. With no escape and no control character the string is its
     * raw bytes, so once we know those are well-formed UTF-8 one allocation
     * and one memcpy are enough. */
    size_t slen = scan - ctx->pos;
    if (has_non_ascii && !string_bytes_are_utf8(ctx, ctx->pos, scan))
      return false;
    char *s = _ccol_mem_alloc(ctx->mp, slen + 1);
    if (!s) return false;
    memcpy(s, ctx->src + ctx->pos, slen);
    s[slen] = '\0';
    ctx->pos = scan + 1; /* move past the closing '"' */
    *out = s;
    return true;
  }

  size_t slen;
  return parse_string_slow(ctx, scan, has_non_ascii, out, &slen);
}

/*
 * The keys of the dictionaries that parse_value() has open, one after the
 * other, each with its terminating '\0'. A key only stays here while its
 * value is being parsed, and the values of dictionaries nested inside that
 * value end before it does, so keys come and go in LIFO order and one buffer
 * can hold all of them. The buffer starts on the stack, so a document whose
 * open keys fit in it allocates nothing for keys at all; a longer total moves
 * it to the heap of ctx->mp, where it grows by doubling.
 */
#define CJSON_PARSE_KEY_INLINE 256
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  char inline_buf[CJSON_PARSE_KEY_INLINE];
} parse_keys_t;

/* Makes room for n more bytes at the end of the key buffer. */
static bool parse_keys_reserve(parse_ctx_t *ctx, parse_keys_t *keys, size_t n) {
  if (n <= keys->cap - keys->len) return true;
  if (n > SIZE_MAX / 2 - keys->len) return false;
  size_t new_cap = keys->cap * 2;
  if (new_cap < keys->len + n) new_cap = keys->len + n;
  char *grown;
  if (keys->buf == keys->inline_buf) {
    grown = _ccol_mem_alloc(ctx->mp, new_cap);
    if (!grown) return false;
    memcpy(grown, keys->inline_buf, keys->len);
  } else {
    grown = _ccol_mem_realloc(ctx->mp, keys->buf, new_cap);
    if (!grown) return false;
  }
  keys->buf = grown;
  keys->cap = new_cap;
  return true;
}

/*
 * Parses a JSON string that is a dictionary key and pushes it, '\0'
 * included, onto the key buffer. *off gets the key's offset in the buffer and
 * *klen its length without the '\0'. A key with no escape is copied straight
 * from the input, and one with an escape is first decoded on the heap and
 * then copied.
 */
static bool parse_key(parse_ctx_t *ctx, parse_keys_t *keys, size_t *off,
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
    _ccol_mem_free(ctx->mp, decoded);
  } else {
    ctx->pos = scan + 1; /* move past the closing '"' */
  }
  return ok;
}

/* A wrapper around parse_string_raw(): it parses a JSON string literal and
 * returns a CJSON_STRING node that owns the heap C string. */
static cjson_node_t *parse_string(parse_ctx_t *ctx) {
  char *s = NULL;
  if (!parse_string_raw(ctx, &s)) return NULL;
  cjson_node_t *n = node_alloc(CJSON_STRING, ctx->mp);
  if (!n) {
    _ccol_mem_free(ctx->mp, s);
    return NULL;
  }
  n->value.string = s;
  return n;
}

/* container nesting */
/*
 * One open '[' or '{' on the explicit stack of parse_value().
 *
 * node        : the CJSON_LIST or CJSON_DICTIONARY being filled.
 * key_off     : for a dictionary, the offset in parse_value()'s key buffer of
 *               the key whose value is being read right now.
 * key_len     : the length of that key, without its '\0'.
 * is_dict     : which of the two kinds of container node is, so the messages
 *               about a separator or an unterminated container name the
 *               right one.
 * started     : false until the first element or member of this container
 *               has been consumed. It tells "an element can follow the
 *               opening bracket" apart from "a ',' must come between this
 *               element and the previous one".
 *
 * A recursive descent costs one native frame per level of nesting, or three
 * for the parse_value, dispatch and container chain that the grammar gives,
 * so the stack a parse needs would depend on the nesting depth of the input,
 * which whoever supplies the document gets to choose. At the cap above such
 * a recursion runs past a small thread stack, like musl's 128 KiB default,
 * and crashes the process instead of returning the documented parse error.
 * The explicit stack is a little more code than a recursive descent, and for
 * input that a stranger can send it's worth it.
 */
typedef struct {
  cjson_node_t *node;
  size_t key_off;
  size_t key_len;
  bool is_dict;
  bool started;
} parse_frame_t;

/* Pushes one frame, growing the stack when it is full. It works the same way
 * as serialize_stack_push(), whose doc comment explains it. */
static bool parse_stack_push(parse_frame_t **frames,
                             parse_frame_t *inline_frames, size_t inline_cap,
                             size_t *cap, size_t *len,
                             const parse_frame_t *frame) {
  if (*len == *cap) {
    size_t new_cap = *cap * 2;
    parse_frame_t *grown;
    if (*frames == inline_frames) {
      grown = _ccol_default_mem_alloc(new_cap * sizeof(**frames));
      if (!grown) return false;
      memcpy(grown, inline_frames, inline_cap * sizeof(**frames));
    } else {
      grown = _ccol_default_mem_realloc(*frames, new_cap * sizeof(**frames));
      if (!grown) return false;
    }
    *frames = grown;
    *cap = new_cap;
  }
  (*frames)[(*len)++] = *frame;
  return true;
}

/* value dispatch */
/* Branches on the first character of a value that isn't a container and
 * calls the right sub-parser. c is the character at ctx->pos that the
 * caller's peek() already found. A '[' or '{' never gets here because
 * parse_value() takes them down its own iterative path. */
static cjson_node_t *parse_scalar_value(parse_ctx_t *ctx, char c) {
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

/*
 * Parses one complete JSON value, with every value nested inside it, and
 * returns its node. Returns NULL on any failure, and then ctx->error carries
 * the message and every node built so far has been freed.
 *
 * The nesting is walked with the explicit stack of frames above instead of
 * recursion, so the native stack we need is the same for a document at
 * CJSON_MAX_PARSE_DEPTH as for a flat one. The walk has four steps and the
 * structure of the document moves us between them: parse a value; hand a
 * complete value to the container waiting for it; decide what comes next
 * inside the innermost open container; and read a dictionary key.
 *
 * A dictionary key that appears twice silently overwrites the earlier one,
 * so the last writer wins, following the permissive guidance in RFC 8259
 * section 4.
 */
static cjson_node_t *parse_value(parse_ctx_t *ctx) {
  parse_frame_t inline_frames[32];
  parse_frame_t *frames = inline_frames;
  size_t cap = sizeof(inline_frames) / sizeof(inline_frames[0]);
  size_t len = 0;
  cjson_node_t *pending = NULL;
  char c;
  parse_keys_t keys;
  keys.buf = keys.inline_buf;
  keys.len = 0;
  keys.cap = sizeof(keys.inline_buf);

value_step:
  if (!peek(ctx, &c)) {
    parse_err(ctx, "unexpected end of input at position %zu", ctx->pos);
    goto fail;
  }
  if (c == '[' || c == '{') {
    /* len is the number of containers open around this one, so this one is
     * level len + 1. The cap only counts containers, and a scalar inside the
     * deepest accepted container adds no level. */
    if (len >= CJSON_MAX_PARSE_DEPTH) {
      parse_err(ctx, "maximum nesting depth (%u) exceeded at position %zu",
                CJSON_MAX_PARSE_DEPTH, ctx->pos);
      goto fail;
    }
    parse_frame_t f = {.node = NULL,
                       .key_off = 0,
                       .key_len = 0,
                       .is_dict = (c == '{'),
                       .started = false};
    ctx->pos++; /* consume the opening bracket that peek() found */
    f.node = f.is_dict
                 ? (cjson_node_t *)cjson_create_dictionary_interned(ctx->mp)
                 : (cjson_node_t *)cjson_create_list_interned(ctx->mp);
    if (!f.node) goto fail;
    if (!parse_stack_push(&frames, inline_frames,
                          sizeof(inline_frames) / sizeof(*inline_frames), &cap,
                          &len, &f)) {
      /* No surrounding container owns this node yet, so we must still free
       * it here. */
      __cjson_destroy((cjson)f.node);
      goto fail;
    }
    goto next_step;
  }
  pending = parse_scalar_value(ctx, c);
  if (!pending) goto fail;

deliver_step:
  /* pending is a complete value: the root itself when no container holds it,
   * and otherwise the next element or member of the innermost open
   * container. */
  if (len == 0) goto done;
  {
    parse_frame_t *f = &frames[len - 1];
    if (f->is_dict) {
      cmap_pair kp = {.ptr = keys.buf + f->key_off, .size = f->key_len + 1};

      /* One hash and one probe either insert the member or hand back the slot
       * of a duplicate key. A duplicate keeps the last value, so the old child
       * is freed and the slot then points at the new one. */
      cmap_pair vp = {.ptr = &pending, .size = sizeof(pending)};
      cmap_pair existing_vp;
      chmap store = dict_store(f->node);
      ccol_retval_t r =
          store ? ccol_chmap_insert_or_get_elem(store, &kp, &vp, &existing_vp)
                : ccol_not_enough_memory;
      keys.len = f->key_off; /* the map holds its own copy of the key */
      if (r == ccol_key_already_present) {
        dict_slot_replace_child(&existing_vp, pending);
      } else if (r != ccol_success) {
        __cjson_destroy((cjson)pending);
        goto fail;
      } else {
        pending->attached = true;
      }
    } else {
      cvec store = list_store(f->node);
      if (!store || cvector_push_back(store, &pending) != ccol_success) {
        __cjson_destroy((cjson)pending);
        goto fail;
      }
      pending->attached = true;
    }
  }

next_step:
  /* Decide what comes next inside the innermost open container: its closing
   * bracket, or a separator and one more element or member. */
  {
    parse_frame_t *f = &frames[len - 1];
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
          parse_err(ctx, "expected ',' or ']' in list at position %zu",
                    ctx->pos);
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

key_step:
  /* A dictionary member: the key string, then a ':', then the value. */
  {
    parse_frame_t *f = &frames[len - 1];
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
  /* Every frame on the stack fully owns its own container. A container only
   * goes to its parent once its closing bracket has been consumed, and its
   * frame is gone by then, so nothing else can reach a container that is
   * still open here. Freeing just the outermost one would leak every
   * container nested inside it along with everything already put into them. */
  for (size_t i = 0; i < len; i++) {
    __cjson_destroy((cjson)frames[i].node);
  }
  pending = NULL;

done:
  if (frames != inline_frames) _ccol_default_mem_free(frames);
  if (keys.buf != keys.inline_buf) _ccol_mem_free(ctx->mp, keys.buf);
  return pending;
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
 * library's thread-local block, which the loader needs to fit into its small
 * reserve when a process loads the library with dlopen(). When the
 * allocation fails, or the thread can't arm the key whose destructor would
 * free the buffer, *err_str points at a fixed message instead, which the
 * caller doesn't free either.
 *
 * 512 bytes is the size of parse_ctx_t.error, where every message of this
 * parser is built, so nothing longer than that can arrive here.
 */
#define CJSON_ERR_BUF_LEN 512
static const char cjson_err_unstored[] =
    "parse failed; the error message could not be stored";

/* Copies msg into the per-thread buffer and points *err_str at it. Does
 * nothing when the caller passed no err_str. */
static void cjson_report_err(char **err_str, const char *msg) {
  if (!err_str) return;
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  _ccol_call_once(_pool_key_once, _do_pool_key_init);
  _cjson_tls_t *t = _cjson_tls_get();
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
    _ccol_call_once(_pool_key_once, _do_pool_key_init);
    _pool_key_arm();
    /* Only the thread-exit drain of an armed key frees the buffer, so a
     * thread whose key can't be armed keeps no buffer at all. */
    if (!_pool_key_armed) {
      free(buf);
      *err_str = (char *)cjson_err_unstored;
      return;
    }
    cjson_err_buf = buf;
  }
  snprintf(cjson_err_buf, CJSON_ERR_BUF_LEN, "%s", msg);
  *err_str = cjson_err_buf;
#endif /* _CCOL_EMULATE_DARWIN_TLS */
}

/*
 * The implementation that every public entry point of the parser shares.
 *
 * It initializes the parse context, runs the descent from parse_value(), and
 * then checks that nothing meaningful follows the root value, so trailing
 * garbage is rejected.
 *
 * On failure *err_str gets a human-readable error message when err_str isn't
 * NULL. The library owns that string and the caller never frees it (see
 * cjson_report_err() above). On success *err_str is set to NULL.
 */
static cjson parse_common(const char *src, size_t len, char **err_str,
                          ccol_memmgmt_procs_t *mp) {
  if (!src) {
    cjson_report_err(err_str, "null input");
    return NULL;
  }
  if (!cjson_intern_procs(&mp)) {
    cjson_report_err(err_str,
                     "too many distinct allocator procs in this process");
    return NULL;
  }
  parse_ctx_t ctx = {.src = src, .pos = 0, .len = len, .error = "", .mp = mp};
  cjson_node_t *root = parse_value(&ctx);
  if (!root) {
    if (err_str) {
      /* Every real syntax rejection in this parser reports its own message with
       * parse_err() before returning a failure, so only one thing can get
       * here with ctx.error still empty: an allocation failure that has no
       * place of its own to report through. It comes from node_alloc(), a
       * raw _ccol_mem_alloc(), or a failed insert into a cvector or a chmap.
       * We say so plainly, because an "unknown parse error" message would
       * sound alarming and send the reader looking for a malformed document
       * when the real problem is memory pressure. */
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
  /* Make sure nothing meaningful follows the root value. */
  skip_ws(&ctx);
  if (ctx.pos != ctx.len) {
    if (err_str) {
      char buf[64];
      snprintf(buf, sizeof(buf), "trailing garbage at position %zu", ctx.pos);
      cjson_report_err(err_str, buf);
    }
    __cjson_destroy((cjson)root);
    return NULL;
  }
  if (err_str) *err_str = NULL;
  return (cjson)root;
}

/* Parses a null-terminated JSON string. mp can be NULL for the default
 * allocator. On failure, when err_str isn't NULL, *err_str points at a
 * per-thread message that the library owns; the caller never frees it, and
 * it stays valid until the next failing parse on the same thread. */
cjson cjson_parse_mp(const char *json_str, char **err_str,
                     ccol_memmgmt_procs_t *mp) {
  if (!json_str) return parse_common(NULL, 0, err_str, mp);
  return parse_common(json_str, strlen(json_str), err_str, mp);
}

/* Like cjson_parse_mp, but with an explicit byte length, so the input need
 * not be null-terminated. Use it for a JSON value that sits inside a
 * larger buffer. */
cjson cjson_parse_n_mp(const char *json_str, size_t len, char **err_str,
                       ccol_memmgmt_procs_t *mp) {
  return parse_common(json_str, len, err_str, mp);
}

/* ========================================================================== */
/*                         SERIALIZER                                         */
/* ========================================================================== */

/*
 * Formats a double into buf in the shortest decimal form that parses back to
 * the same value.
 */
static void format_double(char *buf, size_t cap, double val) {
  if (!isfinite(val)) {
    snprintf(buf, cap, "null"); /* JSON has no Inf and no NaN */
    return;
  }
  _cjson_format_shortest_c(buf, cap, val);
  /* Make sure the output looks like a floating-point literal, so parsing it
   * gives a CJSON_FLOAT and not a CJSON_INTEGER. The %.Ng conversion drops
   * the decimal point of a whole number and turns 1.0 into "1", which would
   * parse back as a CJSON_INTEGER, so we add ".0". The longest case this
   * touches is a signed 17-digit whole number, which "%.17g" writes without
   * an exponent: 18 bytes plus ".0\0" makes 21, which fits easily in the
   * caller's 32-byte buf. */
  if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
    size_t len = strlen(buf);
    if (len + 2 < cap) {
      buf[len] = '.';
      buf[len + 1] = '0';
      buf[len + 2] = '\0';
    }
  }
}

/* Writes a newline and then (depth * indent) spaces, for indented output.
 * Does nothing when indent == 0, which is the compact mode. It appends from a
 * constant block of spaces, one block at a time, so a line costs one append
 * per 128 bytes of indentation instead of one per byte. A width that a size_t
 * can't hold is reported through the oom flag, like every other serializer
 * failure. */
static void sb_append_indent(sbuf_t *sb, unsigned int indent,
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

/* Appends a string value with JSON escapes and a quote on each side. */
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
 * Converts a long long to a decimal string, which is cheaper than snprintf.
 * buf must hold at least 21 bytes.
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
 * The deepest nesting that serialize_node() below accepts, counted in
 * containers exactly like CJSON_MAX_PARSE_DEPTH. A tree given to
 * cjson_serialize() or cjson_serialize_pretty() doesn't have to come from
 * cjson_parse(), because the public cjson_list_push() and
 * cjson_dictionary_set() can build one directly to any depth, so
 * CJSON_MAX_PARSE_DEPTH, which only bounds the parser, says nothing about it.
 * This one has the same value of 500, so a tree exactly at the parser's limit
 * serializes instead of failing for no good reason.
 *
 * It's a policy limit and has nothing to do with the stack: serialize_node()
 * walks the tree with an explicit stack of frames on the heap and uses a
 * fixed, small amount of native stack at any depth (see serialize_frame_t
 * below).
 */
#define CJSON_MAX_SERIALIZE_DEPTH 500

/*
 * One open container on the explicit stack of serialize_node().
 *
 * node   : the CJSON_LIST or CJSON_DICTIONARY whose children we are writing.
 * depth  : the nesting depth of node, so the indent of its children and of
 *          its own closing bracket come out exactly as from the matching
 *          recursive call.
 * emitted: how many children have been written so far. It drives the commas
 *          between children and also the "did this container hold anything"
 *          test that decides the closing indent.
 * count  : the element count of a list, read when the frame is pushed. A
 *          dictionary frame leaves it 0.
 * next   : for a dictionary, the next member to write, in insertion order.
 *          NULL once every member is written, and when the dictionary is
 *          empty.
 * is_dict: which of the two kinds of container node is. The frame keeps it so
 *          the per-child step reads it without following the node
 *          pointer.
 *
 * Recursing once per level of nesting would make the peak native stack use
 * depend on the depth of the tree, and for a tree built through the public
 * API the caller controls that depth and nothing bounds it. At 500 levels
 * such a recursion runs right past a small thread stack, such as musl's
 * 128 KiB default, so the depth cap above would only hold on a stack big
 * enough to get there.
 */
typedef struct {
  cjson_node_t *node;
  size_t emitted;
  size_t count;
  const ccol_chmap_entry_ref *next;
  unsigned int depth;
  bool is_dict;
} serialize_frame_t;

/*
 * Pushes one frame, growing the stack when it is full. Returns false when the
 * allocation fails and leaves the stack exactly as it was then.
 *
 * inline_frames is the caller's small array on its own stack, used until a
 * tree outgrows it: *frames points at it until the first growth, so an
 * ordinary document serializes without any scratch allocation.
 */
static bool serialize_stack_push(serialize_frame_t **frames,
                                 serialize_frame_t *inline_frames,
                                 size_t inline_cap, size_t *cap, size_t *len,
                                 const serialize_frame_t *frame) {
  if (*len == *cap) {
    size_t new_cap = *cap * 2;
    serialize_frame_t *grown;
    if (*frames == inline_frames) {
      grown = _ccol_default_mem_alloc(new_cap * sizeof(**frames));
      if (!grown) return false;
      memcpy(grown, inline_frames, inline_cap * sizeof(**frames));
    } else {
      grown = _ccol_default_mem_realloc(*frames, new_cap * sizeof(**frames));
      if (!grown) return false;
    }
    *frames = grown;
    *cap = new_cap;
  }
  (*frames)[(*len)++] = *frame;
  return true;
}

/* Writes the JSON text of one non-container node, including a NULL pointer,
 * which comes out as the literal "null". */
static void serialize_scalar(sbuf_t *sb, cjson_node_t *n) {
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
    case CJSON_NULL:
    case CJSON_LIST:
    case CJSON_DICTIONARY:
      /* CJSON_NULL is the literal. The two container kinds never get here
       * because serialize_node() takes them down its own path. We list the
       * three cases one by one instead of folding them into a default, so
       * that -Wswitch reports a new node kind that nothing here handles. */
      sb_append_cstr(sb, "null");
      break;
  }
}

/*
 * Writes the JSON text of the subtree rooted at n into sb.
 * indent: spaces per indent level. 0 gives compact output with no added
 *         whitespace.
 * depth:  the nesting depth of n itself. The top-level caller passes 0.
 *
 * It's iterative, with an explicit stack of frames in place of recursion, so
 * the native stack use doesn't depend on the depth of the tree (see the doc
 * comment of serialize_frame_t). Every failure, the depth cap and a failed
 * allocation of the scratch stack included, goes through sb->oom, and
 * cjson_serialize() and cjson_serialize_pretty() already turn that one flag
 * into the NULL that their documentation promises.
 */
static void serialize_node(sbuf_t *sb, cjson_node_t *n, unsigned int indent,
                           unsigned int depth) {
  serialize_frame_t inline_frames[32];
  serialize_frame_t *frames = inline_frames;
  size_t cap = sizeof(inline_frames) / sizeof(inline_frames[0]);
  size_t len = 0;

  cjson_node_t *cur = n;
  unsigned int cur_depth = depth;

  for (;;) {
    /* Write the value at cur, whose nesting depth is cur_depth. */
    if (sb->oom) goto done;
    if (cur && (cur->type == CJSON_LIST || cur->type == CJSON_DICTIONARY)) {
      if (cur_depth >= CJSON_MAX_SERIALIZE_DEPTH) {
        /* See the doc comment of CJSON_MAX_SERIALIZE_DEPTH. cur_depth counts
         * the containers around cur, so this container is level
         * cur_depth + 1, one past the cap. We refuse the tree and report it
         * through the same oom flag as every other serializer failure. */
        sb->oom = true;
        goto done;
      }
      serialize_frame_t f = {.node = cur,
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

    /* Move on: write the next child of the innermost open container, or close
     * that container and carry on with its parent. */
    for (;;) {
      if (len == 0) goto done;
      serialize_frame_t *f = &frames[len - 1];

      if (!f->is_dict) {
        if (f->emitted < f->count) {
          if (f->emitted > 0) sb_append_c(sb, ',');
          if (indent) sb_append_indent(sb, indent, f->depth + 1);
          cur = *(cjson_node_t **)cvector_at(f->node->value.list, f->emitted);
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
        cur = _cjson_read_child(vp->ptr);
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
  if (frames != inline_frames) _ccol_default_mem_free(frames);
}

/* Serializes the node to compact JSON with no added whitespace. Returns a
 * heap string, which you free with cjson_serialize_free() or
 * cjson_serialize_free_mp(), or NULL when an allocation fails. */
char *cjson_serialize(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  sbuf_t sb;
  sb_init(&sb, mp);
  serialize_node(&sb, n, 0, 0);
  if (sb.oom) {
    _ccol_mem_free(sb.m_procs, sb.buf);
    return NULL;
  }
  return sb.buf;
}

/* Serializes the node to indented JSON. indent is the number of spaces per
 * nesting level, and 0 falls back to 4. Free the result with
 * cjson_serialize_free() or cjson_serialize_free_mp(). */
char *cjson_serialize_pretty(cjson node, unsigned int indent) {
  cjson_node_t *n = (cjson_node_t *)node;
  ccol_memmgmt_procs_t *mp = n ? n->m_procs : NULL;
  sbuf_t sb;
  sb_init(&sb, mp);
  serialize_node(&sb, n, indent ? indent : 4, 0);
  if (sb.oom) {
    _ccol_mem_free(sb.m_procs, sb.buf);
    return NULL;
  }
  return sb.buf;
}

/* Frees a string returned by cjson_serialize or cjson_serialize_pretty, with
 * the allocator that made it. A NULL mp means the default allocator.
 */
void cjson_serialize_free_mp(char *s, ccol_memmgmt_procs_t *mp) {
  _ccol_mem_free(mp, s);
}

/* ========================================================================== */
/*                         PATH NAVIGATION                                    */
/* ========================================================================== */

/*
 * The escape sequences of a path, used by every navigate and set helper
 * below.
 *
 *   \.   -> a literal '.' in the key, and not a separator of the path
 *   \\   -> a literal '\' in the key
 *
 * A '\' before any other character is left as it is and passes through. The
 * escapes of a component are resolved after the path has been split on the
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
 * keeps its length, never longer. */
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
 * Strictly parses the decimal digits after the '#' of a "#N" path component,
 * which holds a list index. digits points one byte past the '#'. Returns true
 * and sets *out on success. Returns false, with *out unspecified, when the
 * index isn't a number, when it carries a sign, or when the component is a
 * bare '#' with no digits.
 *
 * The grammar is one or more ASCII digits and nothing else, so whitespace, a
 * '+' or '-' sign and a bare '#' are all malformed. We read the digits by hand
 * instead of with strtol(), which accepts leading whitespace and a sign and
 * whose range depends on the width of long. Any number of digits is a
 * well-formed index: a value too big for a size_t saturates at SIZE_MAX,
 * which no list can reach, so such an index is "not found"
 * (ccol_key_not_found) and never a syntax error, on every target width.
 */
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

/*
 * Walks a dot-separated path through a JSON tree and returns the node at its
 * end, or NULL when a component is absent.
 *
 * path_copy must be a writable copy of the path string. Each unescaped '.' is
 * temporarily replaced with a '\0', which carves out each component in place,
 * and the component's escapes are resolved before it is used as a key.
 *
 * A leading '#' addresses a list element, so "items.#0.name" goes to the
 * 'name' key of the first element of 'items'. Two dots in a row and a
 * trailing dot both give NULL.
 *
 * When invalid isn't NULL, *invalid is set to true when ANY component of the
 * path is a syntax error, wherever it sits and whether or not an earlier
 * component resolved. Three things are syntax errors: an empty component
 * (two dots in a row as in "a..b", or a trailing dot); a component that isn't
 * well-formed UTF-8, which no DOM key can ever match; and a component that
 * starts with '#' on a list but isn't '#' followed by digits only (a bare '#',
 * a sign, or anything other than a digit). The flag is also set when the path
 * has a shape that can't apply to the tree: a component that would have to
 * descend into a scalar (a string, a number, a bool or null), or a component
 * that isn't '#N' on a list. The flag stays false for the only other reason
 * we return NULL, a well-formed name or index that is absent, and it is
 * never set on success.
 *
 * With this flag a caller can tell ccol_invalid_args (an error in the path,
 * of syntax or of shape) from ccol_key_not_found (a syntactically correct
 * component that is absent), and classify a path error the same way at any
 * position. We scan the whole path even after an earlier well-formed
 * component fails to resolve and cur becomes NULL, so a real syntax error
 * never hides behind a "not found" for some earlier, unrelated component.
 * Once cur is NULL we only skip the dictionary and list lookups, since
 * there's nothing left to look further components up in; a '#' component
 * there names no list, so only the UTF-8 test applies to it. A component
 * that resolves is well-formed UTF-8 by construction, because it matched a
 * DOM key or it is a list index of ASCII digits, so the UTF-8 test only runs
 * for a component that doesn't resolve and costs nothing on a successful
 * lookup.
 */
static cjson navigate(cjson root, char *path_copy, bool *invalid) {
  if (invalid) *invalid = false;
  cjson cur = root;
  char *p = path_copy;

  while (1) {
    char *dot = path_find_unescaped_dot(p);
    if (dot) *dot = '\0';

    /* An empty component, from two dots in a row as in "a..b" or a trailing
     * dot as in "a.". This check always runs; the doc comment of this
     * function above explains why nothing may skip it once cur is already
     * NULL. */
    if (p[0] == '\0') {
      if (invalid) *invalid = true;
      cur = NULL;
      break;
    }

    path_unescape_component(p);

    if (cur) {
      cjson_node_t *n = (cjson_node_t *)cur;

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
          cur = _cjson_read_child(vp.ptr);
      } else {
        /* The node is a scalar, or a list and the component isn't a '#N'
         * index, so the path has a shape that can't apply to this tree. We
         * count that as an error in the path, like a malformed component,
         * rather than as an absent component. */
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

/* The implementation behind the public cjson_get(root, path) macro. It copies
 * the path into a writable buffer before handing it to navigate(), so the
 * caller's string is never changed. A path shorter than CJSON_GET_STACK_PATH
 * goes into a stack buffer, so a lookup allocates nothing and can't fail for
 * lack of memory; only a longer path is copied to the heap. */
#define CJSON_GET_STACK_PATH 256
cjson _cjson_get(cjson root, const char *path) {
  if (!root) return NULL;
  if (!path || path[0] == '\0') return root;

  ccol_memmgmt_procs_t *mp = ((cjson_node_t *)root)->m_procs;
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
  if (copy != stack_copy) _ccol_mem_free(mp, copy);
  return result;
}

/*
 * The implementation behind the public cjson_set(root, path, value) macro.
 *
 * The path is split on its LAST dot, which separates the parent path from
 * the leaf key. The leaf string is copied before the copy of the parent path
 * is freed; in the other order, a leaf that is a direct suffix of the whole
 * path gives a use-after-free.
 *
 * For CJSON_STRING, raw always names a pointer to the string. The cjson_set()
 * macro copies its argument into a local of the decayed, unqualified type, so
 * a string literal and a char array both arrive as a pointer to their first
 * character.
 *
 * When the leaf key already exists, that node is changed in place with
 * node_reinit_scalar_checked(); otherwise a new scalar node is allocated and
 * inserted. No intermediate container is ever created automatically, so the
 * parent must already exist.
 *
 * Returns:
 *   ccol_success           - the leaf was created or updated.
 *   ccol_invalid_args      - a NULL root or path. An empty path, or an
 *                            empty path component from a leading dot, a
 *                            trailing dot, or two dots in a row. A path
 *                            whose shape can't apply to the tree, at any
 *                            position: a component that would have to
 *                            descend into or below a scalar, or a component
 *                            without a '#N' on a list. A malformed '#N'
 *                            index (not a number, negative, or a bare '#').
 *                            Or a C type of val that cjson_set() doesn't
 *                            accept.
 *   ccol_key_not_found     - a well-formed name or index on the parent path
 *                            is absent. Also returned for a list index of
 *                            the leaf whose syntax is correct but which is
 *                            out of range. A list can't grow itself to fit
 *                            an arbitrary index the way a dictionary can, so
 *                            this stays an error and the leaf isn't created.
 *   ccol_not_enough_memory - an allocation failed.
 */
ccol_retval_t _cjson_set_typed(cjson root, const char *path,
                               cjson_node_type_t type, void *raw,
                               size_t raw_size, bool is_signed) {
  if (!root || !path || path[0] == '\0') return ccol_invalid_args;

  ccol_memmgmt_procs_t *mp = ((cjson_node_t *)root)->m_procs;

  char *copy = ccol_strdup(mp, path);
  if (!copy) return ccol_not_enough_memory;

  char *last_dot = path_find_last_unescaped_dot(copy);
  char *leaf_copy;

  if (!last_dot) {
    /* No dot at all, and copy already holds a byte-for-byte copy of path, so
     * we reuse it as leaf_copy instead of paying for a second copy of the
     * same string. */
    leaf_copy = copy;
  } else {
    *last_dot = '\0';
    if (copy[0] == '\0') {
      /* A leading dot, so the parent path is empty, which is a syntax
       * error. */
      _ccol_mem_free(mp, copy);
      return ccol_invalid_args;
    }
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    if (!leaf_copy) {
      _ccol_mem_free(mp, copy);
      return ccol_not_enough_memory;
    }
  }

  /* Check the leaf component and resolve its escapes BEFORE walking the
   * parent path. An empty leaf comes from a trailing dot on the whole path,
   * as in "a.", and it is always a syntax error that has to be reported as
   * ccol_invalid_args, even when the parent path doesn't resolve, as in
   * "missing." where "missing" doesn't exist. A parent that really is absent
   * must never hide a syntax error in the leaf behind a ccol_key_not_found. */
  path_unescape_component(leaf_copy);
  /* The leaf is empty or isn't valid UTF-8. This call may have to CREATE a
   * dictionary key, and that key must be one the DOM can hold, since every
   * key in the DOM is valid UTF-8 (see the UTF-8 section of this file).
   * Without this check an ill-formed component would be stored as a key and
   * the serializer would write it raw. _cjson_get() and _cjson_delete() need
   * no such check because they only look a component up, and an ill-formed
   * component can never match an existing key. */
  if (leaf_copy[0] == '\0' || !utf8_cstr_is_valid(leaf_copy)) {
    if (leaf_copy != copy) _ccol_mem_free(mp, leaf_copy);
    _ccol_mem_free(mp, copy);
    return ccol_invalid_args;
  }
  const char *leaf_comp = leaf_copy;

  cjson parent;
  if (!last_dot) {
    parent = root;
  } else {
    bool invalid = false;
    parent = navigate(root, copy, &invalid);
    _ccol_mem_free(mp, copy);
    if (!parent) {
      _ccol_mem_free(mp, leaf_copy);
      /* An error in the parent path (an empty component as in "a..b", a
       * component that isn't valid UTF-8, a malformed "#N" on a list, a
       * component that would have to descend into a scalar, or a name that
       * isn't "#N" on a list) is classified the same way as an error in the
       * leaf component, and not lumped together with an ordinary well-formed
       * component that is simply absent. */
      return invalid ? ccol_invalid_args : ccol_key_not_found;
    }
  }

  cjson_node_t *pn = (cjson_node_t *)parent;
  ccol_retval_t ret;

  if (pn->type == CJSON_DICTIONARY) {
    cmap_pair kp = {.ptr = (void *)leaf_comp, .size = strlen(leaf_comp) + 1};
    /* Refuse a bad payload before the map changes at all, so a refusal never
     * needs to undo an insert. */
    ret = scalar_payload_check(type, raw, raw_size);
    if (ret == ccol_success) {
      /* One hash and one probe either find the leaf or insert the key with a
       * NULL placeholder child. An existing leaf is updated in place, so a
       * borrowed handle to it stays valid. For a new key, the only thing that
       * runs between the insert and writing the real child is building that
       * child, and building it reads no map of this tree. */
      cjson_node_t *placeholder = NULL;
      cmap_pair vp = {.ptr = &placeholder, .size = sizeof(placeholder)};
      cmap_pair slot;
      chmap store = dict_store(pn);
      ccol_retval_t r =
          store ? ccol_chmap_insert_or_get_elem(store, &kp, &vp, &slot)
                : ccol_not_enough_memory;
      if (r == ccol_key_already_present) {
        cjson_node_t *existing = _cjson_read_child(slot.ptr);
        ret = node_reinit_scalar_checked(existing, type, raw, raw_size,
                                         is_signed);
      } else if (r == ccol_success) {
        cjson_node_t *new_node;
        ret = node_make_scalar(type, raw, raw_size, is_signed, pn->m_procs,
                               &new_node);
        if (ret == ccol_success) {
          memcpy(slot.ptr, &new_node, sizeof(new_node));
          new_node->attached = true;
        } else {
          /* Building the child ran out of memory. Remove the placeholder so
           * the map never keeps a NULL child. Deleting a key the map holds
           * always succeeds: a bucket array shrink that can't get memory is
           * skipped and the entry is still removed. */
          chmap_delete_elem(pn->value.dictionary, &kp);
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
        cjson_node_t *existing = list_child_at(pn, idx);
        if (!existing) {
          /* The index is syntactically correct but the element itself is
           * absent, which is a "not found" condition and not a syntax error
           * in the path. _cjson_delete() classifies the same situation the
           * same way (see its own list branch below). A list can't grow
           * itself to fit an arbitrary index the way a dictionary can, so this
           * stays a real error, and both path-based mutators classify it
           * alike. */
          ret = ccol_key_not_found;
        } else {
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
 * The implementation behind the public cjson_delete(root, path) macro.
 *
 * The path is split on its LAST unescaped dot, which gives the parent node
 * and the leaf key; with no dot in the path, root is the parent. The escapes
 * of the leaf key are resolved before it is used, so '\.' and '\\' work
 * exactly as in cjson_get and cjson_set.
 *
 * The addressed node is removed and deep-freed.
 * Returns:
 *   ccol_success          - the node was removed and freed.
 *   ccol_invalid_args     - a NULL root or path. An empty path, or an empty
 *                           path component from a leading dot, a trailing
 *                           dot, or two dots in a row. A path whose shape
 *                           can't apply to the tree, at any position: a
 *                           component that would have to descend into or
 *                           below a scalar, or a component without a '#N' on
 *                           a list. Or a malformed '#N' index (not a number,
 *                           negative, or a bare '#').
 *   ccol_key_not_found    - a well-formed name or index on the parent path
 *                           is absent. Also returned for a leaf key or an
 *                           index whose syntax is correct and which is
 *                           absent.
 *   ccol_not_enough_memory - copying a string failed.
 */
ccol_retval_t _cjson_delete(cjson root, const char *path) {
  if (!root || !path || path[0] == '\0') return ccol_invalid_args;

  ccol_memmgmt_procs_t *mp = ((cjson_node_t *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return ccol_not_enough_memory;

  char *last_dot = path_find_last_unescaped_dot(copy);
  char *leaf_copy;

  if (!last_dot) {
    /* No dot at all, and copy already holds a byte-for-byte copy of path, so
     * we reuse it as leaf_copy instead of paying for a second copy of the
     * same string. */
    leaf_copy = copy;
  } else {
    *last_dot = '\0';
    if (copy[0] == '\0') {
      /* A leading dot, so the parent path is empty, which is a syntax
       * error. */
      _ccol_mem_free(mp, copy);
      return ccol_invalid_args;
    }
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    if (!leaf_copy) {
      _ccol_mem_free(mp, copy);
      return ccol_not_enough_memory;
    }
  }

  /* Check the leaf component and resolve its escapes BEFORE walking the
   * parent path. An empty leaf comes from a trailing dot on the whole path,
   * as in "a.", and it is always a syntax error that has to be reported as
   * ccol_invalid_args, even when the parent path doesn't resolve, as in
   * "missing." where "missing" doesn't exist. A parent that really is absent
   * must never hide a syntax error in the leaf behind a ccol_key_not_found. */
  path_unescape_component(leaf_copy);
  if (leaf_copy[0] == '\0') {
    if (leaf_copy != copy) _ccol_mem_free(mp, leaf_copy);
    _ccol_mem_free(mp, copy);
    return ccol_invalid_args;
  }
  const char *leaf_comp = leaf_copy;

  cjson parent;
  if (!last_dot) {
    parent = root;
  } else {
    bool invalid = false;
    parent = navigate(root, copy, &invalid);
    _ccol_mem_free(mp, copy);
    if (!parent) {
      _ccol_mem_free(mp, leaf_copy);
      /* Same as the comment in _cjson_set_typed(): an error in the parent
       * path is kept apart from an ordinary well-formed component that is
       * absent. */
      return invalid ? ccol_invalid_args : ccol_key_not_found;
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
      } else if (idx >= list_len_of(pn)) {
        /* The index is syntactically correct but the element itself is absent.
         * Under this function's documented contract, which promises
         * ccol_key_not_found when any path component is absent, that's a "not
         * found" condition and not a syntax error. We check the index here
         * instead of leaving it to cjson_list_remove(), which returns
         * ccol_invalid_args for an out-of-bounds index. That is right for a
         * direct call to that function, but here it would drag its narrower
         * contract into this function, whose own documentation says
         * otherwise. */
        ret = ccol_key_not_found;
      } else {
        ret = cjson_list_remove(parent, idx);
      }
    }
  } else {
    ret = ccol_invalid_args;
  }

  _ccol_mem_free(mp, leaf_copy);
  return ret;
}
