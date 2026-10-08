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
#include <internal/cgrowbuf.h>
#include <internal/chashinsert.h>
#include <internal/cnumlocale.h>
#include <internal/cprocsintern.h>
#include <internal/cstrutil.h>
#include <internal/ctlsmodel.h>
#include <internal/cutf8.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>

/* ========================================================================== */
/*                         INTERNAL DOM NODE                                  */
/* ========================================================================== */

/*
 * This is the full definition of the opaque struct that cjson.h declares
 * ahead of it.
 *
 * m_procs: the allocator pointer of one node. node_alloc() sets it, and
 * nothing changes it after that. NULL means the default
 * malloc/free/calloc/realloc. Any other value is an interned copy from
 * ccol_procs_intern(), never the pointer that the caller gave: the caller's
 * struct can go out of scope while the tree is still in use, and the
 * interned copy lives as long as the process. Every public entry point that
 * takes a caller's procs interns it before any node exists. Every node in a
 * tree must carry the same m_procs value. The fast path of the pool is
 * available only when m_procs == NULL.
 *
 * Layout:
 *  - type     : one of the seven cjson_node_type_t values
 *  - attached : true after a push or a set puts this node into a
 *               CJSON_LIST parent or a CJSON_DICTIONARY parent. Read
 *               cjson_list_push() and cjson_dictionary_set() below. In a
 *               tree that the public API builds, exactly one parent slot
 *               can reach each node. This flag is what lets
 *               cjson_list_push() and cjson_dictionary_set() refuse a child
 *               that already has a parent. Without the flag they would
 *               silently make a second owner of the same node. Without this
 *               guard, two container slots point at the same node. Each
 *               slot then destroys the node on its own when its own parent
 *               goes away. That is a double free. Under the default
 *               allocator it corrupts the free-list of the thread-local
 *               node pool into a cycle that points at itself. That cycle
 *               hangs the teardown in __attribute__((destructor)). Under a
 *               custom allocator it segfaults at once. A new node from
 *               node_alloc() starts with attached == false. That is also
 *               true for the fast path of the pool, after its own memset.
 *  - m_procs  : the allocator for this node and for the strings that it owns
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
 * This is a dynamic string buffer. Only the output of the JSON serializer
 * uses it. It sits on the ccol_growbuf_t of cgrowbuf.h. That is the same
 * growable buffer of bytes as the ybuf_t of cyaml.c. The sb_* functions
 * below are thin wrappers that forward to it. They give the serialization
 * call sites of this file one local vocabulary that describes itself. Every
 * sb_* operation is a no-op after oom is set. A caller can therefore check
 * for an error at the end of one serialization pass. It does not need a
 * test after each append.
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

/* Initialize with a capacity that is set in advance. The capacity is at
 * least 64 bytes. */
static inline void sb_init_hint(sbuf_t *sb, ccol_memmgmt_procs_t *mp,
                                size_t hint) {
  ccol_growbuf_init_hint(sb, mp, hint);
}

/* ========================================================================== */
/*                         UTF-8 WELL-FORMEDNESS                              */
/* ========================================================================== */

/*
 * RFC 8259 sec. 8.1 needs JSON text in UTF-8, and cjson.h documents
 * CJSON_STRING as UTF-8. This module therefore keeps one invariant. Every
 * byte sequence of a string that a DOM node can reach is a well-formed
 * UTF-8 encoding of a sequence of Unicode scalar values. This covers a
 * CJSON_STRING value and every dictionary key alike. Every point where
 * bytes enter the DOM sets up this invariant. The invariant is what makes
 * the raw pass-through of any byte >= 0x20 in the serializer correct. The
 * serializer therefore needs no second check over its output. Without the
 * invariant, this library reads a document from a peer that it does not
 * trust and serializes it again. It then passes invalid or overlong
 * sequences straight through to consumers that reject them.
 * cjson_str_val() then also
 * gives application code a buffer that the documentation calls UTF-8 but
 * that is not UTF-8.
 *
 * Both kinds of entry point refuse what is not UTF-8. Neither one rewrites
 * it:
 *
 *  - The parser decodes text that a peer gives. A raw byte sequence in a
 *    string that is not well-formed UTF-8 (truncated, a stray continuation
 *    byte, overlong, an encoded surrogate, or above U+10FFFF) is a parse
 *    error that names the bytes and their position. So is a \uXXXX escape
 *    for a surrogate that is not one half of a high-low pair. A repair, such
 *    as a U+FFFD in place of the defect, would let two keys that differ only
 *    in their ill-formed bytes become one key, and would give this parser a
 *    different reading of the document from a second parser that refuses
 *    it.
 *  - The calling program itself gives a C string to the API. That API
 *    constructs a node directly, or it changes one. A string there that is
 *    not valid UTF-8 is a defect in that program. The API therefore reports
 *    it with NULL or with ccol_invalid_args. It treats the string exactly as
 *    it treats a non-finite double.
 */

/* True when the whole NUL-terminated string is well-formed UTF-8. The
 * function accepts a NULL s. A caller can therefore pass a string that is
 * itself optional. */
static bool utf8_cstr_is_valid(const char *s) {
  return !s || ccol_utf8_is_valid(s, strlen(s));
}

/* ========================================================================== */
/*                    LOCALE-INDEPENDENT NUMBER I/O                           */
/* ========================================================================== */

/*
 * JSON (RFC 8259 sec. 6) always writes the decimal point as the ASCII '.',
 * whatever the locale of the host process is. strtod() and the "%g"
 * conversions of the printf family follow LC_NUMERIC instead. Every strtod()
 * call and every snprintf() call of the "%g" family in this file therefore
 * goes through _cjson_strtod_c() or _cjson_snprintf_g_c() below. Both run
 * their one conversion inside a ccol_c_locale_enter() scope; see
 * internal/cnumlocale.h for the mechanism and its guarantees. Without it,
 * parse_number() reads "3.14" as 3 under a locale with ',' as its decimal
 * point, and format_double() emits text that is not valid JSON.
 */

/* A strtod() that does not follow the locale. It always reads '.' as the
 * decimal point. The LC_NUMERIC of the calling thread does not change
 * this. */
static double _cjson_strtod_c(const char *nptr) {
  ccol_c_locale_scope_t scope = ccol_c_locale_enter();
  double v = strtod(nptr, NULL);
  ccol_c_locale_leave(scope);
  return v;
}

/* An snprintf() that does not follow the locale. format_double() uses it
 * for its own two double conversions, "%.15g" and "%.17g". It always emits
 * '.' as the decimal point. The LC_NUMERIC of the calling thread does not
 * change this. The function takes a choice between the two literals that
 * format_double() needs. It does not take a format string from the caller.
 * The snprintf() call below therefore always has a literal format argument.
 * The shape of the code prevents -Wformat-nonliteral. Nothing suppresses
 * that warning. */
static void _cjson_snprintf_g_c(char *buf, size_t cap, bool wide_precision,
                                double val) {
  ccol_c_locale_scope_t scope = ccol_c_locale_enter();
  if (wide_precision)
    snprintf(buf, cap, "%.17g", val);
  else
    snprintf(buf, cap, "%.15g", val);
  ccol_c_locale_leave(scope);
}

/* ========================================================================== */
/*                         INTERNAL HELPERS                                   */
/* ========================================================================== */

/*
 * The SSO storage of a chmap_entry is aligned by its own construction. This
 * memcpy is therefore a second line of defence and not a live need for
 * alignment. It costs nothing. It matches the pattern that cyaml,
 * clrucache, cthreadcomm and chttpclient use for their own pointer storage
 * in a chmap.
 */
static inline cjson_node_t *_cjson_read_child(const void *src) {
  cjson_node_t *p;
  memcpy(&p, src, sizeof(p));
  return p;
}

/*
 * A container node owns no backing store until it gets its first child:
 * value.list or value.dictionary stays NULL while the container has never
 * held anything. An empty container therefore costs one node and nothing
 * more, and "[]" and "{}" in a parsed document allocate no cvec and no
 * chmap, whose empty forms cost several times the node. Every read of a
 * container goes through the accessors below, which read a NULL store as
 * empty. Every insert first calls list_store() or dict_store(), which
 * create the store on demand; a store, once created, stays until the node
 * is cleared or destroyed, even when its last child goes.
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

/* Look key_pair up in a dictionary. Gives ccol_success and the value
 * accessor in *vp, or a code that is not ccol_success when the key is
 * absent. */
static inline ccol_retval_t dict_lookup(const cjson_node_t *n,
                                        const cmap_pair *key_pair,
                                        const cmap_pair **vp) {
  if (!n->value.dictionary) return ccol_key_not_found;
  return chmap_get_elem_ref(n->value.dictionary, key_pair, vp);
}

/* The backing cvec of a list, created on the first call. NULL when the
 * allocation fails; the node is then unchanged. */
static cvec list_store(cjson_node_t *n) {
  if (!n->value.list)
    n->value.list =
        cvector_create_full(sizeof(cjson_node_t *), n->m_procs, NULL);
  return n->value.list;
}

/* The backing chmap of a dictionary, created on the first call. NULL when
 * the allocation fails; the node is then unchanged. */
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
 * every live entry on one list in that order: a new key goes after every
 * other key, a replacement of the value of a key keeps its place, and a
 * delete takes it out. Parsing, cjson_dictionary_set(), cjson_clone(), the
 * serializers and cjson_dictionary_first() therefore all see the members in
 * the order in which their keys first arrived. A walk allocates nothing and
 * cannot fail. The first call gives the oldest member; each later call gives
 * the member after the one that the previous call read.
 */
static inline const ccol_chmap_entry_ref *dict_first_entry(cjson_node_t *n) {
  return n->value.dictionary ? ccol_chmap_oldest_entry(n->value.dictionary)
                             : NULL;
}

/*
 * This is an explicit worklist on the heap. node_clear_value(),
 * node_clear() and __cjson_destroy() below use it to tear down a whole
 * subtree. They do not recurse once for each level of nesting. A tree that
 * reaches any of them need not come from cjson_parse() at all.
 * CJSON_MAX_PARSE_DEPTH bounds that function on its own; read the PARSER
 * section further down. The public cjson_list_push() and
 * cjson_dictionary_set() API can build a tree of any depth directly.
 * Destruction has no contract that lets it fail cleanly. node_clear() and
 * __cjson_destroy() are both void, and a caller cannot get back a tree that
 * it still owns and that is half free, to try again. Recursion with a cap
 * on the depth is therefore not an option here, although cjson_clone() and
 * serialize_node() return an error past such a cap. Here it would only
 * trade an immediate crash for a silent, permanent leak of every node past
 * the cap. An explicit worklist prevents both failure modes. It keeps the
 * native call stack at O(1) depth, whatever the real depth and width of the
 * tree are.
 */
typedef struct destroy_worklist {
  cjson_node_t **items;
  size_t cap;
  size_t len;
} destroy_worklist_t;

/* Push child onto wl and grow wl when it is full. The growth uses a plain
 * realloc and free, because the worklist is short-lived scratch state with
 * no tie to the m_procs of any node. The function ignores a NULL child
 * silently. Every other call site in this file that destroys a child
 * pointer does the same.
 *
 * When the growth of the worklist cannot allocate, this function tears the
 * child down at once with an ordinary recursive __cjson_destroy() call. It
 * does not defer the child. That recursion can use stack in proportion to
 * the depth of that one child's own subtree. It does so only when BOTH of
 * these are true at the same time: the tree is deep enough to matter, AND
 * the allocator cannot grow a small scratch array. The library accepts that
 * pair of failures as a rare, graceful fall back to a recursive teardown of
 * that one subtree. Nothing engineers around it further. It is much
 * narrower than the stack overflow that this worklist prevents. That
 * overflow happens on depth alone and needs no pressure on memory at
 * all. */
static void destroy_worklist_push(destroy_worklist_t *wl, cjson_node_t *child) {
  if (!child) return;
  if (wl->len == wl->cap) {
    size_t new_cap = wl->cap == 0 ? 32 : wl->cap * 2;
    cjson_node_t **grown =
        ccol_mem_realloc(wl->items, new_cap * sizeof(*wl->items));
    if (!grown) {
      __cjson_destroy((cjson)child);
      return;
    }
    wl->items = grown;
    wl->cap = new_cap;
  }
  wl->items[wl->len++] = child;
}

/* This is the destructor callback for chmap_destroy_with_dtor(). The
 * CJSON_DICTIONARY case of node_clear_value() below uses it. It puts the
 * child node of one dictionary entry onto the worklist that dtor_ctx
 * carries. It does not recurse into the child directly. The same iterative
 * worklist therefore drains a dictionary value and everything else. Read
 * the doc comment of chmap_destroy_with_dtor in chashmap.h. It gives the
 * reason why this is the way to reach every child during a teardown without
 * an allocation. The other way is to walk the dictionary with
 * chashmap_begin_iter() first. That call needs its own small allocation.
 * Under sustained pressure on memory, that allocation can itself fail. The
 * teardown then silently leaks every child that it can no longer reach. */
