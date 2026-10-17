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

#include <assert.h>
#include <cbstmap.h>
#include <internal/test_helpers/cbstmap.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Upper bound on the height of any AVL tree this library can hold. An AVL
 * tree is never taller than about 1.44 * log2(n + 2), so this bound covers
 * every element count a 64-bit size_t can hold with a wide margin. Every
 * internal stack in this file (the ancestor path of an insert, a delete or a
 * rebalance, the post-order destroy stack, and the in-order iterator's
 * stack) holds at most one entry per tree level, so a fixed array of this
 * size can never overflow for a real AVL tree. That lets these hot paths use
 * no heap memory at all, where a heap allocation on every change to the map
 * could fail. A fixed array on the stack is a small price for a delete
 * that can't run out of memory. */
#define CBMAP_MAX_TREE_HEIGHT 128

/* The alignment a key of this declared type needs. We take it with _Alignof
 * on the type the enumerator names, so the answer comes from the target
 * itself and doesn't assume alignment equals width; the two differ on more
 * than one supported target.
 *
 * ccol_other_types is an opaque caller struct whose real requirement this
 * module can't know, and a type we don't recognize is treated the same way:
 * both keep max_align_t. A string key stores its bytes and not a pointer to
 * them, which is why char alignment is right for it. */
static inline size_t cbmap_key_alignment(ccol_data_type type) {
  switch (type) {
    case ccol_char:
    case ccol_signed_char:
    case ccol_unsigned_char:
    case ccol_string:
      return _Alignof(char);
    case ccol_short:
    case ccol_unsigned_short:
      return _Alignof(short);
    case ccol_int:
    case ccol_unsigned_int:
      return _Alignof(int);
    case ccol_long:
    case ccol_unsigned_long:
      return _Alignof(long);
    case ccol_long_long:
    case ccol_unsigned_long_long:
      return _Alignof(long long);
    case ccol_float:
      return _Alignof(float);
    case ccol_double:
      return _Alignof(double);
    case ccol_long_double:
      return _Alignof(long double);
    case ccol_pointer:
      return _Alignof(void *);
    default:
      return _Alignof(max_align_t);
  }
}

#define CBMAP_ALIGN_UP_TO(n, a) (((n) + (a) - 1u) & ~(size_t)((a) - 1u))
/* The value keeps max_align_t. A map records its key type but not its value
   type, so this module can't know what type the caller casts the value
   pointer to, and the strongest alignment is the only correct answer. It
   costs nothing on the path that matters, since a descent through the tree
   reads keys and never values. */
#define CBMAP_NODE_VAL_ALIGN _Alignof(max_align_t)

_Static_assert(CBMAP_MAX_TREE_HEIGHT <= UINT8_MAX,
               "bmap_node stores a height in one byte; raising "
               "CBMAP_MAX_TREE_HEIGHT past UINT8_MAX needs a wider field");

typedef struct bmap_node {
  // Containers for the data
  cmap_pair key_pair;
  cmap_pair val_pair;
  // Pointers to the other nodes
  struct bmap_node *left;
  struct bmap_node *right;
  /* An AVL tree is never taller than about 1.44*log2(n+2), and
     CBMAP_MAX_TREE_HEIGHT is the ceiling this module enforces, so one byte
     holds any height that can occur here. The narrow field is only storage:
     every computation on a height runs at the width node_height() returns,
     so no arithmetic changes. With the narrow field the struct is small
     enough that a node's key bytes start inside the same 64-byte span as the
     pointers and sizes a descent reads; with a wider field the key bytes
     would start at the boundary just past that span. */
  uint8_t height;
  /* False while the value's bytes sit in this node's allocation, which is
     how every node starts. An update that changes the value's size moves the
     bytes into a buffer of their own and sets this to true, because the room
     inside the node is exactly the size the value had at creation and can't
     grow. A size change is the exception though: a typed map writes the same
     width every time, so on the common update path the value stays inside
     the node for the node's whole life. */
  bool val_is_external;
} bmap_node;

typedef struct cbinarymap {
  size_t elem_count;
  bmap_node *root;
  ccol_memmgmt_procs_t *m_procs;
  ccol_data_type key_type;
  /* Computed from key_type once at creation and fixed after that. Without
     this field, every node the map builds would repeat the same switch on
     the path that builds it. */
  size_t key_alignment;
  ccol_comparison_proc_t custom_comparison_proc;
  /* The offset of the key bytes inside every node, and the kind of
     comparison every descent of this map makes. Both follow from key_type
     and custom_comparison_proc at creation and never change. So a descent
     picks its comparison once per call, reads the key it's looking for once
     and reads each node's key at this fixed offset, instead of dispatching
     on the key type, comparing two sizes and chasing key_pair.ptr at every
     node. */
  size_t key_offset;
  uint8_t cmp_kind;
} cbinarymap;

typedef struct cbmap_cmap_iterator {  // A cmap_iterator with extra fields
  /* The free function of the allocator that made this iterator, or NULL for
   * the default one. The scope-exit cleanup of ccol_iter_declare can free an
   * iterator that a loop left early after the caller has already destroyed
   * the map, and with it the procs struct the map owns, so freeing the
   * iterator must never read the map. */
  ccol_free_t free_fn;
  bmap_node *node_stack[CBMAP_MAX_TREE_HEIGHT];
  size_t node_stack_len;
  cmap_iterator user_iter;
} cbmap_cmap_iterator;

#define cmapIter2CbmapIter(u_iter)            \
  (cbmap_cmap_iterator *)((uint8_t *)u_iter - \
                          offsetof(cbmap_cmap_iterator, user_iter))

// Helper structure that keeps the parent-child relations during a tree
// operation
typedef struct node_stack_entry {
  bmap_node *node;
  bmap_node **parent_link;  // Points at the parent's left or right pointer
} node_stack_entry;

/* Pushes node and all its left descendants onto the iterator's node stack.
 * That keeps the "left subtree first" invariant of the iterative in-order
 * walk, so the next pop gives the smallest key of this subtree that hasn't
 * been visited yet. The stack is a fixed array of CBMAP_MAX_TREE_HEIGHT
 * entries inside the iterator itself, and at most one entry per tree level
 * is waiting at any time (see the comment on CBMAP_MAX_TREE_HEIGHT). */
static void push_all_lefts_into_iter_stack(cbmap_cmap_iterator *real_iter,
                                           bmap_node *node) {
  while (node) {
    _ccol_assert(real_iter->node_stack_len < CBMAP_MAX_TREE_HEIGHT);
    real_iter->node_stack[real_iter->node_stack_len++] = node;
    node = node->left;
  }
}

/* Pops the current in-order node from the iterator's stack, pushes all the
 * left descendants of its right child, and then updates the key and value
 * pair pointers the caller sees. With an empty stack the iterator is
 * destroyed and we return NULL, meaning the walk is over. */
static cmap_iterator *cmap_real_iter_next(cbmap_cmap_iterator *real_iter) {
  if (real_iter->node_stack_len == 0) {
    // Nothing left to advance to
    __cbmap_iterator_destroy(&real_iter->user_iter);
    return NULL;
  }
  bmap_node *node = real_iter->node_stack[--real_iter->node_stack_len];
  if (node->right) {
    push_all_lefts_into_iter_stack(real_iter, node->right);
  }

  real_iter->user_iter.key_pair = &node->key_pair;
  real_iter->user_iter.val_pair = &node->val_pair;
  return &real_iter->user_iter;
}

/* Creates an in-order iterator at the first (smallest) key and returns it.
 * The tree is walked iteratively with an explicit fixed-size stack, an
 * array of bmap_node* inside the iterator itself. Returns NULL when the map
 * is empty. */
static cmap_iterator *cbmap_iter_next(cmap_iterator *iter);

