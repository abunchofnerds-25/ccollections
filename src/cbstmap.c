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

/* Upper bound on the height of any AVL tree this library could ever hold: an
 * AVL tree's height is bounded by roughly 1.44 * log2(n + 2), so this covers
 * every element count representable by a 64-bit size_t with a wide safety
 * margin. Every internal traversal stack in this file (ancestor-path
 * tracking for insert/delete/rebalancing, the post-order destroy stack, and
 * the in-order iterator's stack) holds at most one entry per tree level, so a
 * fixed array of this size can never overflow for a real AVL tree, letting
 * these hot paths avoid a heap allocation entirely instead of risking an
 * allocation failure on every mutation. */
#define CBMAP_MAX_TREE_HEIGHT 128

typedef struct bmap_node {
  // Data related containers
  cmap_pair key_pair;
  cmap_pair val_pair;
  // Relational pointers
  struct bmap_node *left;
  struct bmap_node *right;
  // Metadata for self-balancing
  size_t height;
} bmap_node;

typedef struct cbinarymap {
  size_t elem_count;
  bmap_node *root;
  ccol_memmgmt_procs_t *m_procs;
  ccol_data_type key_type;
  ccol_comparison_proc_t custom_comparison_proc;
} cbinarymap;

typedef struct cbmap_cmap_iterator {  // Extended cmap_iterator for cbmap
  cbmap parent_map;
  bmap_node *node_stack[CBMAP_MAX_TREE_HEIGHT];
  size_t node_stack_len;
  cmap_iterator user_iter;
} cbmap_cmap_iterator;

#define cmapIter2CbmapIter(u_iter)            \
  (cbmap_cmap_iterator *)((uint8_t *)u_iter - \
                          offsetof(cbmap_cmap_iterator, user_iter))

// Helper structure for tracking parent-child relationships during tree
// operations
typedef struct node_stack_entry {
  bmap_node *node;
  bmap_node **parent_link;  // Pointer to the parent's left or right pointer
} node_stack_entry;

/* Pushes node and all of its left descendants onto the iterator's node stack.
 * This implements the "visit left subtree first" invariant of the iterative
 * in-order traversal: the next pop will yield the smallest unvisited key in
 * this subtree. The stack is a fixed CBMAP_MAX_TREE_HEIGHT-entry array
 * embedded directly in the iterator, since at most one entry per tree level
 * is ever pending at a time (see CBMAP_MAX_TREE_HEIGHT's own comment). */
static void push_all_lefts_into_iter_stack(cbmap_cmap_iterator *real_iter,
                                           bmap_node *node) {
  while (node) {
    ccol_assert(real_iter->node_stack_len < CBMAP_MAX_TREE_HEIGHT);
    real_iter->node_stack[real_iter->node_stack_len++] = node;
    node = node->left;
  }
}

/* Pops one node from the iterator stack (the current in-order node), pushes
 * all left descendants of its right child, then updates the user-facing
 * key/val pair pointers. When the stack is empty the iterator is destroyed
 * and NULL is returned to signal end of traversal. */
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

/* Creates and returns an in-order iterator positioned at the first (smallest)
 * key. The iterator uses an explicit fixed-size stack (an array of
 * bmap_node* embedded directly in the iterator) to implement the traversal
 * iteratively. Returns NULL when the map is empty. */
static cmap_iterator *cbmap_iter_next(cmap_iterator *iter);

cmap_iterator *cbmap_begin_iter(cbmap cbm, char **err) {
  if (err) {
    *err = NULL;
  }

  // A NULL cbm is treated the same as an empty map (see this function's own
  // doc comment in cbstmap.h): consistent with chashmap_begin_iter's
  // identical, deliberate NULL-tolerance, so a lazily-created map field left
  // uninitialized because nothing has been inserted into it yet can be
  // iterated directly without every caller needing its own NULL guard first.
  if (!cbm || !cbm->root) {
    return NULL;
  }

  cbmap_cmap_iterator *real_iter =
      _mem_alloc(cbm->m_procs, sizeof(cbmap_cmap_iterator));
  if (!real_iter) {
    if (err) {
      *err = CCOL_ERR_STR("Failed to create the iterator buffer");
    }
    return NULL;
  }

  real_iter->node_stack_len = 0;
  real_iter->parent_map = cbm;
  real_iter->user_iter._next_fn = cbmap_iter_next;
  real_iter->user_iter._free_fn = __cbmap_iterator_destroy;
  real_iter->user_iter._direct_ptr = false;
  push_all_lefts_into_iter_stack(real_iter, cbm->root);
  return cmap_real_iter_next(real_iter);
}