static void _cjson_enqueue_dict_child(const cmap_pair *val_pair,
                                      void *dtor_ctx) {
  cjson_node_t *child = _cjson_read_child(val_pair->ptr);
  destroy_worklist_push((destroy_worklist_t *)dtor_ctx, child);
}

/* Push item onto the growable stack array. len and cap give the current
 * length and capacity of that array. The growth uses a plain realloc,
 * because this is short-lived scratch state with no tie to the m_procs of
 * any node. The reason is the reason of destroy_worklist_push above. The
 * function returns false when the allocation fails, and it then leaves
 * stack, cap and len all unchanged.
 *
 * inline_stack is the caller's own small array on the stack. *stack points
 * at it until the search grows past it. A tree that a caller builds from
 * the top down attaches a new container at every level. Each such attach
 * then searches a subtree of one node and costs no allocation at all.
 * Without the inline array, each attach pays one malloc and one free. */
static bool cycle_stack_push(cjson_node_t ***stack, cjson_node_t **inline_stack,
                             size_t inline_cap, size_t *cap, size_t *len,
                             cjson_node_t *item) {
  if (*len == *cap) {
    size_t new_cap = *cap * 2;
    cjson_node_t **grown;
    if (*stack == inline_stack) {
      grown = ccol_mem_alloc(new_cap * sizeof(**stack));
      if (!grown) return false;
      memcpy(grown, inline_stack, inline_cap * sizeof(**stack));
    } else {
      grown = ccol_mem_realloc(*stack, new_cap * sizeof(**stack));
      if (!grown) return false;
    }
    *stack = grown;
    *cap = new_cap;
  }
  (*stack)[(*len)++] = item;
  return true;
}

/*
 * Returns true when the subtree of haystack can reach needle. That subtree
 * includes haystack itself. That is, haystack == needle, or needle is a
 * descendant of haystack somewhere inside its list contents or its
 * dictionary contents.
 *
 * cjson_list_push() and cjson_dictionary_set() use this function to reject
 * a push or a set that would create a cycle. To attach child as a new
 * descendant of target is safe only when a walk down from child itself
 * cannot ALREADY reach target. If such a walk can reach target, target
 * becomes a new ancestor of child through the edge that this call is about
 * to add. Target also stays a descendant of child through the subtree that
 * is already there. That is a real cycle in the graph. The "attached" field
 * rejects a child that already has SOME parent; read the doc comment at the
 * top of this file. It also rejects a child that is the same node as the
 * target. On its own it does not reject a node that is not yet attached.
 * One example is the root of a whole tree that the caller still holds and
 * that nothing ever pushed anywhere. A caller can push such a root into one
 * of its own descendants. This function closes that gap. After such a cycle
 * exists, the worklist teardown of __cjson_destroy() reaches the same node
 * twice and frees it twice; read the doc comment of destroy_worklist_t. A
 * free of an ancestor does not make that ancestor unreachable, because the
 * cycle has its own back edge. A cycle of only two nodes is enough to make
 * glibc abort on a double free inside __cjson_destroy().
 *
 * The search is iterative. It uses an explicit stack on the heap and no
 * recursion. The reason is exactly the reason of destroy_worklist_t. The
 * subtree of child can have any depth, because the public API can build it
 * directly to far past any safe depth of the native stack. A recursive
 * search here would therefore bring back the same risk of a stack overflow
 * that destroy_worklist_t prevents.
 *
 * The function sets *incomplete to true when it cannot run the search to
 * its end and has not yet found needle. That happens only when the growth
 * of the stack cannot allocate. The walk over the members of a dictionary
 * allocates nothing. The caller must treat
 * that exactly as it treats any other failure of an allocation. It reports
 * ccol_not_enough_memory and refuses the operation. The caller must not
 * read "not found yet" as "safe". A real cycle past the point where the
 * search gave up would then stay undetected.
 *
 * needle->attached cuts the whole search to O(1). A walk down from another
 * node can reach a node only when some call inserted that node as a child
 * of somebody at least once. That insert is the only place that sets
 * "attached" to true, and nothing ever clears it again; read the doc
 * comment of the struct. A needle->attached == false therefore proves that
 * needle has no parent at all yet. No subtree of ANY node can reach it, and
 * this includes the subtree of haystack. The function proves this without a
 * walk over one byte of haystack. This is not a heuristic. It is what
 * keeps the most common pattern at O(1) for each call. In that pattern a
 * caller wraps a subtree that it already built in a new outer container
 * that is not attached yet. One example is a deeply nested structure that a
 * caller builds from the bottom up, with one cjson_list_push() for each
 * level. Without the short-circuit, that ordinary and documented use falls
 * to O(size of the subtree so far) for each call, which is O(n^2) for n
 * calls. build_nested_list_via_api(20000) in tests/cjson/tests.c measures
 * exactly this. It stays well under 100ms with the short-circuit and takes
 * more than a second without it.
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
  if (stack != inline_stack) ccol_mem_free(stack);
  return found;
}

/*
 * Thread-local free-list pool for cjson_node_t.
 *
 * The pool holds only nodes with m_procs == NULL, which is the default
 * allocator. A node with a custom allocator never uses the pool. Its own
 * m_procs allocates it and frees it directly. This invariant removes the
 * need to record which allocator a node came from. _node_pool_drain always
 * calls a plain free(), because a calloc() allocated every node in the
 * pool.
 *
 * The reason for the pool: every DOM node normally costs one calloc and one
 * free. In a workload that parses and destroys documents again and again,
 * that round trip to the allocator is most of the cost. A free-list with a
 * cap lets the library reuse a node with no cost for each node. When a node
 * goes back to the pool, the library stores the old head of the list inside
 * the memory of the node itself. This is safe, because
 * sizeof(cjson_node_t) >= sizeof(void *). The next allocation gets that
 * head back with a memcpy, which prevents undefined behaviour from strict
 * aliasing.
 */
#define _NODE_POOL_CAP 512U
#if defined(_CCOL_EMULATE_DARWIN_TLS)
/* The pool of one thread, and its error buffer. The pointer to this block is
 * the value of the pool key, so _node_pool_drain receives it as its argument
 * and reads no __thread variable; see ctlsmodel.h. A thread has a block from
 * its first pooled node or its first stored error message, and a value on the
 * key exactly while it has a block. */
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
atomic_ulong _cjson_pool_key_lock_count_for_tests;
/* How many nodes the pools of all threads hold together. A pool that a
 * thread leaves behind at exit stays counted here. */
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
    /* The default allocator is in use and the pool has a node. Reuse that
     * node. */
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

/* Key for thread-local storage. Its only job is to run _node_pool_drain
 * when a thread exits. It wraps a pthread_key_t with the thread_ls_* macros
 * of common.h. This obeys a standing rule of this codebase. Every use of a
 * pthread_* function in library code must go through a wrapper in common.h.
 * No code calls the raw API directly. */
static ccol_thread_ls_key_t _pool_pthread_key;

/* True after _do_pool_key_init() creates _pool_pthread_key and initializes
 * _pool_key_rwlock below. It becomes false again after _pool_key_fini()
 * deletes the key. The cold path of node_free guards its
 * ccol_thread_ls_set call with this flag, under _pool_key_rwlock; read the
 * doc comment of that lock. The guard prevents a call into a key that
 * something deleted while a background thread is still active during a
 * concurrent dlclose. This flag is also the one signal of whether the
 * one-time init succeeded. It stays false, which is its static first value,
 * when either pthread call in _do_pool_key_init() fails. One cause is a
 * real shortage of resources, for example a process that already reached
 * PTHREAD_KEYS_MAX. A caller must therefore check this flag before it
 * treats the rwlock or the key as valid. */
static atomic_bool _pool_key_live = false;

/* This lock guards the load-and-act pair in the cold path of node_free. It
 * guards that pair against _pool_key_fini, which flips the flag and then
 * deletes the key. _pool_key_fini is the destructor of the shared object
 * below, and it is the only writer. A bare atomic_load and then an act on
 * _pool_key_live is not enough on its own. The destructor can run between
 * the load of node_free and its own ccol_thread_ls_set call. It then
 * deletes the key under a pthread_setspecific() that is already in flight,
 * which POSIX calls undefined behaviour. This lock stays held across the
 * load and the act. The two are therefore mutually exclusive.
 * ccol_thread_ls_set either completes fully before the key goes away, or it
 * never runs at all. _do_pool_key_init() initializes this lock on first
 * use, under the ccol_once_flag_t below. This obeys the standing rule of
 * this codebase against a static or constant initializer for a lock. */
static ccol_rw_lock_t _pool_key_rwlock;

static void _node_pool_drain(void *);

/* The key init runs on first use. It runs exactly once, the first time that
 * node_free() needs the pool. It leaves _pool_key_live false, and it leaves
 * _pool_key_rwlock uninitialized, when either pthread call fails. The read
 * side of node_free therefore never touches either primitive before it
 * confirms that the init succeeded. */
static ccol_once_flag_t _pool_key_once = CCOL_ONCE_INIT;

static void _do_pool_key_init(void) {
  if (ccol_rw_lock_init(_pool_key_rwlock) != 0) return;
  if (ccol_thread_ls_key_create(_pool_pthread_key, _node_pool_drain) != 0)
    return;
  atomic_store(&_pool_key_live, true);
}

/* The loader calls this function when it unloads the shared object. That
 * happens on a dlclose or at process exit.
 * - It drains the pool of the calling thread first. The pool is therefore
 *   free before the code mapping of the shared object goes away.
 * - It flips the liveness flag and deletes the key under the write lock.
 *   This can therefore never interleave with a concurrent node_free() that
 *   uses the key under the read lock; read the doc comment of
 *   _pool_key_rwlock. ccol_thread_ls_set either completes fully before the
 *   key goes away, or it never runs at all. A bare atomic flag only makes
 *   the window of the race smaller. It does not close the window.
 * - It deletes the key. Without the delete, repeated cycles of dlopen and
 *   dlclose use up PTHREAD_KEYS_MAX.
 * This function is a no-op when _do_pool_key_init() never ran. That is the
 * case when no thread ever called a cjson function that touches the pool.
 * _pool_key_rwlock is then not validly initialized. */
__attribute__((destructor)) static void _pool_key_fini(void) {
  if (!atomic_load(&_pool_key_live)) return;
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  _cjson_tls_t *t = _cjson_tls_peek();
  if (t) {
    (void)ccol_thread_ls_set(_pool_pthread_key, NULL);
    _node_pool_drain(t);
  }
#else
  _node_pool_drain(NULL);
#endif
  ccol_rw_lock_wrlock(_pool_key_rwlock);
  atomic_store(&_pool_key_live, false);
  ccol_thread_ls_key_delete(_pool_pthread_key);
  ccol_rw_lock_unlock(_pool_key_rwlock);
}

#if defined(_CCOL_EMULATE_DARWIN_TLS)
/* The block of the calling thread, or NULL when the thread has none. */
static _cjson_tls_t *_cjson_tls_peek(void) {
  if (!atomic_load_explicit(&_pool_key_live, memory_order_acquire)) return NULL;
  return (_cjson_tls_t *)ccol_thread_ls_get(_pool_pthread_key);
}

/* The block of the calling thread. It makes the block, and sets it as the
 * value of the pool key, when the thread has none. It gives NULL when the key
 * does not exist or memory runs out. The caller must have run the once-guard
 * of the key. */
