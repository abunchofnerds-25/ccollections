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

typedef struct cvector cvector;
typedef cvector* cvec;

cvec cvector_create_with_mprocs(uint32_t elem_size,
                                ccol_memmgmt_procs_t* mmgmt_procs, char** err);

#define cvector_create(elem_size, err) \
  cvector_create_with_mprocs(elem_size, NULL, err)

void __cvector_destroy(cvec v);

#define cvector_destroy(v)  \
  do {                      \
    if (v) {                \
      __cvector_destroy(v); \
      v = NULL;             \
    }                       \
  } while (0)

ccol_retval_t cvector_push_back(cvec v, const void* new_elem);

ccol_retval_t cvector_pop_back(cvec v, void* target_elem);

void* cvector_at(cvec v, uint32_t index);

uint32_t cvector_elem_count(cvec v);

void cvector_reset(cvec v);

// Some useful macros, for the majority of the use cases these should be more
// than enough.

// Declare an uninitialized vector 'v' to hold elements of type 'type'
#define cvec_declare(v, type) \
  type* v##__cvec_type_var;   \
  cvec v

#define cvec_enable_local_macros(v, type) \
  type* v##__cvec_type_var __attribute__((unused)) = NULL

// Initialize a previously declared (via 'cvec_declare') vector 'v'
#define cvec_init(v)                                           \
  do {                                                         \
    char* err_str = NULL;                                      \
    v##__cvec_type_var = NULL;                                 \
    v = cvector_create(sizeof(*v##__cvec_type_var), &err_str); \
    if (!v) {                                                  \
      assert(!err_str);                                        \
    }                                                          \
  } while (0)

// Initialize a previously declared (via 'cvec_declare') vector 'v' with
// custom memory management procs
#define cvec_init_with_mprocs(v, mprocs)                                \
  do {                                                                  \
    char* err_str = NULL;                                               \
    v = cvector_create_with_mprocs(sizeof(*v##__cvec_type_var), mprocs, \
                                   &err_str);                           \
    if (!v) {                                                           \
      assert(!err_str);                                                 \
    }                                                                   \
  } while (0)

// Construct (declare and initialize) a vector 'v' to contain data of type
// 'type'
#define cvec_construct(v, type)                                \
  type* v##__cvec_type_var __attribute__((unused)) = NULL;     \
  cvec v;                                                      \
  do {                                                         \
    char* err_str = NULL;                                      \
    v = cvector_create(sizeof(*v##__cvec_type_var), &err_str); \
    if (!v) {                                                  \
      assert(!err_str);                                        \
    }                                                          \
  } while (0)

// Construct (declare and initialize) a vector 'v' to contain data of type
// 'type' with custom memory management procs
#define cvec_construct_with_mprocs(v, type, mprocs)                     \
  type* v##__cvec_type_var = NULL;                                      \
  cvec v;                                                               \
  do {                                                                  \
    char* err_str = NULL;                                               \
    v = cvector_create_with_mprocs(sizeof(*v##__cvec_type_var), mprocs, \
                                   &err_str);                           \
    if (!v) {                                                           \
      assert(!err_str);                                                 \
    }                                                                   \
  } while (0)

#define cvec_destroy(v) cvector_destroy(v)

#define cvec_push(v, new_elem)                                      \
  do {                                                              \
    ccol_retval_t r = cvector_push_back(v, (const void*)&new_elem); \
    if (r != ccol_success) {                                        \
      assert(r == ccol_success);                                    \
    }                                                               \
  } while (0)

// cvec_push_rvalue can be used to push rvalue elements
// that are not "addressable"
#define cvec_push_rvalue(v, new_elem)                                      \
  do {                                                                     \
    ccol_retval_t r = cvector_push_back(v, &(typeof(new_elem)){new_elem}); \
    if (r != ccol_success) {                                               \
      assert(r == ccol_success);                                           \
    }                                                                      \
  } while (0)

#define cvec_pop(v)                               \
  ({                                              \
    typeof(*v##__cvec_type_var) _tmp;             \
    ccol_retval_t r = cvector_pop_back(v, &_tmp); \
    if (r != ccol_success) {                      \
      assert(r == ccol_success);                  \
    }                                             \
    _tmp;                                         \
  })

#define cvec_at(v, index) *(typeof(*v##__cvec_type_var)*)(cvector_at(v, index))

#define cvec_at_ptr(v, index) \
  (typeof(*v##__cvec_type_var)*)(cvector_at(v, index))

#define cvec_size(v) cvector_elem_count(v)

#define cvec_reset(v) cvector_reset(v)
