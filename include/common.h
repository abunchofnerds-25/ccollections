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

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The following block is for having static error messages in place
// when things fail during container "construction".
#define ccol_stringify(s) #s
#define ccol_x_stringify(s) ccol_stringify(s)
#define CCOL_ERR_STR(x) (__FILE__ ":" ccol_x_stringify(__LINE__) " - " x)

#define fatal_err(_err_fmt, ...)                                   \
  do {                                                             \
    char _err_str[512] = {0};                                      \
    snprintf(_err_str, sizeof(_err_str), _err_fmt, ##__VA_ARGS__); \
    fprintf(stderr, "%s\n", _err_str);                             \
    assert(false);                                                 \
  } while (0)

// The following block is there to easily use customized memory management
// functions.
#define mem_alloc(size) malloc(size)
#define mem_calloc(elem_count, elem_size) calloc(elem_count, elem_size)
#define mem_realloc(ptr, new_size) realloc(ptr, new_size)
#define mem_free(ptr) free(ptr)

#define _mem_alloc(m_procs, size) \
  (m_procs) ? m_procs->malloc(size) : mem_alloc(size)
#define _mem_calloc(m_procs, e_count, e_size) \
  (m_procs) ? m_procs->calloc(e_count, e_size) : mem_calloc(e_count, e_size)
#define _mem_realloc(m_procs, ptr, new_size) \
  (m_procs) ? m_procs->realloc(ptr, new_size) : mem_realloc(ptr, new_size)
#define _mem_free(m_procs, ptr) (m_procs) ? m_procs->free(ptr) : mem_free(ptr)

#define max_elem_count UINT64_MAX

typedef enum ccollections_retval_t {
  ccol_unexpected_failure = -9,
  ccol_container_empty,
  ccol_container_full,
  ccol_timed_out,
  ccol_not_permitted,
  ccol_invalid_args,
  ccol_key_not_found,
  ccol_key_already_present,
  ccol_not_enough_memory,
  ccol_success
} ccol_retval_t;

#define _ccol_destructor(destructor) __attribute__((cleanup(destructor)))

typedef void* (*ccol_memmgmt_procs_malloc_t)(size_t size);
typedef void (*ccol_memmgmt_procs_free_t)(void* ptr);
typedef void* (*ccol_memmgmt_procs_calloc_t)(size_t elem_count,
                                             size_t elem_size);
typedef void* (*ccol_memmgmt_procs_realloc_t)(void* ptr, size_t size);

typedef struct ccol_memmgmt_procs_t {
  ccol_memmgmt_procs_malloc_t malloc;
  ccol_memmgmt_procs_free_t free;
  ccol_memmgmt_procs_calloc_t calloc;
  ccol_memmgmt_procs_realloc_t realloc;
} ccol_memmgmt_procs_t;

// The following function type can be used to supply a custom comparison
// function.
typedef bool (*ccol_comparison_proc_t)(const void* ptr1, const void* ptr2);

// The following function type can be used to supply a custom hashing function.
typedef unsigned long (*ccol_hashing_proc_t)(const void* ptr);

typedef struct cmap_pair {
  void* ptr;
  size_t size;
} cmap_pair;

typedef struct cmap_iterator {
  cmap_pair* key_pair;
  cmap_pair* val_pair;
} cmap_iterator;

#define ccol_verify_memmgmt_procs(mmgt_procs, err)                    \
  ({                                                                  \
    bool result = true;                                               \
    if (mmgt_procs && (!mmgt_procs->malloc || !mmgt_procs->calloc ||  \
                       !mmgt_procs->realloc || !mmgt_procs->free)) {  \
      if (err) {                                                      \
        *err = CCOL_ERR_STR(                                          \
            "Detected at least one NULL memory management function"); \
      }                                                               \
      result = false;                                                 \
    }                                                                 \
    result;                                                           \
  })

#define ccol_populate_mem_mgmt_procs(container, mmgmt_procs, err)              \
  ({                                                                           \
    bool result = true;                                                        \
    if (mmgmt_procs) {                                                         \
      container->m_procs = mmgmt_procs->malloc(sizeof(ccol_memmgmt_procs_t));  \
      if (!container->m_procs) {                                               \
        if (err) {                                                             \
          *err = CCOL_ERR_STR(                                                 \
              "Failed to allocate buffer for memory mgmt buffer");             \
        }                                                                      \
        result = false;                                                        \
      } else {                                                                 \
        memcpy(container->m_procs, mmgmt_procs, sizeof(ccol_memmgmt_procs_t)); \
      }                                                                        \
    } else {                                                                   \
      container->m_procs = NULL;                                               \
    }                                                                          \
    result;                                                                    \
  })

#define is_integral_type(x)           \
  _Generic((x),                       \
      char: true,                     \
      short: true,                    \
      int: true,                      \
      long: true,                     \
      long long: true,                \
      unsigned char: true,            \
      unsigned short: true,           \
      unsigned int: true,             \
      unsigned long: true,            \
      unsigned long long: true,       \
      float: true,                    \
      double: true,                   \
      long double: true,              \
      const char: true,               \
      const short: true,              \
      const int: true,                \
      const long: true,               \
      const long long: true,          \
      const unsigned char: true,      \
      const unsigned short: true,     \
      const unsigned int: true,       \
      const unsigned long: true,      \
      const unsigned long long: true, \
      const float: true,              \
      const double: true,             \
      const long double: true,        \
      default: false)

#define is_integral_ptr(x)             \
  _Generic((x),                        \
      char*: true,                     \
      short*: true,                    \
      int*: true,                      \
      long*: true,                     \
      long long*: true,                \
      unsigned char*: true,            \
      unsigned short*: true,           \
      unsigned int*: true,             \
      unsigned long*: true,            \
      unsigned long long*: true,       \
      float*: true,                    \
      double*: true,                   \
      long double*: true,              \
      const char*: true,               \
      const short*: true,              \
      const int*: true,                \
      const long*: true,               \
      const long long*: true,          \
      const unsigned char*: true,      \
      const unsigned short*: true,     \
      const unsigned int*: true,       \
      const unsigned long*: true,      \
      const unsigned long long*: true, \
      const float*: true,              \
      const double*: true,             \
      const long double*: true,        \
      default: false)

#define __is_signed_int_ptr(_ptr) \
  _Generic((_ptr),                \
      char*: true,                \
      short*: true,               \
      int*: true,                 \
      long*: true,                \
      long long*: true,           \
      const char*: true,          \
      const short*: true,         \
      const int*: true,           \
      const long*: true,          \
      const long long*: true,     \
      default: false)

#if defined __clang__
#define is_char_ptr(data)                                                   \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool result = _Generic((data),                                          \
        char*: true,                                                        \
        const char*: true,                                                  \
        unsigned char*: true,                                               \
        const unsigned char*: true,                                         \
        default: false);                                                    \
    _Pragma("GCC diagnostic pop");                                          \
    result;                                                                 \
  })