static _cjson_tls_t *_cjson_tls_get(void) {
  _cjson_tls_t *t = _cjson_tls_peek();
  if (t || !atomic_load(&_pool_key_live)) return t;
  t = (_cjson_tls_t *)calloc(1, sizeof(*t));
  if (!t) return NULL;
  bool set = false;
  ccol_rw_lock_rdlock(_pool_key_rwlock);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&_cjson_pool_key_lock_count_for_tests, 1,
                            memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
  if (atomic_load(&_pool_key_live) &&
      ccol_thread_ls_set(_pool_pthread_key, t) == 0)
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
 * that _node_pool_drain runs when the thread exits. The caller must have run
 * the once-guard of the key. It is always inlined, so node_free() keeps the
 * shape that it has with the code written in place. */
static inline __attribute__((always_inline)) void _pool_key_arm(void) {
  if (!_pool_key_armed && atomic_load(&_pool_key_live)) {
    ccol_rw_lock_rdlock(_pool_key_rwlock);
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add_explicit(&_cjson_pool_key_lock_count_for_tests, 1,
                              memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
    if (atomic_load(&_pool_key_live) &&
        ccol_thread_ls_set(_pool_pthread_key, (void *)1) == 0)
      _pool_key_armed = true;
    ccol_rw_lock_unlock(_pool_key_rwlock);
  }
}

/* The per-thread message buffer of a failed parse; see cjson_report_err().
 * The drain at thread exit frees it with the node pool. */
static __thread char *cjson_err_buf;
#endif /* _CCOL_EMULATE_DARWIN_TLS */

/* Return a node to the thread-local pool, or free it directly through its
 * own allocator. The node goes back to the pool when m_procs == NULL and
 * the pool is not full. The first call from a thread that uses the pool
 * registers a pthread destructor. That destructor drains the pool when the
 * thread exits. */
static void node_free(cjson_node_t *n) {
  if (n->m_procs != NULL) {
    /* This node has a custom allocator. Free it directly and never touch
     * the default pool. */
    _ccol_mem_free(n->m_procs, n);
    return;
  }
  /* This node has the default allocator, so m_procs == NULL. Try to return
   * the node to the pool. */
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  ccol_call_once(_pool_key_once, _do_pool_key_init);
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
  /* This obeys the rule of this codebase for every pthread primitive and
   * every ccol_once_flag_t. A function that touches _pool_key_rwlock
   * directly must always run the once-guard. Do not skip the guard because
   * the call graph says that another function already ran it. That argument
   * breaks silently as soon as a new call path appears. */
  ccol_call_once(_pool_key_once, _do_pool_key_init);
  if (_node_pool_sz == 0) {
    /* This is the cold path. It runs once for each thread, until the pool
     * of that thread becomes empty again. Read the doc comment of
     * _pool_key_rwlock. It gives the reason why a lock must protect the
     * load-and-act, and why a bare atomic check is not enough. The outer
     * check here holds no lock. Its only job is to skip _pool_key_rwlock
     * completely when _do_pool_key_init() never completed. One cause of
     * that is a real shortage of pthread resources. The rwlock is then
     * never validly initialized, and a lock on it is undefined behaviour.
     * This outer check does not open the race that the inner lock closes.
     * _pool_key_live only ever moves from false to true inside
     * _do_pool_key_init() itself. The ccol_call_once() above already ran
     * that function to its end, with the usual happens-before guarantee of
     * pthread_once. A true here therefore means that the rwlock is already
     * fully initialized. It also stays valid memory for the rest of the
     * process, even when a concurrent unload of the shared object flips the
     * flag back to false immediately after. The inner check, under the
     * lock, is what makes that later move to false safe to race
     * against. */
    _pool_key_arm();
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

/* Drain the node pool of the calling thread. By the invariant, every node
 * in the pool has m_procs == NULL. A plain free() is therefore always
 * correct here.
 *
 * The drain also disarms the key for this thread. At thread exit the C
 * library clears the value of the key before it calls this destructor, so
 * the thread holds no value any more. A destructor of another key can run
 * after this one and destroy a tree, and node_free() then refills the pool.
 * With the flag clear, that refill sets the value again, and the C library
 * calls this destructor once more in its next round over the keys. With the
 * flag left set, the refill never arms the key and every node in it leaks. */
#if defined(_CCOL_EMULATE_DARWIN_TLS)
/* Under _CCOL_EMULATE_DARWIN_TLS the drain frees the block that arg names.
 * The C library has already cleared the value of the key, so a node that a
 * destructor of another key frees later makes a new block and sets the value
 * again, and the C library calls this destructor once more for it. */
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
/* This function gives the size of the free-list in the node pool of the
 * calling thread. White-box unit tests use it. They check how the pool
 * evicts a node at its cap, which is _NODE_POOL_CAP. They also check that
 * one thread does not see the pool of another thread. This function is not
 * part of the public API. */
#if defined(_CCOL_EMULATE_DARWIN_TLS)
size_t cjson_debug_pool_size(void) {
  _cjson_tls_t *t = _cjson_tls_peek();
  return t ? (size_t)t->sz : 0;
}
#else
size_t cjson_debug_pool_size(void) { return (size_t)_node_pool_sz; }
#endif
#endif

/* Deep-free the value payload of n. That payload is the bytes of a string,
 * or a list container, or a dictionary container. The function pushes every
 * direct child onto wl. It does not recurse into a child. It leaves n->type
 * and n itself untouched. */
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
      /* This uses chmap_destroy_with_dtor from chashmap.h. It does not walk
       * the map with chashmap_begin_iter first and destroy after that. That
       * helper allocates its own small iterator struct. Under sustained
       * pressure on memory, that allocation can itself fail. There is then
       * no allocation left to reach the entries that remain, and the
       * teardown silently leaks every child past that point.
       * chmap_destroy_with_dtor walks the internal storage of the map
       * directly. It can always reach a live entry. */
      chmap_destroy_with_dtor(n->value.dictionary, _cjson_enqueue_dict_child,
                              wl);
      n->value.dictionary = NULL;
      break;
    }
    default:
      break;
  }
}

/* Drain wl until it is empty. Destroy every node that it contains
 * completely, and this includes the node struct. node_clear_value() above
 * puts the children of each drained node onto the worklist. The worklist
 * therefore keeps work until the whole subtree of the first contents of wl
 * is gone. This is a plain loop and not recursion. The loop is what keeps
 * the stack use of node_clear() and __cjson_destroy() independent of the
 * depth of the tree. */
static void destroy_worklist_drain(destroy_worklist_t *wl) {
  while (wl->len > 0) {
    cjson_node_t *n = wl->items[--wl->len];
    node_clear_value(n, wl);
    node_free(n);
  }
}

/* Deep-free the resources of the value of a node. Do not free the node
 * itself. node_reinit_scalar() uses this function. It throws away the old
 * list value or dictionary value of a node before it writes a new scalar
 * into that node in place, for example for a cjson_set(). The old value can
 * be a tree of any depth; read the doc comment of destroy_worklist_t above.
 * Every descendant below the direct children of n therefore goes through
 * the same iterative worklist as __cjson_destroy() uses. Nothing here
 * recurses. */
static void node_clear(cjson_node_t *n) {
  destroy_worklist_t wl = {NULL, 0, 0};
  node_clear_value(n, &wl);
  destroy_worklist_drain(&wl);
  ccol_mem_free(wl.items);
}

/*
 * Check a scalar payload that cjson_set() or a direct _cjson_set_typed() call
 * describes, without touching any node. It returns ccol_success when
 * node_reinit_scalar_checked() can store the payload, which then fails only
 * for lack of memory. It returns ccol_invalid_args for a wrong type, a wrong
 * size, a float that is not finite, a void * that is not NULL, and a string
 * that is not valid UTF-8.
 */
static ccol_retval_t scalar_payload_check(cjson_node_type_t type, void *raw,
                                          size_t raw_size) {
  /* Reject the composite types CJSON_LIST and CJSON_DICTIONARY. The switch
   * that assigns the value after the clear covers only a scalar. To reach
   * its default branch after node_clear would leave the node in an
   * inconsistent state. This is also the point that keeps the list of types
   * that cjson_set() documents. _cjson_type_of() in cjson.h does not know
   * every C type. A value of a type that it does not know arrives here as
   * _CJSON_TYPE_UNSUPPORTED. That value also falls into the default branch,
   * and this switch rejects it before anything touches the node. The
   * library does not write it silently as CJSON_NULL. */
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

  /* Every type that this function accepts, except CJSON_NULL, carries a
   * payload that the function reads through raw. A NULL raw has nothing to
   * read, and each size check below would dereference it. Only CJSON_NULL
   * accepts raw == NULL. That is how a direct _cjson_set_typed() call says
   * "no payload". */
  if (type != CJSON_NULL && !raw) return ccol_invalid_args;

  /* The _Generic dispatch of cjson_set(), which is _cjson_type_of() in
   * cjson.h, maps ANY C expression of a void pointer type to CJSON_NULL. It
   * does not map only the literal NULL. A caller can pass a live void
   * pointer that is not NULL by mistake, instead of one of the documented
   * types. Those types are bool, integer, float, double, char pointer, const
   * char pointer and NULL. Without this check, the library throws that
   * pointer away silently. It would write the leaf as a JSON null and report
   * nothing at all. This check rejects such a pointer instead. That matches the
   * list of accepted types in the documentation of cjson_set(). The check runs
   * only when raw carries a full payload of the size of a pointer, which is
   * raw_size == sizeof(void*). That is exactly what the macro always gives
   * for this type. A direct _cjson_set_typed() call can pass raw == NULL
   * and raw_size == 0 for a real null with no payload. This check leaves
   * such a call alone. */
  if (type == CJSON_NULL && raw && raw_size == sizeof(void *) &&
      *(void **)raw != NULL)
    return ccol_invalid_args;

  /* Check the size of the float, and check that it is finite. */
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

  /* Check the raw_size of the integer before anything touches the node. */
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

  /* Check the raw_size of the bool before anything touches the node. Only a
   * direct call to _cjson_set_typed() that goes around the macro can reach
   * this check. The cjson_set() macro always gives sizeof(bool) for a C
   * expression of type bool. A mismatch here can therefore only come from a
   * caller of the public back-end function itself. Without this check,
   * *(bool *)raw reads past a raw buffer that is narrower. It reads a
   * garbage byte out of a buffer that is wider. It then stores the bit
   * pattern that it read into a _Bool object. That is a trap representation
   * unless the byte is exactly 0 or 1. */
  if (type == CJSON_BOOL && raw_size != sizeof(bool)) return ccol_invalid_args;

  /* Check the raw_size of the string before anything touches the node. The
   * reason is exactly the reason of the bool check above, and a caller
   * reaches it in the same way. The cjson_set() macro always makes a string
   * into a payload of the size of a pointer, because it copies its argument
   * into a local of the decayed type, so a char array arrives as a pointer
   * to its first character. A mismatch can therefore only come from a direct
   * call to the public back-end function. Without this check, *(const char
   * **)raw reads sizeof(const char *) bytes out of the narrower buffer that the
   * caller gave. It then copies a string through the garbage pointer that
   * those bytes form. */
  if (type == CJSON_STRING && raw_size != sizeof(const char *))
    return ccol_invalid_args;

  /* This is the rule of cjson_create_string_mp(). A string from the caller
   * that is not valid UTF-8 has no JSON form. The function reports it and
   * does not store it. */
  if (type == CJSON_STRING && !utf8_cstr_is_valid(*(const char **)raw))
    return ccol_invalid_args;
  return ccol_success;
}

/*
 * Overwrite the content of a node that exists with a payload that
 * scalar_payload_check() already accepted. Deep free every resource that the
 * old content owned. The function uses n->m_procs to allocate a string.
 *
 * It returns ccol_success, or ccol_not_enough_memory when the copy of the
 * string fails. The copy runs before node_clear(), so that failure leaves the
 * original node completely intact.
 */
static ccol_retval_t node_reinit_scalar_checked(cjson_node_t *n,
                                                cjson_node_type_t type,
                                                void *raw, size_t raw_size,
                                                bool is_signed) {
  double pre_d = 0.0;
  if (type == CJSON_FLOAT)
    pre_d = raw_size == sizeof(float) ? (double)*(float *)raw : *(double *)raw;

  /* Allocate the new string first. A failure of that allocation then leaves
   * the content that is already there untouched. */
  char *new_str = NULL;
  if (type == CJSON_STRING) {
    const char *s = *(const char **)raw;
    if (s) {
      new_str = ccol_strdup(n->m_procs, s);
      if (!new_str) return ccol_not_enough_memory;
    }
  }

  /* === The change starts here. Every check passed, and nothing below can
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
            /* An unsigned 64-bit value above LLONG_MAX has no CJSON_INTEGER
             * form. It is stored as the CJSON_FLOAT nearest to it, which is
             * what cjson_parse() makes of the same decimal literal. A
             * conversion to long long would store a negative number and
             * report success. */
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
      break; /* Nothing reaches this. The cases above cover every scalar
              * type. */
  }
  return ccol_success;
}

/* Overwrite the content of a node that exists with a new scalar value. Every
 * check runs before anything touches the node, so any failure leaves it
 * completely intact. The return codes are those of scalar_payload_check() and
 * node_reinit_scalar_checked(). */
static ccol_retval_t node_reinit_scalar(cjson_node_t *n, cjson_node_type_t type,
                                        void *raw, size_t raw_size,
                                        bool is_signed) {
  ccol_retval_t r = scalar_payload_check(type, raw, raw_size);
  if (r != ccol_success) return r;
  return node_reinit_scalar_checked(n, type, raw, raw_size, is_signed);
}

/* Allocate a new scalar node, with the allocator that the caller gives, from
 * a payload that scalar_payload_check() already accepted. The only failure is
 * ccol_not_enough_memory, for the node or for the copy of a string. */
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