/* Advances the iterator to the next in-order node and returns it. Returns NULL
 * (and destroys the iterator) when iteration is complete. */
static cmap_iterator *cbmap_iter_next(cmap_iterator *iter) {
  // Advance to the next node
  if (!iter) {
    ccol_assert(false);
  }

  cbmap_cmap_iterator *real_iter = cmapIter2CbmapIter(iter);
  return cmap_real_iter_next(real_iter);
}

/* Releases the iterator's node stack and the iterator struct itself. Called
 * automatically by cmap_real_iter_next when the end of traversal is reached,
 * but can also be called early to abort mid-iteration. */
void __cbmap_iterator_destroy(cmap_iterator *iter) {
  if (iter) {
    cbmap_cmap_iterator *real_iter = cmapIter2CbmapIter(iter);
    _mem_free(real_iter->parent_map->m_procs, real_iter);
  }
}

/* Validates the creation inputs for the BST map. Currently only validates the
 * memory management procedures; the key type needs no validation since all
 * ccol_data_type values are legal (an unrecognized value simply falls back to
 * memcmp comparison, the same as ccol_other_types). */
static bool verify_cbmap_create_inputs(ccol_memmgmt_procs_t *mmgmt_procs,
                                       char **err) {
  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return false;
  }

  return true;
}

/* Creates an empty AVL-balanced BST map. key_type selects the default
 * comparison strategy (see compare_keys()); custom_comparison_proc overrides
 * all built-in key comparison when non-NULL. */
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

  cbmap cbm = _mem_alloc(mmgmt_procs, sizeof(cbinarymap));
  if (!cbm) {
    if (err) {
      *err = CCOL_ERR_STR("Failed to allocate cbinarymap area");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(cbm, mmgmt_procs, err)) {
    _mem_free(mmgmt_procs, cbm);
    return NULL;
  }

  cbm->elem_count = 0;
  cbm->root = NULL;
  cbm->key_type = key_type;
  cbm->custom_comparison_proc = custom_comparison_proc;

  return cbm;
}

/* Frees the key buffer, value buffer, and the node struct itself. Does not
 * touch left/right pointers; callers must have already unlinked the node. */
static void destroy_bmap_node(cbmap cbm, bmap_node *node) {
  if (node) {
    _mem_free(cbm->m_procs, node->key_pair.ptr);
    _mem_free(cbm->m_procs, node->val_pair.ptr);
    _mem_free(cbm->m_procs, node);
  }
}

/* Destroys all nodes via an iterative post-order traversal using a fixed
 * CBMAP_MAX_TREE_HEIGHT-entry stack (see that constant's own comment for why
 * this bound is always sufficient). Post-order ensures both children are
 * freed before the parent so the parent's left/right pointers remain valid
 * during traversal. Resets root and elem_count to zero on completion. */
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
      // Peek at the top of stack
      bmap_node *peek = stack[stack_len - 1];

      // If right child exists and not yet processed
      if (peek->right && peek->right != last_visited) {
        current = peek->right;
      } else {
        // Process this node (both children done)
        --stack_len;
        destroy_bmap_node(cbm, peek);
        last_visited = peek;
      }
    }
  }

  cbm->root = NULL;
  cbm->elem_count = 0;
}

/* Destroys all nodes then frees the map struct and its custom allocator. */
void __cbmap_destroy(cbmap cbm) {
  if (cbm) {
    _clear_nodes(cbm);

    if (cbm->m_procs) {
      ccol_free_t free_func = cbm->m_procs->free;
      free_func(cbm->m_procs);
      free_func(cbm);
    } else {
      mem_free(cbm);
    }
  }
}

