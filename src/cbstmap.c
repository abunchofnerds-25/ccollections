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
  struct bmap_node* left;
  struct bmap_node* right;
  // Metadata for self-balancing
  size_t height;
} bmap_node;

typedef struct cbinarymap {
  size_t elem_count;
  bmap_node* root;
  ccol_memmgmt_procs_t* m_procs;
  bool keys_are_signed;
  ccol_comparison_proc_t custom_comparison_proc;
} cbinarymap;

typedef struct cbmap_cmap_iterator {  // Extended cmap_iterator for cbmap
  cbmap parent_map;
  cvec_declare(nodes, bmap_node*);
  cmap_iterator user_iter;
} cbmap_cmap_iterator;

#define cmapIter2CbmapIter(u_iter)          \
  (cbmap_cmap_iterator*)((uint8_t*)u_iter - \
                         offsetof(cbmap_cmap_iterator, user_iter))

bmap_node* get_min_node(bmap_node* root, size_t* depth) {
  if (depth) {
    *depth = 0;
  }

  bmap_node* result = root;

  while (result && result->left) {
    if (depth) {
      ++(*depth);
    }
    result = result->left;
  }

  return result;
}

bmap_node* get_max_node(bmap_node* root, size_t* depth) {
  if (depth) {
    *depth = 0;
  }

  bmap_node* result = root;

  while (result && result->right) {
    if (depth) {
      ++(*depth);
    }
    result = result->right;
  }

  return result;
}

void push_all_lefts_into_iter_stack(cbmap_cmap_iterator* real_iter,
                                    bmap_node* node) {
  cvec vn = real_iter->nodes;
  cvec_enable_local_macros(vn, bmap_node*);
  while (node) {
    cvec_push(vn, node);
    node = node->left;
  }
}

cmap_iterator* cmap_real_iter_next(cbmap_cmap_iterator* real_iter) {
  cvec vn = real_iter->nodes;
  cvec_enable_local_macros(vn, bmap_node*);
  if (cvec_size(vn) == 0) {
    // Nowhere to advance
    __cbmap_iterator_destroy(&real_iter->user_iter);
    return NULL;
  }
  bmap_node* node = cvec_pop(vn);
  if (node->right) {
    push_all_lefts_into_iter_stack(real_iter, node->right);
  }

  real_iter->user_iter.key_pair = &node->key_pair;
  real_iter->user_iter.val_pair = &node->val_pair;
  return &real_iter->user_iter;
}

cmap_iterator* cbmap_begin_iter(cbmap cbm, char** err) {
  if (!cbm) {
    assert(false);
  }

  if (err) {
    *err = NULL;
  }

  if (!cbm->root) {
    return NULL;
  }

  cbmap_cmap_iterator* real_iter =
      _mem_alloc(cbm->m_procs, sizeof(cbmap_cmap_iterator));
  if (!real_iter) {
    if (err) {
      *err = CCOL_ERR_STR("Failed to create the iterator buffer");
    }
    return NULL;
  }

  // Initialize the stack of nodes within the real iterator.
  cvec_init_with_mprocs(real_iter->nodes, cbm->m_procs);

  real_iter->parent_map = cbm;
  push_all_lefts_into_iter_stack(real_iter, cbm->root);
  return cmap_real_iter_next(real_iter);
}

cmap_iterator* cbmap_iter_next(cmap_iterator* iter) {
  // Advance to the next node
  if (!iter) {
    assert(false);
  }

  cbmap_cmap_iterator* real_iter = cmapIter2CbmapIter(iter);
  return cmap_real_iter_next(real_iter);
}

void __cbmap_iterator_destroy(cmap_iterator* iter) {
  if (iter) {
    cbmap_cmap_iterator* real_iter = cmapIter2CbmapIter(iter);
    cvec_destroy(real_iter->nodes);
    _mem_free(real_iter->parent_map->m_procs, real_iter);
  }
}

bool verify_cbmap_create_inputs(ccol_memmgmt_procs_t* mmgmt_procs, char** err) {
  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return false;
  }

  return true;
}

cbmap cbmap_create_full(bool keys_are_signed, ccol_memmgmt_procs_t* mmgmt_procs,
                        ccol_comparison_proc_t custom_comparison_proc,
                        char** err) {
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
  cbm->keys_are_signed = keys_are_signed;
  cbm->custom_comparison_proc = custom_comparison_proc;

  return cbm;
}

void destroy_bmap_node(cbmap cbm, bmap_node* node) {
  if (node) {
    _mem_free(cbm->m_procs, node->key_pair.ptr);
    _mem_free(cbm->m_procs, node->val_pair.ptr);
    _mem_free(cbm->m_procs, node);
  }
}

bmap_node* _clear_nodes_r(cbmap cbm, bmap_node* parent) {
  if (parent) {
    parent->left = _clear_nodes_r(cbm, parent->left);
    parent->right = _clear_nodes_r(cbm, parent->right);
    destroy_bmap_node(cbm, parent);
    parent = NULL;
  }
  return parent;
}

