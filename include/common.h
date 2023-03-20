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
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// The following block is for having static error messages in place
// when things fail during container "construction".
#define ccol_stringify(s) #s
#define ccol_x_stringify(s) ccol_stringify(s)
#define CCOL_ERR_STR(x) (__FILE__ ":" ccol_x_stringify(__LINE__) " - " x)

#define fatal_err(err_fmt, ...)                                 \
  do {                                                          \
    char err_str[512] = {0};                                    \
    snprintf(err_str, sizeof(err_str), err_fmt, ##__VA_ARGS__); \
    fprintf(stderr, "%s\n", err_str);                           \
    assert(false);                                              \
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
  ccol_success,
  ccol_success_threshold = ccol_success
} ccol_retval_t;

// #define _ccol_destructor(destructor) __attribute__((__cleanup__(destructor)))
#define _ccol_destructor(destructor) __attribute__((cleanup(destructor)))

typedef struct ccol_memmgmt_procs_t {
  void* (*malloc)(size_t size);
  void (*free)(void* ptr);
  void* (*calloc)(size_t elem_count, size_t elem_size);
  void* (*realloc)(void* ptr, size_t size);
} ccol_memmgmt_procs_t;

typedef struct cmap_pair {
  void* ptr;
  uint32_t size;
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
                                                                      \
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
                                                                               \
    result;                                                                    \
  })