cmap_iterator *cbmap_begin_iter(cbmap cbm, char **err) {
  if (err) {
    *err = NULL;
  }

  // A NULL cbm is treated like an empty map on purpose (see the doc comment
  // of this function in cbstmap.h), the same as chashmap_begin_iter does
  // with a NULL handle: a map field that stays uninitialized because nothing
  // was put into it can be iterated directly, without a NULL guard of the
  // caller's own first.
  if (!cbm || !cbm->root) {
    return NULL;
  }

  cbmap_cmap_iterator *real_iter =
      _ccol_mem_alloc(cbm->m_procs, sizeof(cbmap_cmap_iterator));
  if (!real_iter) {
    if (err) {
      *err = _CCOL_ERR_STR("Failed to create the iterator buffer");
    }
    return NULL;
  }

  real_iter->node_stack_len = 0;
  real_iter->free_fn = cbm->m_procs ? cbm->m_procs->free : NULL;
  real_iter->user_iter._next_fn = cbmap_iter_next;
  real_iter->user_iter._free_fn = __cbmap_iterator_destroy;
  real_iter->user_iter._direct_ptr = false;
  push_all_lefts_into_iter_stack(real_iter, cbm->root);
  return cmap_real_iter_next(real_iter);
}

/* Moves the iterator to the next in-order node and returns it, or destroys
 * the iterator and returns NULL once the walk is over. */
static cmap_iterator *cbmap_iter_next(cmap_iterator *iter) {
  // Advance to the next node
  if (!iter) {
    _ccol_assert(false);
  }

  cbmap_cmap_iterator *real_iter = cmapIter2CbmapIter(iter);
  return cmap_real_iter_next(real_iter);
}

/* Frees the iterator struct, which keeps its node stack inline.
 * cmap_real_iter_next calls it at the end of the walk, and the caller can
 * call it earlier to stop before the end. It reads nothing of the map, so
 * it is still valid after the map is destroyed. */
void __cbmap_iterator_destroy(cmap_iterator *iter) {
  if (iter) {
    cbmap_cmap_iterator *real_iter = cmapIter2CbmapIter(iter);
    ccol_free_t free_fn = real_iter->free_fn;
    if (free_fn) {
      free_fn(real_iter);
    } else {
      _ccol_default_mem_free(real_iter);
    }
  }
}

/* Validates the creation inputs of the BST map, which means only the memory
 * management procedures. The key type needs no validation since every
 * ccol_data_type value is legal; one we don't recognize just uses the memcmp
 * comparison, like ccol_other_types. */
/* The comparison a descent makes. CBMAP_CMP_GENERIC goes through
 * compare_keys(). Every other kind is a fixed-width integer key, or a plain
 * char key, without a custom comparison proc, and the descent compares two
 * values of that type with < and ==, which is exactly the order
 * compare_keys() gives such a key (_ccol_typed_cmp, and the native char for
 * ccol_char). */
enum {
  CBMAP_CMP_GENERIC = 0,
  CBMAP_CMP_CHAR,
  CBMAP_CMP_S8,
  CBMAP_CMP_S16,
  CBMAP_CMP_S32,
  CBMAP_CMP_S64,
  CBMAP_CMP_U8,
  CBMAP_CMP_U16,
  CBMAP_CMP_U32,
  CBMAP_CMP_U64
};

static uint8_t cbmap_cmp_kind(ccol_data_type key_type,
                              ccol_comparison_proc_t custom_proc) {
  if (custom_proc) return CBMAP_CMP_GENERIC;
  size_t width = ccol_fixed_width_data_type_size(key_type);
  switch (key_type) {
    case ccol_char:
      return CBMAP_CMP_CHAR;
    case ccol_signed_char:
    case ccol_short:
    case ccol_int:
    case ccol_long:
    case ccol_long_long:
      switch (width) {
        case 1:
          return CBMAP_CMP_S8;
        case 2:
          return CBMAP_CMP_S16;
        case 4:
          return CBMAP_CMP_S32;
        case 8:
          return CBMAP_CMP_S64;
        default:
          return CBMAP_CMP_GENERIC;
      }
    case ccol_unsigned_char:
    case ccol_unsigned_short:
    case ccol_unsigned_int:
    case ccol_unsigned_long:
    case ccol_unsigned_long_long:
    case ccol_pointer:
      switch (width) {
        case 1:
          return CBMAP_CMP_U8;
        case 2:
          return CBMAP_CMP_U16;
        case 4:
          return CBMAP_CMP_U32;
        case 8:
          return CBMAP_CMP_U64;
        default:
          return CBMAP_CMP_GENERIC;
      }
    default:
      return CBMAP_CMP_GENERIC;
  }
}

static bool verify_cbmap_create_inputs(ccol_memmgmt_procs_t *mmgmt_procs,
                                       char **err) {
  if (!_ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return false;
  }

  return true;
}

/* Creates an empty AVL-balanced BST map. key_type picks the default
 * comparison strategy (see compare_keys()), and a non-NULL
 * custom_comparison_proc replaces all the built-in key comparisons. */
cbmap cbmap_create_full(ccol_data_type key_type,
                        ccol_memmgmt_procs_t *mmgmt_procs,
                        ccol_comparison_proc_t custom_comparison_proc,
                        char **err) {
  if (err) {
    *err = NULL;
  }

  if (!verify_cbmap_create_inputs(mmgmt_procs, err)) {
    return NULL;
  }

  cbmap cbm = _ccol_mem_alloc(mmgmt_procs, sizeof(cbinarymap));
  if (!cbm) {
    if (err) {
      *err = _CCOL_ERR_STR("Failed to allocate cbinarymap area");
    }
    return NULL;
  }

  if (!_ccol_populate_mem_mgmt_procs(cbm, mmgmt_procs, err)) {
    _ccol_mem_free(mmgmt_procs, cbm);
    return NULL;
  }

  cbm->elem_count = 0;
  cbm->root = NULL;
  cbm->key_type = key_type;
  cbm->key_alignment = cbmap_key_alignment(key_type);
  cbm->custom_comparison_proc = custom_comparison_proc;
  cbm->key_offset = CBMAP_ALIGN_UP_TO(sizeof(bmap_node), cbm->key_alignment);
  cbm->cmp_kind = cbmap_cmp_kind(key_type, custom_comparison_proc);

  return cbm;
}

/* Frees the node struct, plus the value buffer only when an update moved the
 * value out of the struct's allocation. The key never needs a free of its
 * own, and neither does a value that sits inside the node (see
 * create_new_node). The left and right pointers aren't touched, so the
 * caller must unlink the node first. */
static void destroy_bmap_node(cbmap cbm, bmap_node *node) {
  if (node) {
    if (node->val_is_external) {
      _ccol_mem_free(cbm->m_procs, node->val_pair.ptr);
    }
    _ccol_mem_free(cbm->m_procs, node);
  }
}

/* Destroys all the nodes with an iterative post-order walk over a fixed
 * stack of CBMAP_MAX_TREE_HEIGHT entries; the comment on that constant
 * explains why the bound is always enough. A post-order walk frees both
 * children before the parent, so the parent's left and right pointers stay
 * valid during the walk. At the end root is set to NULL and elem_count to
 * zero. */
static void _clear_nodes(cbmap cbm) {
  if (!cbm->root) {
    return;
  }

  bmap_node *stack[CBMAP_MAX_TREE_HEIGHT];
  size_t stack_len = 0;

  bmap_node *current = cbm->root;
  bmap_node *last_visited = NULL;

  while (stack_len > 0 || current) {
    // Go down to the leftmost node
    if (current) {
      _ccol_assert(stack_len < CBMAP_MAX_TREE_HEIGHT);
      stack[stack_len++] = current;
      current = current->left;
    } else {
      // Peek at the top of the stack
      bmap_node *peek = stack[stack_len - 1];

      // There's a right child that we haven't handled yet
      if (peek->right && peek->right != last_visited) {
        current = peek->right;
      } else {
        // Both children are done, so handle this node
        --stack_len;
        destroy_bmap_node(cbm, peek);
        last_visited = peek;
      }
    }
  }

  cbm->root = NULL;
  cbm->elem_count = 0;
}

/* Destroys all the nodes, then frees the map struct and its custom
 * allocator. */
