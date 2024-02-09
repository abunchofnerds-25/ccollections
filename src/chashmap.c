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

#include <chashmap.h>
#include <stdlib.h>
#include <string.h>

const size_t minimum_allowed_bucket_array_size = 64;
const size_t scale_factor = 4;
const size_t minimum_scale_down_threshold =
    scale_factor * (minimum_allowed_bucket_array_size - 1);

static inline void mem_assign(void* dest, void* src, size_t size) {
  if (size == sizeof(unsigned int)) {
    *(unsigned int*)dest = *(unsigned int*)src;
  } else if (size == sizeof(unsigned long)) {
    *(unsigned long*)dest = *(unsigned long*)src;
  } else if (size == sizeof(unsigned char)) {
    *(unsigned char*)dest = *(unsigned char*)src;
  } else if (size == sizeof(unsigned short)) {
    *(unsigned short*)dest = *(unsigned short*)src;
  } else {
    memcpy(dest, src, size);
  }
}

typedef struct chmap_entry {
  size_t hash_val;
  cmap_pair key_pair;
  cmap_pair val_pair;
  ccol_memmgmt_procs_t* m_procs;
} chmap_entry;

typedef struct dllist_ref_node {
  // All these pointers are mere references,
  // and they are not 'owned' by this struct
  struct dllist_ref_node* prev;
  struct dllist_ref_node* next;
} dllist_ref_node;

typedef struct chmap_cmap_iterator {  // Extended cmap_iterator for chmap
  // User doesn't need to know about the fields 'tracker' and 'parent_map' or
  // use them directly
  chmap parent_map;
  dllist_ref_node* tracker;
  cmap_iterator user_iter;
} chmap_cmap_iterator;

#define cmapIter2ChmapIter(u_iter)          \
  (chmap_cmap_iterator*)((uint8_t*)u_iter - \
                         offsetof(chmap_cmap_iterator, user_iter))

void attach_node_to_dllist(dllist_ref_node** head, dllist_ref_node* node) {
  node->prev = NULL;
  if (!(*head)) {
    node->next = NULL;
    *head = node;
  } else {
    node->next = *head;
    (*head)->prev = node;
    *head = node;
  }
}

void detach_node_from_dllist(dllist_ref_node** head, dllist_ref_node* node) {
  if (node->next) {
    node->next->prev = node->prev;
  }

  if (node->prev) {
    node->prev->next = node->next;
  }

  if (*head == node) {
    *head = node->next;
  }
}

typedef struct llist_node {
  struct llist_node* next;
  dllist_ref_node dllist_refs;
  chmap_entry data;
  ccol_memmgmt_procs_t* m_procs;
} llist_node;

#define dllistRefNodePtr2LlistNodePtr(tracker) \
  (llist_node*)((uint8_t*)tracker - offsetof(llist_node, dllist_refs))

void destroy_llist_node(dllist_ref_node** head_of_all_elems, llist_node* elem) {
  if (elem) {
    if (elem->data.key_pair.ptr) {
      _mem_free(elem->m_procs, elem->data.key_pair.ptr);
    }
    if (elem->data.val_pair.ptr) {
      _mem_free(elem->m_procs, elem->data.val_pair.ptr);
    }
    if (head_of_all_elems) {
      detach_node_from_dllist(head_of_all_elems, &elem->dllist_refs);
    }

    if (elem->m_procs) {
      ccol_memmgmt_procs_free_t free_func = elem->m_procs->free;
      // IMPORTANT NOTICE: The member field elem->m_procs is a mere reference
      // pointer, don't free it.
      free_func(elem);
    } else {
      mem_free(elem);
    }
  }
}

