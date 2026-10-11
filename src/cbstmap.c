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
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Upper bound on the height of any AVL tree that this library can hold. The
 * height of an AVL tree is not more than about 1.44 * log2(n + 2), so this
 * bound covers every element count that a 64-bit size_t can hold, with a
 * wide safety margin. Every internal stack in this file (the ancestor path
 * for an insert, a delete or a balance operation, the post-order destroy
 * stack, and the stack of the in-order iterator) holds at most one entry for
 * each tree level, so a fixed array of this size can never overflow for a
 * real AVL tree. This lets these hot paths use no heap memory at all, where a
 * heap allocation on every change to the map could fail. */
#define CBMAP_MAX_TREE_HEIGHT 128

/* The alignment that a key of this declared type needs. The function takes
 * it with _Alignof on the type that the enumerator names, so the answer is
 * the answer of the target itself; it does not assume that the alignment
 * equals the width, because the two differ on more than one supported
 * target.
 *
 * ccol_other_types is an opaque struct of the caller whose real requirement
 * this module cannot know, and the function treats a type that it does not
 * recognize in the same way: both keep max_align_t. A string key stores its
 * bytes and not a pointer to them, which is why char alignment is the
 * correct answer for a string key. */
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
/* The value keeps max_align_t. A map records the type of its keys but not
   the type of its values, so this module cannot know the type that the
   caller casts the value pointer to, and the strongest alignment is the only
   correct answer here. This costs nothing on the path that matters, because
   a descent through the tree reads keys and never values. */
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
  /* The height of an AVL tree is not more than about 1.44*log2(n+2), and
     CBMAP_MAX_TREE_HEIGHT is the ceiling that this module enforces, so one
     byte holds any height that can happen here. The narrow field is storage
     only: every computation on a height runs at the width that node_height()
     returns, so no arithmetic changes. The narrow field makes the struct
     small enough that the key bytes of a node start inside the same 64-byte
     span as the pointers and the sizes that a descent reads; with a wider
     field, the key bytes start at the boundary just past that span. */
  uint8_t height;
  /* False while the bytes of the value sit in the allocation of this node,
     which is the state every node starts in. An update that changes the size
     of the value moves the bytes into a buffer of their own and sets this
     flag to true, because the space inside the node is exactly the size that
     the value had at creation and cannot grow. A change of size is the
     exception and not the rule: a typed map writes the same width every
     time, so the common update path keeps the value inside the node for the
     whole life of the node. */
  bool val_is_external;
} bmap_node;

typedef struct cbinarymap {
  size_t elem_count;
  bmap_node *root;
  ccol_memmgmt_procs_t *m_procs;
  ccol_data_type key_type;
  /* The map computes this from key_type once, at creation, and the value
     cannot change after that. Without this field, every node that the map
     builds repeats the same switch on the path that builds it. */
  size_t key_alignment;
  ccol_comparison_proc_t custom_comparison_proc;
  /* The offset of the key bytes inside every node, and the kind of
     comparison that every descent of this map makes. Both follow from
     key_type and custom_comparison_proc at creation and never change, so a
     descent chooses its comparison once per call, reads the key it looks for
     once, and reads each node key at this fixed offset; it does not
     dispatch on the key type, compare two sizes and chase key_pair.ptr at
     every node. */
  size_t key_offset;
  uint8_t cmp_kind;
} cbinarymap;