/* Returns the number of key-value pairs stored in the map. */
size_t cbmap_elem_count(cbmap cbm) {
  if (!cbm) {
    ccol_assert(false);
  }

  return cbm->elem_count;
}

/* Removes all entries from the map, leaving it empty but otherwise reusable. */
ccol_retval_t cbmap_reset(cbmap cbm) {
  if (!cbm) {
    ccol_assert(false);
  }

  _clear_nodes(cbm);

  return ccol_success;
}

/* Compares two signed integers of size 1/2/4/8 bytes (signed
 * char/short/int/long/long_long) pointed to by ptr1 and ptr2. Using typed
 * dereferences rather than memcmp avoids sign-extension issues (e.g. 0xFF in
 * a signed byte is -1, not 255). Deliberately never invoked for ccol_char
 * (plain `char` keys): whether plain `char` is signed or unsigned is
 * platform-defined, and compare_keys() gives ccol_char its own dedicated case
 * using the native `char` type instead of forcing a signed int8_t
 * reinterpretation here, so a char key sorts exactly the way this platform's
 * own `<`/`>` on `char` would. A genuinely-declared `signed char` (or its
 * typedef, `int8_t`) key is a distinct type (ccol_signed_char) and gets a
 * real, platform-independent signed comparison here, same as every other
 * signed integer width. */
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
      // For non-standard sizes, just complain, as this should not
      // have been classified as a 'signed' number
      ccol_assert(false);
      return 0;  // Unreachable: ccol_assert(false) always aborts. Present
                 // only so this non-void function has a defined return on
                 // every path, satisfying -Wreturn-type.
    }
  }
}

/* Compares two unsigned integers (or raw pointer values) of size 1/2/4/8
 * bytes, falling back to memcmp for non-standard sizes. Only ever invoked for
 * a key_type genuinely known to be an unsigned integer or a pointer; never
 * for an opaque struct that merely happens to share one of these sizes (see
 * ccol_other_types in compare_keys(), which always uses memcmp instead). */
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

/* Three-way comparison for a floating-point type T, with an explicit NaN
 * rule: a BST requires a genuine strict total order to stay internally
 * consistent (every lookup/insert/delete descent must agree on which side of
 * any given node a key belongs on), and IEEE 754's native `<`/`>` cannot
 * provide that for NaN (both are false for any comparison involving a NaN
 * operand, silently collapsing to "equal" against every other key, including
 * completely unrelated ones). Ordering NaN as greater than every non-NaN
 * value, and equal only to another NaN, restores a real total order: NaN
 * keys sort together at the high end of the tree instead of each one
 * comparing "equal" to whatever node the search happens to visit first
 * (which, left unfixed, silently corrupted that unrelated node's value
 * instead of ever inserting the NaN key at all). isnan() is a type-generic
 * (C99 <math.h>) macro, so this one helper serves float/double/long double
 * without needing a per-type variant. */
#define cmp_float_val(T, ptr1, ptr2)                           \
  ({                                                           \
    T _v1 = *(T *)(ptr1);                                      \
    T _v2 = *(T *)(ptr2);                                      \
    bool _nan1 = isnan(_v1);                                   \
    bool _nan2 = isnan(_v2);                                   \
    (_nan1 || _nan2) ? (_nan1 == _nan2 ? 0 : (_nan1 ? 1 : -1)) \
                     : (_v1 > _v2) - (_v1 < _v2);              \
  })

/* Compares two floating-point values pointed to by ptr1 and ptr2, dispatched
 * by the map's own declared key_type rather than by size, since long
 * double's size varies by platform (and can coincide with double's size on
 * some ABIs) in a way a size-based switch cannot disambiguate. Native
 * floating-point comparison orders negative and positive values correctly
 * and treats -0.0 and 0.0 as equal, unlike a raw bit-pattern
 * reinterpretation. See cmp_float_val's own comment for the NaN rule. */
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
      // Only ever called for one of the three floating-point key types.
      ccol_assert(false);
      return 0;  // Unreachable: ccol_assert(false) always aborts. Present
                 // only so this non-void function has a defined return on
                 // every path, satisfying -Wreturn-type.
    }
  }
}