llist_node* create_llist_node(dllist_ref_node** head_of_all_elems,
                              chmap_entry* data) {
  llist_node* new_elem =
      (llist_node*)_mem_calloc(data->m_procs, 1, sizeof(llist_node));
  if (!new_elem) {
    return NULL;
  }
  new_elem->m_procs = data->m_procs;

  new_elem->data.key_pair.ptr = _mem_alloc(data->m_procs, data->key_pair.size);
  if (!new_elem->data.key_pair.ptr) {
    destroy_llist_node(head_of_all_elems, new_elem);
    return NULL;
  }

  new_elem->data.val_pair.ptr = _mem_alloc(data->m_procs, data->val_pair.size);
  if (!new_elem->data.val_pair.ptr) {
    destroy_llist_node(head_of_all_elems, new_elem);
    return NULL;
  }

  attach_node_to_dllist(head_of_all_elems, &new_elem->dllist_refs);
  new_elem->next = NULL;
  new_elem->data.hash_val = data->hash_val;
  new_elem->data.key_pair.size = data->key_pair.size;
  new_elem->data.val_pair.size = data->val_pair.size;
  mem_assign(new_elem->data.key_pair.ptr, data->key_pair.ptr,
             data->key_pair.size);
  mem_assign(new_elem->data.val_pair.ptr, data->val_pair.ptr,
             data->val_pair.size);

  return new_elem;
}

bool reset_val_of_llist_node(llist_node* elem, const cmap_pair* val_pair) {
  bool success = false;

  if (elem->data.val_pair.size == val_pair->size) {
    // The new value has the same size
    mem_assign(elem->data.val_pair.ptr, val_pair->ptr, val_pair->size);
    success = true;
  } else {
    // Sizes do not match, trying to reallocate.
    void* orig_buf = elem->data.val_pair.ptr;

    elem->data.val_pair.ptr =
        _mem_realloc(elem->m_procs, elem->data.val_pair.ptr, val_pair->size);
    if (!elem->data.val_pair.ptr) {
      // Failed to reallocate.
      elem->data.val_pair.ptr = orig_buf;
    } else {
      // Buffer reallocated, all is good.
      mem_assign(elem->data.val_pair.ptr, val_pair->ptr, val_pair->size);
      elem->data.val_pair.size = val_pair->size;
      success = true;
    }
  }

  return success;
}

llist_node* insert_into_llist(llist_node* head,
                              dllist_ref_node** head_of_all_elems,
                              chmap_entry* data, bool* success) {
  llist_node* new_elem = create_llist_node(head_of_all_elems, data);
  if (!new_elem) {
    *success = false;
    return head;
  }

  *success = true;

  new_elem->next = head;
  head = new_elem;

  return head;
}

llist_node* migrate_llist_node_to_another_llist(llist_node* head,
                                                llist_node* prev_node,
                                                llist_node* node) {
  if (prev_node) {
    prev_node->next = node->next;
  }

  node->next = head;
  head = node;

  return head;
}

static inline bool compare_key_pairs(const cmap_pair* kp1,
                                     const cmap_pair* kp2) {
  if (kp1->size != kp2->size) {
    return false;
  }

  if (kp1->size == sizeof(int) &&
      *(unsigned int*)kp1->ptr == *(unsigned int*)kp2->ptr) {
    return true;
  } else if (kp1->size == sizeof(long) &&
             *(unsigned long*)kp1->ptr == *(unsigned long*)kp2->ptr) {
    return true;
  } else if (kp1->size == sizeof(char) &&
             *(unsigned char*)kp1->ptr == *(unsigned char*)kp2->ptr) {
    return true;
  } else if (kp1->size == sizeof(short) &&
             *(unsigned short*)kp1->ptr == *(unsigned short*)kp2->ptr) {
    return true;
  } else if (memcmp(kp1->ptr, kp2->ptr, kp1->size) == 0) {
    return true;
  }

  return false;
}

llist_node* find_in_llist(llist_node* head, const cmap_pair* key_pair) {
  llist_node* tracker = head;
  while (tracker) {
    if (compare_key_pairs(&tracker->data.key_pair, key_pair)) {
      return tracker;
    }
    tracker = tracker->next;
  }

  return tracker;
}

llist_node* destroy_the_whole_llist(llist_node* head,
                                    dllist_ref_node** head_of_all_elems) {
  llist_node* tracker = head;

  while (tracker) {
    llist_node* node_to_be_deleted = tracker;
    tracker = tracker->next;
    destroy_llist_node(head_of_all_elems, node_to_be_deleted);
  }

  return tracker;
}

