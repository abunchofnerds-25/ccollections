/*
MIT License

Copyright (c) 2024 A bunch of nerds

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
#include <cvector.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  bool keys_are_signed_ints;
  ccol_comparison_proc_t custom_comparison_proc;
} cbinarymap;

typedef struct cbmap_cmap_iterator {  // Extended cmap_iterator for cbmap
  cbmap parent_map;
  cvec nodes;
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

bmap_node *get_min_node(bmap_node *root, size_t *depth) {
  if (depth) {
    *depth = 0;
  }

  bmap_node *result = root;

  while (result && result->left) {
    if (depth) {
      ++(*depth);
    }
    result = result->left;
  }

  return result;
}

bmap_node *get_max_node(bmap_node *root, size_t *depth) {
  if (depth) {
    *depth = 0;
  }

  bmap_node *result = root;

  while (result && result->right) {
    if (depth) {
      ++(*depth);
    }
    result = result->right;
  }

  return result;
}

void push_all_lefts_into_iter_stack(cbmap_cmap_iterator *real_iter,
                                    bmap_node *node) {
  while (node) {
    ccol_assert(cvector_push_back(real_iter->nodes, &node) == ccol_success);
    node = node->left;
  }
}

cmap_iterator *cmap_real_iter_next(cbmap_cmap_iterator *real_iter) {
  if (cvector_elem_count(real_iter->nodes) == 0) {
    // Nowhere to advance
    __cbmap_iterator_destroy(&real_iter->user_iter);
    return NULL;
  }
  bmap_node *node;
  ccol_assert(cvector_pop_back(real_iter->nodes, &node) == ccol_success);
  if (node->right) {
    push_all_lefts_into_iter_stack(real_iter, node->right);
  }

  real_iter->user_iter.key_pair = &node->key_pair;
  real_iter->user_iter.val_pair = &node->val_pair;
  return &real_iter->user_iter;
}

cmap_iterator *cbmap_begin_iter(cbmap cbm, char **err) {
  if (!cbm) {
    ccol_assert(false);
  }

  if (err) {
    *err = NULL;
  }

  if (!cbm->root) {
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

  real_iter->nodes =
      cvector_create_full(sizeof(bmap_node *), cbm->m_procs, NULL);
  if (!real_iter->nodes) {
    _mem_free(cbm->m_procs, real_iter);
    if (err) {
      *err = CCOL_ERR_STR("Failed to create iterator node stack");
    }
    return NULL;
  }

  real_iter->parent_map = cbm;
  push_all_lefts_into_iter_stack(real_iter, cbm->root);
  return cmap_real_iter_next(real_iter);
}

cmap_iterator *cbmap_iter_next(cmap_iterator *iter) {
  // Advance to the next node
  if (!iter) {
    ccol_assert(false);
  }

  cbmap_cmap_iterator *real_iter = cmapIter2CbmapIter(iter);
  return cmap_real_iter_next(real_iter);
}

void __cbmap_iterator_destroy(cmap_iterator *iter) {
  if (iter) {
    cbmap_cmap_iterator *real_iter = cmapIter2CbmapIter(iter);
    cvector_destroy(real_iter->nodes);
    _mem_free(real_iter->parent_map->m_procs, real_iter);
  }
}

bool verify_cbmap_create_inputs(ccol_memmgmt_procs_t *mmgmt_procs, char **err) {
  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return false;
  }

  return true;
}

cbmap cbmap_create_full(bool keys_are_signed_ints,
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
  cbm->keys_are_signed_ints = keys_are_signed_ints;
  cbm->custom_comparison_proc = custom_comparison_proc;

  return cbm;
}

void destroy_bmap_node(cbmap cbm, bmap_node *node) {
  if (node) {
    _mem_free(cbm->m_procs, node->key_pair.ptr);
    _mem_free(cbm->m_procs, node->val_pair.ptr);
    _mem_free(cbm->m_procs, node);
  }
}

// Iterative post-order traversal to clear all nodes
void _clear_nodes(cbmap cbm) {
  if (!cbm->root) {
    return;
  }

  // Use a stack for iterative post-order traversal
  cvec stack = cvector_create_full(sizeof(bmap_node *), cbm->m_procs, NULL);
  ccol_assert(stack != NULL);

  bmap_node *current = cbm->root;
  bmap_node *last_visited = NULL;

  while (cvector_elem_count(stack) > 0 || current) {
    // Go to the leftmost node
    if (current) {
      ccol_assert(cvector_push_back(stack, &current) == ccol_success);
      current = current->left;
    } else {
      // Peek at the top of stack
      bmap_node *peek =
          *(bmap_node **)cvector_at(stack, cvector_elem_count(stack) - 1);

      // If right child exists and not yet processed
      if (peek->right && peek->right != last_visited) {
        current = peek->right;
      } else {
        // Process this node (both children done)
        bmap_node *_popped;
        ccol_assert(cvector_pop_back(stack, &_popped) == ccol_success);
        destroy_bmap_node(cbm, peek);
        last_visited = peek;
      }
    }
  }

  cvector_destroy(stack);

  cbm->root = NULL;
  cbm->elem_count = 0;
}

void __cbmap_destroy(cbmap cbm) {
  if (cbm) {
    _clear_nodes(cbm);

    if (cbm->m_procs) {
      ccol_memmgmt_procs_free_t free_func = cbm->m_procs->free;
      free_func(cbm->m_procs);
      free_func(cbm);
    } else {
      mem_free(cbm);
    }
  }
}

size_t cbmap_elem_count(cbmap cbm) {
  if (!cbm) {
    ccol_assert(false);
  }

  return cbm->elem_count;
}

ccol_retval_t cbmap_reset(cbmap cbm) {
  if (!cbm) {
    ccol_assert(false);
  }

  _clear_nodes(cbm);

  return ccol_success;
}

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
    }
  }
}

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

static inline int compare_keys(cbmap cbm, const cmap_pair *key_pair1,
                               const cmap_pair *key_pair2) {
  if (cbm->custom_comparison_proc) {
    return cbm->custom_comparison_proc(key_pair1->ptr, key_pair2->ptr);
  }

  if (key_pair1->size != key_pair2->size) {
    // Different sizes - compare common prefix, then by size
    size_t min_size = ccol_min(key_pair1->size, key_pair2->size);
    int cmp = memcmp(key_pair1->ptr, key_pair2->ptr, min_size);
    if (cmp != 0) {
      return cmp;
    }

    // Common prefix is equal, shorter string comes first
    return (key_pair1->size > key_pair2->size) -
           (key_pair1->size < key_pair2->size);
  }

  if (cbm->keys_are_signed_ints) {
    // Properly handle signed integer comparison for standard sizes
    return cmp_signed_small(key_pair1->ptr, key_pair2->ptr, key_pair1->size);
  }

  return cmp_unsigned_small(key_pair1->ptr, key_pair2->ptr, key_pair1->size);
}

bmap_node *create_new_node(cbmap cbm, const cmap_pair *key_pair,
                           const cmap_pair *val_pair) {
  bmap_node *new_node = _mem_alloc(cbm->m_procs, sizeof(bmap_node));
  if (!new_node) {
    return NULL;
  }

  new_node->key_pair.ptr = _mem_alloc(cbm->m_procs, key_pair->size);
  if (!new_node->key_pair.ptr) {
    _mem_free(cbm->m_procs, new_node);
    return NULL;
  }

  new_node->val_pair.ptr = _mem_alloc(cbm->m_procs, val_pair->size);
  if (!new_node->val_pair.ptr) {
    _mem_free(cbm->m_procs, new_node->key_pair.ptr);
    _mem_free(cbm->m_procs, new_node);
    return NULL;
  }

  mem_cpy(new_node->key_pair.ptr, key_pair->ptr, key_pair->size);
  new_node->key_pair.size = key_pair->size;
  mem_cpy(new_node->val_pair.ptr, val_pair->ptr, val_pair->size);
  new_node->val_pair.size = val_pair->size;

  new_node->left = NULL;
  new_node->right = NULL;
  new_node->height = 0;

  return new_node;
}

// Implemented not to have a dependency on an external library
int absolute(int x) {
  if (x < 0) {
    return -x;
  }
  return x;
}

// Implemented not to have a dependency on an external library
int maximum(int x, int y) {
  if (x >= y) {
    return x;
  }
  return y;
}

void update_bmap_node_value(cbmap cbm, bmap_node *node,
                            const cmap_pair *val_pair, ccol_retval_t *result) {
  if (node->val_pair.size != val_pair->size) {
    // Different value sizes, reallocation needed
    void *new_ptr =
        _mem_realloc(cbm->m_procs, node->val_pair.ptr, val_pair->size);
    if (!new_ptr) {
      // reallocation attempt failed!
      return;
    }
    node->val_pair.ptr = new_ptr;
  }
  mem_cpy(node->val_pair.ptr, val_pair->ptr, val_pair->size);
  *result = ccol_key_already_present;
}

int node_height(bmap_node *node) {
  if (!node) {
    return -1;
  }

  return node->height;
}

int node_balance(bmap_node *node) {
  if (!node) {
    return 0;
  }

  return node_height(node->right) - node_height(node->left);
}

void recalculate_node_height(bmap_node *node) {
  if (!node) {
    return;
  }

  node->height = maximum(node_height(node->left), node_height(node->right)) + 1;
}

bmap_node *check_node_balance(bmap_node *parent) {
  recalculate_node_height(parent);
  int balance = node_balance(parent);

  if (absolute(balance) > 1) {
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

  if (absolute(node_balance(parent)) > 1) {
    ccol_assert(false);
  }
  return parent;
}

// Iterative version of detach extreme node
bmap_node *cbmap_detach_extreme_iter(bmap_node *root, bool max,
                                     bmap_node **extreme,
                                     ccol_memmgmt_procs_t *mprocs) {
  if (!root) {
    ccol_assert(false);
  }

  // Stack to track path from root to extreme node
  cvec path = cvector_create_full(sizeof(node_stack_entry), mprocs, NULL);
  ccol_assert(path != NULL);

  // Find the extreme node and build the path
  bmap_node *current = root;
  bmap_node **parent_link = NULL;

  while (true) {
    if (max) {
      if (current->right) {
        node_stack_entry entry = {current, parent_link};
        ccol_assert(cvector_push_back(path, &entry) == ccol_success);
        parent_link = &(current->right);
        current = current->right;
      } else {
        // Found the max node
        *extreme = current;
        break;
      }
    } else {
      if (current->left) {
        node_stack_entry entry = {current, parent_link};
        ccol_assert(cvector_push_back(path, &entry) == ccol_success);
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
  if (cvector_elem_count(path) == 0) {
    cvector_destroy(path);
    return replacement;
  }

  // Update the last parent's child pointer
  node_stack_entry last_entry =
      *(node_stack_entry *)cvector_at(path, cvector_elem_count(path) - 1);
  if (max) {
    last_entry.node->right = replacement;
  } else {
    last_entry.node->left = replacement;
  }

  // Rebalance from bottom to top
  for (size_t i = cvector_elem_count(path) - 1; i != (size_t)-1; i--) {
    node_stack_entry entry = *(node_stack_entry *)cvector_at(path, i);
    bmap_node *balanced = check_node_balance(entry.node);

    // Update parent's pointer using the parent_link from the path
    if (entry.parent_link) {
      *(entry.parent_link) = balanced;
    } else {
      // This is the root
      root = balanced;
    }
  }

  cvector_destroy(path);
  return root;
}

// Iterative version of insert
ccol_retval_t cbmap_insert_elem(cbmap cbm, const cmap_pair *key_pair,
                                const cmap_pair *val_pair) {
  if (!cbm) {
    ccol_assert(false);
  }

  if (cbm->elem_count == max_elem_count) {
    return ccol_container_full;
  }

  // Handle empty tree case
  if (!cbm->root) {
    cbm->root = create_new_node(cbm, key_pair, val_pair);
    if (!cbm->root) {
      return ccol_not_enough_memory;
    }
    cbm->elem_count++;
    return ccol_success;
  }

  // Stack to track path from root to insertion point
  cvec path = cvector_create_full(sizeof(node_stack_entry), cbm->m_procs, NULL);
  if (!path) {
    return ccol_not_enough_memory;
  }

  bmap_node *current = cbm->root;
  bmap_node **parent_link = &(cbm->root);
  ccol_retval_t result = ccol_not_enough_memory;

  // Navigate to insertion point or existing key
  while (current) {
    int comparison = compare_keys(cbm, key_pair, &current->key_pair);

    if (comparison == 0) {
      // Key already exists - update value
      update_bmap_node_value(cbm, current, val_pair, &result);
      cvector_destroy(path);
      return ccol_success;  // Update succeeded
    }

    // Push current node onto path
    node_stack_entry entry = {current, parent_link};
    if (cvector_push_back(path, &entry) != ccol_success) {
      cvector_destroy(path);
      return ccol_not_enough_memory;
    }

    if (comparison > 0) {
      parent_link = &(current->right);
      current = current->right;
    } else {
      parent_link = &(current->left);
      current = current->left;
    }
  }

  // Create new node at insertion point
  bmap_node *new_node = create_new_node(cbm, key_pair, val_pair);
  if (!new_node) {
    cvector_destroy(path);
    return ccol_not_enough_memory;
  }

  *parent_link = new_node;
  result = ccol_success;

  // Rebalance from bottom to top
  for (size_t i = cvector_elem_count(path) - 1; i != (size_t)-1; i--) {
    node_stack_entry entry = *(node_stack_entry *)cvector_at(path, i);
    bmap_node *balanced = check_node_balance(entry.node);

    // Update parent's pointer to this node
    if (entry.parent_link) {
      *(entry.parent_link) = balanced;
    } else {
      // This is the root
      cbm->root = balanced;
    }
  }

  cvector_destroy(path);
  cbm->elem_count++;
  return result;
}

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

bmap_node *perform_element_removal(cbmap cbm, bmap_node *parent) {
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
      right = cbmap_detach_extreme_iter(right, false, &right_min, cbm->m_procs);
      parent = right_min;
      parent->right = right;
      parent->left = left;
      // Recalculate the height of the replacement node after assigning new
      // children
      recalculate_node_height(parent);
    } else {
      // Left side is deeper
      bmap_node *left_max = NULL;
      left = cbmap_detach_extreme_iter(left, true, &left_max, cbm->m_procs);
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

// Iterative version of delete
ccol_retval_t cbmap_delete_elem(cbmap cbm, const cmap_pair *key_pair) {
  if (!cbm) {
    ccol_assert(false);
  }

  if (!cbm->root) {
    return ccol_key_not_found;
  }

  // Stack to track path from root to node to delete
  cvec path = cvector_create_full(sizeof(node_stack_entry), cbm->m_procs, NULL);
  if (!path) {
    return ccol_not_enough_memory;
  }

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
    node_stack_entry entry = {current, parent_link};
    if (cvector_push_back(path, &entry) != ccol_success) {
      cvector_destroy(path);
      return ccol_not_enough_memory;
    }

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
    for (size_t i = cvector_elem_count(path) - 1; i != (size_t)-1; i--) {
      node_stack_entry entry = *(node_stack_entry *)cvector_at(path, i);
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

  cvector_destroy(path);
  return result;
}