#define is_char_array(data)                   \
  (is_char_ptr((data)) && _Generic((&(data)), \
       char**: false,                         \
       const char**: false,                   \
       unsigned char**: false,                \
       const unsigned char**: false,          \
       default: true))
#else
#define is_char_ptr(data)         \
  _Generic((data),                \
      char*: true,                \
      const char*: true,          \
      unsigned char*: true,       \
      const unsigned char*: true, \
      default: false)

#define is_char_array(data)                   \
  (is_char_ptr((data)) && _Generic((&(data)), \
       char**: false,                         \
       const char**: false,                   \
       unsigned char**: false,                \
       const unsigned char**: false,          \
       default: true))
#endif

#define _populate_cmap_pair(pair, data)                     \
  do {                                                      \
    if (is_char_array(data)) {                              \
      pair->ptr = (char*)&(data);                           \
      pair->size = strlen((char*)pair->ptr) + 1;            \
    } else if (is_char_ptr(data)) {                         \
      char* _ptr = (char*)&data;                            \
      _Pragma("GCC diagnostic push");                       \
      _Pragma("GCC diagnostic ignored \"-Warray-bounds\""); \
      pair->ptr = *((char**)_ptr);                          \
      _Pragma("GCC diagnostic pop");                        \
      pair->size = strlen((char*)pair->ptr) + 1;            \
    } else {                                                \
      pair->ptr = &(data);                                  \
      pair->size = sizeof((data));                          \
    }                                                       \
  } while (0)
