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

#pragma once

#include <common.h>

typedef struct cbinarymap cbinarymap;
typedef cbinarymap* cbmap;

cbmap cbmap_create_full(bool keys_are_signed, ccol_memmgmt_procs_t* mmgmt_procs,
                        ccol_comparison_proc_t custom_comparison_proc,
                        char** err);

static inline __attribute__((always_inline)) cbmap
cbmap_create(bool keys_are_signed, char** err) {
  return cbmap_create_full(keys_are_signed, NULL, NULL, err);
}

static inline __attribute__((always_inline)) cbmap cbmap_create_mp(
    bool keys_are_signed, ccol_memmgmt_procs_t* mmgmt_procs, char** err) {
  return cbmap_create_full(keys_are_signed, mmgmt_procs, NULL, err);
}

static inline __attribute__((always_inline)) cbmap
cbmap_create_ch(bool keys_are_signed,
                ccol_comparison_proc_t custom_comparison_proc, char** err) {
  return cbmap_create_full(keys_are_signed, NULL, custom_comparison_proc, err);
}

size_t cbmap_elem_count(cbmap cbm);

ccol_retval_t cbmap_reset(cbmap cbm);

ccol_retval_t cbmap_insert_elem(cbmap cbm, const cmap_pair* key_pair,
                                const cmap_pair* val_pair);

ccol_retval_t cbmap_get_elem_copy(cbmap cbm, const cmap_pair* key_pair,
                                  void* target_buf, size_t target_buf_size);

ccol_retval_t cbmap_get_elem_ref(cbmap cbm, const cmap_pair* key_pair,
                                 cmap_pair** val_pair);

ccol_retval_t cbmap_delete_elem(cbmap cbm, const cmap_pair* key_pair);

// The function 'cbmap_begin_iter' can be used to acquire an iterator to
// traverse through a binary search tree map.
cmap_iterator* cbmap_begin_iter(cbmap cbm, char** err);

