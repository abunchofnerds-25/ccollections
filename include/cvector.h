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
#define cvec_declare(v, type)                               \
  type* __cvec_type_var_##v __attribute__((unused)) = NULL; \
  cvec v

// Initialize a previously declared (via 'cvec_declare') vector 'v'
#define cvec_init(v)                                            \
  do {                                                          \
    char* err_str = NULL;                                       \
    v = cvector_create(sizeof(*__cvec_type_var_##v), &err_str); \
    if (!v) {                                                   \
      assert(!err_str);                                         \
    }                                                           \
  } while (0)

// Initialize a previously declared (via 'cvec_declare') vector 'v' with
// custom memory management procs
#define cvec_init_with_mprocs(v, mprocs)                                 \
  do {                                                                   \
    char* err_str = NULL;                                                \
    v = cvector_create_with_mprocs(sizeof(*__cvec_type_var_##v), mprocs, \
                                   &err_str);                            \
    if (!v) {                                                            \
      assert(!err_str);                                                  \
    }                                                                    \
  } while (0)

// Construct (declare and initialize) a vector 'v' to contain data of type
// 'type'
#define cvec_construct(v, type)                                 \
  type* __cvec_type_var_##v __attribute__((unused)) = NULL;     \
  cvec v;                                                       \
  do {                                                          \
    char* err_str = NULL;                                       \
    v = cvector_create(sizeof(*__cvec_type_var_##v), &err_str); \
    if (!v) {                                                   \
      assert(!err_str);                                         \
    }                                                           \
  } while (0)

// Construct (declare and initialize) a vector 'v' to contain data of type
// 'type' with custom memory management procs
#define cvec_construct_with_mprocs(v, type, mprocs)                      \
  type* __cvec_type_var_##v = NULL;                                      \
  cvec v;                                                                \
  do {                                                                   \
    char* err_str = NULL;                                                \
    v = cvector_create_with_mprocs(sizeof(*__cvec_type_var_##v), mprocs, \
                                   &err_str);                            \
    if (!v) {                                                            \
      assert(!err_str);                                                  \
    }                                                                    \
  } while (0)

#define cvec_destruct(v) cvector_destroy(v)

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
    typeof(*__cvec_type_var_##v) _tmp;            \
    ccol_retval_t r = cvector_pop_back(v, &_tmp); \
    if (r != ccol_success) {                      \
      assert(r == ccol_success);                  \
    }                                             \
    _tmp;                                         \
  })

#define cvec_at(v, index) *(typeof(*__cvec_type_var_##v)*)(cvector_at(v, index))

#define cvec_at_ptr(v, index) \
  (typeof(*__cvec_type_var_##v)*)(cvector_at(v, index))

#define cvec_size(v) cvector_elem_count(v)

#define cvec_reset(v) cvector_reset(v)