/* Replaces *mp with its interned copy, so that no node keeps a pointer to the
 * caller's struct. Returns false when the intern table is full; *mp is then
 * NULL and the caller must fail without creating anything. */
static inline bool cjson_intern_procs(ccol_memmgmt_procs_t **mp) {
  if (*mp == NULL) return true;
  return ccol_procs_intern(*mp, mp) == ccol_success;
}

/*
 * These are the factory functions for each node type. The mp can be NULL,
 * and the function then uses the default malloc, calloc and free. Every one
 * of them returns NULL when an allocation fails.
 *
 * cjson_create_double_mp also rejects a value that is not finite, which is
 * an Inf or a NaN. The JSON specification has no form for such a value.
 *
 * cjson_create_string_mp with val == NULL gives a CJSON_NULL node. That
 * matches the behaviour of cjson_set for a char * variable that is NULL.
 *
 * cjson_create_list_mp and cjson_create_dictionary_mp give empty containers.
 * A caller fills them with cjson_list_push and cjson_dictionary_set.
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
  /* JSON text is UTF-8 (RFC 8259 sec. 8.1). A string that is not valid
   * UTF-8 has no JSON form. This function therefore refuses it and does not
   * rewrite it silently. cjson_create_double_mp() treats a double that is
   * not finite in the same way. Without this check the byte sequence
   * reaches the serializer, which emits it raw. The output is then text
   * that a consumer which obeys the RFC rejects. */
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
 * create it at the first insert. */
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

/* Free a node and every descendant of it, iteratively. Read the doc comment
 * of destroy_worklist_t for the reason. A tree that reaches here need not
 * come from cjson_parse() at all. It has no bound on its depth when a caller
 * builds it directly through the public API that changes a tree. A
 * call on NULL is safe. This function does NOT set the caller's pointer to
 * NULL. Use the cjson_destroy() macro for that.
 *
 * The function clears and frees n itself directly. n never goes through
 * destroy_worklist_push(). When that function cannot allocate, it falls
 * back to a recursive __cjson_destroy() call on the item that it could not
 * queue. n is the argument of this very call. To push n there could
 * therefore make this function call itself on the same node while the
 * allocator is out of memory. This does not change any real descendant. The
 * worklist still drains every one of them, exactly as node_clear() uses
 * it. */
void __cjson_destroy(cjson node) {
  if (!node) return;
  cjson_node_t *n = (cjson_node_t *)node;

  destroy_worklist_t wl = {NULL, 0, 0};
  node_clear_value(n, &wl);
  node_free(n);

  destroy_worklist_drain(&wl);
  ccol_mem_free(wl.items);
}

/* ========================================================================== */
/*                         LEAF VALUE ACCESS                                  */
/* ========================================================================== */

/* Return the type tag of the node. Returns CJSON_NULL for a NULL handle. */
cjson_node_type_t cjson_type(cjson node) {
  if (!node) return CJSON_NULL;
  return ((cjson_node_t *)node)->type;
}

/*
 * These functions read a typed value. Each one calls ccol_fatal_err() when
 * the type does not match, and also for a NULL handle. Guard such a call
 * with cjson_type() when the call site does not know the type at compile
 * time.
 */
bool cjson_bool_val(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_BOOL)
    ccol_fatal_err("cjson_bool_val: node is %s, expected CJSON_BOOL",
                   n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.boolean;
}

long long cjson_int_val(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_INTEGER)
    ccol_fatal_err("cjson_int_val: node is %s, expected CJSON_INTEGER",
                   n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.integer;
}

double cjson_double_val(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_FLOAT)
    ccol_fatal_err("cjson_double_val: node is %s, expected CJSON_FLOAT",
                   n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.number;
}

const char *cjson_str_val(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_STRING)
    ccol_fatal_err("cjson_str_val: node is %s, expected CJSON_STRING",
                   n ? cjson_type_str((cjson)n) : "NULL");
  return n->value.string;
}

/* Return the number of elements in a list, or the number of keys in a
 * dictionary. Both call ccol_fatal_err() when the node does not have the
 * type that they need.
 */
size_t cjson_list_len(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_LIST)
    ccol_fatal_err("cjson_list_len: node is %s, expected CJSON_LIST",
                   n ? cjson_type_str((cjson)n) : "NULL");
  return list_len_of(n);
}

size_t cjson_dictionary_size(cjson node) {
  cjson_node_t *n = (cjson_node_t *)node;
  if (!n || n->type != CJSON_DICTIONARY)
    ccol_fatal_err(
        "cjson_dictionary_size: node is %s, expected CJSON_DICTIONARY",
        n ? cjson_type_str((cjson)n) : "NULL");
  return dict_size_of(n);
}

/* ========================================================================== */
/*                       LIST / DICTIONARY MANIPULATION                       */
/* ========================================================================== */

/*
 * Append child to the element list of arr.
 *
 * One return code carries one rule of ownership. ccol_invalid_args ALWAYS
 * means that the call rejected its arguments and touched nothing. The
 * caller still owns child, or the container that child was already attached
 * to still owns it. That owner must still destroy child. Every other
 * non-success code, which is ccol_not_enough_memory or
 * ccol_container_full, ALWAYS means that the ownership transferred and that
 * the call already destroyed child. A second free of child is then a double
 * free. Without this split a caller cannot act on a code at all. A free on
 * ccol_invalid_args would double-free the rejections. No free on
 * ccol_not_enough_memory would leak the transfers.
 *
 * child must not already be attached to a list parent or a dictionary
 * parent. This covers arr itself. It also covers the chain of ancestors of
 * arr. That is, child must not already contain arr somewhere inside its own
 * subtree. node_reaches() below checks that. Two parent slots that reach
 * one node each destroy that node in their own teardown, which corrupts the
 * heap. That is a double free, and under the default allocator with its
 * thread-local node pool it also makes a free-list cycle that points at
 * itself. A cycle in the graph, where child can already reach arr, corrupts
 * the heap in the same way at teardown. A free of one node along the cycle
 * does not make that node unreachable, because the cycle has its own back
 * edge.
 *
 * Five conditions therefore give ccol_invalid_args. child is NULL. child is
 * already attached. child is arr itself. arr is NULL, or arr is not a
 * CJSON_LIST. An attach of child would close a cycle that the check finds.
 * Two outcomes transfer ownership: the cycle check runs out of memory, and
 * the insert itself fails.
 */
ccol_retval_t cjson_list_push(cjson arr, cjson child) {
  if (!child) return ccol_invalid_args;
  cjson_node_t *c = (cjson_node_t *)child;

  /* Every rejection that must leave child completely untouched is decided
   * BEFORE the check of the container kind below. That check takes
   * ownership of a child that is not attached and deep-frees it. Both
   * conditions can hold at the same time. That happens when a caller passes
   * one handle as both arr and child, and that handle is not a CJSON_LIST.
   * To answer the branch that destroys first would free a node that the
   * caller still owns. It would also return the very code that the
   * documentation calls "left completely untouched". The comparison is
   * against arr and not against n. The check therefore still holds when arr
   * has the wrong kind of node. */
  if (c->attached || (const void *)c == (const void *)arr) {
    /* Something else already owns child, or child is arr itself. To accept
     * either one would give one node two owners. Each owner then frees the
     * node when its own parent is destroyed. */
    return ccol_invalid_args;
  }

  cjson_node_t *n = (arr && ((cjson_node_t *)arr)->type == CJSON_LIST)
                        ? (cjson_node_t *)arr
                        : NULL;
  if (!n) {
    /* arr is NULL or is not a CJSON_LIST. This is a rejection of an
     * argument, so child stays exactly as it was. A destroy here would make
     * ccol_invalid_args mean "untouched" on one path and "already freed" on
     * another. A caller that acts on the code would then either double-free
     * or leak, and which one it does depends on the path that it hit. */
    return ccol_invalid_args;
  }
  bool cycle_incomplete;
  if (node_reaches(c, (const cjson_node_t *)n, &cycle_incomplete)) {
    /* The subtree of child can already reach arr. To attach child here
     * would make arr a new ancestor of child. arr would also stay a
     * descendant of child. That is a real cycle. Reject the call and touch
     * neither node. Read the doc comment of node_reaches() above. */
    return ccol_invalid_args;
  }
  if (cycle_incomplete) {
    /* The cycle check itself could not run to its end, because it ran out
     * of memory. Refuse the call. To continue would risk a silent cycle
     * that nothing found. child is certainly not attached here.
     * ccol_not_enough_memory is an outcome that transfers ownership for
     * this function. The call therefore destroys child, exactly as it does
     * when the insert below fails for lack of memory. One return code has
     * to carry one rule of ownership. If this path left child alive while
     * the insert path freed it, no caller could act correctly on
     * ccol_not_enough_memory. A free would double-free one case, and no
     * free would leak the other. */
    __cjson_destroy(child);
    return ccol_not_enough_memory;
  }
  cvec store = list_store(n);
  ccol_retval_t r =
      store ? cvector_push_back(store, &c) : ccol_not_enough_memory;
  if (r != ccol_success) {
    /* A failed insert transfers ownership. The caller therefore never has
     * to track child across this error path. Every code that can arrive
     * here transfers ownership. The insert can report ccol_invalid_args
     * only for a NULL element pointer, and the element that it gets is the
     * address of a local variable. A store that cannot be created is a
     * failed insert. */
    __cjson_destroy(child);
    return r;
  }
  c->attached = true;
  return ccol_success;
}

/* Return the element at the position index. That element is a borrowed
 * reference. Do not destroy it on its own, apart from the parent list. The
 * function returns NULL when the index is out of bounds. It also returns
 * NULL when arr is NULL or is not a CJSON_LIST node. */
cjson cjson_list_get(cjson arr, size_t index) {
  if (!arr) return NULL;
  cjson_node_t *n = (cjson_node_t *)arr;
  if (n->type != CJSON_LIST) return NULL;
  return (cjson)list_child_at(n, index);
}

/* Replace the child that the dictionary slot slot_vp holds with child. slot_vp
 * is the accessor that ccol_chmap_insert_or_get_elem() gave for a key that is
 * already present. The function frees the old child first and then writes the
 * new pointer into the slot, so the slot never names freed memory once this
 * returns. Freeing the old child changes no map that holds slot_vp, so the
 * accessor stays valid across the free. child must have no parent, and it
 * becomes attached here. */
static void dict_slot_replace_child(const cmap_pair *slot_vp,
                                    cjson_node_t *child) {
  cjson_node_t *old_child = _cjson_read_child(slot_vp->ptr);
  __cjson_destroy((cjson)old_child);
  memcpy((void *)slot_vp->ptr, &child, sizeof(child));
  child->attached = true;
}

/*
 * Insert or replace the value for key in obj. The split of ownership is
 * exactly the split of cjson_list_push. ccol_invalid_args ALWAYS means that
 * the function rejected the arguments and that child is untouched. Every
 * other non-success code ALWAYS means that the ownership transferred and
 * that the function already destroyed child. When the key already exists,
 * the function frees the old child and points the slot at child, with no
 * failure possible between the two. The slot never holds a dangling pointer
 * once the function returns. A replaced member keeps its place in the
 * insertion order, and a new key goes after every other member.
 *
 * child must not already be attached to a list parent or a dictionary
 * parent. This covers obj itself. It also covers the chain of ancestors of
 * obj. An attach of child would make obj a new ancestor of child while obj
 * stays a descendant of it, and that is a cycle. There is one
 * exception. child can be exactly the value that key already holds. Read
 * the doc comment of cjson_list_push for the reason why the function
 * otherwise rejects a child that is already attached. To accept such a
 * child would give one node two owners. Each owner destroys the node on its
 * own, which corrupts the heap. A cycle corrupts the heap in the same way,
 * because a free of an ancestor does not make that ancestor unreachable
 * through the back edge of the cycle. The function accepts the case where
 * key already holds child as a harmless no-op. That is deliberate, so that
 * `cjson_dictionary_set(obj, k, cjson_dictionary_get(obj, k))` succeeds.
 * That call sets a key to the value that it already holds. It is not an
 * illegal move of a child to a new parent.
 */