#define cbmap_iter_declare(cbm, iter)                             \
  typeof(*cbm##__cbm_key_type_var)* iter##__cbm_iter_key_type_var \
      __attribute__((unused)) = NULL;                             \
  typeof(*cbm##__cbm_val_type_var)* iter##__cbm_iter_val_type_var \
      __attribute__((unused)) = NULL;                             \
  cmap_iterator* iter

#define cbmap_begin(cbm)                               \
  ({                                                   \
    char* err;                                         \
    cmap_iterator* iter = cbmap_begin_iter(cbm, &err); \
    if (err != NULL) {                                 \
      assert(false);                                   \
    }                                                  \
    iter;                                              \
  })

cmap_iterator* cbmap_iter_next(cmap_iterator* iter);

#define cbmap_iter_key_ptr(iter)                       \
  ({                                                   \
    const typeof(*iter##__cbm_iter_key_type_var)* key; \
    if (is_char_ptr(*iter##__cbm_iter_key_type_var)) { \
      key = (typeof(key))(&iter->key_pair->ptr);       \
    } else {                                           \
      key = (typeof(key))(iter->key_pair->ptr);        \
    }                                                  \
    key;                                               \
  })

#define cbmap_iter_val_ptr(iter)                       \
  ({                                                   \
    typeof(*iter##__cbm_iter_val_type_var)* val;       \
    if (is_char_ptr(*iter##__cbm_iter_val_type_var)) { \
      val = (typeof(val))(&iter->val_pair->ptr);       \
    } else {                                           \
      val = (typeof(val))(iter->val_pair->ptr);        \
    }                                                  \
    val;                                               \
  })

void __cbmap_iterator_destroy(cmap_iterator* iter);

#define cbmap_iter_destroy(iter)      \
  do {                                \
    __cbmap_iterator_destroy((iter)); \
    iter = NULL;                      \
  } while (0)

// The function '_cbmap_destroy' is not meant to be used directly, please
// use the macro 'cbmap_destroy' instead.
void __cbmap_destroy(cbmap cbm);

// The macro 'cbmap_destroy' can be used to destroy a hash map. The pointer
// then gets set to NULL.
#define cbmap_destroy(cbm) \
  do {                     \
    __cbmap_destroy(cbm);  \
    cbm = NULL;            \
  } while (0)

static inline void ___cbmap_destroy(cbmap* cbm) {
  if (*cbm) {
    __cbmap_destroy(*cbm);
    *cbm = NULL;
  }
}

#define cbmap_enable_local_macros(hm_name, key_t, val_t)                     \
  typeof(key_t)* hm_name##__cbm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t)* hm_name##__cbm_val_type_var __attribute__((unused)) = NULL

#define cbmap_declare(hm_name, key_t, val_t)                                 \
  typeof(key_t)* hm_name##__cbm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t)* hm_name##__cbm_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name /* _ccol_destructor(___cbmap_destroy) = NULL; */

#define cbmap_init(hm_name)                                                  \
  do {                                                                       \
    char* err = NULL;                                                        \
    hm_name = cbmap_create_full(                                             \
        __is_signed_int_ptr(hm_name##__cbm_key_type_var), NULL, NULL, &err); \
    if (!hm_name) {                                                          \
      fatal_err("%s", err);                                                  \
    }                                                                        \
  } while (0)

#define cbmap_init_mp(hm_name, mmgmt_procs)                                 \
  do {                                                                      \
    char* err = NULL;                                                       \
    hm_name =                                                               \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__cbm_key_type_var), \
                          mmgmt_procs, NULL, &err);                         \
    if (!hm_name) {                                                         \
      fatal_err("%s", err);                                                 \
    }                                                                       \
  } while (0)

#define cbmap_init_cc(hm_name, custom_comparison_proc)                      \
  do {                                                                      \
    char* err = NULL;                                                       \
    hm_name =                                                               \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__cbm_key_type_var), \
                          NULL, custom_comparison_proc, &err);              \
    if (!hm_name) {                                                         \
      fatal_err("%s", err);                                                 \
    }                                                                       \
  } while (0)

#define cbmap_init_full(hm_name, mmgmt_procs, custom_comparison_proc)       \
  do {                                                                      \
    char* err = NULL;                                                       \
    hm_name =                                                               \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__cbm_key_type_var), \
                          mmgmt_procs, custom_comparison_proc, &err);       \
    if (!hm_name) {                                                         \
      fatal_err("%s", err);                                                 \
    }                                                                       \
  } while (0)

#define cbmap_construct(hm_name, key_t, val_t)                               \
  typeof(key_t)* hm_name##__cbm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t)* hm_name##__cbm_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name /* _ccol_destructor(___cbmap_destroy) = NULL; */ = NULL;     \
  do {                                                                       \
    char* err = NULL;                                                        \
    hm_name = cbmap_create_full(                                             \
        __is_signed_int_ptr(hm_name##__cbm_key_type_var), NULL, NULL, &err); \
    if (!hm_name) {                                                          \
      fatal_err("%s", err);                                                  \
    }                                                                        \
  } while (0)

#define cbmap_construct_mp(hm_name, key_t, val_t, mmgmt_procs)               \
  typeof(key_t)* hm_name##__cbm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t)* hm_name##__cbm_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name /* _ccol_destructor(___cbmap_destroy) = NULL; */ = NULL;     \
  do {                                                                       \
    char* err = NULL;                                                        \
    hm_name =                                                                \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__cbm_key_type_var),  \
                          mmgmt_procs, NULL, &err);                          \
    if (!hm_name) {                                                          \
      fatal_err("%s", err);                                                  \
    }                                                                        \
  } while (0)

#define cbmap_construct_cc(hm_name, key_t, val_t, custom_comparison_proc)    \
  typeof(key_t)* hm_name##__cbm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t)* hm_name##__cbm_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name /* _ccol_destructor(___cbmap_destroy) = NULL; */ = NULL;     \
  do {                                                                       \
    char* err = NULL;                                                        \
    hm_name =                                                                \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__cbm_key_type_var),  \
                          NULL, custom_comparison_proc, &err);               \
    if (!hm_name) {                                                          \
      fatal_err("%s", err);                                                  \
    }                                                                        \
  } while (0)

