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

#pragma once

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <common.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DEFAULT_INITIAL_BUCKET_ARRAY_SIZE 64

#define fatal_err(err_fmt, ...)                     \
  do {                                              \
    char err_str[512] = {0};                        \
    snprintf(err_str, 512, err_fmt, ##__VA_ARGS__); \
    fprintf(stderr, "%s\n", err_str);               \
    assert(false);                                  \
  } while (0)

typedef struct chashmap chashmap;
typedef chashmap* chmap;

typedef struct chmap_pair {
  void* ptr;
  uint32_t size;
} chmap_pair;

typedef struct chmap_iterator {
  // User doesn't need to tamper with the fields '_handle' and 'parent_map' or
  // use them directly
  chmap parent_map;
  void* _handle;
  chmap_pair* key_pair;
  chmap_pair* val_pair;
} chmap_iterator;

// The function 'chmap_create' creates a new hash map instance and returns
// the pointer to it. This pointer should be passed to the macro
// 'chmap_destroy', once the hash map is no longer needed. The input parameter
// 'bucket_array_size' determines the initial bucket size of the hash map.
chmap chmap_create_mp(uint32_t initial_bucket_array_size,
                      ccol_memmgmt_procs_t* mmgmt_procs, char** err);

#define chmap_create(initial_bucket_array_size, err) \
  chmap_create_mp(initial_bucket_array_size, NULL, err)

// The function 'chmap_elem_count' can be used to get the number of elements
// in it. If chm is NULL, this function will return 0;
uint32_t chmap_elem_count(chmap chm);

// The function 'chmap_reset' can be used to clear a map by deleting the
// existing elements from it. If the new_bucket_array_size is 0, the
// bucket array size will not change, otherwise, the bucket array will
// get reallocated to match the specified size. If that reallocation fails,
// this function will return -1. If the chm pointer is not NULL, it will
// always destroy the existing elements regardless of its return value,
// the return value conveys information regarding the realloc attempt.
ccol_retval_t chmap_reset(chmap chm, uint32_t new_bucket_array_size);

// The function chmap_insert_elem 'upserts' the val_pair associated with
// the key_pair into the hash map chm. If no element exists in the map
// associated with the provided key_pair, this function will return -1,
// otherwise it will return 0.
ccol_retval_t chmap_insert_elem(chmap chm, const chmap_pair* key_pair,
                                const chmap_pair* val_pair);

// The function chmap_get_elem_copy populates a copy of the data stored in the
// val_pair from the hash map into the target_buf. The pointer target_buf SHOULD
// BE non-null, otherwise the function will fail.
ccol_retval_t chmap_get_elem_copy(chmap chm, const chmap_pair* key_pair,
                                  void* target_buf, uint32_t target_buf_size);

// This function gets a pointer to the element's value pair and stores it into
// the pointer-to-pointer val_pair. Here, naturally, val_pair gets overwritten.
ccol_retval_t chmap_get_elem_ref(chmap chm, const chmap_pair* key_pair,
                                 chmap_pair** val_pair);

// The function 'chmap_delete_elem' can be used to delete an element from the
// hash map.
ccol_retval_t chmap_delete_elem(chmap chm, const chmap_pair* key_pair);

// The function 'chashmap_begin_iter' can be used to acquire an iterator to
// traverse through a hashed map.
chmap_iterator* chashmap_begin_iter(chmap chm, char** err);

#define chmap_iter_declare(chm, iter)                             \
  typeof(__chm_key_type_var_##chm) __chm_iter_key_type_var_##iter \
      __attribute__((unused));                                    \
  typeof(__chm_val_type_var_##chm) __chm_iter_val_type_var_##iter \
      __attribute__((unused));                                    \
  chmap_iterator* iter = NULL

#define chmap_begin(chm)                                   \
  ({                                                       \
    char* err;                                             \
    chmap_iterator* iter = chashmap_begin_iter(chm, &err); \
    if (err != NULL) {                                     \
      assert(false);                                       \
    }                                                      \
    iter;                                                  \
  })

chmap_iterator* chmap_iter_next(chmap_iterator* iter);

#define chmap_iter_key_ptr(iter)                                    \
  ({                                                                \
    bool is_key_string = _Generic((__chm_iter_key_type_var_##iter), \
        char*: true,                                                \
        const char*: true,                                          \
        unsigned char*: true,                                       \
        const unsigned char*: true,                                 \
        default: false);                                            \
    const typeof(__chm_iter_key_type_var_##iter)* key;              \
    if (is_key_string) {                                            \
      key = (typeof(key))(&it->key_pair->ptr);                      \
    } else {                                                        \
      key = (typeof(key))(it->key_pair->ptr);                       \
    }                                                               \
    key;                                                            \
  })

#define chmap_iter_val_ptr(iter)                                    \
  ({                                                                \
    bool is_val_string = _Generic((__chm_iter_val_type_var_##iter), \
        char*: true,                                                \
        const char*: true,                                          \
        unsigned char*: true,                                       \
        const unsigned char*: true,                                 \
        default: false);                                            \
    typeof(__chm_iter_val_type_var_##iter)* val;                    \
    if (is_val_string) {                                            \
      val = (typeof(val))(&it->val_pair->ptr);                      \
    } else {                                                        \
      val = (typeof(val))(it->val_pair->ptr);                       \
    }                                                               \
    val;                                                            \
  })

void __chmap_iterator_destroy(chmap_iterator* iter);

#define chmap_iter_destroy(iter)      \
  do {                                \
    __chmap_iterator_destroy((iter)); \
    iter = NULL;                      \
  } while (0)

// The function '_chmap_destroy' is not meant to be used directly, please
// use the macro 'chmap_destroy' instead.
void __chmap_destroy(chmap chm);

// The macro 'chmap_destroy' can be used to destroy a hash map. The pointer
// then gets set to NULL.
#define chmap_destroy(chm) \
  do {                     \
    __chmap_destroy(chm);  \
    chm = NULL;            \
  } while (0)

#define chmap_enable_local_macros(hm_name, key_t, val_t)              \
  typeof(key_t) __chm_key_type_var_##hm_name __attribute__((unused)); \
  typeof(val_t) __chm_val_type_var_##hm_name __attribute__((unused))

#define chmap_declare(hm_name, key_t, val_t)                          \
  typeof(key_t) __chm_key_type_var_##hm_name __attribute__((unused)); \
  typeof(val_t) __chm_val_type_var_##hm_name __attribute__((unused)); \
  chmap hm_name

#define chmap_init(hm_name)                                                   \
  do {                                                                        \
    char* err = NULL;                                                         \
    hm_name = chmap_create_mp(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, NULL, &err); \
    if (!hm_name) {                                                           \
      fatal_err("%s", err);                                                   \
    }                                                                         \
  } while (0)

#define chmap_construct(hm_name, key_t, val_t)                                \
  typeof(key_t) __chm_key_type_var_##hm_name __attribute__((unused));         \
  typeof(val_t) __chm_val_type_var_##hm_name __attribute__((unused));         \
  chmap hm_name = NULL;                                                       \
  do {                                                                        \
    char* err = NULL;                                                         \
    hm_name = chmap_create_mp(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, NULL, &err); \
    if (!hm_name) {                                                           \
      fatal_err("%s", err);                                                   \
    }                                                                         \
  } while (0)

#define _populate_chmap_pair(pair, data)         \
  do {                                           \
    bool is_data_string = _Generic((data),       \
        char*: true,                             \
        const char*: true,                       \
        unsigned char*: true,                    \
        const unsigned char*: true,              \
        default: false);                         \
    if (is_data_string) {                        \
      pair->ptr = (char*)(&(data));              \
      pair->size = strlen((char*)pair->ptr) + 1; \
    } else {                                     \
      pair->ptr = &data;                         \
      pair->size = sizeof((data));               \
    }                                            \
  } while (0)

#define chmap_insert(hm_name, key, val)                               \
  do {                                                                \
    chmap_pair* key_pair = &(chmap_pair){};                           \
    chmap_pair* val_pair = &(chmap_pair){};                           \
    _populate_chmap_pair(key_pair, key);                              \
    _populate_chmap_pair(val_pair, val);                              \
    ccol_retval_t r = chmap_insert_elem(hm_name, key_pair, val_pair); \
    if (r != ccol_success) {                                          \
      fatal_err("Failed to insert elem - r: %d", r);                  \
    }                                                                 \
  } while (0)

#define chmap_remove(hm_name, key)                          \
  ({                                                        \
    chmap_pair* key_pair = &(chmap_pair){};                 \
    _populate_chmap_pair(key_pair, key);                    \
    ccol_retval_t r = chmap_delete_elem(hm_name, key_pair); \
    r;                                                      \
  })

#define chmap_get(hm_name, key)                                             \
  ({                                                                        \
    typeof(__chm_val_type_var_##hm_name)* val = NULL;                       \
    chmap_pair* key_pair = &(chmap_pair){};                                 \
    chmap_pair* val_pair = NULL;                                            \
    _populate_chmap_pair(key_pair, key);                                    \
    ccol_retval_t r = chmap_get_elem_ref(hm_name, key_pair, &val_pair);     \
    if (r != ccol_success) {                                                \
      fatal_err("Failed to get elem ref - r: %d", r);                       \
    }                                                                       \
    bool is_val_string = _Generic(__chm_val_type_var_##hm_name,             \
        char*: true,                                                        \
        const char*: true,                                                  \
        unsigned char*: true,                                               \
        const unsigned char*: true,                                         \
        default: false);                                                    \
    if (is_val_string) {                                                    \
      val = (typeof(__chm_val_type_var_##hm_name)*)&(val_pair->ptr);        \
    } else if (val_pair->size != sizeof(val)) {                             \
      fatal_err(                                                            \
          "Failed to get elem ref - val_pair->size: %u - sizeof(val): %lu", \
          val_pair->size, sizeof(val));                                     \
    } else {                                                                \
      val = (typeof(__chm_val_type_var_##hm_name)*)(val_pair->ptr);         \
    }                                                                       \
    *val;                                                                   \
  })

#define chmap_get_ptr(hm_name, key)                                         \
  ({                                                                        \
    typeof(__chm_val_type_var_##hm_name)* val = NULL;                       \
    chmap_pair* key_pair = &(chmap_pair){};                                 \
    chmap_pair* val_pair = NULL;                                            \
    _populate_chmap_pair(key_pair, key);                                    \
    ccol_retval_t r = chmap_get_elem_ref(hm_name, key_pair, &val_pair);     \
    if (r != ccol_success) {                                                \
      fatal_err("Failed to get elem ref - r: %d", r);                       \
    }                                                                       \
    bool is_val_string = _Generic(__chm_val_type_var_##hm_name,             \
        char*: true,                                                        \
        const char*: true,                                                  \
        unsigned char*: true,                                               \
        const unsigned char*: true,                                         \
        default: false);                                                    \
    if (is_val_string) {                                                    \
      val = (typeof(__chm_val_type_var_##hm_name)*)&(val_pair->ptr);        \
    } else if (val_pair->size != sizeof(val)) {                             \
      fatal_err(                                                            \
          "Failed to get elem ref - val_pair->size: %u - sizeof(val): %lu", \
          val_pair->size, sizeof(val));                                     \
    } else {                                                                \
      val = (typeof(__chm_val_type_var_##hm_name)*)(val_pair->ptr);         \
    }                                                                       \
    val;                                                                    \
  })