void __cbmap_destroy(cbmap cbm) {
  if (cbm) {
    _clear_nodes(cbm);

    if (cbm->m_procs) {
      ccol_free_t free_func = cbm->m_procs->free;
      free_func(cbm->m_procs);
      free_func(cbm);
    } else {
      _ccol_default_mem_free(cbm);
    }
  }
}

/* Returns how many key-value pairs the map holds. */
size_t cbmap_elem_count(cbmap cbm) {
  if (!cbm) {
    _ccol_assert(false);
  }

  return cbm->elem_count;
}

/* Removes every element; the map ends up empty and stays usable. */
ccol_retval_t cbmap_reset(cbmap cbm) {
  if (!cbm) {
    _ccol_assert(false);
  }

  _clear_nodes(cbm);

  return ccol_success;
}

/* Compares the two signed integers of 1, 2, 4 or 8 bytes (signed char,
 * short, int, long and long long) that ptr1 and ptr2 point at. It uses typed
 * dereferences instead of memcmp, because then there's no sign-extension
 * problem: 0xFF in a signed byte is -1, not 255. It never runs for
 * ccol_char, a plain `char` key. The C standard leaves the signedness of a
 * plain `char` to the platform, so compare_keys() gives ccol_char its own
 * case with the native `char` type instead of forcing a signed int8_t
 * reinterpretation on it, and a char key then sorts exactly like this
 * platform's `<` and `>` on `char`. A key declared as `signed char`, or its
 * typedef `int8_t`, is a different type, ccol_signed_char, and gets a real
 * signed comparison here that doesn't depend on the platform. The same goes
 * for every other signed integer width. */
static inline int cmp_signed_small(void *ptr1, void *ptr2, size_t size) {
  switch (size) {
    case 1: {
      return _ccol_typed_cmp(ptr1, ptr2, int8_t);
    }
    case 2: {
      return _ccol_typed_cmp(ptr1, ptr2, int16_t);
    }
    case 4: {
      return _ccol_typed_cmp(ptr1, ptr2, int32_t);
    }
    case 8: {
      return _ccol_typed_cmp(ptr1, ptr2, int64_t);
    }
    default: {
      // Any other size is a failure; such a key must never be classified
      // as a 'signed' number
      _ccol_assert(false);
      return 0;  // We never get here, since _ccol_assert(false)
                 // always aborts. The line only gives this non-void function
                 // a defined return on every path, which keeps
                 // -Wreturn-type quiet.
    }
  }
}

/* Compares two unsigned integers, or two raw pointer values, of 1, 2, 4 or 8
 * bytes, and falls back to memcmp for any other size. It only runs for a
 * key_type known to be an unsigned integer or a pointer, never for an
 * opaque struct that happens to have one of these sizes (see
 * ccol_other_types in compare_keys(), which always uses memcmp). */
static inline int cmp_unsigned_small(void *ptr1, void *ptr2, size_t size) {
  switch (size) {
    case 1: {
      return _ccol_typed_cmp(ptr1, ptr2, uint8_t);
    }
    case 2: {
      return _ccol_typed_cmp(ptr1, ptr2, uint16_t);
    }
    case 4: {
      return _ccol_typed_cmp(ptr1, ptr2, uint32_t);
    }
    case 8: {
      return _ccol_typed_cmp(ptr1, ptr2, uint64_t);
    }
    default: {
      return memcmp(ptr1, ptr2, size);
    }
  }
}

/* Three-way comparison for a floating-point type T with an explicit rule for
 * NaN. A BST needs a real strict total order to stay consistent, because
 * every descent for a lookup, an insert or a delete must agree on which
 * side of a node a key belongs to. IEEE 754's native `<` and `>` can't give
 * that order for NaN, since both are false for any comparison with a NaN
 * operand, so a NaN key would compare "equal" to every other key, even keys
 * with no relation to it. This macro orders NaN as greater than every
 * non-NaN value and equal only to another NaN, which is a real total order
 * where the NaN keys sort together at the high end of the tree. Without the
 * rule a NaN key compares "equal" to the first node the search visits, so
 * the insert corrupts that unrelated node's value and never adds the NaN
 * key. isnan() is a type-generic macro from C99 <math.h>, so this single
 * helper covers float, double and long double without a variant per type. */
/* Both operands are read with memcpy instead of dereferencing a pointer cast
 * to T *. _ccol_typed_cmp in common.h explains why: a key pair a
 * caller built by hand and passed through cbmap_insert_elem,
 * cbmap_get_elem_ref or cbmap_delete_elem carries neither an alignment
 * guarantee for T nor an effective type of T. A constant-size memcpy
 * compiles to the same single load.
 *
 * Each temporary has a name that mentions this module, because the caller
 * passes ptr1 and ptr2 as arbitrary expressions that end up inside the scope
 * of these temporaries. A temporary called _v1 would capture a caller
 * identifier of the same name and read an uninitialised value instead. */
#define cmp_float_val(T, ptr1, ptr2)                 \
  ({                                                 \
    T _cbmap_cfv_lhs;                                \
    T _cbmap_cfv_rhs;                                \
    memcpy(&_cbmap_cfv_lhs, (ptr1), sizeof(T));      \
    memcpy(&_cbmap_cfv_rhs, (ptr2), sizeof(T));      \
    bool _cbmap_cfv_nan_lhs = isnan(_cbmap_cfv_lhs); \
    bool _cbmap_cfv_nan_rhs = isnan(_cbmap_cfv_rhs); \
    (_cbmap_cfv_nan_lhs || _cbmap_cfv_nan_rhs)       \
        ? (_cbmap_cfv_nan_lhs == _cbmap_cfv_nan_rhs  \
               ? 0                                   \
               : (_cbmap_cfv_nan_lhs ? 1 : -1))      \
        : (_cbmap_cfv_lhs > _cbmap_cfv_rhs) -        \
              (_cbmap_cfv_lhs < _cbmap_cfv_rhs);     \
  })

/* Compares the two floating-point values ptr1 and ptr2 point at. We dispatch
 * on the map's declared key_type and not on the size, because the size of a
 * long double depends on the platform: on some ABIs it's the same as a
 * double, and a switch on the size couldn't tell them apart. A native
 * floating-point comparison orders negative and positive values correctly
 * and treats -0.0 and 0.0 as equal, and a raw reinterpretation of the bit
 * pattern does neither. See the comment on cmp_float_val for the NaN rule. */
static inline int cmp_float_small(ccol_data_type key_type, void *ptr1,
                                  void *ptr2) {
  switch (key_type) {
    case ccol_float: {
      return cmp_float_val(float, ptr1, ptr2);
    }
    case ccol_double: {
      return cmp_float_val(double, ptr1, ptr2);
    }
    case ccol_long_double: {
      return cmp_float_val(long double, ptr1, ptr2);
    }
    default: {
      // This function is only ever called for one of the three
      // floating-point key types.
      _ccol_assert(false);
      return 0;  // We never get here, since _ccol_assert(false)
                 // always aborts. The line only gives this non-void function
                 // a defined return on every path, which keeps
                 // -Wreturn-type quiet.
    }
  }
}

/* Central dispatch for comparing keys. It tries three things in this order:
 * custom_comparison_proc; then the path for two keys of different sizes,
 * which only variable-length keys like strings take; and last a dispatch on
 * the map's declared key_type. Two keys of different sizes are compared on
 * their common prefix first and then on length, so a shorter key sorts
 * before a longer one, which gives variable-length keys a consistent BST
 * order.
 *
 * The key_type dispatch has five groups: real signed integers, real
 * unsigned integers and pointers, real floating-point types, strings, and
 * everything else, which is ccol_other_types (a struct or an enum key, for
 * example). Only the first three get a typed numeric reinterpretation,
 * because that's only valid for a type that really is a number of that
 * kind. Every type the dispatch doesn't recognize, ccol_other_types
 * included, gets a raw memcmp of its representation, which is what this
 * module documents for a struct key of any size. */