llist_node* delete_from_llist(llist_node* head,
                              dllist_ref_node** head_of_all_elems,
                              const cmap_pair* key_pair, bool* found) {
  *found = false;
  llist_node* tracker = head;
  llist_node* previous = NULL;

  while (tracker) {
    if (tracker->data.key_pair.size == key_pair->size) {
      if (compare_key_pairs(&tracker->data.key_pair, key_pair)) {
        // This is the node to be deleted
        *found = true;
        if (!previous) {
          head = tracker->next;
        } else {
          previous->next = tracker->next;
        }
        destroy_llist_node(head_of_all_elems, tracker);

        return head;
      }
    }

    previous = tracker;
    tracker = tracker->next;
  }

  return head;
}

struct chashmap {
  size_t elem_count;
  size_t bucket_arr_size;
  size_t elem_count_to_scale_up;
  size_t elem_count_to_scale_down;
  llist_node** bucket_arr;
  dllist_ref_node* head_of_all_elems;
  ccol_memmgmt_procs_t* m_procs;
  ccol_hashing_proc_t custom_hashing_proc;
};

void set_chmap_scaling_limits(chmap chm) {
  chm->elem_count_to_scale_up = (chm->bucket_arr_size + 1) * 6 / 4;
  chm->elem_count_to_scale_down = (chm->bucket_arr_size + 1) / 8;
}

#define POWERS_OF_TWO_LEN 64
size_t uint64_powers_of_two[POWERS_OF_TWO_LEN] = {
    1,
    2,
    4,
    8,
    16,
    32,
    64,
    128,
    256,
    512,
    1024,
    2048,
    4096,
    8192,
    16384,
    32768,
    65536,
    131072,
    262144,
    524288,
    1048576,
    2097152,
    4194304,
    8388608,
    16777216,
    33554432,
    67108864,
    134217728,
    268435456,
    536870912,
    1073741824,
    2147483648,
    4294967296,
    8589934592,
    17179869184,
    34359738368,
    68719476736,
    137438953472,
    274877906944,
    549755813888,
    1099511627776,
    2199023255552,
    4398046511104,
    8796093022208,
    17592186044416,
    35184372088832,
    70368744177664,
    140737488355328,
    281474976710656,
    562949953421312,
    1125899906842624,
    2251799813685248,
    4503599627370496,
    9007199254740992,
    18014398509481984,
    36028797018963968,
    72057594037927936,
    144115188075855872,
    288230376151711744,
    576460752303423488,
    1152921504606846976,
    2305843009213693952,
    4611686018427387904,
    9223372036854775808UL};  // 18446744073709551616 exceeds 64 bits

// Kind of a "pow(2, ceil(log2(input)))" without the math library.
size_t find_nearest_gte_power_of_two(size_t input) {
  int left = 0;
  int right = POWERS_OF_TWO_LEN - 1;
  int middle = (left + right) / 2;

  if (input <= uint64_powers_of_two[0]) {
    return uint64_powers_of_two[0];
  }

  if (input >= uint64_powers_of_two[POWERS_OF_TWO_LEN - 1]) {
    return uint64_powers_of_two[POWERS_OF_TWO_LEN - 1];
  }

  size_t result = 0;

  while (left < right) {
    if (uint64_powers_of_two[middle] == input ||
        (middle > 0 && uint64_powers_of_two[middle - 1] < input &&
         input < uint64_powers_of_two[middle])) {
      result = uint64_powers_of_two[middle];
      break;
    } else if (middle < (POWERS_OF_TWO_LEN - 1) &&
               uint64_powers_of_two[middle] < input &&
               input <= uint64_powers_of_two[middle + 1]) {
      result = uint64_powers_of_two[middle + 1];
      break;
    }

    if (input < uint64_powers_of_two[middle]) {
      right = middle;
    } else {
      left = middle;
    }

    middle = (left + right) / 2;
  }

  return result;
}

uint64_t random_uint64(void) {
  uint64_t result = 0;

  // random() returns 31 bits of randomness
  result =
      ((uint64_t)random() << 33) | ((uint64_t)random() << 2) | (random() & 0x3);

  return result;
}

