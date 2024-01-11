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
#include <cbstmap.h>
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

typedef struct chmap_entry chmap_entry;

typedef struct dllist_ref_node {
  // All these pointers are mere references,
  // and they are not 'owned' by this struct
  struct dllist_ref_node* prev;
  struct dllist_ref_node* next;
} dllist_ref_node;

typedef struct chmap_entry {
  size_t hash_val;
  cmap_pair key_pair;
  cmap_pair val_pair;
  dllist_ref_node dllist_refs;
  ccol_memmgmt_procs_t* m_procs;
} chmap_entry;

#define dllistRefNode2ChmapEntry(tracker) \
  (chmap_entry*)(((uint8_t*)tracker) - offsetof(chmap_entry, dllist_refs))

typedef struct chmap_cmap_iterator {
  // Extended cmap_iterator for chmap
  // User doesn't need to know about the fields
  // 'tracker' and 'parent_map' or use them directly
  chmap parent_map;
  dllist_ref_node* tracker;
  cmap_iterator user_iter;
} chmap_cmap_iterator;

#define cmapIter2ChmapIter(u_iter)          \
  (chmap_cmap_iterator*)((uint8_t*)u_iter - \
                         offsetof(chmap_cmap_iterator, user_iter))

void attach_node_to_dllist(dllist_ref_node** head, chmap_entry* host) {
  host->dllist_refs.prev = NULL;
  if (!(*head)) {
    host->dllist_refs.next = NULL;
    *head = &host->dllist_refs;
  } else {
    host->dllist_refs.next = *head;
    (*head)->prev = &host->dllist_refs;
    *head = &host->dllist_refs;
  }
}

void detach_node_from_dllist(dllist_ref_node** head, chmap_entry* host) {
  if (host->dllist_refs.next) {
    host->dllist_refs.next->prev = host->dllist_refs.prev;
  }

  if (host->dllist_refs.prev) {
    host->dllist_refs.prev->next = host->dllist_refs.next;
  }

  if (*head == &host->dllist_refs) {
    *head = host->dllist_refs.next;
  }
}