/* Central key comparison dispatch. Priority: custom_comparison_proc > a
 * differing-size fallback (expected only for variable-length keys such as
 * strings) > a dispatch on the map's declared key_type. Keys of differing
 * sizes are compared on their common prefix then by length (shorter <
 * longer), which gives consistent BST ordering for variable-length keys.
 *
 * The key_type dispatch deliberately distinguishes genuine signed integers,
 * genuine unsigned integers/pointers, genuine floating-point types, strings,
 * and everything else (ccol_other_types, e.g. a struct or enum key): only the
 * first three get a typed numeric reinterpretation, since that is only valid
 * for a type that is actually a number of that kind. Anything not
 * specifically recognized (including ccol_other_types) always falls back to
 * a raw memcmp of its representation, matching this module's own documented
 * behavior for struct keys regardless of the struct's size. */
static inline int compare_keys(cbmap cbm, const cmap_pair *key_pair1,
                               const cmap_pair *key_pair2) {
  if (cbm->custom_comparison_proc) {
    return cbm->custom_comparison_proc(key_pair1->ptr, key_pair2->ptr);
  }

  if (key_pair1->size != key_pair2->size) {
    // Different sizes - compare common prefix, then by size. min_size == 0
    // (one side is a zero-size key, stored as ptr == NULL, size == 0 by
    // create_new_node) is skipped without calling memcmp at all: comparing
    // zero bytes is unconditionally "equal" regardless of the pointers
    // involved, but memcmp's own pointer parameters are declared nonnull
    // (glibc's <string.h>, enforced by UBSan) even for a zero length, so
    // calling it with a genuinely NULL pointer (as a zero-size key's own
    // stored representation always is) is undefined behavior in its own
    // right, independent of whether any byte is ever actually read.
    size_t min_size = ccol_min(key_pair1->size, key_pair2->size);
    int cmp =
        min_size == 0 ? 0 : memcmp(key_pair1->ptr, key_pair2->ptr, min_size);
    if (cmp != 0) {
      return cmp;
    }

    // Common prefix is equal, shorter string comes first
    return (key_pair1->size > key_pair2->size) -
           (key_pair1->size < key_pair2->size);
  }

  switch (cbm->key_type) {
    case ccol_string: {
      // A string key's own size always includes at least a null terminator
      // (see _populate_cmap_pair), so key_pair1->size is never genuinely 0
      // via any macro-driven call site; the guard is defensive, matching
      // the identical one below for ccol_other_types, for a caller using
      // the raw cbmap_insert_elem/cbmap_get_elem_ref layer directly with a
      // hand-built, zero-size cmap_pair for a ccol_string-typed map.
      if (key_pair1->size == 0) return 0;
      return memcmp(key_pair1->ptr, key_pair2->ptr, key_pair1->size);
    }

    case ccol_char: {
      // Plain `char` is a distinct type from both `signed char` and
      // `unsigned char`, and the C standard leaves its signedness
      // platform-defined (e.g. signed on x86/x86_64, unsigned on the
      // standard aarch64 AAPCS64 ABI). Comparing via the native `char` type
      // itself (rather than forcing a signed int8_t reinterpretation)
      // guarantees this default comparator always agrees with what this
      // platform's own `<`/`>` on `char` would produce, matching how
      // ccol_unsigned_char is already given its own, unforced comparison
      // below.
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
      // Arbitrary POD types (structs, unions, enums, bool, wchar_t, ...) have
      // no numeric interpretation; order them by raw byte representation.
      // key_pair1->size == 0 (a genuine, deliberately-zero-size key; see
      // create_new_node's own NULL/size-0 representation for one) is
      // checked before calling memcmp, for the same reason as the
      // differing-sizes branch above: memcmp's pointer parameters are
      // declared nonnull even for a zero length, and a zero-size key's own
      // stored ptr genuinely is NULL, so calling memcmp here unconditionally
      // is undefined behavior regardless of no byte ever actually being
      // read. Two zero-size keys of this type are always equal.
      if (key_pair1->size == 0) return 0;
      return memcmp(key_pair1->ptr, key_pair2->ptr, key_pair1->size);
    }
  }
}