void _clear_nodes(cbmap cbm) {
  cbm->root = _clear_nodes_r(cbm, cbm->root);
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
    assert(false);
  }

  return cbm->elem_count;
}

ccol_retval_t cbmap_reset(cbmap cbm) {
  if (!cbm) {
    assert(false);
  }

  _clear_nodes(cbm);

  return ccol_success;
}

static inline int compare_keys(cbmap cbm, const cmap_pair* key_pair1,
                               const cmap_pair* key_pair2) {
  if (cbm->custom_comparison_proc) {
    return cbm->custom_comparison_proc(key_pair1->ptr, key_pair2->ptr);
  }

  if (key_pair1->size == key_pair2->size) {
    if (!cbm->keys_are_signed) {
      return memcmp(key_pair1->ptr, key_pair2->ptr, key_pair1->size);
    }
    // Properly handle signed integer comparison for standard sizes
    switch (key_pair1->size) {
      case 1: {
        int8_t v1 = *(int8_t*)key_pair1->ptr;
        int8_t v2 = *(int8_t*)key_pair2->ptr;
        return (v1 > v2) - (v1 < v2);
      }
      case 2: {
        int16_t v1 = *(int16_t*)key_pair1->ptr;
        int16_t v2 = *(int16_t*)key_pair2->ptr;
        return (v1 > v2) - (v1 < v2);
      }
      case 4: {
        int32_t v1 = *(int32_t*)key_pair1->ptr;
        int32_t v2 = *(int32_t*)key_pair2->ptr;
        return (v1 > v2) - (v1 < v2);
      }
      case 8: {
        int64_t v1 = *(int64_t*)key_pair1->ptr;
        int64_t v2 = *(int64_t*)key_pair2->ptr;
        return (v1 > v2) - (v1 < v2);
      }
      default:
        // For non-standard sizes, just complain, as this should not
        // be classified as a 'signed' number
        assert(false);
    }
  }
  return strcmp(key_pair1->ptr, key_pair2->ptr);
}

