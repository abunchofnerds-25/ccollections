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

// Upper bound on the height of any AVL tree this map can hold. An AVL tree is
// never taller than about 1.44 * log2(n + 2), so 128 covers every count a
// 64-bit size_t can reach. Every internal stack (the ancestor path of an insert
// or delete, the destroy walk, the iterator) holds one entry per level, so a
// fixed array of this size never overflows and those paths need no heap memory;
// a delete can't run out of memory.
#define CBMAP_MAX_TREE_HEIGHT 128

// Gives the alignment a stored key of this type needs. Creation calls it once
// to place the key bytes inside every node. It uses _Alignof of the type
// itself, since alignment and width differ on some targets. A string key stores
// bytes, so char alignment; ccol_other_types and unknown types keep
// max_align_t, because their real requirement isn't known.
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

// Evaluates to n rounded up to a multiple of the power of two a; creation and
// create_new_node() place the key and the value with it. A value is aligned to
// max_align_t because the map doesn't record the value type; a descent never
// reads values, so this costs nothing where it matters.
#define CBMAP_ALIGN_UP_TO(n, a) (((n) + (a) - 1u) & ~(size_t)((a) - 1u))
#define CBMAP_NODE_VAL_ALIGN _Alignof(max_align_t)

// bmap_node keeps its height in one byte.
_Static_assert(CBMAP_MAX_TREE_HEIGHT <= UINT8_MAX,
               "bmap_node stores a height in one byte; raising "
               "CBMAP_MAX_TREE_HEIGHT past UINT8_MAX needs a wider field");

// A tree node. One allocation holds the node, then the key bytes at
// cbm->key_offset, then the value bytes; key_pair and val_pair point into it
// (or are {NULL, 0} for an empty key or value).
typedef struct bmap_node {
  cmap_pair key_pair;
  cmap_pair val_pair;
  struct bmap_node *left;
  struct bmap_node *right;
  // One byte holds any height below CBMAP_MAX_TREE_HEIGHT and keeps the key
  // bytes inside the first 64 bytes of the node, which a descent reads anyway.
  uint8_t height;
  // True once an update of another size moved the value out of the node's
  // allocation into a buffer of its own.
  bool val_is_external;
} bmap_node;

// The map. m_procs is a copy of the caller's allocator, or NULL for the
// default.
typedef struct cbstmap {
  size_t elem_count;
  bmap_node *root;
  ccol_memmgmt_procs *m_procs;
  ccol_data_type key_type;
  size_t key_alignment;  // from key_type, fixed at creation
  ccol_comparison_proc custom_comparison_proc;
  // Where the key bytes sit in every node and which comparison a descent makes;
  // both follow from key_type and the comparator at creation.
  size_t key_offset;
  uint8_t cmp_kind;
} cbstmap;

// An in-order iterator: A cmap_iterator plus a fixed stack of nodes whose left
// subtree is done and which are waiting to be visited.
typedef struct cbmap_cmap_iterator {
  // The free function of the map's allocator (NULL for the default). A scoped
  // iterator can be freed after its map is gone, so freeing it never reads the
  // map.
  ccol_free free_fn;
  bmap_node *node_stack[CBMAP_MAX_TREE_HEIGHT];
  size_t node_stack_len;
  cmap_iterator user_iter;
} cbmap_cmap_iterator;

// Evaluates to the cbmap_cmap_iterator that holds the cmap_iterator u_iter, so
// the iterator callbacks can reach the node stack.
#define cmapIter2CbmapIter(u_iter)            \
  (cbmap_cmap_iterator *)((uint8_t *)u_iter - \
                          offsetof(cbmap_cmap_iterator, user_iter))

// One step of the path a descent records: A node and the link that points at
// it.
typedef struct node_stack_entry {
  bmap_node *node;
  bmap_node **parent_link;  // the parent's left or right pointer, or NULL
} node_stack_entry;

// Pushes node and its chain of left children onto the iterator's stack. The
// in-order walk needs it so the next pop is the smallest key of that subtree
// not visited yet. Asserts if the fixed stack would overflow.
static void push_all_lefts_into_iter_stack(cbmap_cmap_iterator *real_iter,
                                           bmap_node *node) {
  while (node) {
    ccol__assert(real_iter->node_stack_len < CBMAP_MAX_TREE_HEIGHT);
    real_iter->node_stack[real_iter->node_stack_len++] = node;
    node = node->left;
  }
}