/* Allocates a new BST node and copies the key and value data into separately
 * allocated buffers. On any allocation failure, previously allocated buffers
 * are freed before returning NULL.
 *
 * A zero-size key or value is never passed to the allocator: malloc(0) is
 * permitted by the C standard to return either NULL or a unique pointer, so
 * treating a NULL result as "allocation failed" would misreport a genuine
 * zero-byte key/value as ccol_not_enough_memory under an allocator that
 * legitimately chooses NULL for a zero-size request. A zero-size pair is
 * instead stored as ptr == NULL, size == 0, which every reader in this file
 * (compare_keys' memcmp, mem_cpy, destroy_bmap_node's _mem_free) already
 * handles safely for a zero length. */
static bmap_node *create_new_node(cbmap cbm, const cmap_pair *key_pair,
                                  const cmap_pair *val_pair) {
  bmap_node *new_node = _mem_alloc(cbm->m_procs, sizeof(bmap_node));
  if (!new_node) {
    return NULL;
  }

  if (key_pair->size > 0) {
    new_node->key_pair.ptr = _mem_alloc(cbm->m_procs, key_pair->size);
    if (!new_node->key_pair.ptr) {
      _mem_free(cbm->m_procs, new_node);
      return NULL;
    }
  } else {
    new_node->key_pair.ptr = NULL;
  }

  if (val_pair->size > 0) {
    new_node->val_pair.ptr = _mem_alloc(cbm->m_procs, val_pair->size);
    if (!new_node->val_pair.ptr) {
      _mem_free(cbm->m_procs, new_node->key_pair.ptr);
      _mem_free(cbm->m_procs, new_node);
      return NULL;
    }
  } else {
    new_node->val_pair.ptr = NULL;
  }

  mem_cpy(new_node->key_pair.ptr, key_pair->ptr, key_pair->size);
  new_node->key_pair.size = key_pair->size;
  mem_cpy(new_node->val_pair.ptr, val_pair->ptr, val_pair->size);
  new_node->val_pair.size = val_pair->size;

  new_node->left = NULL;
  new_node->right = NULL;
  new_node->height = 1;

  return new_node;
}

/* Returns the larger of x and y. Implemented locally to avoid pulling in
 * <stdlib.h> just for a two-argument max. */
static size_t maximum(size_t x, size_t y) {
  if (x >= y) {
    return x;
  }
  return y;
}

/* Overwrites the value of an existing node. If the new value has a different
 * size a reallocation is attempted; on failure the old value and size are
 * preserved and *result is left unchanged so the caller sees
 * ccol_not_enough_memory. On success, both the value bytes and the size field
 * are updated and *result is set to ccol_key_already_present.
 *
 * A new size of 0 is handled without ever calling the allocator's realloc:
 * realloc(ptr, 0) has implementation-defined behavior per the C standard,
 * and on glibc it frees ptr and returns NULL, which is indistinguishable
 * from "reallocation failed, old buffer still valid" via the return value
 * alone. Treating that NULL as failure (the naive realloc-and-check-NULL
 * approach) would leave node->val_pair.ptr dangling while reporting the old,
 * already-freed value as still present; a use-after-free on the next read
 * and a double free at node/map destruction. Shrinking to zero is instead
 * always a genuine, unconditional success: free the old buffer directly and
 * store the same NULL/size-0 representation create_new_node uses for a
 * zero-size value. */
static void update_bmap_node_value(cbmap cbm, bmap_node *node,
                                   const cmap_pair *val_pair,
                                   ccol_retval_t *result) {
  if (node->val_pair.size != val_pair->size) {
    if (val_pair->size == 0) {
      _mem_free(cbm->m_procs, node->val_pair.ptr);
      node->val_pair.ptr = NULL;
      node->val_pair.size = 0;
      *result = ccol_key_already_present;
      return;
    }

    // Different, non-zero value size, reallocation needed. node->val_pair.ptr
    // may itself be NULL here (the node's current value has size 0), which
    // is fine: realloc(NULL, n) is defined to behave like malloc(n).
    void *new_ptr =
        _mem_realloc(cbm->m_procs, node->val_pair.ptr, val_pair->size);
    if (!new_ptr) {
      // reallocation attempt failed!
      return;
    }
    node->val_pair.ptr = new_ptr;
    node->val_pair.size = val_pair->size;
  }
  mem_cpy(node->val_pair.ptr, val_pair->ptr, val_pair->size);
  *result = ccol_key_already_present;
}