void destroy_chmap_entry(dllist_ref_node** head_of_all_elems,
                         chmap_entry* elem) {
  if (elem) {
    if (elem->key_pair.ptr) {
      _mem_free(elem->m_procs, elem->key_pair.ptr);
    }

    if (elem->val_pair.ptr) {
      _mem_free(elem->m_procs, elem->val_pair.ptr);
    }

    if (head_of_all_elems) {
      detach_node_from_dllist(head_of_all_elems, elem);
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

typedef struct reduced_chmap_entry {
  chmap chm;
  const cmap_pair* key_pair;
  const cmap_pair* val_pair;
  size_t hash_val;
} reduced_chmap_entry;

struct chashmap {
  size_t elem_count;
  size_t bucket_arr_size;
  size_t elem_count_to_scale_up;
  size_t elem_count_to_scale_down;
  cbmap* bucket_arr;
  dllist_ref_node* head_of_all_elems;
  ccol_memmgmt_procs_t* m_procs;
  ccol_hashing_proc_t custom_hashing_proc;
};

chmap_entry* create_chmap_entry(reduced_chmap_entry* inp) {
  chmap_entry* elem =
      (chmap_entry*)_mem_calloc(inp->chm->m_procs, 1, sizeof(chmap_entry));
  if (!elem) {
    return NULL;
  }

  elem->key_pair.ptr = _mem_alloc(inp->chm->m_procs, inp->key_pair->size);
  if (!elem->key_pair.ptr) {
    destroy_chmap_entry(&inp->chm->head_of_all_elems, elem);
    return NULL;
  }

  elem->val_pair.ptr = _mem_alloc(inp->chm->m_procs, inp->val_pair->size);
  if (!elem->val_pair.ptr) {
    destroy_chmap_entry(&inp->chm->head_of_all_elems, elem);
    return NULL;
  }

  elem->key_pair.size = inp->key_pair->size;
  memcpy(elem->key_pair.ptr, inp->key_pair->ptr, inp->key_pair->size);
  elem->val_pair.size = inp->val_pair->size;
  memcpy(elem->val_pair.ptr, inp->val_pair->ptr, inp->val_pair->size);
  elem->hash_val = inp->hash_val;
  attach_node_to_dllist(&inp->chm->head_of_all_elems, elem);

  return elem;
}

bool reset_val_of_chmap_entry(chmap_entry* elem, const cmap_pair* val_pair) {
  if (elem->val_pair.size == val_pair->size) {
    // The new value has the same size
    mem_assign(elem->val_pair.ptr, val_pair->ptr, val_pair->size);
    return true;
  }

  // Sizes do not match, trying to reallocate.
  void* orig_buf = elem->val_pair.ptr;
  elem->val_pair.ptr =
      _mem_realloc(elem->m_procs, elem->val_pair.ptr, val_pair->size);
  if (!elem->val_pair.ptr) {
    // Failed to reallocate.
    elem->val_pair.ptr = orig_buf;
    return false;
  }

  // Buffer reallocated, all is good.
  mem_assign(elem->val_pair.ptr, val_pair->ptr, val_pair->size);
  elem->val_pair.size = val_pair->size;
  return true;
}

bool insert_elem_into_bucket(cbmap* bucket, reduced_chmap_entry* inp) {
  if (!*bucket) {
    *bucket = cbmap_create_mp(false, inp->chm->m_procs, NULL);
    if (!bucket) {
      return false;
    }
  }

  chmap_entry* elem = create_chmap_entry(inp);
  if (!elem) {
    if (cbmap_elem_count(*bucket) == 0) {
      cbmap_destroy(*bucket);
    }
    return false;
  }

  cmap_pair v_pair = {.ptr = &elem, .size = sizeof(elem)};
  ccol_retval_t r = cbmap_insert_elem(*bucket, inp->key_pair, &v_pair);
  if (r != ccol_success) {
    destroy_chmap_entry(&inp->chm->head_of_all_elems, elem);
    if (cbmap_elem_count(*bucket) == 0) {
      cbmap_destroy(*bucket);
    }
    return false;
  }

  return true;
}

chmap_entry* find_elem_in_bucket(cbmap bucket, const cmap_pair* key_pair) {
  if (!bucket) {
    return NULL;
  }

  cmap_pair* v_pair;

  ccol_retval_t r = cbmap_get_elem_ref(bucket, key_pair, &v_pair);
  if (r == ccol_success) {
    return *(chmap_entry**)v_pair->ptr;
  }

  return NULL;
}

bool delete_elem_from_bucket(cbmap* bucket, const cmap_pair* key_pair,
                             dllist_ref_node** head_of_all_elems) {
  if (!*bucket) {
    return false;
  }

  cmap_pair* v_pair;

  ccol_retval_t r = cbmap_get_elem_ref(*bucket, key_pair, &v_pair);
  if (r == ccol_success) {
    chmap_entry* elem = *(chmap_entry**)v_pair->ptr;
    cbmap_delete_elem(*bucket, key_pair);
    destroy_chmap_entry(head_of_all_elems, elem);

    if (cbmap_elem_count(*bucket) == 0) {
      cbmap_destroy(*bucket);
    }

    return true;
  }

  return false;
}

bool copy_chmap_entry_into_bucket(chmap_entry* elem, cbmap* dst) {
  if (!*dst) {
    *dst = cbmap_create_mp(false, elem->m_procs, NULL);
    if (!*dst) {
      return false;
    }
  }

  cmap_pair val_pair = {.ptr = &elem, .size = sizeof(elem)};
  return (ccol_success == cbmap_insert_elem(*dst, &elem->key_pair, &val_pair));
}

void destroy_the_whole_bucket(cbmap* bucket,
                              dllist_ref_node** head_of_all_elems) {
  if (!*bucket) {
    return;
  }

  for (cmap_iterator* iter = cbmap_begin_iter(*bucket, NULL); iter;
       iter = cbmap_iter_next(iter)) {
    destroy_chmap_entry(head_of_all_elems, *(chmap_entry**)iter->val_pair->ptr);
  }

  cbmap_destroy(*bucket);
}

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

  chm->bucket_arr = (cbmap*)_mem_calloc(mmgmt_procs, initial_bucket_array_size,
                                        sizeof(cbmap));
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

void dump_cmap_pair(const cmap_pair* pair) {
  for (size_t i = 0; i < pair->size; ++i) {
    printf("%02x ", ((char*)pair->ptr)[i]);
  }
  printf("\n");
}

void scale_chmap(chmap chm, bool up) {
  size_t new_arr_size = (chm->bucket_arr_size + 1) * scale_factor - 1;
  if (!up) {
    new_arr_size = (chm->bucket_arr_size + 1) / scale_factor - 1;
  }

  cbmap* new_bucket_arr;
  new_bucket_arr =
      (cbmap**)_mem_calloc(chm->m_procs, new_arr_size, sizeof(cbmap));
  if (!new_bucket_arr) {
    // We don't have enough memory to scale, return.
    return;
  }

  bool all_went_well = true;

  // We have enough memory, for now.
  for (size_t i = 0; i < chm->bucket_arr_size; ++i) {
    if (chm->bucket_arr[i]) {
      cmap_iterator* iter = cbmap_begin_iter(chm->bucket_arr[i], NULL);
      for (; iter; iter = cbmap_iter_next(iter)) {
        chmap_entry* elem = *(chmap_entry**)iter->val_pair->ptr;
        size_t new_index = elem->hash_val % new_arr_size;
        if (!copy_chmap_entry_into_bucket(elem, &new_bucket_arr[new_index])) {
          // We faced a failure, set all_went_well
          // to false to go for a rollback below.
          all_went_well = false;
          break;
        }
      }
    }
    if (!all_went_well) {
      break;
    }
  }

  if (!all_went_well) {
    for (size_t i = 0; i < new_arr_size; ++i) {
      cbmap_destroy(new_bucket_arr[i]);
    }
    _mem_free(chm->m_procs, new_bucket_arr);
    return;
  }

  for (size_t i = 0; i < chm->bucket_arr_size; ++i) {
    cbmap_destroy(chm->bucket_arr[i]);
  }
  _mem_free(chm->m_procs, chm->bucket_arr);

  chm->bucket_arr = new_bucket_arr;
  chm->bucket_arr_size = new_arr_size;
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

  bool result = false;

  size_t hash_val = 0;
  size_t index = calculate_bucket_index(chm, key_pair, &hash_val);
  chmap_entry* r = find_elem_in_bucket(chm->bucket_arr[index], key_pair);
  if (r) {
    // The entry already exists
    result = reset_val_of_chmap_entry(r, val_pair);
  } else {
    // This key does not exist,
    // we need to insert a new entry
    reduced_chmap_entry inp = {.chm = chm,
                               .hash_val = hash_val,
                               .key_pair = key_pair,
                               .val_pair = val_pair};
    result = insert_elem_into_bucket(&chm->bucket_arr[index], &inp);
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

  chmap_entry* r = find_elem_in_bucket(chm->bucket_arr[index], key_pair);
  if (r) {
    size_t min_size = target_buf_size;
    if (r->val_pair.size < min_size) {
      min_size = r->val_pair.size;
    }
    mem_assign(target_buf, r->val_pair.ptr, min_size);
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

  chmap_entry* r = find_elem_in_bucket(chm->bucket_arr[index], key_pair);
  if (r) {
    *val_pair = &r->val_pair;
    result = ccol_success;
  }

  return result;
}

ccol_retval_t chmap_delete_elem(chmap chm, const cmap_pair* key_pair) {
  if (!chm || !key_pair || !key_pair->ptr || key_pair->size == 0) {
    return ccol_invalid_args;
  }

  size_t index = calculate_bucket_index(chm, key_pair, NULL);

  bool found = delete_elem_from_bucket(&chm->bucket_arr[index], key_pair,
                                       &chm->head_of_all_elems);
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
  chmap_entry* host = dllistRefNode2ChmapEntry(tracker);
  real_iter->user_iter.key_pair = &host->key_pair;
  real_iter->user_iter.val_pair = &host->val_pair;

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
    chmap_entry* host = dllistRefNode2ChmapEntry(real_iter->tracker);

    iter->key_pair = &host->key_pair;
    iter->val_pair = &host->val_pair;
  } else {
    // The iterator reached to the end of the map, nowhere to
    // advance. Let's destroy it and return NULL.
    __chmap_iterator_destroy(iter);
    iter = NULL;
  }

  return iter;
}

ccol_retval_t chmap_reset(chmap chm, size_t new_arr_size) {
  if (!chm) {
    assert(false);
  }

  int result = ccol_success;

  if (new_arr_size > 0 && new_arr_size < minimum_allowed_bucket_array_size) {
    new_arr_size = minimum_allowed_bucket_array_size - 1;
  } else {
    new_arr_size = find_nearest_gte_power_of_two(new_arr_size) - 1;
  }

  for (size_t i = 0; i < chm->bucket_arr_size; ++i) {
    destroy_the_whole_bucket(&chm->bucket_arr[i], &chm->head_of_all_elems);
  }

  if (new_arr_size > 0 && new_arr_size != chm->bucket_arr_size) {
    cbmap* orig = chm->bucket_arr;

    chm->bucket_arr = _mem_realloc(chm->m_procs, chm->bucket_arr,
                                   new_arr_size * sizeof(cbmap));
    if (!chm->bucket_arr) {
      chm->bucket_arr = orig;
      result = ccol_not_enough_memory;
    } else {
      chm->bucket_arr_size = new_arr_size;
    }
  }

  memset(chm->bucket_arr, 0, chm->bucket_arr_size * sizeof(cbmap));
  chm->elem_count = 0;

  return result;
}

void __chmap_destroy(chmap chm) {
  if (chm) {
    for (size_t i = 0; i < chm->bucket_arr_size; ++i) {
      destroy_the_whole_bucket(&chm->bucket_arr[i], &chm->head_of_all_elems);
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