ccol_retval_t cjson_dictionary_set(cjson obj, const char *key, cjson child) {
  if (!child) return ccol_invalid_args;
  cjson_node_t *c = (cjson_node_t *)child;

  /* The function rejects a self-attach and touches child in no way. It must
   * decide that BEFORE the check of the container kind below. That check
   * takes ownership of a child that is not attached and deep-frees it. Both
   * conditions hold at the same time when a caller passes one handle as
   * both obj and child and that handle is not a CJSON_DICTIONARY. To answer
   * the branch that destroys first would free a node that the caller still
   * owns. It would also return the very code that the documentation calls
   * "left completely untouched". The comparison is against obj and not
   * against n. The check therefore still holds when obj has the wrong kind
   * of node. Only the self-check sits up here. The rejection of an
   * already-attached child must stay below the c == old_child no-op. Only
   * an attached child can reach that no-op; read the comment of that
   * check. */
  if ((const void *)c == (const void *)obj) return ccol_invalid_args;

  cjson_node_t *n = (obj && ((cjson_node_t *)obj)->type == CJSON_DICTIONARY)
                        ? (cjson_node_t *)obj
                        : NULL;
  if (!n || !key || !utf8_cstr_is_valid(key)) {
    /* obj is NULL or is not a CJSON_DICTIONARY, or key is NULL or is not
     * valid UTF-8. This is a rejection of an argument, so child stays
     * exactly as it was. This is true both for a free-standing tree that
     * the caller still owns and for a node that another container already
     * owns. A key that is not valid UTF-8 has no JSON form, and the
     * serializer would emit it raw. The function refuses it for the same
     * reason as it refuses a string value; read cjson_create_string_mp().
     * A destroy of child here would make ccol_invalid_args mean "untouched"
     * on one path and "already freed" on another. A caller that acts on the
     * code would then either double-free or leak, and which one it does
     * depends on the path that it hit. */
    return ccol_invalid_args;
  }

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};

  if (c->attached) {
    /* Only an attached child can be the node that key already holds, so
     * only this branch needs to look the key up before it decides. A call
     * that sets a key to the value that it already holds is a harmless
     * no-op. Any other attached child belongs to another parent: reject the
     * call, and touch neither child nor the slot. The top of this function
     * already rejects child == obj, before the check of the container
     * kind. */
    const cmap_pair *held_vp = NULL;
    if (dict_lookup(n, &kp, &held_vp) == ccol_success &&
        _cjson_read_child(held_vp->ptr) == c)
      return ccol_success;
    return ccol_invalid_args;
  }

  bool cycle_incomplete;
  if (node_reaches(c, (const cjson_node_t *)n, &cycle_incomplete)) {
    /* The subtree of child can already reach obj. To attach child here
     * would make obj a new ancestor of child. obj would also stay a
     * descendant of child. That is a real cycle. Reject the call and touch
     * neither node. Read the doc comment of node_reaches(). */
    return ccol_invalid_args;
  }
  if (cycle_incomplete) {
    /* The cycle check itself could not run to its end, because it ran out
     * of memory. Refuse the call. To continue would risk a silent cycle
     * that nothing found. child is certainly not attached here.
     * ccol_not_enough_memory is an outcome that transfers ownership for
     * this function. The call therefore destroys child, exactly as it does
     * when the insert below fails for lack of memory. One return code has
     * to carry one rule of ownership. If this path left child alive while
     * the insert path freed it, no caller could act correctly on
     * ccol_not_enough_memory. A free would double-free one case, and no
     * free would leak the other. */
    __cjson_destroy(child);
    return ccol_not_enough_memory;
  }

  /* Every refusal is decided above, so nothing below can leave child
   * untouched. One hash and one probe either insert child under a new key
   * or hand back the slot of the key that is already present. */
  cmap_pair vp = {.ptr = &c, .size = sizeof(c)};
  const cmap_pair *old_vp = NULL;
  chmap store = dict_store(n);
  ccol_retval_t r =
      store ? ccol_chmap_insert_or_get_elem(store, &kp, &vp, &old_vp)
            : ccol_not_enough_memory;
  if (r == ccol_success) {
    c->attached = true;
    return ccol_success;
  }
  if (r == ccol_key_already_present) {
    /* The map is unchanged. Free the old child, whose subtree cannot hold
     * child because child has no parent, then point the slot at child. */
    dict_slot_replace_child(old_vp, c);
    return ccol_success;
  }
  /* A failed insert transfers ownership, as it does in cjson_list_push.
   * Every code that can arrive here transfers ownership. The insert of the
   * map can report ccol_invalid_args only for a key pair or a value pair
   * that is NULL or has a size of zero. This function builds both pairs
   * here, from a key that is not empty and from the address of a local
   * variable. A store that cannot be created is a failed insert. */
  __cjson_destroy((cjson)child);
  return r;
}

/* Look up key in obj and return the child that it holds. That child is a
 * borrowed reference. The function returns NULL when it does not find the
 * key, when obj is NULL, or when obj is not a CJSON_DICTIONARY node. */
cjson cjson_dictionary_get(cjson obj, const char *key) {
  if (!obj || !key) return NULL;
  cjson_node_t *n = (cjson_node_t *)obj;
  if (n->type != CJSON_DICTIONARY) return NULL;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  const cmap_pair *vp = NULL;
  if (dict_lookup(n, &kp, &vp) != ccol_success) return NULL;
  return _cjson_read_child(vp->ptr);
}

/*
 * Remove and deep-free the element at the position index of a list.
 *
 * The function destroys the element at that index. It then moves every
 * element after it one position to the left. That move is O(n) in the
 * number of elements after the index. cvector_pop_back then makes the
 * vector one element shorter.
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
 * Remove and deep-free the entry with the given key from a dictionary.
 *
 * The function reads the child pointer out before it deletes the map entry.
 * It therefore frees the subtree only after the hash table no longer points
 * at it.
 */
ccol_retval_t cjson_dictionary_remove(cjson obj, const char *key) {
  if (!obj || !key) return ccol_invalid_args;
  cjson_node_t *n = (cjson_node_t *)obj;
  if (n->type != CJSON_DICTIONARY) return ccol_invalid_args;
  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  const cmap_pair *vp = NULL;
  if (dict_lookup(n, &kp, &vp) != ccol_success) return ccol_key_not_found;
  cjson_node_t *child = _cjson_read_child(vp->ptr);
  ccol_retval_t r = chmap_delete_elem(n->value.dictionary, &kp);
  if (r == ccol_success) __cjson_destroy((cjson)child);
  return r;
}

/* ========================================================================== */
/*                         DICTIONARY ITERATION                               */
/* ========================================================================== */

/* The cursor reads the successor of a member at the moment it steps onto
 * that member. A removal of the current member frees only that member's
 * entry, so the successor that the cursor holds stays valid. */
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
 * This is the maximum depth of nesting that cjson_clone() accepts, counted
 * in containers: a tree may hold this many lists and dictionaries inside
 * one another, and any value inside the innermost of them. A tree that a
 * caller gives to cjson_clone() need not come from cjson_parse() at all. The
 * public cjson_list_push() and cjson_dictionary_set() API can build such a
 * tree directly, to any depth. CJSON_MAX_PARSE_DEPTH therefore says nothing
 * about it, because that constant only bounds what the parser accepts, and
 * this file defines it further down. This constant has the same value, so a
 * tree exactly at the limit of the parser can still be cloned and does not
 * fail for no good reason. It is its own constant only because this file
 * defines CJSON_MAX_PARSE_DEPTH further down.
 *
 * This is a policy limit and not a limit of the stack. The clone below
 * walks the tree with an explicit stack of frames on the heap. It uses a
 * fixed, small amount of native stack at any depth.
 */
#define CJSON_CLONE_MAX_DEPTH 500

/*
 * One container that the explicit stack of clone_tree() fills.
 *
 * src   : the source container. Its children are not copied yet.
 * dst   : the copy that this frame fills. It is already attached to its own
 *         parent.
 * depth : the depth of nesting of dst. The cap below therefore applies to
 *         exactly the same nodes as a recursive clone would apply it to.
 * taken : the count of list elements that the frame copied so far. Only a
 *         list uses this.
 * next  : for a dictionary, the next member of src to copy, in insertion
 *         order. It is NULL once every member is copied, and when src is
 *         empty.
 *
 * Recursion once for each level of nesting would make the peak use of the
 * native stack a function of the depth of the source tree. For a tree that
 * the public API builds, the caller controls that depth and nothing bounds
 * it.
 */
typedef struct {
  cjson_node_t *src;
  cjson_node_t *dst;
  unsigned int depth;
  size_t taken;
  const ccol_chmap_entry_ref *next;
} clone_frame_t;

/* Push one frame and grow the stack when it is full. Read the doc comment
 * of serialize_stack_push(). This function works in the same way. */
static bool clone_stack_push(clone_frame_t **frames,
                             clone_frame_t *inline_frames, size_t inline_cap,
                             size_t *cap, size_t *len,
                             const clone_frame_t *frame) {
  if (*len == *cap) {
    size_t new_cap = *cap * 2;
    clone_frame_t *grown;
    if (*frames == inline_frames) {
      grown = ccol_mem_alloc(new_cap * sizeof(**frames));
      if (!grown) return false;
      memcpy(grown, inline_frames, inline_cap * sizeof(**frames));
    } else {
      grown = ccol_mem_realloc(*frames, new_cap * sizeof(**frames));
      if (!grown) return false;
    }
    *frames = grown;
    *cap = new_cap;
  }
  (*frames)[(*len)++] = *frame;
  return true;
}

/* Copy the value of one node, without its children. The function copies a
 * scalar completely. It creates a container empty, and the caller fills it.
 * It returns NULL when an allocation fails. */
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
    case CJSON_STRING:
      return (cjson_node_t *)cjson_create_string_interned(src->value.string,
                                                          mp);
    case CJSON_LIST:
      return (cjson_node_t *)cjson_create_list_interned(mp);
    case CJSON_DICTIONARY:
      return (cjson_node_t *)cjson_create_dictionary_interned(mp);
  }
  return NULL;
}

/*
 * Make an independent deep copy of the subtree whose root is src. Use the
 * same allocator, which is the m_procs of the source. The function returns
 * NULL when an allocation fails, and also when the depth goes past
 * CJSON_CLONE_MAX_DEPTH. It destroys any half-built copy before it returns.
 *
 * The function creates each container empty, attaches it to its own parent,
 * and only then fills it. That order is what lets the walk be a loop over
 * an explicit stack of frames. It needs no native frame for each level of
 * nesting.
 *
 * The function stores a child into the container of its parent directly. It
 * does not go through cjson_list_push() or cjson_dictionary_set(). The
 * parser does the same, for the same reason: every check that those two run
 * is already settled here. The copy is new and not attached. It cannot
 * contain its own parent. The destination is a container that this call
 * just built. The key comes out of the source map, so it differs from every
 * key that the call already inserted, and it is already valid UTF-8. The
 * members of a source dictionary are copied in insertion order, so the copy
 * keeps the order of its source.
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
  unsigned int cur_depth = 0;

  for (;;) {
    /* cur_depth counts the containers around cur, so a container at
     * cur_depth is level cur_depth + 1. Only a container adds a level. */
    if (cur_depth >= CJSON_CLONE_MAX_DEPTH &&
        (cur->type == CJSON_LIST || cur->type == CJSON_DICTIONARY))
      goto fail;

    cjson_node_t *copy = (cjson_node_t *)clone_shallow(cur);
    if (!copy) goto fail;

    if (!parent_dst) {
      root = copy;
    } else if (parent_key) {
      cmap_pair kp = {.ptr = (void *)parent_key,
                      .size = strlen(parent_key) + 1};
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

    /* Move to the next source child of the innermost open container. Pop
     * that container after the copy of it is complete. */
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
        /* parent_key points at the key that the source map stores. The
         * source does not change during the clone, so it stays valid for
         * the attach above. */
        const cmap_pair *kp, *vp;
        f->next = ccol_chmap_entry_read(f->next, &kp, &vp);
        cur = _cjson_read_child(vp->ptr);
        parent_dst = f->dst;
        parent_key = (const char *)kp->ptr;
        cur_depth = f->depth + 1;
        break;
      }
      len--;
    }
  }

fail:
  if (frames != inline_frames) ccol_mem_free(frames);
  __cjson_destroy((cjson)root);
  return NULL;

done:
  if (frames != inline_frames) ccol_mem_free(frames);
  return root;
}

/* Make an independent deep copy of the whole subtree whose root is node.
 * Read the doc comment of CJSON_CLONE_MAX_DEPTH above. The function reports
 * a tree with more than CJSON_CLONE_MAX_DEPTH levels of nesting as a
 * failure of an allocation, which is a NULL. That is how it reports every
 * other failure of a clone. */
cjson cjson_clone(cjson node) {
  if (!node) return NULL;
  return (cjson)clone_tree((cjson_node_t *)node);
}

/* ========================================================================== */
/*                         PARSER                                             */
/* ========================================================================== */

/*
 * This is the state that every parse function of the recursive descent
 * receives.
 *
 * src / pos / len : the source text and the current offset in bytes.
 * error           : a message for a person, which parse_err() fills. It
 *                   means something only after a parse function returns
 *                   NULL or false.
 * mp              : the allocator for every node and every string.
 *
 * The parser makes one pass over the whole JSON grammar of RFC 8259. It
 * builds no separate stream of tokens. Each sub-parser reads from src
 * directly, through pos. The parser walks the nesting with an explicit
 * stack of frames and not with recursion; read parse_frame_t below. The
 * depth of a document therefore does not decide how much native stack a
 * parse needs.
 */