/* Returns the height of a node, or 0 for NULL. Leaf nodes start at height 1,
 * so the NULL sentinel is 0 and all arithmetic stays in size_t. */
static size_t node_height(bmap_node *node) {
  if (!node) {
    return 0;
  }

  return node->height;
}

/* Returns the balance factor of a node as right_height - left_height.
 * A value outside [-1, 1] means the node violates the AVL invariant. */
static int node_balance(bmap_node *node) {
  if (!node) {
    return 0;
  }

  return (int)node_height(node->right) - (int)node_height(node->left);
}

/* Recomputes node->height from the heights of its children. Must be called
 * after any rotation or structural change to keep the height metadata accurate
 * for balance factor calculations up the ancestor chain. */
static void recalculate_node_height(bmap_node *node) {
  if (!node) {
    return;
  }

  node->height = maximum(node_height(node->left), node_height(node->right)) + 1;
}

/* Restores the AVL invariant at parent if needed, performing one of four
 * rotations: simple left, simple right, right-left double, or left-right
 * double. Returns the new subtree root after the rotation (which may be a
 * different node). Heights are recalculated bottom-up after each rotation. */
static bmap_node *check_node_balance(bmap_node *parent) {
  recalculate_node_height(parent);
  int balance = node_balance(parent);

  if (balance > 1 || balance < -1) {
    // The symmetry is lost! A rotation is needed
    bmap_node *p = parent;

    if (balance > 1) {
      // Right side is deeper
      if (node_balance(parent->right) >= 0 || !(parent->right->left)) {
        // Simple left rotation
        parent = parent->right;
        p->right = parent->left;
        parent->left = p;
        // Recalculate heights bottom-up
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
        // Recalculate heights for both children, then parent
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
        // Recalculate heights bottom-up
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
        // Recalculate heights for both children, then parent
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

/* Detaches and returns the minimum (max=false) or maximum (max=true) node from
 * the subtree rooted at root. The replacement node (the detached node's only
 * child, if any) is linked into the parent's slot. The ancestor path is then
 * rebalanced bottom-up using check_node_balance. Used by
 * perform_element_removal to find an in-order successor/predecessor without
 * recursive calls.
 *
 * The ancestor path is tracked in a fixed CBMAP_MAX_TREE_HEIGHT-entry array
 * rather than a heap-allocated stack, so this function has no allocation to
 * fail: unlike a heap allocation, which could otherwise abort the whole
 * process under memory pressure on what is supposed to be a graceful
 * deletion path, this can only ever fail via the same
 * provably-unreachable-for-a-real-AVL-tree depth bound the rest of this file
 * already treats as structurally impossible (see CBMAP_MAX_TREE_HEIGHT). */
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

  // Replace extreme node with its child
  bmap_node *replacement = max ? current->left : current->right;

  // Update parent link or return replacement if extreme was root
  if (path_len == 0) {
    return replacement;
  }

  // Update the last parent's child pointer
  node_stack_entry last_entry = path[path_len - 1];
  if (max) {
    last_entry.node->right = replacement;
  } else {
    last_entry.node->left = replacement;
  }

  // Rebalance from bottom to top
  for (size_t i = path_len; i-- > 0;) {
    node_stack_entry entry = path[i];
    bmap_node *balanced = check_node_balance(entry.node);

    // Update parent's pointer using the parent_link from the path
    if (entry.parent_link) {
      *(entry.parent_link) = balanced;
    } else {
      // This is the root
      root = balanced;
    }
  }

  return root;
}

/* Inserts or updates a key-value pair. Uses an explicit, fixed-size path
 * array (see CBMAP_MAX_TREE_HEIGHT) to record the ancestor chain so the tree
 * can be rebalanced bottom-up after insertion without recursion and without
 * a heap allocation on every call. Returns ccol_key_already_present when the
 * key already exists and its value was updated successfully. */
ccol_retval_t cbmap_insert_elem(cbmap cbm, const cmap_pair *key_pair,
                                const cmap_pair *val_pair) {
  if (!cbm) {
    ccol_assert(false);
  }

  // Handle empty tree case. elem_count is always 0 here (root is only ever
  // NULL when the map is empty), so no capacity check is needed: this is
  // always a genuinely new element, and max_elem_count (a real, if
  // astronomically large, cap) can never already be reached.
  if (!cbm->root) {
    cbm->root = create_new_node(cbm, key_pair, val_pair);
    if (!cbm->root) {
      return ccol_not_enough_memory;
    }
    cbm->elem_count++;
    return ccol_success;
  }

  // Fixed-size stack to track path from root to insertion point
  node_stack_entry path[CBMAP_MAX_TREE_HEIGHT];
  size_t path_len = 0;

  bmap_node *current = cbm->root;
  bmap_node **parent_link = &(cbm->root);
  ccol_retval_t result = ccol_not_enough_memory;

  // Navigate to insertion point or existing key
  while (current) {
    int comparison = compare_keys(cbm, key_pair, &current->key_pair);

    if (comparison == 0) {
      // Key already exists - update value
      update_bmap_node_value(cbm, current, val_pair, &result);
      return result;
    }

    // Push current node onto path
    ccol_assert(path_len < CBMAP_MAX_TREE_HEIGHT);
    path[path_len++] = (node_stack_entry){current, parent_link};

    if (comparison > 0) {
      parent_link = &(current->right);
      current = current->right;
    } else {
      parent_link = &(current->left);
      current = current->left;
    }
  }

  // Key genuinely not found: this insertion would grow elem_count, so this
  // is the correct point to enforce the capacity cap (checked only now,
  // after confirming the key doesn't already exist, so updating an existing
  // key's value at a full map still succeeds rather than being rejected).
  if (cbm->elem_count == max_elem_count) {
    return ccol_container_full;
  }

  // Create new node at insertion point
  bmap_node *new_node = create_new_node(cbm, key_pair, val_pair);
  if (!new_node) {
    return ccol_not_enough_memory;
  }

  *parent_link = new_node;
  result = ccol_success;

  // Rebalance from bottom to top
  for (size_t i = path_len; i-- > 0;) {
    node_stack_entry entry = path[i];
    bmap_node *balanced = check_node_balance(entry.node);

    // Update parent's pointer to this node
    if (entry.parent_link) {
      *(entry.parent_link) = balanced;
    } else {
      // This is the root
      cbm->root = balanced;
    }
  }

  cbm->elem_count++;
  return result;
}

/* Searches for key_pair and copies the associated value into target_buf.
 * Returns ccol_invalid_args when target_buf_size does not match the stored
 * value size exactly (this prevents silent truncation). */
ccol_retval_t cbmap_get_elem_copy(cbmap cbm, const cmap_pair *key_pair,
                                  void *target_buf, size_t target_buf_size) {
  if (!cbm) {
    ccol_assert(false);
  }

  bmap_node *tracker = cbm->root;
  while (tracker) {
    register int comparison = compare_keys(cbm, key_pair, &tracker->key_pair);
    if (comparison == 0) {
      if (target_buf_size != tracker->val_pair.size) {
        return ccol_invalid_args;
      }
      mem_cpy(target_buf, tracker->val_pair.ptr, target_buf_size);
      return ccol_success;
    }

    if (comparison < 0) {
      tracker = tracker->left;
    } else {
      tracker = tracker->right;
    }
  }

  return ccol_key_not_found;
}

/* Searches for key_pair and sets *val_pair to point directly into the node's
 * value buffer. The returned pointer is valid only as long as the key is not
 * deleted or updated with a value of different size. */
ccol_retval_t cbmap_get_elem_ref(cbmap cbm, const cmap_pair *key_pair,
                                 cmap_pair **val_pair) {
  if (!cbm) {
    ccol_assert(false);
  }

  bmap_node *tracker = cbm->root;
  while (tracker) {
    register int comparison = compare_keys(cbm, key_pair, &tracker->key_pair);
    if (comparison == 0) {
      *val_pair = &tracker->val_pair;
      return ccol_success;
    }

    if (comparison < 0) {
      tracker = tracker->left;
    } else {
      tracker = tracker->right;
    }
  }

  return ccol_key_not_found;
}

/* Removes parent from the tree and returns the replacement subtree root.
 * When both children exist, the deeper side donates its extreme node
 * (right side -> min node; left side -> max node) to replace parent, which
 * preserves the BST ordering and keeps the tree balanced with minimal rotation.
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
    // Both left and right are non-NULL
    if (right->height >= left->height) {
      // Right side is deeper
      bmap_node *right_min = NULL;
      right = cbmap_detach_extreme_iter(right, false, &right_min);
      parent = right_min;
      parent->right = right;
      parent->left = left;
      // Recalculate the height of the replacement node after assigning new
      // children
      recalculate_node_height(parent);
    } else {
      // Left side is deeper
      bmap_node *left_max = NULL;
      left = cbmap_detach_extreme_iter(left, true, &left_max);
      parent = left_max;
      parent->right = right;
      parent->left = left;
      // Recalculate the height of the replacement node after assigning new
      // children
      recalculate_node_height(parent);
    }
  }

  return parent;
}

/* Deletes the entry with key_pair from the map. Uses the same explicit,
 * fixed-size path array as cbmap_insert_elem to rebalance ancestors
 * bottom-up after the node is removed, with no heap allocation involved.
 * Returns ccol_key_not_found when the key does not exist. */
ccol_retval_t cbmap_delete_elem(cbmap cbm, const cmap_pair *key_pair) {
  if (!cbm) {
    ccol_assert(false);
  }

  if (!cbm->root) {
    return ccol_key_not_found;
  }

  // Fixed-size stack to track path from root to node to delete
  node_stack_entry path[CBMAP_MAX_TREE_HEIGHT];
  size_t path_len = 0;

  bmap_node *current = cbm->root;
  bmap_node **parent_link = &(cbm->root);
  ccol_retval_t result = ccol_key_not_found;

  // Navigate to node to delete
  while (current) {
    int comparison = compare_keys(cbm, key_pair, &current->key_pair);

    if (comparison == 0) {
      // Found the node to delete
      bmap_node *replacement = perform_element_removal(cbm, current);
      *parent_link = replacement;
      result = ccol_success;
      break;
    }

    // Push current node onto path
    ccol_assert(path_len < CBMAP_MAX_TREE_HEIGHT);
    path[path_len++] = (node_stack_entry){current, parent_link};

    if (comparison > 0) {
      parent_link = &(current->right);
      current = current->right;
    } else {
      parent_link = &(current->left);
      current = current->left;
    }
  }

  if (result == ccol_success) {
    // Rebalance from bottom to top
    for (size_t i = path_len; i-- > 0;) {
      node_stack_entry entry = path[i];
      bmap_node *balanced = check_node_balance(entry.node);

      // Update parent's pointer to this node
      if (entry.parent_link) {
        *(entry.parent_link) = balanced;
      } else {
        // This is the root
        cbm->root = balanced;
      }
    }

    cbm->elem_count--;
  }

  return result;
}

#ifdef RUNNING_UNIT_TESTS
/* Walks the whole tree, checking at every node that the AVL balance
 * invariant (abs(right_height - left_height) <= 1) and the height
 * bookkeeping invariant (height == max(height(left), height(right)) + 1)
 * both hold. bmap_node is opaque outside this translation unit, so this is
 * the only way for test code to check these invariants; it deliberately
 * returns a single pass/fail rather than any node pointer, so no internal
 * pointer ever crosses the public API boundary.
 *
 * Iterative (an explicit fixed-size stack, not recursion), matching this
 * module's own established preference for iterative tree traversal, and
 * using the same CBMAP_MAX_TREE_HEIGHT bound the rest of this file's own
 * traversal stacks rely on. */
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
        return false; /* tree deeper than any real AVL tree can be */
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