typedef struct cbmap_cmap_iterator {  // A cmap_iterator with more fields
  /* The free function of the allocator that made this iterator, or NULL for
   * the default one. The scope-exit cleanup of ccol_iter_declare can free an
   * iterator that a loop left early after the caller has already destroyed
   * the map, and with it the procs struct that the map owns, so freeing the
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
  bmap_node **parent_link;  // Points to the left or right pointer of the parent
} node_stack_entry;

/* Pushes node and all of its left descendants onto the node stack of the
 * iterator. This keeps the "visit the left subtree first" invariant of the
 * iterative in-order walk, so the next pop gives the smallest key of this
 * subtree that the walk has not visited. The stack is a fixed array of
 * CBMAP_MAX_TREE_HEIGHT entries inside the iterator itself, and at most one
 * entry for each tree level waits at one time (see the comment on
 * CBMAP_MAX_TREE_HEIGHT). */
static void push_all_lefts_into_iter_stack(cbmap_cmap_iterator *real_iter,
                                           bmap_node *node) {
  while (node) {
    ccol_assert(real_iter->node_stack_len < CBMAP_MAX_TREE_HEIGHT);
    real_iter->node_stack[real_iter->node_stack_len++] = node;
    node = node->left;
  }
}

/* Pops one node, the current in-order node, from the stack of the iterator,
 * pushes all the left descendants of the right child of that node, and then
 * updates the key pair pointer and the value pair pointer that the caller
 * sees. If the stack is empty, the function destroys the iterator and
 * returns NULL to show that the walk is complete. */
static cmap_iterator *cmap_real_iter_next(cbmap_cmap_iterator *real_iter) {
  if (real_iter->node_stack_len == 0) {
    // Nowhere to advance
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

/* Creates an in-order iterator at the first key, which is the smallest key,
 * and returns it. The iterator walks the tree iteratively with an explicit
 * fixed-size stack, an array of bmap_node* inside the iterator itself. The
 * function returns NULL when the map is empty. */
static cmap_iterator *cbmap_iter_next(cmap_iterator *iter);

cmap_iterator *cbmap_begin_iter(cbmap cbm, char **err) {
  if (err) {
    *err = NULL;
  }

  // This function treats a NULL cbm in the same way as an empty map (see the
  // doc comment of this function in cbstmap.h), as chashmap_begin_iter does
  // with a NULL handle, on purpose: a map field that stays uninitialized
  // because the caller has put nothing into it can be iterated directly,
  // with no NULL guard of the caller's own first.
  if (!cbm || !cbm->root) {
    return NULL;
  }

  cbmap_cmap_iterator *real_iter =
      _ccol_mem_alloc(cbm->m_procs, sizeof(cbmap_cmap_iterator));
  if (!real_iter) {
    if (err) {
      *err = CCOL_ERR_STR("Failed to create the iterator buffer");
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
 * the iterator and returns NULL when the walk is complete. */
static cmap_iterator *cbmap_iter_next(cmap_iterator *iter) {
  // Advance to the next node
  if (!iter) {
    ccol_assert(false);
  }

  cbmap_cmap_iterator *real_iter = cmapIter2CbmapIter(iter);
  return cmap_real_iter_next(real_iter);
}

/* Frees the iterator struct, which holds its node stack inline.
 * cmap_real_iter_next calls this function at the end of the walk, and the
 * caller can also call it early to stop the walk before its end. It reads
 * nothing of the map, so it stays valid after the map is destroyed. */
void __cbmap_iterator_destroy(cmap_iterator *iter) {
  if (iter) {
    cbmap_cmap_iterator *real_iter = cmapIter2CbmapIter(iter);
    ccol_free_t free_fn = real_iter->free_fn;
    if (free_fn) {
      free_fn(real_iter);
    } else {
      ccol_mem_free(real_iter);
    }
  }
}

/* Validates the creation inputs for the BST map, which means only the memory
 * management procedures. The key type needs no validation, because every
 * ccol_data_type value is legal: a value that the library does not recognize
 * uses the memcmp comparison, in the same way as ccol_other_types. */
/* The comparison that a descent makes. CBMAP_CMP_GENERIC goes through
 * compare_keys(); every other kind is a fixed-width integer key, or a plain
 * char key, with no custom comparison proc: the descent compares two values
 * of that type with < and ==, which is exactly the order that compare_keys()
 * gives such a key (ccol_typed_cmp, and the native char for ccol_char). */
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
  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return false;
  }

  return true;
}

/* Creates an empty AVL-balanced BST map. key_type selects the default
 * comparison strategy (see compare_keys()). A custom_comparison_proc that is
 * not NULL replaces every built-in key comparison. */
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
      *err = CCOL_ERR_STR("Failed to allocate cbinarymap area");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(cbm, mmgmt_procs, err)) {
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

/* Frees the node struct, and the value buffer only when an update moved the
 * value out of the allocation of that struct. The key never needs a free of
 * its own, and neither does a value that sits inside the node (see
 * create_new_node). This function does not touch the left and right
 * pointers, so the caller must unlink the node before this call. */
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
 * explains why this bound is always enough. A post-order walk frees both
 * children before the parent, so the left and right pointers of the parent
 * stay valid during the walk. At the end, the function sets root to NULL and
 * elem_count to zero. */
static void _clear_nodes(cbmap cbm) {
  if (!cbm->root) {
    return;
  }

  bmap_node *stack[CBMAP_MAX_TREE_HEIGHT];
  size_t stack_len = 0;

  bmap_node *current = cbm->root;
  bmap_node *last_visited = NULL;

  while (stack_len > 0 || current) {
    // Go to the leftmost node
    if (current) {
      ccol_assert(stack_len < CBMAP_MAX_TREE_HEIGHT);
      stack[stack_len++] = current;
      current = current->left;
    } else {
      // Look at the top of the stack
      bmap_node *peek = stack[stack_len - 1];

      // If a right child exists and the walk has not handled it
      if (peek->right && peek->right != last_visited) {
        current = peek->right;
      } else {
        // Handle this node, now that both children are done
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
      ccol_mem_free(cbm);
    }
  }
}

/* Returns the number of key-value pairs that the map stores. */
size_t cbmap_elem_count(cbmap cbm) {
  if (!cbm) {
    ccol_assert(false);
  }

  return cbm->elem_count;
}

/* Removes all the elements from the map, which becomes empty and stays
 * usable. */
ccol_retval_t cbmap_reset(cbmap cbm) {
  if (!cbm) {
    ccol_assert(false);
  }

  _clear_nodes(cbm);

  return ccol_success;
}

/* Compares the two signed integers of size 1, 2, 4 or 8 bytes (signed char,
 * short, int, long and long long) that ptr1 and ptr2 point at. The function
 * uses typed dereferences instead of memcmp, because a typed dereference has
 * no sign-extension problem: 0xFF in a signed byte is -1, and not 255. This
 * function never runs for ccol_char, which is a plain `char` key: the C
 * standard leaves the signedness of a plain `char` to the platform, so
 * compare_keys() gives ccol_char its own case that uses the native `char`
 * type, instead of forcing a signed int8_t reinterpretation on it. A char key
 * then sorts exactly like the `<` and `>` operators of this platform on
 * `char`. A key that the caller declares as `signed char`, or as its typedef
 * `int8_t`, is a different type, ccol_signed_char, and gets a real signed
 * comparison here that does not depend on the platform. The same is true for
 * every other signed integer width. */
static inline int cmp_signed_small(void *ptr1, void *ptr2, size_t size) {
  switch (size) {
    case 1: {
      return ccol_typed_cmp(ptr1, ptr2, int8_t);
    }
    case 2: {
      return ccol_typed_cmp(ptr1, ptr2, int16_t);
    }
    case 4: {
      return ccol_typed_cmp(ptr1, ptr2, int32_t);
    }
    case 8: {
      return ccol_typed_cmp(ptr1, ptr2, int64_t);
    }
    default: {
      // For any other size, report a failure: the library must not classify
      // such a key as a 'signed' number
      ccol_assert(false);
      return 0;  // The code never reaches this line, because
                 // ccol_assert(false) always aborts. The line is here so
                 // that this non-void function has a defined return on every
                 // path, which keeps -Wreturn-type quiet.
    }
  }
}

/* Compares two unsigned integers, or two raw pointer values, of size 1, 2, 4
 * or 8 bytes, and uses memcmp for any other size. This function runs only
 * for a key_type that the library knows to be an unsigned integer or a
 * pointer, and never for an opaque struct that has one of these sizes (see
 * ccol_other_types in compare_keys(), which always uses memcmp). */
static inline int cmp_unsigned_small(void *ptr1, void *ptr2, size_t size) {
  switch (size) {
    case 1: {
      return ccol_typed_cmp(ptr1, ptr2, uint8_t);
    }
    case 2: {
      return ccol_typed_cmp(ptr1, ptr2, uint16_t);
    }
    case 4: {
      return ccol_typed_cmp(ptr1, ptr2, uint32_t);
    }
    case 8: {
      return ccol_typed_cmp(ptr1, ptr2, uint64_t);
    }
    default: {
      return memcmp(ptr1, ptr2, size);
    }
  }
}

/* Three-way comparison for a floating-point type T, with an explicit rule
 * for NaN. A BST needs a real strict total order to stay consistent, because
 * every descent for a lookup, an insert or a delete must agree on the side
 * of a node that a key belongs to. The native `<` and `>` of IEEE 754 cannot
 * give that order for NaN, since both operators are false for any
 * comparison with a NaN operand, so a NaN key compares "equal" to every
 * other key, including keys that have no relation to it. This macro orders
 * NaN as greater than every non-NaN value and equal only to another NaN,
 * which gives a real total order in which NaN keys sort together at the
 * high end of the tree. Without the rule, a NaN key compares "equal" to the
 * first node that the search visits, so the insert corrupts the value of
 * that unrelated node and never adds the NaN key. isnan() is a type-generic
 * macro from the C99 <math.h>, so this one helper serves float, double and
 * long double, with no variant for each type. */
/* The macro reads both operands with memcpy instead of dereferencing a
 * pointer that it cast to T *. ccol_typed_cmp in common.h carries the full
 * reasoning: a key pair that a caller built by hand and passed through
 * cbmap_insert_elem, cbmap_get_elem_ref or cbmap_delete_elem carries neither
 * an alignment guarantee for T nor an effective type of T. A constant-size
 * memcpy compiles to the same single load.
 *
 * Each temporary carries a name that names this module, because a caller of
 * this macro supplies ptr1 and ptr2 as arbitrary expressions, and those
 * expressions land inside the scope of these temporaries: a temporary called
 * _v1 would capture a caller identifier of the same name and read an
 * uninitialised value instead. */
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

/* Compares the two floating-point values that ptr1 and ptr2 point at. The
 * function dispatches on the declared key_type of the map, and not on the
 * size, because the size of a long double changes with the platform: on some
 * ABIs it equals the size of a double, and a switch on the size cannot
 * separate the two. A native floating-point comparison orders negative and
 * positive values correctly and treats -0.0 and 0.0 as equal, while a raw
 * reinterpretation of the bit pattern does neither. See the comment on
 * cmp_float_val for the NaN rule. */
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
      // The library calls this function only for one of the three
      // floating-point key types.
      ccol_assert(false);
      return 0;  // The code never reaches this line, because
                 // ccol_assert(false) always aborts. The line is here so
                 // that this non-void function has a defined return on every
                 // path, which keeps -Wreturn-type quiet.
    }
  }
}

/* Central dispatch for the key comparison. The function tries three things,
 * in this order: custom_comparison_proc; then the path for two keys of
 * different sizes, which runs only for keys of variable length, such as
 * strings; and last a dispatch on the declared key_type of the map. For two
 * keys of different sizes, the function compares the common prefix first,
 * and then the length, so a shorter key sorts before a longer one. This
 * gives a consistent BST order for keys of variable length.
 *
 * The dispatch on key_type separates five groups: real signed integers, real
 * unsigned integers and pointers, real floating-point types, strings, and
 * everything else, which is ccol_other_types, for example a struct key or an
 * enum key. Only the first three groups get a typed numeric
 * reinterpretation, because that reinterpretation is only valid for a type
 * that is a number of that kind. Every type that the dispatch does not
 * recognize, ccol_other_types included, uses a raw memcmp of its
 * representation, which matches the documented behavior of this module for
 * a struct key of any size. */
static inline int compare_keys(cbmap cbm, const cmap_pair *key_pair1,
                               const cmap_pair *key_pair2) {
  if (cbm->custom_comparison_proc) {
    return cbm->custom_comparison_proc(key_pair1->ptr, key_pair2->ptr);
  }

  if (key_pair1->size != key_pair2->size) {
    // The sizes differ: compare the common prefix first, then the size. The
    // function calls no memcmp when min_size is 0, which means that one side
    // is a key of size zero; create_new_node stores such a key
    // as ptr == NULL and size == 0. A comparison of zero bytes is always
    // "equal", whatever the two pointers are, but the pointer parameters of
    // memcmp are declared nonnull in the <string.h> of glibc, and UBSan
    // enforces that even for a length of zero. A call to memcmp with a real
    // NULL pointer is undefined behavior on its own, even when
    // memcmp reads no byte at all, and the stored representation of a key of
    // size zero always has such a NULL pointer.
    size_t min_size = ccol_min(key_pair1->size, key_pair2->size);
    int cmp =
        min_size == 0 ? 0 : memcmp(key_pair1->ptr, key_pair2->ptr, min_size);
    if (cmp != 0) {
      return cmp;
    }

    // The common prefix is equal, so the shorter string comes first
    return (key_pair1->size > key_pair2->size) -
           (key_pair1->size < key_pair2->size);
  }

  switch (cbm->key_type) {
    case ccol_string: {
      // The size of a string key always includes at least a null terminator
      // (see _populate_cmap_pair), so key_pair1->size is never 0 for a call
      // that comes through a macro. This guard, like the same guard below for
      // ccol_other_types, is a standing defence: it protects a caller that
      // uses the raw cbmap_insert_elem or cbmap_get_elem_ref layer directly
      // with a hand-built cmap_pair of size zero, on a map whose key type is
      // ccol_string.
      if (key_pair1->size == 0) return 0;
      return memcmp(key_pair1->ptr, key_pair2->ptr, key_pair1->size);
    }

    case ccol_char: {
      // A plain `char` is a different type from both `signed char` and
      // `unsigned char`, and the C standard leaves its signedness to the
      // platform: it is signed on x86 and x86_64, for example, and unsigned
      // on the standard aarch64 AAPCS64 ABI. This case compares with the
      // native `char` type itself instead of forcing a signed int8_t
      // reinterpretation, so the default comparator always agrees with the
      // `<` and `>` operators of this platform on `char`. ccol_unsigned_char
      // gets its own comparison below, in the same way.
      return ccol_typed_cmp(key_pair1->ptr, key_pair2->ptr, char);
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
      // A POD type has no numeric interpretation; this covers structs,
      // unions, enums, bool, wchar_t and other types of that kind, which this
      // case orders by their raw byte representation. The case checks for
      // key_pair1->size == 0 before it calls memcmp, because that size is a
      // real key of size zero, which create_new_node stores as a NULL pointer
      // with a size of 0. The reason for the check is the one given in the
      // branch above for two different sizes: the pointer parameters of memcmp
      // are declared nonnull even for a length of zero, and the stored ptr of
      // a key of size zero is NULL, so a call to memcmp here without the
      // check is undefined behavior, even when memcmp reads no byte at all.
      // Two keys of size zero of this type are always equal.
      if (key_pair1->size == 0) return 0;
      return memcmp(key_pair1->ptr, key_pair2->ptr, key_pair1->size);
    }
  }
}

/* Copies n bytes. A key or a value is most often a scalar of 1, 2, 4, 8 or
 * 16 bytes, and for those sizes each case is a memcpy of constant length that
 * the compiler turns into one load and one store, where a memcpy of a length
 * known only at run time is a call into the C library. cbmap_move_bytes is
 * the same for ranges that may overlap: a constant-size memmove loads every
 * byte before it stores any. */
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

/* The descents of this map. Both choose the comparison once per call from
 * cbm->cmp_kind. For an integer or char key with no custom proc they read
 * the key they look for once, read each node key at cbm->key_offset with a
 * constant-size memcpy, and compare with < and ==; every other key goes
 * through compare_keys(). Both rely on the caller having checked that a
 * fixed-width key has the width of its type (key_size_matches_type_if_fixed_
 * width), so the read of the key the caller passed never runs past its
 * bytes. */
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

/* Gives the node that holds key_pair, or NULL. */
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
      ccol_assert(len < CBMAP_MAX_TREE_HEIGHT);                            \
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

/* Walks from the root toward key_pair and records every node it passes in
 * path, each with the link that points at it. It gives the node that holds
 * the key, or NULL when the key is absent, and sets *link_out to the link
 * that points at the node found, or to the empty link where the key belongs. */
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
        ccol_assert(len < CBMAP_MAX_TREE_HEIGHT);
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

/* Allocates a new BST node. One single allocation of the node carries the key
 * bytes and the value bytes, and neither one starts in a buffer of its own.
 *
 * This function never frees node->key_pair.ptr, and frees node->val_pair.ptr
 * only when val_is_external says that an update moved that value out: a free
 * of one of these two pointers without that condition is a free of an
 * interior pointer, which is heap corruption, and not a leak. Read
 * destroy_bmap_node and the comment on val_is_external before you change what
 * this function allocates.
 *
 * The key does not change for the whole life of the node: this function never
 * resizes a key, and a removal that needs a replacement relinks the donor node
 * instead of copying its contents (see perform_element_removal), so a key
 * never has to outlive a node or to move between nodes. One allocation for all
 * three parts saves two allocations for each node, and for the key it also
 * removes the pointer chase that every comparison along a search path pays
 * when the key sits in a buffer of its own. A walk reads only keys, from the
 * node that it has already loaded, which keeps a descent inside the cache
 * lines that it has already taken. Only an update can change the size of a
 * value, and that is what moves the value out.
 *
 * This function never gives a key or a value of size zero to the allocator.
 * The C standard lets malloc(0) return NULL or a unique pointer, so an
 * allocator can legally choose NULL for a request of size zero, and a test of
 * the result against NULL then reports a real key or value of zero bytes as
 * ccol_not_enough_memory. A pair of size zero is stored as ptr == NULL and
 * size == 0 instead, a representation that every reader in this file handles
 * safely for a length of zero: the memcmp of compare_keys, the size-guarded
 * copies of create_new_node and cbmap_get_elem_copy, and the _ccol_mem_free
 * of destroy_bmap_node. */
static bmap_node *create_new_node(cbmap cbm, const cmap_pair *key_pair,
                                  const cmap_pair *val_pair) {
  /* The node, the key and the value are one allocation, and this function
     forms the byte count here instead of leaving that to the allocator.
     Without these checks, a size that makes this sum wrap becomes a small
     allocation that succeeds, followed by copies through that small block.
     The function checks each term against what remains, so no intermediate
     value can overflow either. */
  /* Each offset is rounded to what the object at that offset needs: the key
     to the requirement of its own declared type, and the value to the
     strongest alignment, because the map does not record the type of a
     value. A key that is rounded to max_align_t starts past the end of the
     64-byte span of the struct, for every key that is narrower than
     max_align_t, while a descent has already loaded that span when it
     compares. */
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

  /* A key or a value of size zero keeps the representation with a NULL
     pointer and a size of 0, which every reader in this file already
     handles, instead of a pointer one past the block. */
  new_node->key_pair.ptr =
      (key_pair->size > 0) ? (char *)new_node + key_offset : NULL;
  new_node->val_pair.ptr =
      (val_pair->size > 0) ? (char *)new_node + val_offset : NULL;
  new_node->val_is_external = false;

  /* These copies are guarded on the size, because a key or a value of size
     zero carries a NULL pointer here (the representation of empty in this
     file), and memcpy declares both of its pointer parameters as never null,
     even for a length of zero. Without these guards,
     UndefinedBehaviorSanitizer reports "null pointer passed as argument 1,
     which is declared to never be null". */
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

/* Returns the larger value of x and y. This file has its own version so that
 * it does not have to include <stdlib.h> only for a max of two arguments. */
static size_t maximum(size_t x, size_t y) {
  if (x >= y) {
    return x;
  }
  return y;
}

/* Writes over the value of a node that is already in the map. If the new
 * value has a different size, the function tries a new allocation; on a
 * failure it keeps the old value and the old size and does not change
 * *result, so the caller sees ccol_not_enough_memory. On success it updates
 * the value bytes and the size field, and sets *result to
 * ccol_key_already_present.
 *
 * A value whose size does not change goes straight into the place where it
 * already is, which for almost every map is the allocation of the node,
 * because a typed map writes the same width every time. Only a change of
 * size moves the value out into a buffer of its own, since the room inside
 * the node is exactly the size that the value had at creation. That move
 * deliberately goes only one way: the node does not record how much room it
 * reserved, so a value that becomes smaller later stays in its own buffer,
 * and the function does not put it back inside the node, where the space is
 * not known. The function never asks the allocator to realloc a pointer into
 * the block of the node, which is undefined. It allocates the new buffer
 * first and frees the old buffer only after the new allocation succeeds, so
 * a failure leaves the node exactly as it was, and the caller sees
 * ccol_not_enough_memory.
 *
 * A new size of 0 frees any buffer that the value moved out to, then stores
 * the representation with a NULL pointer and a size of 0, which is the
 * representation that create_new_node uses for a value of size zero. The
 * function does not ask realloc for zero bytes, because the C standard
 * leaves that behavior to the implementation: glibc answers such a request
 * with a free and a NULL return, and the return value alone cannot separate
 * that answer from a failure that left the old buffer valid. */
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
      // The allocation failed, so the node keeps the value that it already had
      return;
    }
    /* This copy goes into the new storage BEFORE the function frees the old
       storage, and the function then returns here instead of falling through
       to the copy below, because val_pair->ptr can BE the storage that the
       function is about to free: cbmap_get_elem_ref gives a caller a pointer
       into the own value of the node, and a caller can normally give that
       pointer back to resize the value. A read of that pointer after the free
       is a use-after-free that AddressSanitizer reports. */
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
  /* The size is the same, so the value goes where it already is. The source
     can alias that storage, for the same reason as above. This path frees
     nothing, but a copy whose ranges overlap is undefined all the same, which
     is why the copy uses memmove and not memcpy. */
  if (node->val_pair.ptr != val_pair->ptr && val_pair->size > 0) {
    cbmap_move_bytes(node->val_pair.ptr, val_pair->ptr, val_pair->size);
  }
  *result = ccol_key_already_present;
}

/* Returns the height of a node, or 0 for NULL. A leaf node starts at height
 * 1, so the NULL sentinel is 0 and all the arithmetic stays in size_t. */
static size_t node_height(bmap_node *node) {
  if (!node) {
    return 0;
  }

  return node->height;
}

/* Returns the balance factor of a node, which is right_height minus
 * left_height. A value outside [-1, 1] means that the node breaks the AVL
 * invariant. */
static int node_balance(bmap_node *node) {
  if (!node) {
    return 0;
  }

  return (int)node_height(node->right) - (int)node_height(node->left);
}

/* Computes node->height again from the heights of its children. The caller
 * must call this function after every rotation and after every change to the
 * structure, which keeps the height data correct for the balance factor
 * computations up the chain of ancestors. */
static void recalculate_node_height(bmap_node *node) {
  if (!node) {
    return;
  }

  /* The cast narrows the value without a check. The height of an AVL tree is
     not more than about 1.44*log2(n+2), and the address space bounds the node
     count, so the tallest tree that any 64-bit target can hold is about 92
     levels, while the field holds 255. Only one thing can make this cast
     truncate: a CBMAP_MAX_TREE_HEIGHT above what one byte holds, which the
     _Static_assert beside that constant refuses at compile time. A run-time
     check here would sit on the path of every insert, and it could never
     fire. */
  node->height =
      (uint8_t)(maximum(node_height(node->left), node_height(node->right)) + 1);
}

/* Restores the AVL invariant at parent when the tree needs it, with one of
 * four rotations: a simple left one, a simple right one, a right-left double
 * one, or a left-right double one. It returns the new root of the subtree
 * after the rotation, which can be a different node, and computes the
 * heights again, from the bottom up, after each rotation. */
static bmap_node *check_node_balance(bmap_node *parent) {
  recalculate_node_height(parent);
  int balance = node_balance(parent);

  if (balance > 1 || balance < -1) {
    // The symmetry is lost, so a rotation is needed
    bmap_node *p = parent;

    if (balance > 1) {
      // Right side is deeper
      if (node_balance(parent->right) >= 0 || !(parent->right->left)) {
        // Simple left rotation
        parent = parent->right;
        p->right = parent->left;
        parent->left = p;
        // Compute the heights again, from the bottom up
        recalculate_node_height(p);
        recalculate_node_height(parent);
      } else {  // node_balance(parent->right) < 0
        // Right-Left double rotation
        bmap_node *rl_left = parent->right->left->left;
        bmap_node *rl_right = parent->right->left->right;
        parent = parent->right->left;
        parent->left = p;
        parent->right = p->right;
        parent->right->left = rl_right;
        parent->left->right = rl_left;
        // Compute the heights of both children again, then the parent
        recalculate_node_height(parent->left);
        recalculate_node_height(parent->right);
        recalculate_node_height(parent);
      }
    } else {
      // Left side is deeper
      if (node_balance(parent->left) <= 0 || !(parent->left->right)) {
        // Simple right rotation
        parent = parent->left;
        p->left = parent->right;
        parent->right = p;
        // Compute the heights again, from the bottom up
        recalculate_node_height(p);
        recalculate_node_height(parent);
      } else {  // node_balance(parent->left) > 0
        // Left-Right double rotation
        bmap_node *lr_left = parent->left->right->left;
        bmap_node *lr_right = parent->left->right->right;
        parent = parent->left->right;
        parent->right = p;
        parent->left = p->left;
        parent->left->right = lr_left;
        parent->right->left = lr_right;
        // Compute the heights of both children again, then the parent
        recalculate_node_height(parent->left);
        recalculate_node_height(parent->right);
        recalculate_node_height(parent);
      }
    }
  }

  int final_balance = node_balance(parent);
  if (final_balance > 1 || final_balance < -1) {
    ccol_assert(false);
  }
  return parent;
}

/* Detaches the minimum node or the maximum node of the subtree at root, and
 * returns it: a max of false selects the minimum node, and a max of true
 * selects the maximum node. The detached node can have one child, which the
 * function links into the slot of the parent before it balances the ancestor
 * path again, from the bottom up, with check_node_balance.
 * perform_element_removal uses this function to find an in-order successor or
 * predecessor without any recursive call.
 *
 * The function keeps the ancestor path in a fixed array of
 * CBMAP_MAX_TREE_HEIGHT entries instead of a stack on the heap, so it has no
 * allocation that can fail; a heap allocation can abort the whole process
 * under memory pressure, on a delete path that must stay graceful. This
 * function can only fail through the same depth bound that the rest of this
 * file treats as structurally impossible for a real AVL tree (see
 * CBMAP_MAX_TREE_HEIGHT). */
static bmap_node *cbmap_detach_extreme_iter(bmap_node *root, bool max,
                                            bmap_node **extreme) {
  if (!root) {
    ccol_assert(false);
  }

  node_stack_entry path[CBMAP_MAX_TREE_HEIGHT];
  size_t path_len = 0;

  // Find the extreme node and build the path
  bmap_node *current = root;
  bmap_node **parent_link = NULL;

  while (true) {
    if (max) {
      if (current->right) {
        ccol_assert(path_len < CBMAP_MAX_TREE_HEIGHT);
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
        ccol_assert(path_len < CBMAP_MAX_TREE_HEIGHT);
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

  // Update the parent link, and return the replacement if the extreme node
  // is the root
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

  // Balance the tree again, from the bottom to the top
  for (size_t i = path_len; i-- > 0;) {
    node_stack_entry entry = path[i];
    bmap_node *balanced = check_node_balance(entry.node);

    // Update the pointer of the parent with the parent_link from the path
    if (entry.parent_link) {
      *(entry.parent_link) = balanced;
    } else {
      // This is the root
      root = balanced;
    }
  }

  return root;
}

/* A key of a fixed-width key type must arrive at exactly that type's own
 * size, on every raw-layer entry point that takes a key_pair.
 *
 * compare_keys() orders two keys of different sizes by their common prefix
 * first, and then by their size, which gives a key type of variable width
 * (ccol_string and ccol_other_types) its total order. The same rule on a
 * fixed-width key type turns a key of the wrong type, such as a long that a
 * caller gives to a map with int keys, into a different key: no lookup with
 * the correct type can reach that key again, and nothing reports the
 * mistake. A custom_comparison_proc does not make the mismatch harmless,
 * because it gets two bare pointers and no sizes, so it can only read the
 * width of the declared key type, and a shorter key_pair then makes it read
 * past the buffer of the caller.
 *
 * ccol_fixed_width_data_type_size() reports 0 for a key type whose width is
 * not fixed, which is what lets a string key or a struct key arrive at any
 * size, 0 included. chashmap enforces the same rule through the same helper,
 * so the two map modules agree on the key types that the rule covers. */
static inline bool key_size_matches_type_if_fixed_width(
    cbmap cbm, const cmap_pair *key_pair) {
  size_t fixed_width = ccol_fixed_width_data_type_size(cbm->key_type);
  return fixed_width == 0 || key_pair->size == fixed_width;
}

/* Answers the question whether a pair from the caller describes bytes that
 * the library can read. Every reader in this file (the memcmp of
 * compare_keys, the copy of create_new_node and the copy of
 * update_bmap_node_value) trusts the size of the pair, so a NULL pointer with
 * a non-zero size is a read through a null pointer, and not a lookup miss.
 * Without this check, the first such call gets a segmentation fault inside
 * memcpy or memcmp.
 *
 * A size of 0 is legal, with or without a pointer, because that is the
 * representation of an empty key or value in this module, and every reader
 * here already handles it. chashmap differs on exactly that point, because it
 * rejects a pair of size zero: the two map modules agree on the null-pointer
 * half of the rule, and they deliberately part on the zero-size half. */
static inline bool pair_bytes_are_readable(const cmap_pair *pair) {
  return pair->ptr != NULL || pair->size == 0;
}

/* Inserts a key-value pair, or updates one that is already in the map. The
 * function records the chain of ancestors in an explicit, fixed-size path
 * array (see CBMAP_MAX_TREE_HEIGHT), which lets it balance the tree again
 * after the insert, from the bottom up, with no recursion and with no heap
 * allocation on any call. The function returns ccol_key_already_present
 * when the key is already in the map and the update of its value succeeds. */
ccol_retval_t cbmap_insert_elem(cbmap cbm, const cmap_pair *key_pair,
                                const cmap_pair *val_pair) {
  if (!cbm) {
    ccol_assert(false);
  }

  /* This function reads a pair from the caller instead of only passing the
     pair on, so it reports a NULL pair, and a pair that promises bytes but
     has no pointer to them, without dereferencing either one.
     chmap_insert_elem rejects both of them in the same way; it also rejects
     a key or a value of size zero, which this module accepts (see
     pair_bytes_are_readable). */
  if (!key_pair || !val_pair || !pair_bytes_are_readable(key_pair) ||
      !pair_bytes_are_readable(val_pair)) {
    return ccol_invalid_args;
  }

  if (!key_size_matches_type_if_fixed_width(cbm, key_pair)) {
    return ccol_invalid_args;
  }

  // Handle the case of an empty tree. elem_count is always 0 here, because
  // root is NULL only when the map is empty, so no capacity check is needed
  // here: this is always a new element, and the map can never hold
  // ccol_max_elem_count elements at this point. That cap is real, but it is
  // very large.
  if (!cbm->root) {
    cbm->root = create_new_node(cbm, key_pair, val_pair);
    if (!cbm->root) {
      return ccol_not_enough_memory;
    }
    cbm->elem_count++;
    return ccol_success;
  }

  // A fixed-size stack that keeps the path from the root to the insert point
  node_stack_entry path[CBMAP_MAX_TREE_HEIGHT];
  size_t path_len = 0;
  bmap_node **parent_link = NULL;
  ccol_retval_t result = ccol_not_enough_memory;

  // Go to the insert point, or to the key that is already in the map
  bmap_node *existing =
      cbmap_descend_with_path(cbm, key_pair, path, &path_len, &parent_link);
  if (existing) {
    // The key is already in the map, so update its value
    update_bmap_node_value(cbm, existing, val_pair, &result);
    return result;
  }

  // The key is not in the map. This insert grows elem_count, so this is the
  // correct point for the capacity cap. The function checks the cap only
  // here, after it confirms that the key is not in the map, so that an
  // update of the value of a key in a full map succeeds, and the function
  // does not reject it.
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

  /* Balance the tree again, from the bottom up, and stop as soon as the
     height of a subtree is the height it had before the insert: every
     ancestor above that point then sees the same child heights as before, so
     it needs neither a new height nor a rotation. An insert into an AVL tree
     causes at most one rotation, and that rotation gives its subtree back
     the height it had before the insert, so the walk also stops right after
     it. Without the stop, every insert recomputes the height of every
     ancestor and reads both children of each, which touches nodes off the
     search path all the way to the root. */
  for (size_t i = path_len; i-- > 0;) {
    node_stack_entry entry = path[i];
    size_t height_before = entry.node->height;
    bmap_node *balanced = check_node_balance(entry.node);

    // Update the pointer of the parent to this node
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

/* Searches for key_pair and copies the value of that key into target_buf.
 * The function returns ccol_invalid_args when target_buf_size does not match
 * the size of the stored value exactly, which stops a truncation that
 * nothing would report. */
ccol_retval_t cbmap_get_elem_copy(cbmap cbm, const cmap_pair *key_pair,
                                  void *target_buf, size_t target_buf_size) {
  if (!cbm) {
    ccol_assert(false);
  }

  /* See the guard of cbmap_insert_elem. */
  if (!key_pair || !pair_bytes_are_readable(key_pair)) {
    return ccol_invalid_args;
  }

  /* A stored value of size 0 is read with a NULL buffer of size 0. */
  if (!target_buf && target_buf_size != 0) {
    return ccol_invalid_args;
  }

  if (!key_size_matches_type_if_fixed_width(cbm, key_pair)) {
    return ccol_invalid_args;
  }

  bmap_node *tracker = cbmap_find_node(cbm, key_pair);
  if (!tracker) {
    return ccol_key_not_found;
  }
  if (target_buf_size != tracker->val_pair.size) {
    return ccol_invalid_args;
  }
  /* A stored value of size 0 keeps a NULL pointer, and the buffer of the
     caller for such a value can also be NULL, so this copy is guarded for
     the same reason as the copy in create_bmap_node. */
  if (target_buf_size > 0) {
    cbmap_copy_bytes(target_buf, tracker->val_pair.ptr, target_buf_size);
  }
  return ccol_success;
}

/* Searches for key_pair and reports the {ptr, size} accessor of the node
 * for its value, which stays valid only while the key is in the map and no
 * update gives it a value of a different size. The target of the accessor
 * is const: the caller can read the accessor and edit the bytes that it
 * describes, but an assignment to either field is a compile error, because
 * the pointer and the size describe one another, and the map cannot own a
 * pointer that it did not allocate. */
ccol_retval_t cbmap_get_elem_ref(cbmap cbm, const cmap_pair *key_pair,
                                 const cmap_pair **val_pair) {
  if (!cbm) {
    ccol_assert(false);
  }

  /* val_pair is the out-parameter that this function writes its result
     through, so a NULL val_pair has nowhere to report a hit. See the guard
     of cbmap_insert_elem for the reason why the key pair gets the same
     answer. */
  if (!key_pair || !val_pair || !pair_bytes_are_readable(key_pair)) {
    return ccol_invalid_args;
  }

  if (!key_size_matches_type_if_fixed_width(cbm, key_pair)) {
    return ccol_invalid_args;
  }

  bmap_node *tracker = cbmap_find_node(cbm, key_pair);
  if (!tracker) {
    return ccol_key_not_found;
  }
  *val_pair = &tracker->val_pair;
  return ccol_success;
}

/* Removes parent from the tree and returns the new root of the subtree. If
 * both children exist, the deeper side gives its extreme node as the
 * replacement for parent: the right side gives its minimum node, and the
 * left side gives its maximum node. This keeps the BST order, and keeps the
 * tree balanced with the smallest number of rotations.
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
    // Both left and right are not NULL
    if (right->height >= left->height) {
      // Right side is deeper
      bmap_node *right_min = NULL;
      right = cbmap_detach_extreme_iter(right, false, &right_min);
      parent = right_min;
      parent->right = right;
      parent->left = left;
      // Compute the height of the replacement node again, after the new
      // children go into it
      recalculate_node_height(parent);
    } else {
      // Left side is deeper
      bmap_node *left_max = NULL;
      left = cbmap_detach_extreme_iter(left, true, &left_max);
      parent = left_max;
      parent->right = right;
      parent->left = left;
      // Compute the height of the replacement node again, after the new
      // children go into it
      recalculate_node_height(parent);
    }
  }

  return parent;
}

/* Deletes the element with key_pair from the map. The function uses the same
 * explicit, fixed-size path array as cbmap_insert_elem, which lets it balance
 * the ancestors again after it removes the node, from the bottom up, with no
 * heap allocation. The function returns ccol_key_not_found when the key is
 * not in the map. */
ccol_retval_t cbmap_delete_elem(cbmap cbm, const cmap_pair *key_pair) {
  if (!cbm) {
    ccol_assert(false);
  }

  /* See the guard of cbmap_insert_elem. */
  if (!key_pair || !pair_bytes_are_readable(key_pair)) {
    return ccol_invalid_args;
  }

  if (!key_size_matches_type_if_fixed_width(cbm, key_pair)) {
    return ccol_invalid_args;
  }

  if (!cbm->root) {
    return ccol_key_not_found;
  }

  // A fixed-size stack that keeps the path from the root to the node to
  // delete
  node_stack_entry path[CBMAP_MAX_TREE_HEIGHT];
  size_t path_len = 0;
  bmap_node **parent_link = NULL;
  ccol_retval_t result = ccol_key_not_found;

  // Go to the node to delete
  bmap_node *current =
      cbmap_descend_with_path(cbm, key_pair, path, &path_len, &parent_link);
  if (current) {
    // This is the node to delete
    bmap_node *replacement = perform_element_removal(cbm, current);
    *parent_link = replacement;
    result = ccol_success;
  }

  if (result == ccol_success) {
    /* Balance the tree again, from the bottom up, and stop at the first
       subtree whose height after balancing equals the height it had before
       the delete, because the ancestors above it then see the same child
       heights as before. A rotation after a delete can lower the height of
       its subtree, so the walk stops on the height and not on whether it
       rotated. */
    for (size_t i = path_len; i-- > 0;) {
      node_stack_entry entry = path[i];
      size_t height_before = entry.node->height;
      bmap_node *balanced = check_node_balance(entry.node);

      // Update the pointer of the parent to this node
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
 * opaque outside this translation unit, so this function is the only way for
 * test code to check these invariants. The function returns one pass or fail
 * answer and no node pointer, so no internal pointer crosses the boundary of
 * the public API.
 *
 * The walk is iterative, with an explicit fixed-size stack instead of
 * recursion, which matches the preference of this module for an iterative
 * tree walk. The stack uses the same CBMAP_MAX_TREE_HEIGHT bound as every
 * other walk stack in this file. */
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
        return false; /* Deeper than any real AVL tree can be */
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