typedef struct {
  const char *src;
  size_t pos;
  size_t len;
  char error[512];
  ccol_memmgmt_procs_t *mp;
} parse_ctx_t;

/*
 * This is the maximum depth of nesting that cjson_parse() and the functions
 * beside it accept. A document may hold this many '[' or '{' containers
 * inside one another, and any value, scalar or empty container, inside the
 * innermost of them. One more container inside that is a parse error that
 * names the limit. A value of 500 covers any real JSON
 * document with room to spare, both from a person and from a machine.
 *
 * This is a policy limit on the documents that the library accepts. The
 * native stack does not impose it. parse_value() walks the nesting with an
 * explicit stack of frames on the heap. A document at the cap therefore
 * costs the same fixed, small amount of native stack as a flat one.
 */
#define CJSON_MAX_PARSE_DEPTH 500

/* Format a parse error for a person into ctx->error. Only the message of
 * the last call stays. The function silently overwrites an earlier message.
 *
 * The declaration carries __attribute__((format(printf, 2, 3))). Nothing
 * suppresses -Wformat-nonliteral at the vsnprintf call site. Every call in
 * this file passes a string literal. The attribute therefore lets the
 * compiler check the format string of each call site against the arguments
 * of that call. That catches a real class of bug, for example a "%d" for a
 * size_t. A suppression would only hide the warning. */