static inline int compare_keys(cbmap cbm, const cmap_pair *key_pair1,
                               const cmap_pair *key_pair2) {
  if (cbm->custom_comparison_proc) {
    return cbm->custom_comparison_proc(key_pair1->ptr, key_pair2->ptr);
  }

  if (key_pair1->size != key_pair2->size) {
    // The sizes differ, so compare the common prefix first and then the
    // size. No memcmp is called when min_size is 0, which means one side is
    // a zero-size key, and create_new_node stores such a key as
    // ptr == NULL and size == 0. Comparing zero bytes is always "equal"
    // whatever the two pointers are, but glibc's <string.h> declares
    // memcmp's pointer parameters nonnull and UBSan enforces that even for a
    // zero length. Calling memcmp with a real NULL pointer is undefined
    // behavior, even when no byte is read, and the stored form of
    // a zero-size key always has such a NULL pointer.
    size_t min_size = _ccol_min(key_pair1->size, key_pair2->size);
    int cmp =
        min_size == 0 ? 0 : memcmp(key_pair1->ptr, key_pair2->ptr, min_size);
    if (cmp != 0) {
      return cmp;
    }

    // Equal common prefix, so the shorter string comes first
    return (key_pair1->size > key_pair2->size) -
           (key_pair1->size < key_pair2->size);
  }

  switch (cbm->key_type) {
    case ccol_string: {
      // The two sizes are equal here, and a string key's size is never 0:
      // every raw entry point refuses such a key through
      // key_size_is_valid_for_type() before comparing anything, so both
      // pointers address at least one byte.
      return memcmp(key_pair1->ptr, key_pair2->ptr, key_pair1->size);
    }

    case ccol_char: {
      // A plain `char` is a different type from both `signed char` and
      // `unsigned char`, and the C standard leaves its signedness to the
      // platform. It's signed on x86 and x86_64, for example, and unsigned
      // on the standard aarch64 AAPCS64 ABI. So this case compares with the
      // native `char` type itself instead of forcing a signed int8_t
      // reinterpretation, and the default comparator always agrees with
      // this platform's `<` and `>` on `char`. ccol_unsigned_char gets its
      // own comparison below in the same way.
      return _ccol_typed_cmp(key_pair1->ptr, key_pair2->ptr, char);
    }

    case ccol_signed_char:
    case ccol_short:
    case ccol_int:
    case ccol_long:
    case ccol_long_long: {
      return cmp_signed_small(key_pair1->ptr, key_pair2->ptr, key_pair1->size);
    }

    case ccol_unsigned_char:
    case ccol_unsigned_short:
    case ccol_unsigned_int:
    case ccol_unsigned_long:
    case ccol_unsigned_long_long:
    case ccol_pointer: {
      return cmp_unsigned_small(key_pair1->ptr, key_pair2->ptr,
                                key_pair1->size);
    }

    case ccol_float:
    case ccol_double:
    case ccol_long_double: {
      return cmp_float_small(cbm->key_type, key_pair1->ptr, key_pair2->ptr);
    }

    case ccol_other_types:
    default: {
      // A POD type with no numeric meaning: structs, unions, enums, bool,
      // wchar_t and the like, which this case orders by their raw bytes. We
      // check key_pair1->size == 0 before calling memcmp because that size
      // is a real zero-size key, which create_new_node stores as a NULL
      // pointer with a size of 0. The reason is the same as in the branch
      // above for two different sizes: memcmp's pointer parameters are
      // declared nonnull even for a zero length, and the stored ptr of a
      // zero-size key is NULL, so calling memcmp here without the check is
      // undefined behavior even when no byte gets read. Two zero-size keys
      // of this type are always equal.
      if (key_pair1->size == 0) return 0;
      return memcmp(key_pair1->ptr, key_pair2->ptr, key_pair1->size);
    }
  }
}

/* Copies n bytes. A key or a value is usually a scalar of 1, 2, 4, 8 or 16
 * bytes, and for those sizes each case is a constant-length memcpy that the
 * compiler turns into one load and one store, while a memcpy whose length is
 * only known at run time is a call into the C library. cbmap_move_bytes is
 * the same thing for ranges that may overlap, since a constant-size memmove
 * loads every byte before storing any. */
#define CBMAP_SMALL_COPY_BODY(fn) \
  switch (n) {                    \
    case 1:                       \
      fn(dst, src, 1);            \
      break;                      \
    case 2:                       \
      fn(dst, src, 2);            \
      break;                      \
    case 4:                       \
      fn(dst, src, 4);            \
      break;                      \
    case 8:                       \
      fn(dst, src, 8);            \
      break;                      \
    case 16:                      \
      fn(dst, src, 16);           \
      break;                      \
    default:                      \
      fn(dst, src, n);            \
      break;                      \
  }
static inline void cbmap_copy_bytes(void *restrict dst,
                                    const void *restrict src, size_t n) {
  CBMAP_SMALL_COPY_BODY(memcpy)
}
static inline void cbmap_move_bytes(void *dst, const void *src, size_t n) {
  CBMAP_SMALL_COPY_BODY(memmove)
}
#undef CBMAP_SMALL_COPY_BODY

/* The descents of this map. Both pick the comparison once per call from
 * cbm->cmp_kind. For an integer or char key without a custom proc they read
 * the wanted key once, read each node's key at cbm->key_offset with a
 * constant-size memcpy, and compare with < and ==; every other key goes
 * through compare_keys(). Both rely on the caller having checked that a
 * fixed-width key has its type's width (key_size_matches_type_if_fixed_
 * width), so reading the key the caller passed never runs past its bytes. */
#define CBMAP_FIND_TYPED(T)                                                    \
  do {                                                                         \
    T cbmap_want_;                                                             \
    memcpy(&cbmap_want_, key_pair->ptr, sizeof(T));                            \
    const size_t cbmap_off_ = cbm->key_offset;                                 \
    bmap_node *cbmap_n_ = cbm->root;                                           \
    while (cbmap_n_) {                                                         \
      T cbmap_have_;                                                           \
      memcpy(&cbmap_have_, (const char *)cbmap_n_ + cbmap_off_, sizeof(T));    \
      if (cbmap_want_ == cbmap_have_) return cbmap_n_;                         \
      cbmap_n_ = cbmap_want_ < cbmap_have_ ? cbmap_n_->left : cbmap_n_->right; \
    }                                                                          \
    return NULL;                                                               \
  } while (0)

/* Gives the node holding key_pair, or NULL. */
static inline bmap_node *cbmap_find_node(cbmap cbm, const cmap_pair *key_pair) {
  switch (cbm->cmp_kind) {
    case CBMAP_CMP_CHAR:
      CBMAP_FIND_TYPED(char);
    case CBMAP_CMP_S8:
      CBMAP_FIND_TYPED(int8_t);
    case CBMAP_CMP_S16:
      CBMAP_FIND_TYPED(int16_t);
    case CBMAP_CMP_S32:
      CBMAP_FIND_TYPED(int32_t);
    case CBMAP_CMP_S64:
      CBMAP_FIND_TYPED(int64_t);
    case CBMAP_CMP_U8:
      CBMAP_FIND_TYPED(uint8_t);
    case CBMAP_CMP_U16:
      CBMAP_FIND_TYPED(uint16_t);
    case CBMAP_CMP_U32:
      CBMAP_FIND_TYPED(uint32_t);
    case CBMAP_CMP_U64:
      CBMAP_FIND_TYPED(uint64_t);
    default:
      break;
  }
  bmap_node *tracker = cbm->root;
  while (tracker) {
    int comparison = compare_keys(cbm, key_pair, &tracker->key_pair);
    if (comparison == 0) return tracker;
    tracker = comparison < 0 ? tracker->left : tracker->right;
  }
  return NULL;
}
#undef CBMAP_FIND_TYPED

