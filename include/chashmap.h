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

#include <common.h>
#include <string.h>

#define DEFAULT_INITIAL_BUCKET_ARRAY_SIZE 64

typedef struct chashmap chashmap;
typedef chashmap* chmap;

// The function 'chmap_create' creates a new hash map instance and returns
// the pointer to it. This pointer should be passed to the macro
// 'chmap_destroy', once the hash map is no longer needed. The input parameter
// 'bucket_array_size' determines the initial bucket size of the hash map.
chmap chmap_create_full(size_t initial_bucket_array_size,
                        ccol_memmgmt_procs_t* mmgmt_procs,
                        ccol_hashing_proc_t custom_hashing_proc, char** err);

static inline __attribute__((always_inline)) chmap
chmap_create(size_t initial_bucket_array_size, char** err) {
  return chmap_create_full(initial_bucket_array_size, NULL, NULL, err);
}

static inline __attribute__((always_inline)) chmap
chmap_create_mp(size_t initial_bucket_array_size,
                ccol_memmgmt_procs_t* mmgmt_procs, char** err) {
  return chmap_create_full(initial_bucket_array_size, mmgmt_procs, NULL, err);
}

static inline __attribute__((always_inline)) chmap
chmap_create_ch(size_t initial_bucket_array_size,
                ccol_hashing_proc_t custom_hashing_proc, char** err) {
  return chmap_create_full(initial_bucket_array_size, NULL, custom_hashing_proc,
                           err);
}

// The function 'chmap_elem_count' can be used to get the number of elements
// in it. If chm is NULL, this function cause an assert exception.
size_t chmap_elem_count(chmap chm);

// The function 'chmap_reset' can be used to clear a map by deleting the
// existing elements from it. If the new_bucket_array_size is 0, the
// bucket array size will not change, otherwise, the bucket array will
// get reallocated to match the specified size. If that reallocation fails,
// this function will return -1. If the chm pointer is not NULL, it will
// always destroy the existing elements regardless of its return value,
// the return value conveys information regarding the realloc attempt.
ccol_retval_t chmap_reset(chmap chm, size_t new_bucket_array_size);

// The function chmap_insert_elem 'upserts' the val_pair associated with
// the key_pair into the hash map chm. If no element exists in the map
// associated with the provided key_pair, this function will return -1,
// otherwise it will return 0.
ccol_retval_t chmap_insert_elem(chmap chm, const cmap_pair* key_pair,
                                const cmap_pair* val_pair);

// The function chmap_get_elem_copy populates a copy of the data stored in the
// val_pair from the hash map into the target_buf. The pointer target_buf SHOULD
// BE non-null, otherwise the function will fail.
ccol_retval_t chmap_get_elem_copy(chmap chm, const cmap_pair* key_pair,
                                  void* target_buf, size_t target_buf_size);

// This function gets a pointer to the element's value pair and stores it into
// the pointer-to-pointer val_pair. Here, naturally, val_pair gets overwritten.
ccol_retval_t chmap_get_elem_ref(chmap chm, const cmap_pair* key_pair,
                                 cmap_pair** val_pair);

// The function 'chmap_delete_elem' can be used to delete an element from the
// hash map.
ccol_retval_t chmap_delete_elem(chmap chm, const cmap_pair* key_pair);

// The function 'chashmap_begin_iter' can be used to acquire an iterator to
// traverse through a hashed map.
cmap_iterator* chashmap_begin_iter(chmap chm, char** err);

#define chmap_begin(chm)                                  \
  ({                                                      \
    char* err;                                            \
    cmap_iterator* iter = chashmap_begin_iter(chm, &err); \
    if (err != NULL) {                                    \
      fatal_err("failed to create iterator: %s", err);    \
    }                                                     \
    iter;                                                 \
  })