static void parse_err(parse_ctx_t *ctx, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void parse_err(parse_ctx_t *ctx, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
  va_end(ap);
}

/* True when a byte of the input may appear as it is inside an error
 * message. Every message that quotes an input byte prints any other byte as
 * 0xNN. A message therefore stays printable ASCII, and so valid UTF-8,
 * whatever the document holds. A raw byte of 0x80 or above would make the
 * message invalid UTF-8, which a JSON or syslog log sink rejects or
 * mangles, and a raw control byte, or a NUL that cjson_parse_n() can carry,
 * would corrupt or truncate it. */
static bool parse_err_byte_is_printable(unsigned char b) {
  return b >= 0x20 && b <= 0x7E;
}

/* Move ctx->pos past any JSON whitespace. That is a space, a tab, a CR or
 * an LF. */
static void skip_ws(parse_ctx_t *ctx) {
  while (ctx->pos < ctx->len) {
    char c = ctx->src[ctx->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      ctx->pos++;
    else
      break;
  }
}

/* Skip the whitespace and store the next character in *out. Do not consume
 * that character. The function returns false at the end of the input. */
static bool peek(parse_ctx_t *ctx, char *out) {
  skip_ws(ctx);
  if (ctx->pos >= ctx->len) return false;
  *out = ctx->src[ctx->pos];
  return true;
}

/* Consume the next character that is not whitespace and return true, when
 * that character is the expected one. */
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
/* Consume the literal "null" token and return a CJSON_NULL node. */
static cjson_node_t *parse_null(parse_ctx_t *ctx) {
  if (ctx->pos + 4 > ctx->len || memcmp(ctx->src + ctx->pos, "null", 4) != 0) {
    parse_err(ctx, "expected 'null' at position %zu", ctx->pos);
    return NULL;
  }
  ctx->pos += 4;
  return node_alloc(CJSON_NULL, ctx->mp);
}

/* bool */
/* Consume "true" or "false" and return the CJSON_BOOL node for it. */
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
 * Parse a JSON number as RFC 8259 section 6 states.
 *
 * An integer has no decimal point and no exponent. The function stores an
 * integer that fits in a long long as a CJSON_INTEGER. Everything else
 * becomes a CJSON_FLOAT. A leading zero must have no more digits after it.
 * An integer literal that overflows a long long falls back to a
 * CJSON_FLOAT, through strtod. The function rejects a value that would give
 * an infinite double.
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
    /* RFC 8259 sec. 6: a leading zero must have no more digits after it. */
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
   * Build a null-terminated token for strtoll or strtod. A stack buffer of
   * 360 bytes handles every real JSON number and needs no allocation. It
   * covers the full decimal form of any IEEE 754 double, which is about 328
   * characters at its longest. It also covers any LLONG_MIN, which is 20
   * characters. RFC 8259 sec. 6 puts no limit on the length of a number
   * literal. A token with a correct syntax can therefore be much longer.
   * One example is an ordinary, finite value with many leading or trailing
   * zeros that it does not need. The function still accepts such a token.
   * It allocates a buffer on the heap of exactly the size of that token,
   * and it does not reject the token.
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
    /* Try an integer first. Fall back to a double when the value is out of
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

/* Encode a Unicode code point as UTF-8 into an sbuf_t. */
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

/* Read exactly 4 hexadecimal digits and give the value of the code
 * point. */
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

/* Check that the raw bytes src[start..end) of a string are well-formed
 * UTF-8. On a defect, report it with parse_err(), naming the bytes as 0xNN
 * and the position of the first of them, and return false. */
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
 * Parse a JSON string, from the opening '"' to the closing '"'. Store the
 * result in *out as a C string on the heap. The function allocates with
 * ctx->mp. Raw bytes that are not well-formed UTF-8, and a \uXXXX escape for
 * a surrogate that is not one half of a high-low pair, are parse errors.
 */
static bool parse_string_raw(parse_ctx_t *ctx, char **out) {
  *out = NULL;
  if (ctx->pos >= ctx->len || ctx->src[ctx->pos] != '"') {
    parse_err(ctx, "expected '\"' at position %zu", ctx->pos);
    return false;
  }
  ctx->pos++; /* skip the opening quote */

  /* Scan first. Find the closing '"'. Also check for an escape or a control
   * character. */
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
      /* Only a byte that is not ASCII can start an ill-formed UTF-8
       * sequence. A literal of pure ASCII, which is the common case,
       * therefore skips the checks below completely. */
      has_non_ascii = true;
    }
    scan++;
  }

  if (scan >= ctx->len) {
    parse_err(ctx, "unterminated string starting before position %zu",
              ctx->pos - 1);
    return false;
  }

  if (!need_slow) {
    /* This is the fast path. There is no escape and no control character,
     * so the string is its raw bytes. One allocation and one memcpy are
     * enough once those bytes are known to be well-formed UTF-8. */
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

  /* This is the slow path. The string has an escape or a control
   * character. */
  sbuf_t sb;
  sb_init_hint(&sb, ctx->mp, scan - ctx->pos);

  while (ctx->pos < ctx->len) {
    /* Copy a run of plain characters in one block, up to the next special
     * one. */
    size_t chunk_start = ctx->pos;
    while (ctx->pos < ctx->len) {
      unsigned char c2 = (unsigned char)ctx->src[ctx->pos];
      if (c2 == '"' || c2 == '\\' || c2 < 0x20) break;
      ctx->pos++;
    }
    if (ctx->pos > chunk_start) {
      /* An escape and a control character are ASCII, so a run between two
       * of them never splits a multi-byte sequence. Checking each run in
       * order reports the first defect of the string, whichever kind it
       * is. */
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
          /* A surrogate is a code point only as one half of a pair: a high
           * surrogate escape followed at once by a low surrogate escape.
           * Any other surrogate escape names no Unicode scalar value, and
           * the parser refuses it rather than store a replacement that
           * would make two different keys equal. */
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
          /* A \u0000 gives a null byte. A null-terminated C string has no
           * form for such a byte. The parser therefore rejects it. It does
           * not silently truncate the string at that null. */
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
      /* This is a control character, which is a byte below 0x20. */
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

/* This function wraps parse_string_raw(). It parses a JSON string literal.
 * It returns a CJSON_STRING node that owns the C string on the heap. */
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
 * One open '[' or '{' in the explicit stack of parse_value().
 *
 * node        : the CJSON_LIST or CJSON_DICTIONARY that the parser fills.
 * pending_key : for a dictionary, the owned key string whose value the
 *               parser reads at this moment. It is NULL at every other
 *               moment. That is what lets the failure path free exactly the
 *               keys that this function must still free.
 * is_dict     : which of the two kinds of container node is. The messages
 *               about a separator and about an unterminated container
 *               therefore name the right one.
 * started     : false until the parser consumes the first element or member
 *               of this container. It separates "an element can follow the
 *               opening bracket" from "a ',' must come between this element
 *               and the last one".
 *
 * A descent by recursion costs one native frame for each level of nesting.
 * It costs three, for the chain of parse_value, dispatch and container that
 * the grammar gives. The stack that a parse needs is then a function of the
 * depth of nesting of the input. Whoever gives the document chooses that
 * depth. At the cap above, such a recursion runs past a small thread stack,
 * for example the 128 KiB default of musl. It then crashes the process
 * instead of a return of the documented parse error.
 */
typedef struct {
  cjson_node_t *node;
  char *pending_key;
  bool is_dict;
  bool started;
} parse_frame_t;

/* Push one frame and grow the stack when it is full. Read the doc comment
 * of serialize_stack_push(). This function works in the same way. */
static bool parse_stack_push(parse_frame_t **frames,
                             parse_frame_t *inline_frames, size_t inline_cap,
                             size_t *cap, size_t *len,
                             const parse_frame_t *frame) {
  if (*len == *cap) {
    size_t new_cap = *cap * 2;
    parse_frame_t *grown;
    if (*frames == inline_frames) {
      grown = ccol_mem_alloc(new_cap * sizeof(**frames));
      if (!grown) return false;
      memcpy(grown, inline_frames, inline_cap * sizeof(**frames));
    } else {
      grown = ccol_mem_realloc(*frames, new_cap * sizeof(**frames));
      if (!grown) return false;
    }
    *frames = grown;
    *cap = new_cap;
  }
  (*frames)[(*len)++] = *frame;
  return true;
}

/* value dispatch */
/* Branch on the first character of a value that is not a container. Then
 * call the correct sub-parser. c is the character at ctx->pos, and the
 * peek() of the caller already found it. A '[' and a '{' never reach here,
 * because parse_value() takes them down its own iterative path. */
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
 * Parse one complete JSON value, and this includes every value nested
 * inside it. Return the node of that value. The function returns NULL on
 * any failure. ctx->error then carries the message, and the function frees
 * every node that it built so far.
 *
 * The function walks the nesting with the explicit stack of frames above
 * and not with recursion. The native stack that it needs is therefore the
 * same for a document at CJSON_MAX_PARSE_DEPTH and for a flat one. The walk
 * is four steps, and the structure of a document moves between them. The
 * steps are: parse a value; hand a complete value to the container that
 * waits for it; decide what follows inside the innermost open container;
 * and read a dictionary key.
 *
 * A dictionary key that comes twice silently overwrites the earlier one, so
 * the last writer wins. This follows the permissive guidance in RFC 8259
 * section 4.
 */
static cjson_node_t *parse_value(parse_ctx_t *ctx) {
  parse_frame_t inline_frames[32];
  parse_frame_t *frames = inline_frames;
  size_t cap = sizeof(inline_frames) / sizeof(inline_frames[0]);
  size_t len = 0;
  cjson_node_t *pending = NULL;
  char c;

value_step:
  if (!peek(ctx, &c)) {
    parse_err(ctx, "unexpected end of input at position %zu", ctx->pos);
    goto fail;
  }
  if (c == '[' || c == '{') {
    /* len is the number of containers that are open around this one. This
     * container is therefore level len + 1, and the cap counts containers
     * only: a scalar inside the deepest accepted container adds no level. */
    if (len >= CJSON_MAX_PARSE_DEPTH) {
      parse_err(ctx, "maximum nesting depth (%u) exceeded at position %zu",
                CJSON_MAX_PARSE_DEPTH, ctx->pos);
      goto fail;
    }
    parse_frame_t f = {.node = NULL,
                       .pending_key = NULL,
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
      /* No container around it owns this node yet. This call must
       * therefore still free it. */
      __cjson_destroy((cjson)f.node);
      goto fail;
    }
    goto next_step;
  }
  pending = parse_scalar_value(ctx, c);
  if (!pending) goto fail;

deliver_step:
  /* pending is a complete value. It is the root itself when no container
   * holds it. In every other case it is the next element or member of the
   * innermost open container. */
  if (len == 0) goto done;
  {
    parse_frame_t *f = &frames[len - 1];
    if (f->is_dict) {
      cmap_pair kp = {.ptr = f->pending_key,
                      .size = strlen(f->pending_key) + 1};

      /* One hash and one probe insert the member, or hand back the slot of
       * a duplicate key. A duplicate keeps the last value: the old child is
       * freed and the slot then points at the new one. */
      cmap_pair vp = {.ptr = &pending, .size = sizeof(pending)};
      const cmap_pair *existing_vp = NULL;
      chmap store = dict_store(f->node);
      ccol_retval_t r =
          store ? ccol_chmap_insert_or_get_elem(store, &kp, &vp, &existing_vp)
                : ccol_not_enough_memory;
      _ccol_mem_free(ctx->mp, f->pending_key);
      f->pending_key = NULL;
      if (r == ccol_key_already_present) {
        dict_slot_replace_child(existing_vp, pending);
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
  /* Decide what follows inside the innermost open container. It is the
   * closing bracket of that container, or a separator and one more element
   * or member. */
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
  /* This is a dictionary member. It is the key string, then a ':', then the
   * value. */
  {
    parse_frame_t *f = &frames[len - 1];
    skip_ws(ctx);
    if (ctx->pos >= ctx->len || ctx->src[ctx->pos] != '"') {
      parse_err(ctx, "expected string key at position %zu", ctx->pos);
      goto fail;
    }
    if (!parse_string_raw(ctx, &f->pending_key)) goto fail;
    if (!expect_char(ctx, ':')) goto fail;
    skip_ws(ctx);
    goto value_step;
  }

fail:
  /* Every frame on the stack owns its own container completely. A container
   * goes to its parent only after the parser consumes its closing bracket,
   * and the frame of that container is gone at that point. Nothing else can
   * therefore reach a container that is still open here. A free of only the
   * outermost one would leak every container nested inside it, together
   * with everything that the parser already put into them. This loop also
   * owns a key that the parser read and whose value never arrived. */
  for (size_t i = 0; i < len; i++) {
    _ccol_mem_free(ctx->mp, frames[i].pending_key);
    __cjson_destroy((cjson)frames[i].node);
  }
  pending = NULL;

done:
  if (frames != inline_frames) ccol_mem_free(frames);
  return pending;
}

/* public parse entry */
/*
 * Every public entry point of the parser shares this implementation.
 *
 * The function initializes the parse context. It then runs the descent from
 * parse_value(). It then checks that no content of any meaning follows the
 * root value. It rejects trailing garbage.
 *
 * On failure, *err_str gets an error message for a person, when err_str is
 * not NULL. The library owns that string and the caller never frees it; see
 * cjson_report_err() below. On success, the function sets *err_str to NULL.
 */
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
#define CJSON_ERR_BUF_LEN 512
static const char cjson_err_unstored[] =
    "parse failed; no memory was left to store the error message";

/* Copies msg into the per-thread buffer and points *err_str at it. It is a
 * no-op when the caller passed no err_str. */
static void cjson_report_err(char **err_str, const char *msg) {
  if (!err_str) return;
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  ccol_call_once(_pool_key_once, _do_pool_key_init);
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
    cjson_err_buf = buf;
    ccol_call_once(_pool_key_once, _do_pool_key_init);
    _pool_key_arm();
  }
  snprintf(cjson_err_buf, CJSON_ERR_BUF_LEN, "%s", msg);
  *err_str = cjson_err_buf;
#endif /* _CCOL_EMULATE_DARWIN_TLS */
}

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
      /* Every real rejection of the syntax in this parser reports its own
       * message with parse_err() before it returns a failure. Only one
       * thing can reach here with ctx.error still empty. That is a failure
       * of an allocation with no place of its own to report through. Its
       * sources are node_alloc(), a raw _ccol_mem_alloc(), and a failed
       * insert into a cvector or a chmap. Report that honestly. A message
       * of "unknown parse error" would sound alarming and would mislead the
       * reader. It would point at a malformed document and not at pressure
       * on memory. */
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
  /* Make sure that nothing of any meaning follows the root value. */
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

/* Parse a null-terminated JSON string. The mp can be NULL for the default
 * allocator. On failure, when err_str is not NULL, *err_str points at a
 * per-thread message that the library owns. The caller never frees it, and
 * it stays valid until the next failing parse on the same thread. */
cjson cjson_parse_mp(const char *json_str, char **err_str,
                     ccol_memmgmt_procs_t *mp) {
  if (!json_str) return parse_common(NULL, 0, err_str, mp);
  return parse_common(json_str, strlen(json_str), err_str, mp);
}

/* This function is like cjson_parse_mp, but it takes an explicit length in
 * bytes. The input therefore need not be null-terminated. Use it for a JSON
 * value that sits inside a larger buffer. */
cjson cjson_parse_n_mp(const char *json_str, size_t len, char **err_str,
                       ccol_memmgmt_procs_t *mp) {
  return parse_common(json_str, len, err_str, mp);
}

/* ========================================================================== */
/*                         SERIALIZER                                         */
/* ========================================================================== */

/*
 * Format a double into buf. Use the shortest decimal form that a parse
 * turns back into the same value.
 */
static void format_double(char *buf, size_t cap, double val) {
  if (!isfinite(val)) {
    snprintf(buf, cap, "null"); /* JSON has no Inf and no NaN */
    return;
  }
  _cjson_snprintf_g_c(buf, cap, false, val);
  if (_cjson_strtod_c(buf) != val) {
    _cjson_snprintf_g_c(buf, cap, true, val);
  }
  /* Make sure that the output looks like a floating-point literal. A parse
   * of it then gives a CJSON_FLOAT and not a CJSON_INTEGER. The %.Ng
   * conversion removes the decimal point for a whole number, and it turns
   * 1.0 into "1". A parse of that gives a CJSON_INTEGER. An added ".0"
   * prevents this. The longest case that this touches is +/-1e14. That is
   * 15 digits plus ".0\0", which is 18 bytes. That fits easily in the
   * 32-byte buf that the caller gives. */
  if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
    size_t len = strlen(buf);
    if (len + 2 < cap) {
      buf[len] = '.';
      buf[len + 1] = '0';
      buf[len + 2] = '\0';
    }
  }
}

/* Emit a newline and then (depth * indent) spaces, for indented output.
 * The function does nothing when indent == 0, which is the compact output
 * mode. It appends from a constant block of spaces, one block at a time, so
 * a line costs one append for each 128 bytes of indentation and not one for
 * each byte. A width that a size_t cannot hold is reported through the oom
 * flag, as every other failure of the serializer is. */
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

/* Append a string value with JSON escapes. Put a quote on each side of
 * it. */
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
 * Convert a long long to a decimal string. This costs less than snprintf.
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
 * This is the maximum depth of nesting that serialize_node() below accepts,
 * counted in containers exactly as CJSON_MAX_PARSE_DEPTH counts them. A
 * tree that a caller gives to cjson_serialize() or to
 * cjson_serialize_pretty() need not come from cjson_parse() at all. The
 * public cjson_list_push() and cjson_dictionary_set() API can build such a
 * tree directly, to any depth. CJSON_MAX_PARSE_DEPTH therefore says nothing
 * about it, because that constant only bounds what the parser accepts. This
 * constant has the same value of 500. A tree exactly at the limit of the
 * parser therefore still serializes and does not fail for no good reason.
 *
 * This is a policy limit and not a limit of the stack. serialize_node()
 * walks the tree with an explicit stack of frames on the heap. It uses a
 * fixed, small amount of native stack at any depth; read serialize_frame_t
 * below.
 */
#define CJSON_MAX_SERIALIZE_DEPTH 500

/*
 * One open container in the explicit stack of serialize_node().
 *
 * node   : the CJSON_LIST or CJSON_DICTIONARY whose children the serializer
 *          emits.
 * depth  : the depth of nesting of node. The indent of its children, and
 *          the indent of its own closing bracket, therefore come out
 *          exactly as they do from the matching recursive call.
 * emitted: the count of children that the serializer already emitted. It
 *          drives the commas between children. It also drives the test for
 *          "did this container hold anything", which decides the closing
 *          indent.
 * count  : the count of elements of a list. The serializer reads it when
 *          it pushes the frame. A dictionary frame leaves it 0.
 * next   : for a dictionary, the next member to emit, in insertion order.
 *          It is NULL once every member is emitted, and when the
 *          dictionary is empty.
 * is_dict: which of the two kinds of container node is. The frame holds it,
 *          so the step for each child does not follow the node pointer to
 *          read it.
 *
 * Recursion once for each level of nesting would make the peak use of the
 * native stack a function of the depth of the tree. For a tree that the
 * public API builds, the caller controls that depth and nothing bounds it.
 * At 500 levels such a recursion runs right past a small thread stack, for
 * example the 128 KiB default of musl. The cap on the depth above would
 * then hold only on a stack that is large enough to reach it.
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
 * Push one frame and grow the stack when it is full. The function returns
 * false when the allocation fails, and it then leaves the stack exactly as
 * it was.
 *
 * inline_frames is the caller's own small array on the stack. The function
 * uses it until a tree grows past it. *frames points at it until the first
 * growth. An ordinary document therefore serializes with no scratch
 * allocation at all.
 */
static bool serialize_stack_push(serialize_frame_t **frames,
                                 serialize_frame_t *inline_frames,
                                 size_t inline_cap, size_t *cap, size_t *len,
                                 const serialize_frame_t *frame) {
  if (*len == *cap) {
    size_t new_cap = *cap * 2;
    serialize_frame_t *grown;
    if (*frames == inline_frames) {
      grown = ccol_mem_alloc(new_cap * sizeof(**frames));
      if (!grown) return false;
      memcpy(grown, inline_frames, inline_cap * sizeof(**frames));
    } else {
      grown = ccol_mem_realloc(*frames, new_cap * sizeof(**frames));
      if (!grown) return false;
    }
    *frames = grown;
    *cap = new_cap;
  }
  (*frames)[(*len)++] = *frame;
  return true;
}

/* Emit the JSON text for one node that is not a container. This covers a
 * NULL pointer, which the function emits as the literal "null". */
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
      /* CJSON_NULL is the literal. The two kinds of container never reach
       * here, because serialize_node() takes them down its own path. These
       * three cases are listed one by one and not folded into a default.
       * -Wswitch therefore still reports a new kind of node that nothing
       * here handles. */
      sb_append_cstr(sb, "null");
      break;
  }
}

/*
 * Emit the JSON text for the subtree whose root is n into sb.
 * indent: the spaces for each level of indent. A 0 gives compact output
 *         and adds no whitespace.
 * depth:  the depth of nesting of n itself. The caller at the top level
 *         passes 0.
 *
 * The function is iterative. An explicit stack of frames stands in for the
 * recursion. The use of the native stack is therefore independent of the
 * depth of the tree; read the doc comment of serialize_frame_t. Every
 * failure goes through sb->oom, and this includes the cap on the depth and
 * a failed allocation of the scratch stack. cjson_serialize() and
 * cjson_serialize_pretty() already turn that one flag into the NULL that
 * their documentation promises.
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
    /* Emit the value at cur. The depth of nesting of cur is cur_depth. */
    if (sb->oom) goto done;
    if (cur && (cur->type == CJSON_LIST || cur->type == CJSON_DICTIONARY)) {
      if (cur_depth >= CJSON_MAX_SERIALIZE_DEPTH) {
        /* Read the doc comment of CJSON_MAX_SERIALIZE_DEPTH. cur_depth
         * counts the containers around cur, so this container is level
         * cur_depth + 1, one past the cap. The function refuses the tree,
         * and reports that through the same oom flag as every other
         * failure of the serializer. */
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

    /* Move on. Emit the next child of the innermost open container, or
     * close that container and continue with its own parent. */
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
  if (frames != inline_frames) ccol_mem_free(frames);
}

/* Serialize the node to compact JSON, which adds no whitespace. The
 * function returns a string on the heap. Free that string with
 * cjson_serialize_free() or with cjson_serialize_free_mp(). The function
 * returns NULL when an allocation fails. */
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

/* Serialize the node to indented JSON. indent is the number of spaces for
 * each level of nesting. A 0 falls back to 4. Free the result with
 * cjson_serialize_free() or with cjson_serialize_free_mp(). */
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

/* Free a string that cjson_serialize or cjson_serialize_pretty returned.
 * Use the same allocator that made that string. An mp of NULL means the
 * default allocator.
 */
void cjson_serialize_free_mp(char *s, ccol_memmgmt_procs_t *mp) {
  _ccol_mem_free(mp, s);
}

/* ========================================================================== */
/*                         PATH NAVIGATION                                    */
/* ========================================================================== */

/*
 * These are the escape sequences of a path. Every helper below that
 * navigates or sets uses them.
 *
 *   \.   -> a literal '.' in the key, and not a separator of the path
 *   \\   -> a literal '\' in the key
 *
 * A '\' before any other character stays unchanged. It passes through as it
 * is. The helper resolves an escape in one component, after it splits the
 * path on the separator.
 */

/* Return a pointer to the first '.' in s that carries no escape. Return
 * NULL when there is none. */
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

/* Return a pointer to the last '.' in s that carries no escape. Return NULL
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
 * Parse the decimal digits after the '#' of a "#N" path component, which
 * holds a list index. The parse is strict. digits points one byte past the
 * '#'. The function returns true and sets *out on success. It returns false
 * and leaves *out unspecified in four cases. The index is not a number. The
 * index is negative. The index is out of range. The component is a bare '#'
 * with no digits.
 *
 * A bare strtol() call accepts leading whitespace and an explicit '+' sign
 * before the digits. Both are part of its own documented grammar. Such a
 * call would therefore silently accept a component like "#  5" or "#+5" as
 * a well-formed index. That does not match the documented contract of this
 * path syntax, which rejects a malformed "#N" index and a component that is
 * not a number. The first byte here must already be an ASCII digit. That
 * rule removes whitespace, '+', '-' and the bare '#' at the start. The
 * strtol() call below can therefore never skip or read anything before the
 * digits that it consumes.
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
 * Walk a dot-separated path through a JSON tree. Return the node at the end
 * of the path. Return NULL when a component is absent.
 *
 * path_copy must be a copy of the path string that this function can write
 * to. The function replaces each '.' that carries no escape with a '\0' for
 * a short time. That carves out each component in place. It then resolves
 * the escapes of the component before it uses the component as a key.
 *
 * A '#' at the start addresses a list element. For example,
 * "items.#0.name" goes to the 'name' key of the first element of 'items'.
 * Two dots together, and a trailing dot, both give NULL.
 *
 * The function sets *empty_component to true when ANY component of the path
 * is empty, and it does this only when empty_component is not NULL. An
 * empty component comes from two dots together, as in "a..b", or from a
 * trailing dot. The position of that component does not matter, and an
 * earlier component that did not resolve does not matter either. The
 * function leaves the flag false for every other reason why navigate()
 * returns NULL. Those reasons are a key or an index that is truly absent, a
 * malformed "#N" index, and a type that does not match. It never sets the
 * flag on success.
 *
 * A caller can split ccol_invalid_args, which is an error of syntax, from
 * ccol_key_not_found, which is a component with a correct syntax that is
 * absent. This flag lets such a caller class an empty component in the same
 * way at any position. The function still scans the whole path for a later
 * empty component, even after an earlier, well-formed component does not
 * resolve and cur becomes NULL. A real error of syntax therefore never
 * hides behind a "not found" outcome for an earlier, unrelated component.
 * After cur is NULL, the function skips only the lookups in a dictionary or
 * a list, because there is nothing left to look a further component up in.
 */
static cjson navigate(cjson root, char *path_copy, bool *empty_component) {
  if (empty_component) *empty_component = false;
  cjson cur = root;
  char *p = path_copy;

  while (1) {
    char *dot = path_find_unescaped_dot(p);
    if (dot) *dot = '\0';

    /* This component is empty. It comes from two dots together, as in
     * "a..b", or from a trailing dot, as in "a.". This check always runs.
     * Read the doc comment of this function above. It gives the reason why
     * nothing may skip this check after cur is already NULL. */
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
          cur = (cjson)list_child_at(n, idx);
        }
      } else if (n->type == CJSON_DICTIONARY) {
        cmap_pair kp = {.ptr = p, .size = strlen(p) + 1};
        const cmap_pair *vp = NULL;
        if (dict_lookup(n, &kp, &vp) != ccol_success)
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

/* This is the public implementation of the cjson_get(root, path) macro. It
 * copies the path into a buffer that it can write to, before it hands that
 * buffer to navigate(). Nothing therefore ever changes the string of the
 * caller. */
cjson _cjson_get(cjson root, const char *path) {
  if (!root) return NULL;
  if (!path || path[0] == '\0') return root;

  ccol_memmgmt_procs_t *mp = ((cjson_node_t *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return NULL;
  cjson result = navigate(root, copy, NULL);
  _ccol_mem_free(mp, copy);
  return result;
}

/*
 * This is the public implementation of the cjson_set(root, path, value)
 * macro.
 *
 * The function splits the path on the LAST dot. That separates the path of
 * the parent from the key of the leaf. It copies the leaf string before it
 * frees the copy of the parent path. Without that order, a leaf that is a
 * direct suffix of the whole path gives a use-after-free.
 *
 * For CJSON_STRING, raw always names a pointer to the string. The
 * cjson_set() macro copies its argument into a local of the decayed,
 * unqualified type, so a string literal and a char array both arrive as a
 * pointer to their first character.
 *
 * When the key of the leaf already exists, the function changes that node
 * in place with node_reinit_scalar. In every other case it allocates a new
 * scalar node and inserts it. It never creates a container in between
 * automatically. The parent must already exist.
 *
 * Returns:
 *   ccol_success           - the function created or updated the leaf.
 *   ccol_invalid_args      - a NULL root or path. An empty path, or an
 *                            empty path component from a leading dot, a
 *                            trailing dot, or two dots together. A parent
 *                            whose type does not fit the leaf component,
 *                            which is a leaf without a '#N' on a list
 *                            parent, or any leaf on a scalar parent. A
 *                            malformed '#N' index, which is one that is not
 *                            a number, one that is negative, or a bare '#'.
 *                            Or a C type of val that cjson_set() does not
 *                            accept.
 *   ccol_key_not_found     - the parent path is absent. The function also
 *                            returns this for a list index that has a
 *                            correct syntax but is out of range. A list has
 *                            no way to grow itself to fit any index, and a
 *                            dictionary does. This therefore stays an
 *                            error, and the function does not create the
 *                            leaf.
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
    /* There is no dot at all. copy already holds an unchanged copy of path,
     * byte for byte. The function therefore reuses it directly as
     * leaf_copy. It does not pay for a second copy of the same string. */
    leaf_copy = copy;
  } else {
    *last_dot = '\0';
    if (copy[0] == '\0') {
      /* There is a leading dot, so the path of the parent is empty. That is
       * an error of syntax. */
      _ccol_mem_free(mp, copy);
      return ccol_invalid_args;
    }
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    if (!leaf_copy) {
      _ccol_mem_free(mp, copy);
      return ccol_not_enough_memory;
    }
  }

  /* Check the leaf component and resolve its escapes BEFORE any walk to the
   * path of the parent. An empty leaf comes from a trailing dot on the
   * whole path, as in "a.". Such a leaf is always an error of syntax, and
   * the function must always report it as ccol_invalid_args. This is also
   * true when the path of the parent does not resolve, as in "missing."
   * where "missing" does not exist. A parent that is truly absent must
   * never hide an error of syntax in the leaf behind a
   * ccol_key_not_found. */
  path_unescape_component(leaf_copy);
  /* The leaf is empty, or it is not valid UTF-8. This call may have to
   * CREATE a dictionary key, and that key must be one that the DOM can
   * hold. Every key that the DOM holds is valid UTF-8; read the UTF-8
   * section of this file. Without this check, the function stores an
   * ill-formed component as a key, and the serializer emits it raw.
   * _cjson_get() and _cjson_delete() need no check like this. They only
   * ever look a component up, and an ill-formed component can never match a
   * key that is already there. */
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
    bool empty_component = false;
    parent = navigate(root, copy, &empty_component);
    _ccol_mem_free(mp, copy);
    if (!parent) {
      _ccol_mem_free(mp, leaf_copy);
      /* An empty component in the middle of the path, as in "a..b", is an
       * error of syntax. The function classes it in the same way as the
       * empty leaf component above. It does not mix it with an ordinary
       * component that is well-formed but absent. */
      return empty_component ? ccol_invalid_args : ccol_key_not_found;
    }
  }

  cjson_node_t *pn = (cjson_node_t *)parent;
  ccol_retval_t ret;

  if (pn->type == CJSON_DICTIONARY) {
    cmap_pair kp = {.ptr = (void *)leaf_comp, .size = strlen(leaf_comp) + 1};
    /* Refuse a bad payload before the map changes at all, so that a refusal
     * never needs to undo an insert. */
    ret = scalar_payload_check(type, raw, raw_size);
    if (ret == ccol_success) {
      /* One hash and one probe either find the leaf or insert the key with
       * a NULL placeholder child. A leaf that exists is updated in place, so
       * a borrowed handle to it stays valid. For a new key, nothing runs
       * between the insert and the write of the real child except the build
       * of that child, and that build reads no map of this tree. */
      cjson_node_t *placeholder = NULL;
      cmap_pair vp = {.ptr = &placeholder, .size = sizeof(placeholder)};
      const cmap_pair *slot = NULL;
      chmap store = dict_store(pn);
      ccol_retval_t r =
          store ? ccol_chmap_insert_or_get_elem(store, &kp, &vp, &slot)
                : ccol_not_enough_memory;
      if (r == ccol_key_already_present) {
        cjson_node_t *existing = _cjson_read_child(slot->ptr);
        ret = node_reinit_scalar_checked(existing, type, raw, raw_size,
                                         is_signed);
      } else if (r == ccol_success) {
        cjson_node_t *new_node;
        ret = node_make_scalar(type, raw, raw_size, is_signed, pn->m_procs,
                               &new_node);
        if (ret == ccol_success) {
          memcpy((void *)slot->ptr, &new_node, sizeof(new_node));
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
          /* The index has a correct syntax, but the element itself is
           * absent. That is a "not found" condition and not an error of
           * syntax in the path. _cjson_delete() classes the same situation
           * in the same way; read its own list branch below. A list has no
           * way to grow itself to fit any index, and a dictionary does.
           * This therefore stays a real error. Both of these functions that
           * change a tree through a path class it in the same way. */
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
 * This is the public implementation of the cjson_delete(root, path) macro.
 *
 * The function splits the path on the LAST dot that carries no escape. That
 * gives the parent node and the key of the leaf. When the path has no dot,
 * root is the parent. The function resolves the escapes of the key of the
 * leaf before it uses that key. A '\.' and a '\\' therefore work exactly as
 * they do in cjson_get and cjson_set.
 *
 * The function removes and deep-frees the node that the path addresses.
 * Returns:
 *   ccol_success          - the function removed and freed the node.
 *   ccol_invalid_args     - a NULL root or path. An empty path, or an empty
 *                           path component from a leading dot, a trailing
 *                           dot, or two dots together. A leaf component
 *                           without a '#N' on a list parent. Or a malformed
 *                           '#N' index, which is one that is not a number,
 *                           one that is negative, or a bare '#'.
 *   ccol_key_not_found    - the parent path is absent. The function also
 *                           returns this for a key of a leaf, or an index,
 *                           that has a correct syntax and is absent.
 *   ccol_not_enough_memory - the copy of a string failed.
 */
ccol_retval_t _cjson_delete(cjson root, const char *path) {
  if (!root || !path || path[0] == '\0') return ccol_invalid_args;

  ccol_memmgmt_procs_t *mp = ((cjson_node_t *)root)->m_procs;
  char *copy = ccol_strdup(mp, path);
  if (!copy) return ccol_not_enough_memory;

  char *last_dot = path_find_last_unescaped_dot(copy);
  char *leaf_copy;

  if (!last_dot) {
    /* There is no dot at all. copy already holds an unchanged copy of path,
     * byte for byte. The function therefore reuses it directly as
     * leaf_copy. It does not pay for a second copy of the same string. */
    leaf_copy = copy;
  } else {
    *last_dot = '\0';
    if (copy[0] == '\0') {
      /* There is a leading dot, so the path of the parent is empty. That is
       * an error of syntax. */
      _ccol_mem_free(mp, copy);
      return ccol_invalid_args;
    }
    leaf_copy = ccol_strdup(mp, last_dot + 1);
    if (!leaf_copy) {
      _ccol_mem_free(mp, copy);
      return ccol_not_enough_memory;
    }
  }

  /* Check the leaf component and resolve its escapes BEFORE any walk to the
   * path of the parent. An empty leaf comes from a trailing dot on the
   * whole path, as in "a.". Such a leaf is always an error of syntax, and
   * the function must always report it as ccol_invalid_args. This is also
   * true when the path of the parent does not resolve, as in "missing."
   * where "missing" does not exist. A parent that is truly absent must
   * never hide an error of syntax in the leaf behind a
   * ccol_key_not_found. */
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
    bool empty_component = false;
    parent = navigate(root, copy, &empty_component);
    _ccol_mem_free(mp, copy);
    if (!parent) {
      _ccol_mem_free(mp, leaf_copy);
      /* Read the same comment in _cjson_set_typed(). An empty component in
       * the middle of the path is an error of syntax. It is not an ordinary
       * component that is well-formed but absent. */
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
      } else if (idx >= list_len_of(pn)) {
        /* The index has a correct syntax, but the element itself is absent.
         * That is a "not found" condition under the documented contract of
         * this function, which promises ccol_key_not_found when any path
         * component is absent. It is not an error of syntax. This function
         * checks the index here. It does not leave the check to
         * cjson_list_remove(), which returns ccol_invalid_args for an index
         * out of bounds. That return is correct for a direct call to that
         * function. Here it would bring that narrower contract into this
         * function, whose own documentation says something different. */
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