#define CBMAP_DESCEND_TYPED(T)                                             \
  do {                                                                     \
    T cbmap_want_;                                                         \
    memcpy(&cbmap_want_, key_pair->ptr, sizeof(T));                        \
    const size_t cbmap_off_ = cbm->key_offset;                             \
    while (current) {                                                      \
      T cbmap_have_;                                                       \
      memcpy(&cbmap_have_, (const char *)current + cbmap_off_, sizeof(T)); \
      if (cbmap_want_ == cbmap_have_) break;                               \
      _ccol_assert(len < CBMAP_MAX_TREE_HEIGHT);                           \
      path[len++] = (node_stack_entry){current, link};                     \
      if (cbmap_want_ < cbmap_have_) {                                     \
        link = &current->left;                                             \
        current = current->left;                                           \
      } else {                                                             \
        link = &current->right;                                            \
        current = current->right;                                          \
      }                                                                    \
    }                                                                      \
  } while (0)

/* Walks from the root towards key_pair and records every node it passes in
 * path, each with the link that points at it. Gives the node holding the
 * key, or NULL when the key is absent, and sets *link_out to the link that
 * points at the node found, or to the empty link where the key belongs. */
static inline __attribute__((always_inline)) bmap_node *cbmap_descend_with_path(
    cbmap cbm, const cmap_pair *key_pair, node_stack_entry *path,
    size_t *path_len_out, bmap_node ***link_out) {
  bmap_node *current = cbm->root;
  bmap_node **link = &cbm->root;
  size_t len = 0;
  switch (cbm->cmp_kind) {
    case CBMAP_CMP_CHAR:
      CBMAP_DESCEND_TYPED(char);
      break;
    case CBMAP_CMP_S8:
      CBMAP_DESCEND_TYPED(int8_t);
      break;
    case CBMAP_CMP_S16:
      CBMAP_DESCEND_TYPED(int16_t);
      break;
    case CBMAP_CMP_S32:
      CBMAP_DESCEND_TYPED(int32_t);
      break;
    case CBMAP_CMP_S64:
      CBMAP_DESCEND_TYPED(int64_t);
      break;
    case CBMAP_CMP_U8:
      CBMAP_DESCEND_TYPED(uint8_t);
      break;
    case CBMAP_CMP_U16:
      CBMAP_DESCEND_TYPED(uint16_t);
      break;
    case CBMAP_CMP_U32:
      CBMAP_DESCEND_TYPED(uint32_t);
      break;
    case CBMAP_CMP_U64:
      CBMAP_DESCEND_TYPED(uint64_t);
      break;
    default:
      while (current) {
        int comparison = compare_keys(cbm, key_pair, &current->key_pair);
        if (comparison == 0) break;
        _ccol_assert(len < CBMAP_MAX_TREE_HEIGHT);
        path[len++] = (node_stack_entry){current, link};
        if (comparison > 0) {
          link = &current->right;
          current = current->right;
        } else {
          link = &current->left;
          current = current->left;
        }
      }
      break;
  }
  *path_len_out = len;
  *link_out = link;
  return current;
}
#undef CBMAP_DESCEND_TYPED

/* Allocates a new BST node. A single allocation carries the node, the key
 * bytes and the value bytes, so neither of them starts in a buffer of its
 * own.
 *
 * We never free node->key_pair.ptr, and node->val_pair.ptr is freed only
 * when val_is_external says an update moved that value out. Freeing either
 * pointer without that condition frees an interior pointer, which is heap
 * corruption and not a leak. Read destroy_bmap_node and the comment on
 * val_is_external before changing what this function allocates.
 *
 * The key never changes for the node's whole life. A key is never resized,
 * and a removal that needs a replacement relinks the donor node instead of
 * copying its contents (see perform_element_removal), so a key never has to
 * outlive a node or move between nodes. One allocation for all three parts
 * saves two allocations per node, and for the key it also removes the
 * pointer chase that every comparison along a search path would pay if the
 * key sat in its own buffer. A walk only reads keys, from the node it has
 * already loaded, so a descent stays inside the cache lines it has already
 * pulled in. Only an update can change a value's size, and that's what
 * moves the value out.
 *
 * A zero-size key or value never goes to the allocator. The C standard lets
 * malloc(0) return NULL or a unique pointer, so an allocator may legally
 * answer a zero-size request with NULL, and checking the result against NULL
 * would then report a real zero-byte key or value as
 * ccol_not_enough_memory. Instead a zero-size pair is stored as
 * ptr == NULL and size == 0, which every reader in this file handles safely
 * for a zero length: the memcmp of compare_keys, the size-guarded copies of
 * create_new_node and cbmap_get_elem_copy, and the _ccol_mem_free of
 * destroy_bmap_node. */
static bmap_node *create_new_node(cbmap cbm, const cmap_pair *key_pair,
                                  const cmap_pair *val_pair) {
  /* The node, the key and the value are one allocation, and we form the
     byte count here instead of leaving that to the allocator. Without these
     checks a size that makes the sum wrap would turn into a small allocation
     that succeeds, followed by copies through that small block. Each term is
     checked against what remains, so no intermediate value can overflow
     either. */
  /* Each offset is rounded to what the object at that offset needs: the key
     to the requirement of its own declared type, and the value to the
     strongest alignment, since the map doesn't record a value's type. If we
     rounded the key to max_align_t, every key narrower than max_align_t
     would start past the end of the struct's 64-byte span, and a descent has
     already loaded that span when it compares. */
  size_t key_offset = cbm->key_offset;
  if (key_pair->size > SIZE_MAX - key_offset - CBMAP_NODE_VAL_ALIGN) {
    return NULL;
  }
  size_t val_offset =
      CBMAP_ALIGN_UP_TO(key_offset + key_pair->size, CBMAP_NODE_VAL_ALIGN);
  if (val_pair->size > SIZE_MAX - val_offset) {
    return NULL;
  }
  bmap_node *new_node =
      _ccol_mem_alloc(cbm->m_procs, val_offset + val_pair->size);
  if (!new_node) {
    return NULL;
  }

  /* A zero-size key or value keeps the NULL pointer and size 0 form that
     every reader in this file already handles, instead of a pointer one
     past the block. */
  new_node->key_pair.ptr =
      (key_pair->size > 0) ? (char *)new_node + key_offset : NULL;
  new_node->val_pair.ptr =
      (val_pair->size > 0) ? (char *)new_node + val_offset : NULL;
  new_node->val_is_external = false;

  /* These copies are guarded on the size because a zero-size key or value
     carries a NULL pointer here (this file's form of empty), and memcpy
     declares both pointer parameters as never null, even for a zero length.
     Without the guards UndefinedBehaviorSanitizer reports "null pointer
     passed as argument 1, which is declared to never be null". */
  if (key_pair->size > 0) {
    cbmap_copy_bytes(new_node->key_pair.ptr, key_pair->ptr, key_pair->size);
  }
  new_node->key_pair.size = key_pair->size;
  if (val_pair->size > 0) {
    cbmap_copy_bytes(new_node->val_pair.ptr, val_pair->ptr, val_pair->size);
  }
  new_node->val_pair.size = val_pair->size;

  new_node->left = NULL;
  new_node->right = NULL;
  new_node->height = 1;

  return new_node;
}

/* Returns the larger of x and y. This file has its own version so it
 * doesn't have to include <stdlib.h> just for a max of two arguments. */
static size_t maximum(size_t x, size_t y) {
  if (x >= y) {
    return x;
  }
  return y;
}

/* Overwrites the value of a node already in the map. When the new value has
 * a different size we try a new allocation; if that fails the old value and
 * size are kept and *result isn't touched, so the caller sees
 * ccol_not_enough_memory. On success the value bytes and the size field are
 * updated and *result is set to ccol_key_already_present.
 *
 * A value whose size doesn't change goes straight where it already is,
 * which for nearly every map is the node's allocation, since a typed map
 * writes the same width every time. Only a size change moves the value out
 * into its own buffer, because the room inside the node is exactly the size
 * the value had at creation. That move only goes one way on purpose: the
 * node doesn't record how much room it reserved, so a value that later gets
 * smaller stays in its own buffer and isn't put back inside the node, where
 * the room isn't known. We never ask the allocator to realloc a pointer into
 * the node's block, which would be undefined. The new buffer is allocated
 * first and the old one freed only once that has succeeded, so a failure
 * leaves the node exactly as it was and the caller sees
 * ccol_not_enough_memory.
 *
 * A new size of 0 frees any buffer the value had moved out to and stores the
 * NULL pointer and size 0 form, the same one create_new_node uses for a
 * zero-size value. We don't ask realloc for zero bytes because the C
 * standard leaves that behavior to the implementation: glibc answers such a
 * request with a free and a NULL return, and from the return value alone you
 * can't tell that answer from a failure that left the old buffer valid. */
