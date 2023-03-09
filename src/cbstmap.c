/*
MIT License

Copyright (c) 2018 Danis Ozdemir

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

typedef enum bmap_node_colour { red = 0, black } bmap_node_colour;

typedef struct bmap_node {
  // Data related pointers
  cmap_pair key_pair;
  cmap_pair val_pair;
  // Relational pointers
  struct bmap_node* parent;
  struct bmap_node* left;
  struct bmap_node* right;
  // Metadata for auto-balancing
  bmap_node_colour colour;
  uint32_t height;
} bmap_node;

typedef struct cbinarymap {
  uint32_t elem_count;
  bmap_node* root;
  ccol_memmgmt_procs_t* m_procs;
} cbinarymap;

typedef struct cbmap_cmap_iterator {  // Extended cmap_iterator for cbmap
  // User doesn't need to know about the fields '_handle' and 'parent_map' or
  // use them directly
  cbmap parent_map;
  bmap_node* tracker;
  bmap_node* final;
  cvec_declare(visited_nodes, bmap_node*);
  cmap_iterator user_iter;
} cbmap_cmap_iterator;

#define cmapIter2CbmapIter(u_iter)          \
  (cbmap_cmap_iterator*)((uint8_t*)u_iter - \
                         offsetof(cbmap_cmap_iterator, user_iter))

bmap_node* get_min_node(bmap_node* root, uint32_t* depth) {
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

bmap_node* get_max_node(bmap_node* root, uint32_t* depth) {
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

  cvec vn = real_iter->visited_nodes;
  cvec_enable_local_macros(vn, bmap_node*);
  cvec_init(vn);
  real_iter->visited_nodes = vn;

  real_iter->parent_map = cbm;
  real_iter->tracker = cbm->root;

  real_iter->final = get_max_node(cbm->root, NULL);
  if (!real_iter->tracker->left) {
    cvec_push(vn, real_iter->tracker);
  }

  while (real_iter->tracker && real_iter->tracker->left) {
    cvec_push(vn, real_iter->tracker);
    real_iter->tracker = real_iter->tracker->left;
  }

  real_iter->user_iter.key_pair = &real_iter->tracker->key_pair;
  real_iter->user_iter.val_pair = &real_iter->tracker->val_pair;

  return &real_iter->user_iter;
}

cmap_iterator* cbmap_iter_next(cmap_iterator* iter) {
  // Advance to the next node
  if (!iter) {
    assert(false);
  }

  cbmap_cmap_iterator* real_iter = cmapIter2CbmapIter(iter);

  if (real_iter->final == real_iter->tracker) {
    __cbmap_iterator_destroy(iter);
    return NULL;
  }

  cvec vn = real_iter->visited_nodes;
  cvec_enable_local_macros(vn, bmap_node*);
  bmap_node* previous_node = cvec_at(vn, cvec_size(vn) - 1);

  bool dont_go_left = false;
  bool dont_go_right = false;

  if (previous_node == real_iter->tracker->left) {
    // We came here from our left child
    cvec_pop(vn);
    dont_go_left = true;
  } else if (previous_node == real_iter->tracker->right) {
    // We came here from our right child
    cvec_pop(vn);
    dont_go_left = true;
    dont_go_right = true;
  }

  if (!dont_go_left && real_iter->tracker->left) {
    // We can go left, so let's go as much as we can.
    while (real_iter->tracker && real_iter->tracker->left) {
      cvec_push(vn, real_iter->tracker);
      real_iter->tracker = real_iter->tracker->left;
    }
    iter->key_pair = &real_iter->tracker->key_pair;
    iter->val_pair = &real_iter->tracker->val_pair;
    return iter;
  }

  if (!dont_go_right && real_iter->tracker->right) {
    // No direct left is available, but there is a right leg
    if (real_iter->tracker->right->left) {
      // That right leg itself has a left leg which is less
      cvec_push(vn, real_iter->tracker->right);
      real_iter->tracker = real_iter->tracker->right;
      while (real_iter->tracker && real_iter->tracker->left) {
        cvec_push(vn, real_iter->tracker);
        real_iter->tracker = real_iter->tracker->left;
      }
    } else {
      // That right leg itself does not have a left leg
      cvec_push(vn, real_iter->tracker);
      real_iter->tracker = real_iter->tracker->right;
    }
    iter->key_pair = &real_iter->tracker->key_pair;
    iter->val_pair = &real_iter->tracker->val_pair;
    return iter;
  }

  if (!real_iter->tracker->left && !real_iter->tracker->right) {
    // No right or left sub node is present, this is a leaf node.
    cvec_push(vn, real_iter->tracker);
  }
  real_iter->tracker = real_iter->tracker->parent;

  // if (!real_iter->tracker) {
  //   // We are at the root node trying to go to a parent meaning nowhere to
  //   go.
  //   // It's time to destroy, we are done.
  //   __cbmap_iterator_destroy(iter);
  //   return NULL;
  // }

  iter->key_pair = &real_iter->tracker->key_pair;
  iter->val_pair = &real_iter->tracker->val_pair;
  return iter;
}

void __cbmap_iterator_destroy(cmap_iterator* iter) {
  if (iter) {
    cbmap_cmap_iterator* real_iter = cmapIter2CbmapIter(iter);
    cvec_destruct(real_iter->visited_nodes);
    _mem_free(real_iter->parent_map->m_procs, real_iter);
  }
}

bool verify_cbmap_create_inputs(ccol_memmgmt_procs_t* mmgmt_procs, char** err) {
  if (mmgmt_procs && (!mmgmt_procs->malloc || !mmgmt_procs->calloc ||
                      !mmgmt_procs->realloc || !mmgmt_procs->free)) {
    if (err) {
      *err =
          CCOL_ERR_STR("Detected at least one NULL memory management function");
    }
    return false;
  }

  return true;
}

bool cbm_populate_mem_mgmt_procs(cbmap cbm, ccol_memmgmt_procs_t* mmgmt_procs,
                                 char** err) {
  if (mmgmt_procs) {
    cbm->m_procs = mmgmt_procs->malloc(sizeof(ccol_memmgmt_procs_t));
    if (!cbm->m_procs) {
      if (err) {
        *err = CCOL_ERR_STR("Failed to allocate buffer for memory mgmt buffer");
      }
      return false;
    }
    memcpy(cbm->m_procs, mmgmt_procs, sizeof(ccol_memmgmt_procs_t));
  } else {
    cbm->m_procs = NULL;
  }

  return true;
}

cbmap cbmap_create_mp(ccol_memmgmt_procs_t* mmgmt_procs, char** err) {
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

  if (!cbm_populate_mem_mgmt_procs(cbm, mmgmt_procs, err)) {
    _mem_free(mmgmt_procs, cbm);
    return NULL;
  }

  cbm->elem_count = 0;
  cbm->root = NULL;

  return cbm;
}

void destroy_bmap_node(cbmap cbm, bmap_node* node) {
  if (node) {
    _mem_free(cbm->m_procs, node->key_pair.ptr);
    _mem_free(cbm->m_procs, node->val_pair.ptr);
    _mem_free(cbm->m_procs, node);
  }
}

void point_node_parent_to_another_node(bmap_node* node, bmap_node* new_child) {
  if (!node->parent) {
    assert(false);
  }

  if (node->parent->left == node) {
    node->parent->left = new_child;
  } else if (node->parent->right == node) {
    node->parent->right = new_child;
  } else {
    assert(false);
  }
}

void _clear_nodes(cbmap cbm) {
  bmap_node* tracker = cbm->root;

  while (tracker) {
    if (tracker->left) {
      tracker = tracker->left;
    } else if (tracker->right) {
      tracker = tracker->right;
    } else if (tracker->parent) {
      bmap_node* parent = tracker->parent;
      point_node_parent_to_another_node(tracker, NULL);
      destroy_bmap_node(cbm, tracker);
      tracker = parent;
    } else {
      destroy_bmap_node(cbm, tracker);
      cbm->root = NULL;
      break;
    }
  }

  cbm->elem_count = 0;
}

void __cbmap_destroy(cbmap cbm) {
  if (cbm) {
    _clear_nodes(cbm);

    if (cbm->m_procs) {
      void (*free_func)(void*) = cbm->m_procs->free;
      free_func(cbm->m_procs);
      free_func(cbm);
    } else {
      mem_free(cbm);
    }
  }
}

uint32_t cbmap_elem_count(cbmap cbm) {
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

int compare_keys(const cmap_pair* key_pair1, const cmap_pair* key_pair2) {
  if (key_pair1->size == key_pair2->size) {
    return memcmp(key_pair1->ptr, key_pair2->ptr, key_pair1->size);
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

  new_node->parent = NULL;
  new_node->left = NULL;
  new_node->right = NULL;
  new_node->colour = red;
  new_node->height = 0;

  return new_node;
}

ccol_retval_t attach_new_node_to_parent(bmap_node** parent, bool left,
                                        cbmap cbm, const cmap_pair* key_pair,
                                        const cmap_pair* val_pair) {
  bmap_node* new_node = create_new_node(cbm, key_pair, val_pair);
  if (!new_node) {
    return ccol_not_enough_memory;
  }

  new_node->parent = *parent;
  if (*parent) {
    if (left) {
      (*parent)->left = new_node;
    } else {
      (*parent)->right = new_node;
    }
  } else {
    (*parent) = new_node;
    new_node->colour = black;
  }

  ++cbm->elem_count;

  return ccol_success;
}

ccol_retval_t cbmap_insert_elem(cbmap cbm, const cmap_pair* key_pair,
                                const cmap_pair* val_pair) {
  if (!cbm) {
    assert(false);
  }

  if (!cbm->root) {
    return attach_new_node_to_parent(&cbm->root, false, cbm, key_pair,
                                     val_pair);
  }

  bmap_node* tracker = cbm->root;
  while (tracker) {
    int comparison = compare_keys(key_pair, &tracker->key_pair);
    if (comparison == 0) {
      // Existing entry
      if (tracker->val_pair.size != val_pair->size) {
        void* new_ptr =
            _mem_realloc(cbm->m_procs, tracker->val_pair.ptr, val_pair->size);
        if (!new_ptr) {
          return ccol_not_enough_memory;
        }
        tracker->val_pair.ptr = new_ptr;
      }
      memcpy(tracker->val_pair.ptr, val_pair->ptr, val_pair->size);
      return ccol_success;
    } else if (comparison < 0) {
      // Should go left
      if (!tracker->left) {
        return attach_new_node_to_parent(&tracker, true, cbm, key_pair,
                                         val_pair);
      } else {
        tracker = tracker->left;
      }
    } else {
      // Should go right
      if (!tracker->right) {
        return attach_new_node_to_parent(&tracker, false, cbm, key_pair,
                                         val_pair);
      } else {
        tracker = tracker->right;
      }
    }
  }

  return ccol_unexpected_failure;
}

ccol_retval_t cbmap_get_elem_copy(cbmap cbm, const cmap_pair* key_pair,
                                  void* target_buf, uint32_t target_buf_size) {
  if (!cbm) {
    assert(false);
  }

  bmap_node* tracker = cbm->root;
  while (tracker) {
    int comparison = compare_keys(key_pair, &tracker->key_pair);
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
    int comparison = compare_keys(key_pair, &tracker->key_pair);
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

bmap_node* determine_replacer_of_node_to_be_deleted(bmap_node* tracker) {
  bmap_node* replacer = NULL;

  uint32_t left_depth;
  uint32_t right_depth;
  bmap_node* left_max = get_max_node(tracker->left, &left_depth);
  bmap_node* right_min = get_min_node(tracker->right, &right_depth);

  if ((left_max && right_min && left_depth >= right_depth) ||
      (left_max && !right_min)) {
    replacer = left_max;
  } else if ((left_max && right_min && left_depth < right_depth) ||
             (!left_max && right_min)) {
    replacer = right_min;
  }

  return replacer;
}

ccol_retval_t cbmap_delete_elem(cbmap cbm, const cmap_pair* key_pair) {
  if (!cbm) {
    assert(false);
  }

  bmap_node* tracker = cbm->root;
  while (tracker) {
    int comparison = compare_keys(key_pair, &tracker->key_pair);
    if (comparison == 0) {
      bmap_node* replacer = determine_replacer_of_node_to_be_deleted(tracker);
      if (replacer) {
        point_node_parent_to_another_node(
            replacer, replacer->right
                          ? replacer->right
                          : (replacer->left ? replacer->left : NULL));

        replacer->left = tracker->left;
        if (tracker->left) {
          tracker->left->parent = replacer;
        }
        replacer->right = tracker->right;
        if (tracker->right) {
          tracker->right->parent = replacer;
        }
        replacer->parent = tracker->parent;
      }

      if (tracker->parent) {
        point_node_parent_to_another_node(tracker, replacer);
      } else {
        cbm->root = replacer;
      }

      destroy_bmap_node(cbm, tracker);
      --cbm->elem_count;
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