#define cbmap_construct_full(hm_name, key_t, val_t, mmgmt_procs,             \
                             custom_comparison_proc)                         \
  typeof(key_t)* hm_name##__cbm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t)* hm_name##__cbm_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name /* _ccol_destructor(___cbmap_destroy) = NULL; */ = NULL;     \
  do {                                                                       \
    char* err = NULL;                                                        \
    hm_name =                                                                \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__cbm_key_type_var),  \
                          mmgmt_procs, custom_comparison_proc, &err);        \
    if (!hm_name) {                                                          \
      fatal_err("%s", err);                                                  \
    }                                                                        \
  } while (0)

#define cbmap_insert(hm_name, key, val)                               \
  do {                                                                \
    cmap_pair* key_pair = &(cmap_pair){};                             \
    cmap_pair* val_pair = &(cmap_pair){};                             \
    _populate_cmap_pair(key_pair, key);                               \
    _populate_cmap_pair(val_pair, val);                               \
    ccol_retval_t r = cbmap_insert_elem(hm_name, key_pair, val_pair); \
    if (r != ccol_success) {                                          \
      fatal_err("Failed to insert elem - r: %d", r);                  \
    }                                                                 \
  } while (0)

#define cbmap_remove(hm_name, key)                          \
  ({                                                        \
    cmap_pair* key_pair = &(cmap_pair){};                   \
    _populate_cmap_pair(key_pair, key);                     \
    ccol_retval_t r = cbmap_delete_elem(hm_name, key_pair); \
    r;                                                      \
  })

#define cbmap_get(hm_name, key)                                              \
  ({                                                                         \
    typeof(*hm_name##__cbm_val_type_var)* val = NULL;                        \
    cmap_pair* key_pair = &(cmap_pair){};                                    \
    cmap_pair* val_pair = NULL;                                              \
    _populate_cmap_pair(key_pair, key);                                      \
    ccol_retval_t r = cbmap_get_elem_ref(hm_name, key_pair, &val_pair);      \
    if (r != ccol_success) {                                                 \
      fatal_err("Failed to get elem ref - r: %d", r);                        \
    }                                                                        \
    if (is_char_ptr(*hm_name##__cbm_val_type_var)) {                         \
      val = (typeof(*hm_name##__cbm_val_type_var)*)&(val_pair->ptr);         \
    } else if (val_pair->size != sizeof(*val)) {                             \
      fatal_err(                                                             \
          "Failed to get elem ref - val_pair->size: %lu - sizeof(val): %lu", \
          (unsigned long)val_pair->size, (unsigned long)sizeof(val));        \
    } else {                                                                 \
      val = (typeof(*hm_name##__cbm_val_type_var)*)(val_pair->ptr);          \
    }                                                                        \
    *val;                                                                    \
  })

#define cbmap_get_ptr(hm_name, key)                                            \
  ({                                                                           \
    typeof(*hm_name##__cbm_val_type_var)* val = NULL;                          \
    cmap_pair* key_pair = &(cmap_pair){};                                      \
    cmap_pair* val_pair = NULL;                                                \
    _populate_cmap_pair(key_pair, key);                                        \
    ccol_retval_t r = cbmap_get_elem_ref(hm_name, key_pair, &val_pair);        \
    if (r == ccol_success) {                                                   \
      if (is_char_ptr(*hm_name##__cbm_val_type_var)) {                         \
        val = (typeof(*hm_name##__cbm_val_type_var)*)&(val_pair->ptr);         \
      } else if (val_pair->size != sizeof(*val)) {                             \
        fatal_err(                                                             \
            "Failed to get elem ref - val_pair->size: %lu - sizeof(val): %lu", \
            (unsigned long)val_pair->size, sizeof(val));                       \
      } else {                                                                 \
        val = (typeof(*hm_name##__cbm_val_type_var)*)(val_pair->ptr);          \
      }                                                                        \
    }                                                                          \
    val;                                                                       \
  })