static void update_bmap_node_value(cbmap cbm, bmap_node *node,
                                   const cmap_pair *val_pair,
                                   ccol_retval_t *result) {
  if (node->val_pair.size != val_pair->size) {
    if (val_pair->size == 0) {
      if (node->val_is_external) {
        _ccol_mem_free(cbm->m_procs, node->val_pair.ptr);
      }
      node->val_pair.ptr = NULL;
      node->val_pair.size = 0;
      node->val_is_external = false;
      *result = ccol_key_already_present;
      return;
    }

    void *new_ptr = _ccol_mem_alloc(cbm->m_procs, val_pair->size);
    if (!new_ptr) {
      // The allocation failed, so the node keeps the value it already had
      return;
    }
    /* This copy goes into the new storage BEFORE the old storage is freed,
       and we return here instead of falling through to the copy below,
       because val_pair->ptr may BE the storage we're about to free.
       cbmap_get_elem_ref hands the caller a pointer into the node's own
       value, and a caller can perfectly well pass that pointer back to
       resize the value. Reading it after the free is a use-after-free that
       AddressSanitizer reports. */
    memcpy(new_ptr, val_pair->ptr, val_pair->size);
    if (node->val_is_external) {
      _ccol_mem_free(cbm->m_procs, node->val_pair.ptr);
    }
    node->val_pair.ptr = new_ptr;
    node->val_pair.size = val_pair->size;
    node->val_is_external = true;
    *result = ccol_key_already_present;
    return;
  }
  /* Same size, so the value goes where it already is. The source may alias
     that storage for the same reason as above. Nothing gets freed on this
     path, but a copy with overlapping ranges is undefined anyway, which is
     why it's memmove and not memcpy. */
  if (node->val_pair.ptr != val_pair->ptr && val_pair->size > 0) {
    cbmap_move_bytes(node->val_pair.ptr, val_pair->ptr, val_pair->size);
  }
  *result = ccol_key_already_present;
}

/* Returns a node's height, or 0 for NULL. A leaf starts at height 1, so the
 * NULL sentinel is 0 and all the arithmetic stays in size_t. */
static size_t node_height(bmap_node *node) {
  if (!node) {
    return 0;
  }

  return node->height;
}

/* Returns a node's balance factor, right_height minus left_height. A value
 * outside [-1, 1] means the node breaks the AVL invariant. */
static int node_balance(bmap_node *node) {
  if (!node) {
    return 0;
  }

  return (int)node_height(node->right) - (int)node_height(node->left);
}

/* Recomputes node->height from its children's heights. The caller must
 * call it after every rotation and every structural change, so the height
 * data stays right for the balance factor computations up the chain of
 * ancestors. */
static void recalculate_node_height(bmap_node *node) {
  if (!node) {
    return;
  }

  /* The cast narrows without a check. An AVL tree is never taller than about
     1.44*log2(n+2) and the address space bounds the node count, so the
     tallest tree any 64-bit target can hold is about 92 levels, and the
     field holds 255. The only thing that could make this cast truncate is a
     CBMAP_MAX_TREE_HEIGHT above what one byte holds, and the _Static_assert
     next to that constant refuses that at compile time. A run-time check
     here would sit on the path of every insert and could never fire, so we
     don't add one. */
  node->height =
      (uint8_t)(maximum(node_height(node->left), node_height(node->right)) + 1);
}

/* Restores the AVL invariant at parent when needed, with one of four
 * rotations: simple left, simple right, right-left double or left-right
 * double. Returns the subtree's new root after the rotation, which may be a
 * different node, and recomputes the heights bottom-up after each
 * rotation. */
static bmap_node *check_node_balance(bmap_node *parent) {
  recalculate_node_height(parent);
  int balance = node_balance(parent);

  if (balance > 1 || balance < -1) {
    // Out of balance, so we need a rotation
    bmap_node *p = parent;

    if (balance > 1) {
      // The right side is deeper
      if (node_balance(parent->right) >= 0 || !(parent->right->left)) {
        // Simple left rotation
        parent = parent->right;
        p->right = parent->left;
        parent->left = p;
        // Recompute the heights bottom-up
        recalculate_node_height(p);
        recalculate_node_height(parent);
      } else {  // node_balance(parent->right) < 0
        // Right-left double rotation
        bmap_node *rl_left = parent->right->left->left;
        bmap_node *rl_right = parent->right->left->right;
        parent = parent->right->left;
        parent->left = p;
        parent->right = p->right;
        parent->right->left = rl_right;
        parent->left->right = rl_left;
        // Recompute the heights of both children, then the parent's
        recalculate_node_height(parent->left);
        recalculate_node_height(parent->right);
        recalculate_node_height(parent);
      }
    } else {
      // The left side is deeper
      if (node_balance(parent->left) <= 0 || !(parent->left->right)) {
        // Simple right rotation
        parent = parent->left;
        p->left = parent->right;
        parent->right = p;
        // Recompute the heights bottom-up
        recalculate_node_height(p);
        recalculate_node_height(parent);
      } else {  // node_balance(parent->left) > 0
        // Left-right double rotation
        bmap_node *lr_left = parent->left->right->left;
        bmap_node *lr_right = parent->left->right->right;
        parent = parent->left->right;
        parent->right = p;
        parent->left = p->left;
        parent->left->right = lr_left;
        parent->right->left = lr_right;
        // Recompute the heights of both children, then the parent's
        recalculate_node_height(parent->left);
        recalculate_node_height(parent->right);
        recalculate_node_height(parent);
      }
    }
  }

  int final_balance = node_balance(parent);
  if (final_balance > 1 || final_balance < -1) {
    _ccol_assert(false);
  }
  return parent;
}

/* Detaches the minimum or the maximum node of the subtree at root and
 * returns it; max false picks the minimum and max true the maximum. The
 * detached node may have one child, which is linked into the parent's slot
 * before the ancestor path is rebalanced bottom-up with check_node_balance.
 * perform_element_removal uses this to find an in-order successor or
 * predecessor without any recursion.
 *
 * The ancestor path lives in a fixed array of CBMAP_MAX_TREE_HEIGHT entries
 * instead of a heap stack, so there's no allocation that can fail; under
 * memory pressure a heap allocation could abort the whole process, on a
 * delete path that has to stay graceful. The only way this function can
 * fail is the same depth bound that the rest of this file treats as
 * structurally impossible for a real AVL tree (see
 * CBMAP_MAX_TREE_HEIGHT). */
static bmap_node *cbmap_detach_extreme_iter(bmap_node *root, bool max,
                                            bmap_node **extreme) {
  if (!root) {
    _ccol_assert(false);
  }

  node_stack_entry path[CBMAP_MAX_TREE_HEIGHT];
  size_t path_len = 0;

  // Find the extreme node and build the path to it
  bmap_node *current = root;
  bmap_node **parent_link = NULL;

  while (true) {
    if (max) {
      if (current->right) {
        _ccol_assert(path_len < CBMAP_MAX_TREE_HEIGHT);
        path[path_len++] = (node_stack_entry){current, parent_link};
        parent_link = &(current->right);
        current = current->right;
      } else {
        // Found the max node
        *extreme = current;
        break;
      }
    } else {
      if (current->left) {
        _ccol_assert(path_len < CBMAP_MAX_TREE_HEIGHT);
        path[path_len++] = (node_stack_entry){current, parent_link};
        parent_link = &(current->left);
        current = current->left;
      } else {
        // Found the min node
        *extreme = current;
        break;
      }
    }
  }

  // Replace the extreme node with its child
  bmap_node *replacement = max ? current->left : current->right;

  // Update the parent link; if the extreme node is the root, return the
  // replacement
  if (path_len == 0) {
    return replacement;
  }

  // Update the child pointer of the last parent
  node_stack_entry last_entry = path[path_len - 1];
  if (max) {
    last_entry.node->right = replacement;
  } else {
    last_entry.node->left = replacement;
  }

  // Rebalance from the bottom to the top
  for (size_t i = path_len; i-- > 0;) {
    node_stack_entry entry = path[i];
    bmap_node *balanced = check_node_balance(entry.node);

    // Update the parent's pointer through the parent_link from the path
    if (entry.parent_link) {
      *(entry.parent_link) = balanced;
    } else {
      // This is the root
      root = balanced;
    }
  }

  return root;
}