bmap_node* create_new_node(cbmap cbm, const cmap_pair* key_pair,
                           const cmap_pair* val_pair) {
  bmap_node* new_node = _mem_alloc(cbm->m_procs, sizeof(bmap_node));
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

  memcpy(new_node->key_pair.ptr, key_pair->ptr, key_pair->size);
  new_node->key_pair.size = key_pair->size;
  memcpy(new_node->val_pair.ptr, val_pair->ptr, val_pair->size);
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

void update_bmap_node_value(cbmap cbm, bmap_node* node,
                            const cmap_pair* val_pair, ccol_retval_t* result) {
  if (node->val_pair.size != val_pair->size) {
    // Different value sizes, reallocation needed
    void* new_ptr =
        _mem_realloc(cbm->m_procs, node->val_pair.ptr, val_pair->size);
    if (!new_ptr) {
      // reallocation attempt failed!
      return;
    }
    node->val_pair.ptr = new_ptr;
  }
  memcpy(node->val_pair.ptr, val_pair->ptr, val_pair->size);
  *result = ccol_key_already_present;
}

int node_height(bmap_node* node) {
  if (!node) {
    return -1;
  }

  return node->height;
}

int node_balance(bmap_node* node) {
  if (!node) {
    return 0;
  }

  return node_height(node->right) - node_height(node->left);
}

void recalculate_node_height(bmap_node* node) {
  if (!node) {
    return;
  }

  node->height = maximum(node_height(node->left), node_height(node->right)) + 1;
}

bmap_node* check_node_balance(bmap_node* parent) {
  recalculate_node_height(parent);
  int balance = node_balance(parent);

  if (absolute(balance) > 1) {
    // The symmetry is lost! A rotation is needed
    bmap_node* p = parent;

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
        bmap_node* rl_left = parent->right->left->left;
        bmap_node* rl_right = parent->right->left->right;
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
        bmap_node* lr_left = parent->left->right->left;
        bmap_node* lr_right = parent->left->right->right;
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
    assert(false);
  }
  return parent;
}

bmap_node* cbmap_detach_extreme_r(bmap_node* parent, bool max,
                                  bmap_node** extreme) {
  if (!parent) {
    assert(false);
  }

  if (max) {
    if (parent->right) {
      parent->right = cbmap_detach_extreme_r(parent->right, max, extreme);
    } else {
      *extreme = parent;
      parent = parent->left;
    }
  } else {
    if (parent->left) {
      parent->left = cbmap_detach_extreme_r(parent->left, max, extreme);
    } else {
      *extreme = parent;
      parent = parent->right;
    }
  }

  parent = check_node_balance(parent);

  return parent;
}

typedef struct cbmap_insert_elem_r_arg {
  cbmap cbm;
  const cmap_pair* key_pair;
  const cmap_pair* val_pair;
  ccol_retval_t result;
} cbmap_insert_elem_r_arg;

bmap_node* cbmap_insert_elem_r(bmap_node* parent,
                               cbmap_insert_elem_r_arg* args) {
  if (!parent) {
    parent = create_new_node(args->cbm, args->key_pair, args->val_pair);
    if (!parent) {
      return parent;
    }

    args->result = ccol_success;
    return parent;
  }

  register int comparison =
      compare_keys(args->cbm, args->key_pair, &parent->key_pair);
  if (comparison == 0) {
    // This entry exists
    update_bmap_node_value(args->cbm, parent, args->val_pair, &args->result);
    return parent;
  }

  if (comparison > 0) {
    // We should go right!
    parent->right = cbmap_insert_elem_r(parent->right, args);
  } else {
    // We should go left!
    parent->left = cbmap_insert_elem_r(parent->left, args);
  }

  if (args->result == ccol_success) {
    parent = check_node_balance(parent);
  }

  return parent;
}

ccol_retval_t cbmap_insert_elem(cbmap cbm, const cmap_pair* key_pair,
                                const cmap_pair* val_pair) {
  if (!cbm) {
    assert(false);
  }

  if (cbm->elem_count == max_elem_count) {
    return ccol_container_full;
  }

  cbmap_insert_elem_r_arg args = {.cbm = cbm,
                                  .key_pair = key_pair,
                                  .val_pair = val_pair,
                                  .result = ccol_not_enough_memory};

  cbm->root = cbmap_insert_elem_r(cbm->root, &args);
  if (args.result == ccol_success) {
    // That was an insertion
    ++(cbm->elem_count);
  } else if (args.result == ccol_key_already_present) {
    // That was an update
    args.result = ccol_success;
  }
  return args.result;
}

ccol_retval_t cbmap_get_elem_copy(cbmap cbm, const cmap_pair* key_pair,
                                  void* target_buf, size_t target_buf_size) {
  if (!cbm) {
    assert(false);
  }

  bmap_node* tracker = cbm->root;
  while (tracker) {
    register int comparison = compare_keys(cbm, key_pair, &tracker->key_pair);
    if (comparison == 0) {
      if (target_buf_size != tracker->val_pair.size) {
        return ccol_invalid_args;
      }
      memcpy(target_buf, tracker->val_pair.ptr, target_buf_size);
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

ccol_retval_t cbmap_get_elem_ref(cbmap cbm, const cmap_pair* key_pair,
                                 cmap_pair** val_pair) {
  if (!cbm) {
    assert(false);
  }

  bmap_node* tracker = cbm->root;
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

bmap_node* perform_element_removal(cbmap cbm, bmap_node* parent) {
  bmap_node* left = parent->left;
  bmap_node* right = parent->right;
  destroy_bmap_node(cbm, parent);

  if (!left) {
    parent = right;
  } else if (!right) {
    parent = left;
  } else {
    // Both left and right are non-NULL
    if (right->height >= left->height) {
      // Right side is deeper
      bmap_node* right_min = NULL;
      right = cbmap_detach_extreme_r(right, false, &right_min);
      parent = right_min;
      parent->right = right;
      parent->left = left;
      // Recalculate the height of the replacement node after assigning new
      // children
      recalculate_node_height(parent);
    } else {
      // Left side is deeper
      bmap_node* left_max = NULL;
      left = cbmap_detach_extreme_r(left, true, &left_max);
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

typedef struct cbmap_delete_elem_r_args {
  cbmap cbm;
  const cmap_pair* key_pair;
  ccol_retval_t result;
} cbmap_delete_elem_r_args;

bmap_node* cbmap_delete_elem_r(bmap_node* parent,
                               cbmap_delete_elem_r_args* args) {
  if (!parent) {
    // The key is not present within the map
    return parent;
  }

  register int comparison =
      compare_keys(args->cbm, args->key_pair, &parent->key_pair);
  if (comparison == 0) {
    // Found the entry!
    parent = perform_element_removal(args->cbm, parent);
    args->result = ccol_success;
    return parent;
  }

  if (comparison > 0) {
    // We should go right!
    parent->right = cbmap_delete_elem_r(parent->right, args);
  } else {
    // We should go left!
    parent->left = cbmap_delete_elem_r(parent->left, args);
  }

  if (args->result == ccol_success) {
    parent = check_node_balance(parent);
  }

  return parent;
}

ccol_retval_t cbmap_delete_elem(cbmap cbm, const cmap_pair* key_pair) {
  if (!cbm) {
    assert(false);
  }

  cbmap_delete_elem_r_args args = {
      .cbm = cbm, .key_pair = key_pair, .result = ccol_key_not_found};

  cbm->root = cbmap_delete_elem_r(cbm->root, &args);
  if (args.result == ccol_success) {
    --(cbm->elem_count);
  }

  return args.result;
}