#define chmap_iter_declare(chm, iter)                             \
  typeof(*chm##__chm_key_type_var)* iter##__chm_iter_key_type_var \
      __attribute__((unused)) = NULL;                             \
  typeof(*chm##__chm_val_type_var)* iter##__chm_iter_val_type_var \
      __attribute__((unused)) = NULL;                             \
  cmap_iterator* iter _ccol_destructor(___chmap_iterator_destroy)

cmap_iterator* chmap_iter_next(cmap_iterator* iter);

#define chmap_iter_key_ptr(iter)                       \
  ({                                                   \
    const typeof(*iter##__chm_iter_key_type_var)* key; \
    if (is_char_ptr(*iter##__chm_iter_key_type_var)) { \
      key = (typeof(key))(&it->key_pair->ptr);         \
    } else {                                           \
      key = (typeof(key))(it->key_pair->ptr);          \
    }                                                  \
    key;                                               \
  })

#define chmap_iter_val_ptr(iter)                       \
  ({                                                   \
    typeof(*iter##__chm_iter_val_type_var)* val;       \
    if (is_char_ptr(*iter##__chm_iter_val_type_var)) { \
      val = (typeof(val))(&it->val_pair->ptr);         \
    } else {                                           \
      val = (typeof(val))(it->val_pair->ptr);          \
    }                                                  \
    val;                                               \
  })

void __chmap_iterator_destroy(cmap_iterator* iter);

#define chmap_iter_destroy(iter)      \
  do {                                \
    __chmap_iterator_destroy((iter)); \
    iter = NULL;                      \
  } while (0)

static inline void ___chmap_iterator_destroy(cmap_iterator** iter) {
  if (*iter) {
    __chmap_iterator_destroy(*iter);
    *iter = NULL;
  }
}

// The function '_chmap_destroy' is not meant to be used directly, please
// use the macro 'chmap_destroy' instead.
void __chmap_destroy(chmap chm);

static inline void ___chmap_destroy(chmap* chm) {
  if (*chm) {
    __chmap_destroy(*chm);
    *chm = NULL;
  }
}

// The macro 'chmap_destroy' can be used to destroy a hash map. The pointer
// then gets set to NULL.
#define chmap_destroy(chm) \
  do {                     \
    __chmap_destroy(chm);  \
    chm = NULL;            \
  } while (0)

#define chmap_enable_local_macros(hm_name, key_t, val_t)                     \
  typeof(key_t)* hm_name##__chm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t)* hm_name##__chm_val_type_var __attribute__((unused)) = NULL

#define chmap_declare(hm_name, key_t, val_t)                                 \
  typeof(key_t)* hm_name##__chm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t)* hm_name##__chm_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */

#define chmap_init_full(hm_name, mmgmt_procs, custom_hashing_proc)       \
  do {                                                                   \
    char* err = NULL;                                                    \
    hm_name = chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,       \
                                mmgmt_procs, custom_hashing_proc, &err); \
    if (!hm_name) {                                                      \
      fatal_err("%s", err);                                              \
    }                                                                    \
  } while (0)

#define chmap_construct_full(hm_name, key_t, val_t, mmgmt_procs,             \
                             custom_hashing_proc)                            \
  typeof(key_t)* hm_name##__chm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t)* hm_name##__chm_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;             \
  do {                                                                       \
    char* err = NULL;                                                        \
    hm_name = chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,           \
                                mmgmt_procs, custom_hashing_proc, &err);     \
    if (!hm_name) {                                                          \
      fatal_err("%s", err);                                                  \
    }                                                                        \
  } while (0)

#define chmap_init(hm_name)                                          \
  do {                                                               \
    char* err = NULL;                                                \
    hm_name = chmap_create(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, &err); \
    if (!hm_name) {                                                  \
      fatal_err("%s", err);                                          \
    }                                                                \
  } while (0)

#define chmap_construct(hm_name, key_t, val_t)                               \
  typeof(key_t)* hm_name##__chm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t)* hm_name##__chm_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;             \
  do {                                                                       \
    char* err = NULL;                                                        \
    hm_name = chmap_create(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, &err);         \
    if (!hm_name) {                                                          \
      fatal_err("%s", err);                                                  \
    }                                                                        \
  } while (0)

#define chmap_init_mp(hm_name, mmgmt_procs)                                    \
  do {                                                                         \
    char* err = NULL;                                                          \
    hm_name =                                                                  \
        chmap_create_mp(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, mmgmt_procs, &err); \
    if (!hm_name) {                                                            \
      fatal_err("%s", err);                                                    \
    }                                                                          \
  } while (0)

#define chmap_construct_mp(hm_name, key_t, val_t, mmgmt_procs)                 \
  typeof(key_t)* hm_name##__chm_key_type_var __attribute__((unused)) = NULL;   \
  typeof(val_t)* hm_name##__chm_val_type_var __attribute__((unused)) = NULL;   \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;               \
  do {                                                                         \
    char* err = NULL;                                                          \
    hm_name =                                                                  \
        chmap_create_mp(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, mmgmt_procs, &err); \
    if (!hm_name) {                                                            \
      fatal_err("%s", err);                                                    \
    }                                                                          \
  } while (0)

#define chmap_init_ch(hm_name, custom_hashing_proc)              \
  do {                                                           \
    char* err = NULL;                                            \
    hm_name = chmap_create_ch(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, \
                              custom_hashing_proc, &err);        \
    if (!hm_name) {                                              \
      fatal_err("%s", err);                                      \
    }                                                            \
  } while (0)

#define chmap_construct_ch(hm_name, key_t, val_t, custom_hashing_proc)       \
  typeof(key_t)* hm_name##__chm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t)* hm_name##__chm_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;             \
  do {                                                                       \
    char* err = NULL;                                                        \
    hm_name = chmap_create_ch(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,             \
                              custom_hashing_proc, &err);                    \
    if (!hm_name) {                                                          \
      fatal_err("%s", err);                                                  \
    }                                                                        \
  } while (0)

#define chmap_insert(hm_name, key, val)                               \
  do {                                                                \
    cmap_pair* key_pair = &(cmap_pair){};                             \
    cmap_pair* val_pair = &(cmap_pair){};                             \
    _populate_cmap_pair(key_pair, key);                               \
    _populate_cmap_pair(val_pair, val);                               \
    ccol_retval_t r = chmap_insert_elem(hm_name, key_pair, val_pair); \
    if (r != ccol_success) {                                          \
      fatal_err("Failed to insert elem - r: %d", r);                  \
    }                                                                 \
  } while (0)

#define chmap_remove(hm_name, key)                          \
  ({                                                        \
    cmap_pair* key_pair = &(cmap_pair){};                   \
    _populate_cmap_pair(key_pair, key);                     \
    ccol_retval_t r = chmap_delete_elem(hm_name, key_pair); \
    r;                                                      \
  })

#define chmap_get(hm_name, key)                                              \
  ({                                                                         \
    typeof(*hm_name##__chm_val_type_var)* val = NULL;                        \
    cmap_pair* key_pair = &(cmap_pair){};                                    \
    cmap_pair* val_pair = NULL;                                              \
    _populate_cmap_pair(key_pair, key);                                      \
    ccol_retval_t r = chmap_get_elem_ref(hm_name, key_pair, &val_pair);      \
    if (r != ccol_success) {                                                 \
      fatal_err("Failed to get elem ref - r: %d", r);                        \
    }                                                                        \
    if (is_char_ptr(*hm_name##__chm_val_type_var)) {                         \
      val = (typeof(*hm_name##__chm_val_type_var)*)&(val_pair->ptr);         \
    } else if (val_pair->size != sizeof(*val)) {                             \
      fatal_err(                                                             \
          "Failed to get elem ref - val_pair->size: %lu - sizeof(val): %lu", \
          (unsigned long)val_pair->size, (unsigned long)sizeof(val));        \
    } else {                                                                 \
      val = (typeof(*hm_name##__chm_val_type_var)*)(val_pair->ptr);          \
    }                                                                        \
    *val;                                                                    \
  })

#define chmap_get_ptr(hm_name, key)                                            \
  ({                                                                           \
    typeof(*hm_name##__chm_val_type_var)* val = NULL;                          \
    cmap_pair* key_pair = &(cmap_pair){};                                      \
    cmap_pair* val_pair = NULL;                                                \
    _populate_cmap_pair(key_pair, key);                                        \
    ccol_retval_t r = chmap_get_elem_ref(hm_name, key_pair, &val_pair);        \
    if (r == ccol_success) {                                                   \
      if (is_char_ptr(*hm_name##__chm_val_type_var)) {                         \
        val = (typeof(*hm_name##__chm_val_type_var)*)&(val_pair->ptr);         \
      } else if (val_pair->size != sizeof(*val)) {                             \
        fatal_err(                                                             \
            "Failed to get elem ref - val_pair->size: %lu - sizeof(val): %lu", \
            (unsigned long)val_pair->size, (unsigned long)sizeof(val));        \
      } else {                                                                 \
        val = (typeof(*hm_name##__chm_val_type_var)*)(val_pair->ptr);          \
      }                                                                        \
    }                                                                          \
    val;                                                                       \
  })