/* A key of a fixed-width key type must come in at exactly its type's own
 * size, on every raw-layer entry point that takes a key_pair.
 *
 * compare_keys() orders two keys of different sizes by their common prefix
 * first and then by size, which is how a variable-width key type
 * (ccol_string and ccol_other_types) gets its total order. Applied to a
 * fixed-width key type, the same rule turns a key of the wrong type, like a
 * long passed to a map with int keys, into a different key that no lookup
 * with the right type can ever reach again, and nothing reports the
 * mistake. A loud refusal is much better than a key that silently goes
 * missing. A custom_comparison_proc doesn't make the mismatch harmless
 * either: it gets two bare pointers and no sizes, so it can only read the
 * declared key type's width, and a shorter key_pair makes it read past the
 * caller's buffer.
 *
 * ccol_fixed_width_data_type_size() reports 0 for a key type whose width
 * isn't fixed. A struct key (ccol_other_types) may then come in at any size,
 * 0 included. A string key (ccol_string) needs a size of at least 1, because
 * a string's size includes its terminating NUL; a pair of size 0 isn't a
 * string, and a stored key like that would reach a caller of the
 * type-inferred iterator as a NULL char *. chashmap enforces the same two
 * rules (it refuses a zero-size key for every key type), so the two map
 * modules agree on every string key. */
static inline bool key_size_is_valid_for_type(cbmap cbm,
                                              const cmap_pair *key_pair) {
  size_t fixed_width = ccol_fixed_width_data_type_size(cbm->key_type);
  if (fixed_width != 0) {
    return key_pair->size == fixed_width;
  }
  return key_pair->size != 0 || cbm->key_type != ccol_string;
}

/* Says whether a caller's pair describes bytes we can read. Every reader in
 * this file (the memcmp of compare_keys, the copy in create_new_node and the
 * copy in update_bmap_node_value) trusts the pair's size, so a NULL pointer
 * with a non-zero size is a read through a null pointer and not a lookup
 * miss. Without this check the first such call would segfault inside memcpy
 * or memcmp.
 *
 * A size of 0 passes, with or without a pointer, because in this module
 * that's how an empty value, and an empty key of a struct key type, look,
 * and every reader here handles it already. A zero-size string key passes
 * too and is then refused by key_size_is_valid_for_type(). chashmap rejects
 * a zero-size pair for every type, so the two map modules agree on the
 * null-pointer half of the rule and on string keys, and differ on the
 * zero-size half for values and struct keys. */
static inline bool pair_bytes_are_readable(const cmap_pair *pair) {
  return pair->ptr != NULL || pair->size == 0;
}

/* Inserts a key-value pair, or updates one that's already in the map. The
 * chain of ancestors goes into an explicit fixed-size path array (see
 * CBMAP_MAX_TREE_HEIGHT), so after the insert the tree can be rebalanced
 * bottom-up without recursion and without a heap allocation on any call.
 * Returns ccol_key_already_present when the key was already there and its
 * value was updated successfully. */
ccol_retval_t cbmap_insert_elem(cbmap cbm, const cmap_pair *key_pair,
                                const cmap_pair *val_pair) {
  if (!cbm) {
    _ccol_assert(false);
  }

  /* This function reads the caller's pair rather than just passing it on,
     so it reports a NULL pair, and a pair that promises bytes without a
     pointer to them, without dereferencing either. chmap_insert_elem
     rejects both the same way; it also rejects a zero-size key or value,
     which this module accepts for a value and for a struct key (see
     pair_bytes_are_readable and key_size_is_valid_for_type). */
  if (!key_pair || !val_pair || !pair_bytes_are_readable(key_pair) ||
      !pair_bytes_are_readable(val_pair)) {
    return ccol_invalid_args;
  }

  if (!key_size_is_valid_for_type(cbm, key_pair)) {
    return ccol_invalid_args;
  }

  // Empty tree. elem_count is always 0 here because root is only NULL when
  // the map is empty, so no capacity check is needed: this is always a new
  // element and the map can't hold ccol_max_elem_count elements at this
  // point. That cap is real, but it's very large.
  if (!cbm->root) {
    cbm->root = create_new_node(cbm, key_pair, val_pair);
    if (!cbm->root) {
      return ccol_not_enough_memory;
    }
    cbm->elem_count++;
    return ccol_success;
  }

  // Fixed-size stack holding the path from the root to the insert point
  node_stack_entry path[CBMAP_MAX_TREE_HEIGHT];
  size_t path_len = 0;
  bmap_node **parent_link = NULL;
  ccol_retval_t result = ccol_not_enough_memory;

  // Go down to the insert point, or to the key if it's already there
  bmap_node *existing =
      cbmap_descend_with_path(cbm, key_pair, path, &path_len, &parent_link);
  if (existing) {
    // The key is already there, so update its value
    update_bmap_node_value(cbm, existing, val_pair, &result);
    return result;
  }

  // The key isn't there. This insert grows elem_count, so here is the right
  // place for the capacity cap. We check it only now, once we know the key
  // is new, so that updating the value of a key in a full map still
  // succeeds instead of being rejected.
  if (cbm->elem_count == ccol_max_elem_count) {
    return ccol_container_full;
  }

  // Create a new node at the insert point
  bmap_node *new_node = create_new_node(cbm, key_pair, val_pair);
  if (!new_node) {
    return ccol_not_enough_memory;
  }

  *parent_link = new_node;
  result = ccol_success;

  /* Rebalance bottom-up, and stop as soon as a subtree's height is what it
     was before the insert: every ancestor above that point sees the same
     child heights as before, so it needs neither a new height nor a
     rotation. An insert into an AVL tree causes at most one rotation, and
     that rotation gives its subtree back the height it had before the
     insert, so the walk stops right after it as well. Without the stop,
     every insert would recompute the height of every ancestor and read both
     children of each, touching nodes off the search path all the way up to
     the root. */
  for (size_t i = path_len; i-- > 0;) {
    node_stack_entry entry = path[i];
    size_t height_before = entry.node->height;
    bmap_node *balanced = check_node_balance(entry.node);

    // Point the parent at this node
    if (entry.parent_link) {
      *(entry.parent_link) = balanced;
    } else {
      // This is the root
      cbm->root = balanced;
    }

    if (balanced != entry.node || balanced->height == height_before) {
      break;
    }
  }

  cbm->elem_count++;
  return result;
}

/* Looks up key_pair and copies its value into target_buf. Returns
 * ccol_invalid_args when target_buf_size doesn't match the stored value's
 * size exactly, which prevents a silent truncation. */
ccol_retval_t cbmap_get_elem_copy(cbmap cbm, const cmap_pair *key_pair,
                                  void *target_buf, size_t target_buf_size) {
  if (!cbm) {
    _ccol_assert(false);
  }

  /* See the guard in cbmap_insert_elem. */
  if (!key_pair || !pair_bytes_are_readable(key_pair)) {
    return ccol_invalid_args;
  }

  /* A stored value of size 0 is read with a NULL buffer of size 0. */
  if (!target_buf && target_buf_size != 0) {
    return ccol_invalid_args;
  }

  if (!key_size_is_valid_for_type(cbm, key_pair)) {
    return ccol_invalid_args;
  }

  bmap_node *tracker = cbmap_find_node(cbm, key_pair);
  if (!tracker) {
    return ccol_key_not_found;
  }
  if (target_buf_size != tracker->val_pair.size) {
    return ccol_invalid_args;
  }
  /* A stored zero-size value keeps a NULL pointer, and the caller's buffer
     for such a value may be NULL too, so this copy is guarded for the same
     reason as the copy in create_bmap_node. */
  if (target_buf_size > 0) {
    cbmap_copy_bytes(target_buf, tracker->val_pair.ptr, target_buf_size);
  }
  return ccol_success;
}