// Advances real_iter to the next key in order. begin and next both use it. It
// pops a node, pushes the left chain of its right child and points the user
// iterator at the node's pairs. Gives the user iterator, or NULL at the end,
// when it frees the iterator.
static cmap_iterator *cmap_real_iter_next(cbmap_cmap_iterator *real_iter) {
  if (real_iter->node_stack_len == 0) {
    cbstmap__iterator_destroy(&real_iter->user_iter);
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

static cmap_iterator *cbmap_iter_next(cmap_iterator *iter);

// Creates an iterator at the smallest key; ccol_begin() calls it for a cbmap.
// It allocates through the map's allocator and gives NULL with *err set when
// that fails. A NULL or empty map gives NULL with *err NULL, the same contract
// as chashmap_begin_iter(), so an unused map field can be iterated with no
// guard.
cmap_iterator *cbstmap_begin_iter(cbmap cbm, char **err) {
  if (err) {
    *err = NULL;
  }

  if (!cbm || !cbm->root) {
    return NULL;
  }

  cbmap_cmap_iterator *real_iter =
      ccol__mem_alloc(cbm->m_procs, sizeof(cbmap_cmap_iterator));
  if (!real_iter) {
    if (err) {
      *err = CCOL__ERR_STR("Failed to create the iterator buffer");
    }
    return NULL;
  }

  real_iter->node_stack_len = 0;
  real_iter->free_fn = cbm->m_procs ? cbm->m_procs->free : NULL;
  real_iter->user_iter.ccol__next_fn = cbmap_iter_next;
  real_iter->user_iter.ccol__free_fn = cbstmap__iterator_destroy;
  real_iter->user_iter.ccol__direct_ptr = false;
  push_all_lefts_into_iter_stack(real_iter, cbm->root);
  return cmap_real_iter_next(real_iter);
}

// Moves iter to the next key; it's the ccol__next_fn that ccol_iter_next()
// calls. Gives the iterator, or NULL at the end, when the iterator is freed.
// Asserts on a NULL iter.
static cmap_iterator *cbmap_iter_next(cmap_iterator *iter) {
  if (!iter) {
    ccol__assert(false);
  }

  cbmap_cmap_iterator *real_iter = cmapIter2CbmapIter(iter);
  return cmap_real_iter_next(real_iter);
}

// Frees an iterator; it's the ccol__free_fn, used at the end of a walk and by
// ccol_iter_destroy() or a scoped iterator that leaves early. It uses the free
// function saved in the iterator and never reads the map, so it's valid after
// the map is destroyed.
void cbstmap__iterator_destroy(cmap_iterator *iter) {
  if (iter) {
    cbmap_cmap_iterator *real_iter = cmapIter2CbmapIter(iter);
    ccol_free free_fn = real_iter->free_fn;
    if (free_fn) {
      free_fn(real_iter);
    } else {
      ccol__default_mem_free(real_iter);
    }
  }
}

// The comparison a descent makes. CBMAP_CMP_GENERIC goes through
// compare_keys(); every other kind is an integer or char key with no custom
// comparator, compared with < and ==, which gives the same order.
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

// Picks the comparison every descent of a new map makes, so the hot loops don't
// dispatch on the key type per node. An integer or char key without a custom
// comparator gets a typed kind by its real width; everything else gets
// CBMAP_CMP_GENERIC.
static uint8_t cbmap_cmp_kind(ccol_data_type key_type,
                              ccol_comparison_proc custom_proc) {
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

// Checks the creation inputs of cbstmap_create_full(), which means only the
// allocator: Every key type is legal, and one it doesn't know uses memcmp like
// ccol_other_types. Sets *err and gives false on failure.
static bool verify_cbmap_create_inputs(ccol_memmgmt_procs *mmgmt_procs,
                                       char **err) {
  if (!ccol__verify_memmgmt_procs(mmgmt_procs, err)) {
    return false;
  }

  return true;
}

// Creates an empty map for keys of key_type. It's the one constructor behind
// every create function and cbmap_construct form, so the inputs are checked
// here. mmgmt_procs is the allocator (NULL for malloc and free), which the map
// copies; custom_comparison_proc replaces the default order. Gives the handle,
// or NULL with *err set on failure. key_alignment, key_offset and cmp_kind are
// fixed here for the map's life.
cbmap cbstmap_create_full(ccol_data_type key_type,
                          ccol_memmgmt_procs *mmgmt_procs,
                          ccol_comparison_proc custom_comparison_proc,
                          char **err) {
  if (err) {
    *err = NULL;
  }

  if (!verify_cbmap_create_inputs(mmgmt_procs, err)) {
    return NULL;
  }

  cbmap cbm = ccol__mem_alloc(mmgmt_procs, sizeof(cbstmap));
  if (!cbm) {
    if (err) {
      *err = CCOL__ERR_STR("Failed to allocate cbstmap area");
    }
    return NULL;
  }

  if (!ccol__populate_mem_mgmt_procs(cbm, mmgmt_procs, err)) {
    ccol__mem_free(mmgmt_procs, cbm);
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

// Frees one node and, when an update moved it out, its value buffer. Clear,
// delete and the replacement logic use it. The key is part of the node's block.
// The caller unlinks the node first; the child pointers aren't touched.
static void destroy_bmap_node(cbmap cbm, bmap_node *node) {
  if (node) {
    if (node->val_is_external) {
      ccol__mem_free(cbm->m_procs, node->val_pair.ptr);
    }
    ccol__mem_free(cbm->m_procs, node);
  }
}

// Frees every node of the map, for reset and destroy. It walks in post-order
// over a fixed stack, so a parent's child pointers stay valid until both
// children are gone, and needs no memory. Sets root to NULL and elem_count to
// 0.
static void clear_nodes(cbmap cbm) {
  if (!cbm->root) {
    return;
  }

  bmap_node *stack[CBMAP_MAX_TREE_HEIGHT];
  size_t stack_len = 0;

  bmap_node *current = cbm->root;
  bmap_node *last_visited = NULL;

  while (stack_len > 0 || current) {
    if (current) {
      ccol__assert(stack_len < CBMAP_MAX_TREE_HEIGHT);
      stack[stack_len++] = current;
      current = current->left;
    } else {
      bmap_node *peek = stack[stack_len - 1];

      if (peek->right && peek->right != last_visited) {
        current = peek->right;
      } else {
        --stack_len;
        destroy_bmap_node(cbm, peek);
        last_visited = peek;
      }
    }
  }

  cbm->root = NULL;
  cbm->elem_count = 0;
}

// Frees every node, the allocator copy and the map; cbstmap_destroy() and the
// scoped cleanup call it. NULL is fine. The free function is read before the
// copy that holds it goes away.
void cbstmap__destroy(cbmap cbm) {
  if (cbm) {
    clear_nodes(cbm);

    if (cbm->m_procs) {
      ccol_free free_func = cbm->m_procs->free;
      free_func(cbm->m_procs);
      free_func(cbm);
    } else {
      ccol__default_mem_free(cbm);
    }
  }
}

// Gives the number of entries in cbm, for callers and cbmap_size(). Asserts on
// a NULL cbm.
size_t cbstmap_elem_count(cbmap cbm) {
  if (!cbm) {
    ccol__assert(false);
  }

  return cbm->elem_count;
}

// Removes every entry and keeps the empty map usable, for callers and
// cbmap_reset(). Gives ccol_success. Asserts on a NULL cbm.
ccol_retval cbstmap_reset(cbmap cbm) {
  if (!cbm) {
    ccol__assert(false);
  }

  clear_nodes(cbm);

  return ccol_success;
}

// Compares two signed integers of 1, 2, 4 or 8 bytes as signed values, so 0xFF
// is -1. compare_keys() uses it for the signed key types. A plain char key
// never comes here; ccol_char compares as the platform's own char.
static inline int cmp_signed_small(void *ptr1, void *ptr2, size_t size) {
  switch (size) {
    case 1: {
      return ccol__typed_cmp(ptr1, ptr2, int8_t);
    }
    case 2: {
      return ccol__typed_cmp(ptr1, ptr2, int16_t);
    }
    case 4: {
      return ccol__typed_cmp(ptr1, ptr2, int32_t);
    }
    case 8: {
      return ccol__typed_cmp(ptr1, ptr2, int64_t);
    }
    default: {
      ccol__assert(false);
      return 0;
    }
  }
}

// Compares two unsigned integers or pointers of 1, 2, 4 or 8 bytes, with memcmp
// for any other size. compare_keys() uses it only for unsigned and pointer key
// types, never for a struct that happens to have one of these sizes.
static inline int cmp_unsigned_small(void *ptr1, void *ptr2, size_t size) {
  switch (size) {
    case 1: {
      return ccol__typed_cmp(ptr1, ptr2, uint8_t);
    }
    case 2: {
      return ccol__typed_cmp(ptr1, ptr2, uint16_t);
    }
    case 4: {
      return ccol__typed_cmp(ptr1, ptr2, uint32_t);
    }
    case 8: {
      return ccol__typed_cmp(ptr1, ptr2, uint64_t);
    }
    default: {
      return memcmp(ptr1, ptr2, size);
    }
  }
}

// Three-way comparison of two T values with NaN above every other value and
// equal only to NaN. A BST needs a real total order: With the native < and > a
// NaN key compares equal to the first node it meets, and an insert would
// overwrite that node's value. The operands are read with memcpy because a
// caller's pair carries no alignment or effective type of T. The temporaries
// have module-specific names so they can't capture a caller's ptr expression.
#define cmp_float_val(T, ptr1, ptr2)                                         \
  ({                                                                         \
    T cbmap_cfv_lhs;                                                         \
    T cbmap_cfv_rhs;                                                         \
    memcpy(&cbmap_cfv_lhs, (ptr1), sizeof(T));                               \
    memcpy(&cbmap_cfv_rhs, (ptr2), sizeof(T));                               \
    bool cbmap_cfv_nan_lhs = isnan(cbmap_cfv_lhs);                           \
    bool cbmap_cfv_nan_rhs = isnan(cbmap_cfv_rhs);                           \
    (cbmap_cfv_nan_lhs || cbmap_cfv_nan_rhs)                                 \
        ? (cbmap_cfv_nan_lhs == cbmap_cfv_nan_rhs                            \
               ? 0                                                           \
               : (cbmap_cfv_nan_lhs ? 1 : -1))                               \
        : (cbmap_cfv_lhs > cbmap_cfv_rhs) - (cbmap_cfv_lhs < cbmap_cfv_rhs); \
  })

// Compares two floating keys by value for compare_keys(). It dispatches on
// key_type because long double can have the size of double. -0.0 equals 0.0 and
// NaN follows the rule of cmp_float_val.
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
      ccol__assert(false);
      return 0;
    }
  }
}

// Compares two keys of the map and gives < 0, 0 or > 0. Every descent that
// can't use a typed comparison comes here. It uses the custom comparator when
// there is one; for two sizes the common prefix and then the length (only
// variable-width keys get here); otherwise by key_type, as signed, unsigned,
// floating, string bytes or, for ccol_other_types and unknown types, memcmp. A
// stored empty key is {NULL, 0}, and memcmp must not get a NULL pointer even
// for 0 bytes, hence the zero-size checks.
static inline int compare_keys(cbmap cbm, const cmap_pair *key_pair1,
                               const cmap_pair *key_pair2) {
  if (cbm->custom_comparison_proc) {
    return cbm->custom_comparison_proc(key_pair1->ptr, key_pair2->ptr);
  }

  if (key_pair1->size != key_pair2->size) {
    size_t min_size = ccol__min(key_pair1->size, key_pair2->size);
    int cmp =
        min_size == 0 ? 0 : memcmp(key_pair1->ptr, key_pair2->ptr, min_size);
    if (cmp != 0) {
      return cmp;
    }

    return (key_pair1->size > key_pair2->size) -
           (key_pair1->size < key_pair2->size);
  }

  switch (cbm->key_type) {
    case ccol_string: {
      return memcmp(key_pair1->ptr, key_pair2->ptr, key_pair1->size);
    }

    case ccol_char: {
      return ccol__typed_cmp(key_pair1->ptr, key_pair2->ptr, char);
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
      if (key_pair1->size == 0) return 0;
      return memcmp(key_pair1->ptr, key_pair2->ptr, key_pair1->size);
    }
  }
}

// The body of cbmap_copy_bytes() and cbmap_move_bytes(), with fn being memcpy
// or memmove. It copies n bytes from src to dst with a constant length for 1,
// 2, 4, 8 and 16 bytes, which compile to one load and one store; other sizes
// call the C library.
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
// These copy n bytes of a key or value: cbmap_copy_bytes() for separate ranges,
// cbmap_move_bytes() for ranges that may overlap. Node creation and updates use
// them so the usual small sizes need no library call.
static inline void cbmap_copy_bytes(void *restrict dst,
                                    const void *restrict src, size_t n) {
  CBMAP_SMALL_COPY_BODY(memcpy)
}
static inline void cbmap_move_bytes(void *dst, const void *src, size_t n) {
  CBMAP_SMALL_COPY_BODY(memmove)
}
#undef CBMAP_SMALL_COPY_BODY

// The typed loop of cbmap_find_node(): Reads the wanted key once, reads each
// node's key at key_offset and compares with < and ==. The caller has checked
// that the key has its type's width, so the read stays inside its bytes.
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

// Gives the node holding key_pair, or NULL; the lookups and the accessor use
// it. It picks the comparison once per call from cmp_kind. The caller has
// checked the key size.
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

// The typed loop of cbmap_descend_with_path(), like CBMAP_FIND_TYPED but
// recording the path.
#define CBMAP_DESCEND_TYPED(T)                                             \
  do {                                                                     \
    T cbmap_want_;                                                         \
    memcpy(&cbmap_want_, key_pair->ptr, sizeof(T));                        \
    const size_t cbmap_off_ = cbm->key_offset;                             \
    while (current) {                                                      \
      T cbmap_have_;                                                       \
      memcpy(&cbmap_have_, (const char *)current + cbmap_off_, sizeof(T)); \
      if (cbmap_want_ == cbmap_have_) break;                               \
      ccol__assert(len < CBMAP_MAX_TREE_HEIGHT);                           \
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

// Walks from the root towards key_pair and records each node it passes in path,
// with the link that points at it; insert and delete need that path to
// rebalance upward without recursion. Gives the node holding the key or NULL,
// the path length in *path_len_out, and in *link_out that node's link or the
// empty link where the key belongs.
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
        ccol__assert(len < CBMAP_MAX_TREE_HEIGHT);
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

// Creates a detached node holding copies of key_pair and val_pair, for insert.
// It allocates one block for the node, the key at key_offset and the value at
// the next max_align_t boundary; each term of the size is checked against what
// remains, so the sum can't wrap. The key lives in the node, so it's never
// freed on its own and never moves; a removal relinks the replacement node
// instead of copying it. An empty key or value never goes to the allocator
// (malloc(0) may give NULL) and is stored as {NULL, 0}. Gives NULL when the
// allocation fails.
static bmap_node *create_new_node(cbmap cbm, const cmap_pair *key_pair,
                                  const cmap_pair *val_pair) {
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
      ccol__mem_alloc(cbm->m_procs, val_offset + val_pair->size);
  if (!new_node) {
    return NULL;
  }

  new_node->key_pair.ptr =
      (key_pair->size > 0) ? (char *)new_node + key_offset : NULL;
  new_node->val_pair.ptr =
      (val_pair->size > 0) ? (char *)new_node + val_offset : NULL;
  new_node->val_is_external = false;

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

// Gives the larger of x and y, for the height computation.
static size_t maximum(size_t x, size_t y) {
  if (x >= y) {
    return x;
  }
  return y;
}

// Replaces the value of a node already in the map, for an insert of an existing
// key. On success it sets *result to ccol_key_already_present; on a failed
// allocation it changes nothing and leaves *result alone. A value of the same
// size is copied in place with memmove, because val_pair can point at the
// stored value (from cbstmap_get_elem_ref()). Another size goes into a new
// buffer before the old one is freed, for the same reason, and never moves back
// into the node, whose room isn't recorded. Size 0 frees any buffer and stores
// {NULL, 0}, since realloc(p, 0) can't tell a free from a failure.
static void update_bmap_node_value(cbmap cbm, bmap_node *node,
                                   const cmap_pair *val_pair,
                                   ccol_retval *result) {
  if (node->val_pair.size != val_pair->size) {
    if (val_pair->size == 0) {
      if (node->val_is_external) {
        ccol__mem_free(cbm->m_procs, node->val_pair.ptr);
      }
      node->val_pair.ptr = NULL;
      node->val_pair.size = 0;
      node->val_is_external = false;
      *result = ccol_key_already_present;
      return;
    }

    void *new_ptr = ccol__mem_alloc(cbm->m_procs, val_pair->size);
    if (!new_ptr) {
      return;
    }
    memcpy(new_ptr, val_pair->ptr, val_pair->size);
    if (node->val_is_external) {
      ccol__mem_free(cbm->m_procs, node->val_pair.ptr);
    }
    node->val_pair.ptr = new_ptr;
    node->val_pair.size = val_pair->size;
    node->val_is_external = true;
    *result = ccol_key_already_present;
    return;
  }
  if (node->val_pair.ptr != val_pair->ptr && val_pair->size > 0) {
    cbmap_move_bytes(node->val_pair.ptr, val_pair->ptr, val_pair->size);
  }
  *result = ccol_key_already_present;
}

// Gives the height of node, 0 for NULL (a leaf is 1), so the balance code needs
// no NULL checks.
static size_t node_height(bmap_node *node) {
  if (!node) {
    return 0;
  }

  return node->height;
}

// Gives right height minus left height, which the rebalance and the validator
// test against [-1, 1].
static int node_balance(bmap_node *node) {
  if (!node) {
    return 0;
  }

  return (int)node_height(node->right) - (int)node_height(node->left);
}

// Sets node->height from its children, after every structural change. The cast
// to one byte can't truncate; the _Static_assert on CBMAP_MAX_TREE_HEIGHT
// guarantees it.
static void recalculate_node_height(bmap_node *node) {
  if (!node) {
    return;
  }

  node->height =
      (uint8_t)(maximum(node_height(node->left), node_height(node->right)) + 1);
}

// Updates the height of parent and restores the AVL rule there with a left,
// right, right-left or left-right rotation when needed. Insert, delete and the
// extreme detach call it on every ancestor. Gives the subtree's new root;
// heights are recomputed bottom-up after each rotation.
static bmap_node *check_node_balance(bmap_node *parent) {
  recalculate_node_height(parent);
  int balance = node_balance(parent);

  if (balance > 1 || balance < -1) {
    bmap_node *p = parent;

    if (balance > 1) {
      if (node_balance(parent->right) >= 0 || !(parent->right->left)) {
        parent = parent->right;
        p->right = parent->left;
        parent->left = p;
        recalculate_node_height(p);
        recalculate_node_height(parent);
      } else {
        bmap_node *rl_left = parent->right->left->left;
        bmap_node *rl_right = parent->right->left->right;
        parent = parent->right->left;
        parent->left = p;
        parent->right = p->right;
        parent->right->left = rl_right;
        parent->left->right = rl_left;
        recalculate_node_height(parent->left);
        recalculate_node_height(parent->right);
        recalculate_node_height(parent);
      }
    } else {
      if (node_balance(parent->left) <= 0 || !(parent->left->right)) {
        parent = parent->left;
        p->left = parent->right;
        parent->right = p;
        recalculate_node_height(p);
        recalculate_node_height(parent);
      } else {
        bmap_node *lr_left = parent->left->right->left;
        bmap_node *lr_right = parent->left->right->right;
        parent = parent->left->right;
        parent->right = p;
        parent->left = p->left;
        parent->left->right = lr_left;
        parent->right->left = lr_right;
        recalculate_node_height(parent->left);
        recalculate_node_height(parent->right);
        recalculate_node_height(parent);
      }
    }
  }

  int final_balance = node_balance(parent);
  if (final_balance > 1 || final_balance < -1) {
    ccol__assert(false);
  }
  return parent;
}

// Detaches the smallest (max false) or largest (max true) node of the subtree
// at root, for the replacement of a deleted node with two children. The node
// goes to *extreme, its only child takes its place, the path is rebalanced
// bottom-up, and it gives the new subtree root. The path is a fixed array, so
// the delete can't fail for lack of memory.
static bmap_node *cbmap_detach_extreme_iter(bmap_node *root, bool max,
                                            bmap_node **extreme) {
  if (!root) {
    ccol__assert(false);
  }

  node_stack_entry path[CBMAP_MAX_TREE_HEIGHT];
  size_t path_len = 0;

  bmap_node *current = root;
  bmap_node **parent_link = NULL;

  while (true) {
    if (max) {
      if (current->right) {
        ccol__assert(path_len < CBMAP_MAX_TREE_HEIGHT);
        path[path_len++] = (node_stack_entry){current, parent_link};
        parent_link = &(current->right);
        current = current->right;
      } else {
        *extreme = current;
        break;
      }
    } else {
      if (current->left) {
        ccol__assert(path_len < CBMAP_MAX_TREE_HEIGHT);
        path[path_len++] = (node_stack_entry){current, parent_link};
        parent_link = &(current->left);
        current = current->left;
      } else {
        *extreme = current;
        break;
      }
    }
  }

  bmap_node *replacement = max ? current->left : current->right;

  if (path_len == 0) {
    return replacement;
  }

  node_stack_entry last_entry = path[path_len - 1];
  if (max) {
    last_entry.node->right = replacement;
  } else {
    last_entry.node->left = replacement;
  }

  for (size_t i = path_len; i-- > 0;) {
    node_stack_entry entry = path[i];
    bmap_node *balanced = check_node_balance(entry.node);

    if (entry.parent_link) {
      *(entry.parent_link) = balanced;
    } else {
      root = balanced;
    }
  }

  return root;
}

// Says whether key_pair has a size the map's key type allows; every raw entry
// point checks it. A fixed-width key must have exactly its type's width, or the
// prefix and length order of compare_keys() would make it a different key that
// no correct lookup finds, and a custom comparator would read past it. A string
// key needs at least 1 byte (its NUL); a ccol_other_types key may have any
// size, 0 included.
static inline bool key_size_is_valid_for_type(cbmap cbm,
                                              const cmap_pair *key_pair) {
  size_t fixed_width = ccol_fixed_width_data_type_size(cbm->key_type);
  if (fixed_width != 0) {
    return key_pair->size == fixed_width;
  }
  return key_pair->size != 0 || cbm->key_type != ccol_string;
}

// Says whether the bytes of a caller's pair can be read; every raw entry point
// checks it so a bad pair is refused instead of dereferenced. A NULL ptr with a
// non-zero size fails; size 0 passes, with or without a ptr.
static inline bool pair_bytes_are_readable(const cmap_pair *pair) {
  return pair->ptr != NULL || pair->size == 0;
}

// Inserts a copy of the key and the value, or replaces the value of a key that
// is already there (ccol_key_already_present). cbmap_insert() and raw callers
// use it. It descends with a fixed path array, links a new node and rebalances
// upward, updating root and elem_count. The capacity check runs only for a new
// key, so an update in a full map works. The rebalance stops at the first
// ancestor whose height didn't change, or after the one rotation an insert can
// need, since nothing above changes.
ccol_retval cbstmap_insert_elem(cbmap cbm, const cmap_pair *key_pair,
                                const cmap_pair *val_pair) {
  if (!cbm) {
    ccol__assert(false);
  }

  if (!key_pair || !val_pair || !pair_bytes_are_readable(key_pair) ||
      !pair_bytes_are_readable(val_pair)) {
    return ccol_invalid_args;
  }

  if (!key_size_is_valid_for_type(cbm, key_pair)) {
    return ccol_invalid_args;
  }

  if (!cbm->root) {
    cbm->root = create_new_node(cbm, key_pair, val_pair);
    if (!cbm->root) {
      return ccol_not_enough_memory;
    }
    cbm->elem_count++;
    return ccol_success;
  }

  node_stack_entry path[CBMAP_MAX_TREE_HEIGHT];
  size_t path_len = 0;
  bmap_node **parent_link = NULL;
  ccol_retval result = ccol_not_enough_memory;

  bmap_node *existing =
      cbmap_descend_with_path(cbm, key_pair, path, &path_len, &parent_link);
  if (existing) {
    update_bmap_node_value(cbm, existing, val_pair, &result);
    return result;
  }

  if (cbm->elem_count == ccol_max_elem_count) {
    return ccol_container_full;
  }

  bmap_node *new_node = create_new_node(cbm, key_pair, val_pair);
  if (!new_node) {
    return ccol_not_enough_memory;
  }

  *parent_link = new_node;
  result = ccol_success;

  for (size_t i = path_len; i-- > 0;) {
    node_stack_entry entry = path[i];
    size_t height_before = entry.node->height;
    bmap_node *balanced = check_node_balance(entry.node);

    if (entry.parent_link) {
      *(entry.parent_link) = balanced;
    } else {
      cbm->root = balanced;
    }

    if (balanced != entry.node || balanced->height == height_before) {
      break;
    }
  }

  cbm->elem_count++;
  return result;
}

// Looks up key_pair and copies its value into target_buf, for callers that want
// their own copy. The sizes must match exactly, so nothing is truncated. Gives
// ccol_success, ccol_key_not_found or ccol_invalid_args.
ccol_retval cbstmap_get_elem_copy(cbmap cbm, const cmap_pair *key_pair,
                                  void *target_buf, size_t target_buf_size) {
  if (!cbm) {
    ccol__assert(false);
  }

  if (!key_pair || !pair_bytes_are_readable(key_pair)) {
    return ccol_invalid_args;
  }

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
  if (target_buf_size > 0) {
    cbmap_copy_bytes(target_buf, tracker->val_pair.ptr, target_buf_size);
  }
  return ccol_success;
}

// Checks key_pair and finds its node; cbstmap_get_elem_ref() and
// cbstmap__get_val_accessor() share it. Gives the node, or NULL with the
// failure code in *ret.
static bmap_node *cbmap_lookup_checked(cbmap cbm, const cmap_pair *key_pair,
                                       ccol_retval *ret) {
  if (!key_pair || !pair_bytes_are_readable(key_pair) ||
      !key_size_is_valid_for_type(cbm, key_pair)) {
    *ret = ccol_invalid_args;
    return NULL;
  }
  bmap_node *tracker = cbmap_find_node(cbm, key_pair);
  *ret = tracker ? ccol_success : ccol_key_not_found;
  return tracker;
}

// Looks up key_pair and fills val_pair with the address and size of its value,
// so callers can read or edit it in place; cbmap_get() and cbmap_get_ptr() use
// it. On failure val_pair gets {NULL, 0}.
ccol_retval cbstmap_get_elem_ref(cbmap cbm, const cmap_pair *key_pair,
                                 cmap_pair *val_pair) {
  if (!cbm) {
    ccol__assert(false);
  }

  if (!val_pair) {
    return ccol_invalid_args;
  }

  ccol_retval ret;
  bmap_node *tracker = cbmap_lookup_checked(cbm, key_pair, &ret);
  if (!tracker) {
    val_pair->ptr = NULL;
    val_pair->size = 0;
    return ret;
  }
  *val_pair = tracker->val_pair;
  return ccol_success;
}

// Looks up key_pair and hands out the address of its value accessor, for the
// string arm of cbmap_get_ptr(): The accessor's ptr field is the only char *
// object of a string value. *val_accessor is NULL on failure.
ccol_retval cbstmap__get_val_accessor(cbmap cbm, const cmap_pair *key_pair,
                                      const cmap_pair **val_accessor) {
  if (!cbm) {
    ccol__assert(false);
  }
  if (!val_accessor) {
    return ccol_invalid_args;
  }

  ccol_retval ret;
  bmap_node *tracker = cbmap_lookup_checked(cbm, key_pair, &ret);
  *val_accessor = tracker ? &tracker->val_pair : NULL;
  return ret;
}

// Frees parent and gives the root of the subtree that replaces it, for delete.
// With two children the taller side gives up its extreme node (the right
// minimum on a tie), which keeps the order and needs the fewest rotations.
static bmap_node *perform_element_removal(cbmap cbm, bmap_node *parent) {
  bmap_node *left = parent->left;
  bmap_node *right = parent->right;
  destroy_bmap_node(cbm, parent);

  if (!left) {
    parent = right;
  } else if (!right) {
    parent = left;
  } else {
    if (right->height >= left->height) {
      bmap_node *right_min = NULL;
      right = cbmap_detach_extreme_iter(right, false, &right_min);
      parent = right_min;
      parent->right = right;
      parent->left = left;
      recalculate_node_height(parent);
    } else {
      bmap_node *left_max = NULL;
      left = cbmap_detach_extreme_iter(left, true, &left_max);
      parent = left_max;
      parent->right = right;
      parent->left = left;
      recalculate_node_height(parent);
    }
  }

  return parent;
}

// Deletes key_pair and its value; cbmap_remove() and raw callers use it. Gives
// ccol_success, ccol_key_not_found or ccol_invalid_args. It descends with a
// fixed path array, removes the node and rebalances upward, updating root and
// elem_count. A rotation after a delete can lower a subtree, so the walk stops
// on an unchanged height, not on whether a rotation happened.
ccol_retval cbstmap_delete_elem(cbmap cbm, const cmap_pair *key_pair) {
  if (!cbm) {
    ccol__assert(false);
  }

  if (!key_pair || !pair_bytes_are_readable(key_pair)) {
    return ccol_invalid_args;
  }

  if (!key_size_is_valid_for_type(cbm, key_pair)) {
    return ccol_invalid_args;
  }

  if (!cbm->root) {
    return ccol_key_not_found;
  }

  node_stack_entry path[CBMAP_MAX_TREE_HEIGHT];
  size_t path_len = 0;
  bmap_node **parent_link = NULL;
  ccol_retval result = ccol_key_not_found;

  bmap_node *current =
      cbmap_descend_with_path(cbm, key_pair, path, &path_len, &parent_link);
  if (current) {
    bmap_node *replacement = perform_element_removal(cbm, current);
    *parent_link = replacement;
    result = ccol_success;
  }

  if (result == ccol_success) {
    for (size_t i = path_len; i-- > 0;) {
      node_stack_entry entry = path[i];
      size_t height_before = entry.node->height;
      bmap_node *balanced = check_node_balance(entry.node);

      if (entry.parent_link) {
        *(entry.parent_link) = balanced;
      } else {
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
// Checks the height and the balance of every node, for the tests, which can't
// see the opaque nodes. It walks iteratively over a fixed stack. Only a pass or
// fail leaves this file, so no node pointer crosses the API.
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
      if (sp >= CBMAP_MAX_TREE_HEIGHT) return false;
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