bool verify_chmap_create_inputs(size_t initial_bucket_array_size,
                                ccol_memmgmt_procs_t* mmgmt_procs, char** err) {
  if (initial_bucket_array_size == 0) {
    if (err) {
      *err = CCOL_ERR_STR("The initial bucket size is zero");
    }
    return false;
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return false;
  }

  return true;
}

chmap chmap_create_full(size_t initial_bucket_array_size,
                        ccol_memmgmt_procs_t* mmgmt_procs,
                        ccol_hashing_proc_t custom_hashing_proc, char** err) {
  if (!verify_chmap_create_inputs(initial_bucket_array_size, mmgmt_procs,
                                  err)) {
    return NULL;
  }

  if (initial_bucket_array_size <= minimum_allowed_bucket_array_size) {
    initial_bucket_array_size = minimum_allowed_bucket_array_size - 1;
  } else {
    initial_bucket_array_size =
        find_nearest_gte_power_of_two(initial_bucket_array_size) - 1;
  }

  chmap chm = (chmap)_mem_alloc(mmgmt_procs, sizeof(chashmap));
  if (!chm) {
    if (err) {
      *err = CCOL_ERR_STR("Failed to allocate buffer");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(chm, mmgmt_procs, err)) {
    _mem_free(mmgmt_procs, chm);
    return NULL;
  }

  chm->bucket_arr_size = initial_bucket_array_size;
  chm->elem_count = 0;
  chm->head_of_all_elems = NULL;
  set_chmap_scaling_limits(chm);
  chm->custom_hashing_proc = custom_hashing_proc;

  chm->bucket_arr = (llist_node**)_mem_calloc(
      mmgmt_procs, initial_bucket_array_size, sizeof(llist_node*));
  if (!chm->bucket_arr) {
    // Failed to allocate buffer for bucket_arr
    if (err) {
      *err = CCOL_ERR_STR("Failed to allocate bucket_arr");
    }
    _mem_free(mmgmt_procs, chm->m_procs);
    _mem_free(mmgmt_procs, chm);
    return NULL;
  }

  if (err) {
    *err = NULL;
  }

  return chm;
}

static inline size_t calculate_bucket_index(chmap chm,
                                            const cmap_pair* key_pair,
                                            size_t* hash_ptr) {
  size_t id = 0;

  if (!chm->custom_hashing_proc) {
    unsigned char* c_key_ptr = (unsigned char*)key_pair->ptr;
    size_t size = key_pair->size;

    // DJB2
    id = 5381;
    for (size_t i = 0; i < size; ++i) {
      id = ((id << 5) + id) + c_key_ptr[i];
    }
  } else {
    id = chm->custom_hashing_proc(key_pair->ptr);
  }

  if (hash_ptr) {
    *hash_ptr = id;
  }

  size_t index = (id % chm->bucket_arr_size);

  return index;
}

#ifdef RUNNING_UNIT_TESTS
size_t chmap_get_bucket_arr_size(chmap chm) {
  if (!chm) {
    return 0;
  }

  return chm->bucket_arr_size;
}

size_t chmap_get_elem_count_to_scale_up(chmap chm) {
  if (!chm) {
    return 0;
  }

  return chm->elem_count_to_scale_up;
}

size_t chmap_get_elem_count_to_scale_down(chmap chm) {
  if (!chm) {
    return 0;
  }

  return chm->elem_count_to_scale_down;
}
#endif

void scale_chmap(chmap chm, bool up) {
  size_t new_bucket_array_size = 0;
  if (up) {
    new_bucket_array_size = (chm->bucket_arr_size + 1) * scale_factor - 1;
  } else {
    new_bucket_array_size = (chm->bucket_arr_size + 1) / scale_factor - 1;
  }

  llist_node** new_bucket_arr;
  new_bucket_arr = (llist_node**)_mem_calloc(
      chm->m_procs, new_bucket_array_size, sizeof(llist_node*));
  if (!new_bucket_arr) {
    // We don't have enough memory to scale, return.
    return;
  }

  // We have enough memory.
  for (size_t i = 0; i < chm->bucket_arr_size; ++i) {
    llist_node* tracker = chm->bucket_arr[i];
    while (tracker) {
      llist_node* next = tracker->next;
      size_t new_index = tracker->data.hash_val % new_bucket_array_size;
      new_bucket_arr[new_index] = migrate_llist_node_to_another_llist(
          new_bucket_arr[new_index], NULL, tracker);
      tracker = next;
    }
    chm->bucket_arr[i] = NULL;
  }

  _mem_free(chm->m_procs, chm->bucket_arr);

  chm->bucket_arr = new_bucket_arr;
  chm->bucket_arr_size = new_bucket_array_size;
  set_chmap_scaling_limits(chm);
}

ccol_retval_t chmap_insert_elem(chmap chm, const cmap_pair* key_pair,
                                const cmap_pair* val_pair) {
  if (!chm || !key_pair || !val_pair || !key_pair->ptr || !val_pair->ptr ||
      key_pair->size == 0 || val_pair->size == 0) {
    return ccol_invalid_args;
  }

  if (chm->elem_count == max_elem_count) {
    return ccol_container_full;
  }

  chmap_entry data = {
      .hash_val = 0,
      .key_pair = {.ptr = key_pair->ptr, .size = key_pair->size},
      .val_pair = {.ptr = val_pair->ptr, .size = val_pair->size},
      .m_procs = chm->m_procs};

  bool result = false;

  size_t index = calculate_bucket_index(chm, key_pair, &data.hash_val);

  llist_node* r = find_in_llist(chm->bucket_arr[index], key_pair);
  if (r) {
    // The entry already exists
    result = reset_val_of_llist_node(r, val_pair);
  } else {
    chm->bucket_arr[index] = insert_into_llist(
        chm->bucket_arr[index], &chm->head_of_all_elems, &data, &result);
    if (result) {
      if (++chm->elem_count >= chm->elem_count_to_scale_up) {
        // Time to scale up!
        scale_chmap(chm, true);
      }
    }
  }

  return result ? ccol_success : ccol_not_enough_memory;
}

ccol_retval_t chmap_get_elem_copy(chmap chm, const cmap_pair* key_pair,
                                  void* target_buf, size_t target_buf_size) {
  if (!chm || !key_pair || !key_pair->ptr || key_pair->size == 0 ||
      !target_buf || target_buf_size == 0) {
    return ccol_invalid_args;
  }

  ccol_retval_t result = ccol_key_not_found;

  size_t index = calculate_bucket_index(chm, key_pair, NULL);

  llist_node* r = find_in_llist(chm->bucket_arr[index], key_pair);
  if (r) {
    size_t min_size = target_buf_size;
    if (r->data.val_pair.size < min_size) {
      min_size = r->data.val_pair.size;
    }
    mem_assign(target_buf, r->data.val_pair.ptr, min_size);
    result = ccol_success;
  }

  return result;
}

ccol_retval_t chmap_get_elem_ref(chmap chm, const cmap_pair* key_pair,
                                 cmap_pair** val_pair) {
  if (!chm || !key_pair || !key_pair->ptr || key_pair->size == 0 || !val_pair) {
    return ccol_invalid_args;
  }

  ccol_retval_t result = ccol_key_not_found;

  size_t index = calculate_bucket_index(chm, key_pair, NULL);

  llist_node* r = find_in_llist(chm->bucket_arr[index], key_pair);
  if (r) {
    *val_pair = &r->data.val_pair;
    result = ccol_success;
  }

  return result;
}

ccol_retval_t chmap_delete_elem(chmap chm, const cmap_pair* key_pair) {
  if (!chm || !key_pair || !key_pair->ptr || key_pair->size == 0) {
    return ccol_invalid_args;
  }

  bool found = false;

  size_t index = calculate_bucket_index(chm, key_pair, NULL);

  chm->bucket_arr[index] = delete_from_llist(
      chm->bucket_arr[index], &chm->head_of_all_elems, key_pair, &found);

  if (found) {
    if (--chm->elem_count < chm->elem_count_to_scale_down &&
        chm->bucket_arr_size >= minimum_scale_down_threshold) {
      // Time to scale down!
      scale_chmap(chm, false);
    }
    return ccol_success;
  }

  return ccol_key_not_found;
}

size_t chmap_elem_count(chmap chm) {
  if (!chm) {
    assert(false);
  }

  return chm->elem_count;
}

cmap_iterator* chashmap_begin_iter(chmap chm, char** err) {
  if (err) {
    *err = NULL;
  }

  if (!chm) {
    assert(false);
  }

  if (!chm->head_of_all_elems) {
    return NULL;
  }

  chmap_cmap_iterator* real_iter =
      _mem_calloc(chm->m_procs, 1, sizeof(chmap_cmap_iterator));
  if (!real_iter) {
    if (err) {
      *err = CCOL_ERR_STR("Failed to allocate iterator");
    }
    return NULL;
  }

  dllist_ref_node* tracker = chm->head_of_all_elems;
  real_iter->parent_map = chm;
  real_iter->tracker = tracker;
  llist_node* host = dllistRefNodePtr2LlistNodePtr(tracker);
  real_iter->user_iter.key_pair = &(host->data.key_pair);
  real_iter->user_iter.val_pair = &(host->data.val_pair);

  return &(real_iter->user_iter);
}

void __chmap_iterator_destroy(cmap_iterator* iter) {
  if (iter) {
    chmap_cmap_iterator* real_iter = cmapIter2ChmapIter(iter);
    _mem_free(real_iter->parent_map->m_procs, real_iter);
  }
}

cmap_iterator* chmap_iter_next(cmap_iterator* iter) {
  chmap_cmap_iterator* real_iter = cmapIter2ChmapIter(iter);
  real_iter->tracker = real_iter->tracker->next;

  if (real_iter->tracker) {
    // iter already points to the correct location, no need to
    // use real_iter for the following two lines.
    dllist_ref_node* tracker = real_iter->tracker;
    llist_node* host = dllistRefNodePtr2LlistNodePtr(tracker);
    iter->key_pair = &(host->data.key_pair);
    iter->val_pair = &(host->data.val_pair);
  } else {
    // The iterator reached to the end of the map, nowhere to
    // advance. Let's destroy it and return NULL.
    __chmap_iterator_destroy(iter);
    iter = NULL;
  }

  return iter;
}

ccol_retval_t chmap_reset(chmap chm, size_t new_bucket_array_size) {
  if (!chm) {
    assert(false);
  }

  int result = ccol_success;

  if (new_bucket_array_size > 0 &&
      new_bucket_array_size < minimum_allowed_bucket_array_size) {
    new_bucket_array_size = minimum_allowed_bucket_array_size - 1;
  } else {
    new_bucket_array_size =
        find_nearest_gte_power_of_two(new_bucket_array_size) - 1;
  }

  for (size_t i = 0; i < chm->bucket_arr_size; ++i) {
    destroy_the_whole_llist(chm->bucket_arr[i], &chm->head_of_all_elems);
  }

  if (new_bucket_array_size > 0 &&
      new_bucket_array_size != chm->bucket_arr_size) {
    llist_node** orig = chm->bucket_arr;

    chm->bucket_arr = _mem_realloc(chm->m_procs, chm->bucket_arr,
                                   new_bucket_array_size * sizeof(llist_node*));
    if (!chm->bucket_arr) {
      chm->bucket_arr = orig;
      result = ccol_not_enough_memory;
    } else {
      chm->bucket_arr_size = new_bucket_array_size;
    }
  }

  memset(chm->bucket_arr, 0, chm->bucket_arr_size * sizeof(llist_node*));
  chm->elem_count = 0;

  return result;
}

void __chmap_destroy(chmap chm) {
  if (chm) {
    for (size_t i = 0; i < chm->bucket_arr_size; ++i) {
      destroy_the_whole_llist(chm->bucket_arr[i], &chm->head_of_all_elems);
    }
    _mem_free(chm->m_procs, (void*)chm->bucket_arr);

    if (chm->m_procs) {
      ccol_memmgmt_procs_free_t free_func = chm->m_procs->free;
      free_func(chm->m_procs);
      free_func(chm);
    } else {
      mem_free(chm);
    }
  }
}