/* The checks and the lookup that cbmap_get_elem_ref() and
 * _cbmap_get_val_accessor() share. Gives the node of key_pair, or NULL with
 * the failure code in *ret. */
static bmap_node *cbmap_lookup_checked(cbmap cbm, const cmap_pair *key_pair,
                                       ccol_retval_t *ret) {
  if (!key_pair || !pair_bytes_are_readable(key_pair) ||
      !key_size_is_valid_for_type(cbm, key_pair)) {
    *ret = ccol_invalid_args;
    return NULL;
  }
  bmap_node *tracker = cbmap_find_node(cbm, key_pair);
  *ret = tracker ? ccol_success : ccol_key_not_found;
  return tracker;
}

/* Looks up key_pair and fills the caller's pair with the {ptr, size}
 * accessor of the node's value. ptr only stays valid while the key is in
 * the map and no update gives it a value of another size. Every failure
 * with a non-NULL val_pair sets it to {NULL, 0}. */
ccol_retval_t cbmap_get_elem_ref(cbmap cbm, const cmap_pair *key_pair,
                                 cmap_pair *val_pair) {
  if (!cbm) {
    _ccol_assert(false);
  }

  /* val_pair is the out-parameter the result goes through, so with a NULL
     val_pair there's nowhere to report a hit. See the guard in
     cbmap_insert_elem for why the key pair gets the same answer. */
  if (!val_pair) {
    return ccol_invalid_args;
  }

  ccol_retval_t ret;
  bmap_node *tracker = cbmap_lookup_checked(cbm, key_pair, &ret);
  if (!tracker) {
    val_pair->ptr = NULL;
    val_pair->size = 0;
    return ret;
  }
  *val_pair = tracker->val_pair;
  return ccol_success;
}

/* The lookup behind the string arm of cbmap_get_ptr(); see cbstmap.h. For
 * an entry with a string value, the only char* object is the ptr field of
 * the node's value accessor, so this hands out that accessor's address.
 * Every failure with a non-NULL val_accessor sets it to NULL. */
ccol_retval_t _cbmap_get_val_accessor(cbmap cbm, const cmap_pair *key_pair,
                                      const cmap_pair **val_accessor) {
  if (!cbm) {
    _ccol_assert(false);
  }
  if (!val_accessor) {
    return ccol_invalid_args;
  }

  ccol_retval_t ret;
  bmap_node *tracker = cbmap_lookup_checked(cbm, key_pair, &ret);
  *val_accessor = tracker ? &tracker->val_pair : NULL;
  return ret;
}

/* Removes parent from the tree and returns the subtree's new root. When both
 * children exist, the deeper side gives its extreme node to replace parent:
 * the right side its minimum node and the left side its maximum node. That
 * keeps the BST order and keeps the tree balanced with as few rotations as
 * possible.
 */
static bmap_node *perform_element_removal(cbmap cbm, bmap_node *parent) {
  bmap_node *left = parent->left;
  bmap_node *right = parent->right;
  destroy_bmap_node(cbm, parent);

  if (!left) {
    parent = right;
  } else if (!right) {
    parent = left;
  } else {
    // Neither left nor right is NULL
    if (right->height >= left->height) {
      // The right side is deeper
      bmap_node *right_min = NULL;
      right = cbmap_detach_extreme_iter(right, false, &right_min);
      parent = right_min;
      parent->right = right;
      parent->left = left;
      // Recompute the replacement node's height now that its new children
      // are in place
      recalculate_node_height(parent);
    } else {
      // The left side is deeper
      bmap_node *left_max = NULL;
      left = cbmap_detach_extreme_iter(left, true, &left_max);
      parent = left_max;
      parent->right = right;
      parent->left = left;
      // Recompute the replacement node's height now that its new children
      // are in place
      recalculate_node_height(parent);
    }
  }

  return parent;
}

/* Deletes the element with key_pair from the map. It uses the same
 * explicit fixed-size path array as cbmap_insert_elem, so after removing the
 * node the ancestors can be rebalanced bottom-up without a heap allocation.
 * Returns ccol_key_not_found when the key isn't in the map. */
ccol_retval_t cbmap_delete_elem(cbmap cbm, const cmap_pair *key_pair) {
  if (!cbm) {
    _ccol_assert(false);
  }

  /* See the guard in cbmap_insert_elem. */
  if (!key_pair || !pair_bytes_are_readable(key_pair)) {
    return ccol_invalid_args;
  }

  if (!key_size_is_valid_for_type(cbm, key_pair)) {
    return ccol_invalid_args;
  }

  if (!cbm->root) {
    return ccol_key_not_found;
  }

  // Fixed-size stack holding the path from the root to the node to delete
  node_stack_entry path[CBMAP_MAX_TREE_HEIGHT];
  size_t path_len = 0;
  bmap_node **parent_link = NULL;
  ccol_retval_t result = ccol_key_not_found;

  // Go down to the node to delete
  bmap_node *current =
      cbmap_descend_with_path(cbm, key_pair, path, &path_len, &parent_link);
  if (current) {
    // This is the node to delete
    bmap_node *replacement = perform_element_removal(cbm, current);
    *parent_link = replacement;
    result = ccol_success;
  }

  if (result == ccol_success) {
    /* Rebalance bottom-up and stop at the first subtree whose height after
       balancing is the height it had before the delete, since the ancestors
       above it then see the same child heights as before. A rotation after a
       delete can lower its subtree's height, so the walk stops on the height
       and not on whether a rotation happened. */
    for (size_t i = path_len; i-- > 0;) {
      node_stack_entry entry = path[i];
      size_t height_before = entry.node->height;
      bmap_node *balanced = check_node_balance(entry.node);

      // Point the parent at this node
      if (entry.parent_link) {
        *(entry.parent_link) = balanced;
      } else {
        // This is the root
        cbm->root = balanced;
      }

      if (balanced->height == height_before) {
        break;
      }
    }

    cbm->elem_count--;
  }

  return result;
}

#ifdef RUNNING_UNIT_TESTS
/* Walks the whole tree and checks two invariants at every node: the AVL
 * balance invariant, abs(right_height - left_height) <= 1, and the height
 * invariant, height == max(height(left), height(right)) + 1. bmap_node is
 * opaque outside this translation unit, so test code has no other way to
 * check them. The answer is a single pass or fail and no node pointer, so no
 * internal pointer crosses the public API boundary.
 *
 * The walk is iterative with an explicit fixed-size stack instead of
 * recursion, in line with the rest of this module, and the stack uses the
 * same CBMAP_MAX_TREE_HEIGHT bound as every other walk stack in this
 * file. */
bool cbmap_debug_validate_avl(cbmap cbm) {
  if (!cbm || !cbm->root) {
    return true;
  }

  bmap_node *stack[CBMAP_MAX_TREE_HEIGHT];
  size_t sp = 0;
  stack[sp++] = cbm->root;

  while (sp > 0) {
    bmap_node *node = stack[--sp];

    size_t expected_height =
        1 + (node_height(node->left) > node_height(node->right)
                 ? node_height(node->left)
                 : node_height(node->right));
    if (node->height != expected_height) {
      return false;
    }

    int balance = node_balance(node);
    if (balance > 1 || balance < -1) {
      return false;
    }

    if (node->left) {
      if (sp >= CBMAP_MAX_TREE_HEIGHT)
        return false; /* Deeper than any real AVL tree can get */
      stack[sp++] = node->left;
    }
    if (node->right) {
      if (sp >= CBMAP_MAX_TREE_HEIGHT) return false;
      stack[sp++] = node->right;
    }
  }

  return true;
}
#endif
