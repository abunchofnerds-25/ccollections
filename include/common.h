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

/**
 * @file common.h
 * @brief Common definitions, types, and utilities for the C collections library
 *
 * Provides foundational infrastructure used across all collection types:
 * - Threading primitives (mutex, rwlock, condition variables)
 * - Error handling and reporting macros
 * - Memory management abstraction layer
 * - Return value codes
 * - Type introspection via C11 _Generic
 * - Map key-value pair structures
 * - Custom comparison and hashing function types
 *
 * This header is included by all collection implementations and provides
 * a consistent interface for thread safety, memory management, and error
 * handling throughout the library.
 */

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/*                         THREADING PRIMITIVES                               */
/* ========================================================================== */

/** @brief Mutex type (wraps pthread_mutex_t) */
#define mutex_t pthread_mutex_t

/** @brief Destroy a mutex */
#define mutex_destroy(m) pthread_mutex_destroy(&m)

/** @brief Initialize a mutex with default attributes */
#define mutex_init(m) pthread_mutex_init(&m, NULL)

/** @brief Lock a mutex (blocking) */
#define mutex_lock(m) pthread_mutex_lock(&m)

/** @brief Unlock a mutex */
#define mutex_unlock(m) pthread_mutex_unlock(&m)

/** @brief Read-write lock type (wraps pthread_rwlock_t) */
#define rw_lock_t pthread_rwlock_t

/** @brief Destroy a read-write lock */
#define rw_lock_destroy(a) pthread_rwlock_destroy(&a)

/** @brief Initialize a read-write lock with default attributes */
#define rw_lock_init(a) pthread_rwlock_init(&a, NULL)

/** @brief Acquire write lock (exclusive access) */
#define rw_lock_wrlock(a) pthread_rwlock_wrlock(&a)

/** @brief Acquire read lock (shared access) */
#define rw_lock_rdlock(a) pthread_rwlock_rdlock(&a)

/** @brief Release read-write lock */
#define rw_lock_unlock(a) pthread_rwlock_unlock(&a)

/** @brief Condition variable type (wraps pthread_cond_t) */
#define cond_var_t pthread_cond_t

/** @brief Destroy a condition variable */
#define cond_var_destroy(c) pthread_cond_destroy(&c)

/** @brief Initialize a condition variable with default attributes */
#define cond_var_init(c) pthread_cond_init(&c, NULL)

/** @brief Wait on condition variable (releases mutex while waiting) */
#define cond_var_wait(c, m) pthread_cond_wait(&c, &m)

/** @brief Timed wait on condition variable with absolute timeout */
#define cond_var_timedwait(c, m, t) pthread_cond_timedwait(&c, &m, &t)

/** @brief Signal one waiting thread on condition variable */
#define cond_var_signal(c) pthread_cond_signal(&c)

/** @brief Signal all waiting threads on condition variable */
#define cond_var_broadcast(c) pthread_cond_broadcast(&c)

/** @brief Thread ID type (wraps pthread_t) */
#define thread_id_t pthread_t

/** @brief Get current thread ID */
#define get_thread_id pthread_self

/**
 * @brief Assertion macro for collections library
 *
 * Wrapper around standard assert() for consistency across the library.
 * Enables runtime assertion checking in debug builds.
 */
#define ccol_assert assert

/* ========================================================================== */
/*                         ERROR HANDLING                                     */
/* ========================================================================== */

/**
 * @brief Convert argument to string literal (internal helper)
 * @param s Argument to stringify
 */
#define ccol_stringify(s) #s

/**
 * @brief Expand and stringify macro argument
 * @param s Macro to expand then stringify
 */
#define ccol_x_stringify(s) ccol_stringify(s)

/**
 * @brief Create error string with file and line information
 *
 * Generates a compile-time error string containing the source file,
 * line number, and custom error message.
 *
 * @param x Error message string
 * @return String literal: "file:line - message"
 *
 * Example:
 * @code
 * char *err = CCOL_ERR_STR("failed to allocate memory");
 * // Results in: "myfile.c:42 - failed to allocate memory"
 * @endcode
 */
#define CCOL_ERR_STR(x) (__FILE__ ":" ccol_x_stringify(__LINE__) " - " x)

/**
 * @brief Fatal error macro - print message and assert
 *
 * Formats an error message to stderr and triggers an assertion failure.
 * Used for unrecoverable errors that should terminate the program.
 *
 * @param _err_fmt Printf-style format string
 * @param ... Format arguments
 *
 * @note Always terminates program via ccol_assert(false)
 * @note Message limited to 512 characters
 *
 * Example:
 * @code
 * if (!ptr) {
 *   fatal_err("allocation failed: size=%zu", requested_size);
 * }
 * @endcode
 */
#define fatal_err(_err_fmt, ...)                                   \
  do {                                                             \
    char _err_str[512] = {0};                                      \
    snprintf(_err_str, sizeof(_err_str), _err_fmt, ##__VA_ARGS__); \
    fprintf(stderr, "%s\n", _err_str);                             \
    ccol_assert(false);                                            \
  } while (0)

/* ========================================================================== */
/*                         MEMORY MANAGEMENT                                  */
/* ========================================================================== */

/**
 * @brief Allocate memory (default: malloc)
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
#define mem_alloc(size) malloc(size)

/**
 * @brief Allocate and zero-initialize memory (default: calloc)
 * @param elem_count Number of elements
 * @param elem_size Size of each element
 * @return Pointer to allocated memory, or NULL on failure
 */
#define mem_calloc(elem_count, elem_size) calloc(elem_count, elem_size)

/**
 * @brief Reallocate memory (default: realloc)
 * @param ptr Existing pointer to reallocate
 * @param new_size New size in bytes
 * @return Pointer to reallocated memory, or NULL on failure
 */
#define mem_realloc(ptr, new_size) realloc(ptr, new_size)

/**
 * @brief Free memory (default: free)
 * @param ptr Pointer to free
 */
#define mem_free(ptr) free(ptr)

/**
 * @brief Allocate memory using custom or default allocator
 * @param m_procs Memory management procedures (or NULL for default)
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
#define _mem_alloc(m_procs, size) \
  (m_procs) ? m_procs->malloc(size) : mem_alloc(size)

/**
 * @brief Allocate zeroed memory using custom or default allocator
 * @param m_procs Memory management procedures (or NULL for default)
 * @param e_count Number of elements
 * @param e_size Size of each element
 * @return Pointer to allocated memory, or NULL on failure
 */
#define _mem_calloc(m_procs, e_count, e_size) \
  (m_procs) ? m_procs->calloc(e_count, e_size) : mem_calloc(e_count, e_size)

/**
 * @brief Reallocate memory using custom or default allocator
 * @param m_procs Memory management procedures (or NULL for default)
 * @param ptr Existing pointer
 * @param new_size New size in bytes
 * @return Pointer to reallocated memory, or NULL on failure
 */
#define _mem_realloc(m_procs, ptr, new_size) \
  (m_procs) ? m_procs->realloc(ptr, new_size) : mem_realloc(ptr, new_size)

/**
 * @brief Free memory using custom or default allocator
 * @param m_procs Memory management procedures (or NULL for default)
 * @param ptr Pointer to free
 */
#define _mem_free(m_procs, ptr) (m_procs) ? m_procs->free(ptr) : mem_free(ptr)

/**
 * @brief The invalid size_t for size related operations
 *
 * The maximum value that can be contained by size_t
 */
#define ccol_invalid_size ((size_t)-1)

/**
 * @brief The maximum power of two that can be stored in a size_t
 *
 * on 32 bit archs -> 2^31
 *
 * on 64 bit archs -> 2^63
 *
 * As 2^(#arch_bits) would exceed size_t's storage area, the maximum
 * integer power of two that can be contained by a size_t variable
 * is the half of the 2^(#arch_bits).
 */
#define max_power_of_two_size_t ((size_t)1 << ((sizeof(size_t) * CHAR_BIT) - 1))

/**
 * @brief Maximum element count for collections
 *
 * It is the maximum power of 2 that can be stored by a size_t
 */
#define max_elem_count max_power_of_two_size_t

/* ========================================================================== */
/*                         RETURN VALUE CODES                                 */
/* ========================================================================== */

/**
 * @brief Standard return codes for collection operations
 *
 * All collection functions return one of these codes to indicate success
 * or the specific type of failure. Negative values indicate errors,
 * zero indicates success.
 */
typedef enum ccollections_retval_t {
  ccol_unexpected_failure = -9, /**< Unexpected/unknown error */
  ccol_container_empty,         /**< Container has no elements */
  ccol_container_full,          /**< Container at maximum capacity */
  ccol_timed_out,               /**< Operation timed out */
  ccol_not_permitted,           /**< Operation not allowed in current state */
  ccol_invalid_args,            /**< Invalid arguments provided */
  ccol_key_not_found,           /**< Key does not exist in map */
  ccol_key_already_present, /**< Key already exists (for update operations) */
  ccol_not_enough_memory,   /**< Memory allocation failed */
  ccol_success              /**< Operation succeeded */
} ccol_retval_t;

/**
 * @brief Attribute for automatic cleanup on scope exit
 *
 * Uses GCC/Clang cleanup attribute to call destructor when variable
 * goes out of scope.
 *
 * @param destructor Function to call with pointer to variable
 *
 * Example:
 * @code
 * void cleanup_int(int **p) { free(*p); *p = NULL; }
 * int *ptr _ccol_destructor(cleanup_int) = malloc(sizeof(int));
 * // ptr automatically cleaned up when leaving scope
 * @endcode
 */
#define _ccol_destructor(destructor) __attribute__((cleanup(destructor)))

/* ========================================================================== */
/*                    CUSTOM MEMORY MANAGEMENT TYPES                          */
/* ========================================================================== */

/**
 * @brief Custom malloc function pointer type
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
typedef void *(*ccol_memmgmt_procs_malloc_t)(size_t size);

/**
 * @brief Custom free function pointer type
 * @param ptr Pointer to free
 */
typedef void (*ccol_memmgmt_procs_free_t)(void *ptr);

/**
 * @brief Custom calloc function pointer type
 * @param elem_count Number of elements
 * @param elem_size Size of each element
 * @return Pointer to allocated memory, or NULL on failure
 */
typedef void *(*ccol_memmgmt_procs_calloc_t)(size_t elem_count,
                                             size_t elem_size);

/**
 * @brief Custom realloc function pointer type
 * @param ptr Existing pointer
 * @param size New size in bytes
 * @return Pointer to reallocated memory, or NULL on failure
 */
typedef void *(*ccol_memmgmt_procs_realloc_t)(void *ptr, size_t size);

/**
 * @brief Custom memory management procedures
 *
 * Structure containing custom memory allocation/deallocation functions.
 * All collections can be created with custom memory management by providing
 * this structure.
 *
 * @note All four function pointers must be non-NULL if structure is provided
 * @note Functions should have same semantics as standard
 * malloc/free/calloc/realloc
 */
typedef struct ccol_memmgmt_procs_t {
  ccol_memmgmt_procs_malloc_t malloc;   /**< Custom malloc */
  ccol_memmgmt_procs_free_t free;       /**< Custom free */
  ccol_memmgmt_procs_calloc_t calloc;   /**< Custom calloc */
  ccol_memmgmt_procs_realloc_t realloc; /**< Custom realloc */
} ccol_memmgmt_procs_t;

/* ========================================================================== */
/*                    COMPARISON AND HASHING TYPES                            */
/* ========================================================================== */

/**
 * @brief Custom comparison function type
 *
 * Used for sorting and ordered map implementations. Should return:
 * - Negative if first < second
 * - Zero if first == second
 * - Positive if first > second
 *
 * @param first Pointer to first element
 * @param second Pointer to second element
 * @return Comparison result (negative/zero/positive)
 *
 * Example:
 * @code
 * int compare_ints(const void *a, const void *b) {
 *   int ia = *(const int*)a;
 *   int ib = *(const int*)b;
 *   return (ia > ib) - (ia < ib);
 * }
 * @endcode
 */
typedef int (*ccol_comparison_proc_t)(const void *first, const void *second);

/**
 * @brief Custom hashing function type
 *
 * Used for hash map implementations. Should return a hash value for the
 * given data. Good hash functions distribute values uniformly.
 *
 * @param ptr Pointer to data to hash
 * @return Hash value (unsigned long)
 *
 * Example:
 * @code
 * unsigned long hash_int(const void *p) {
 *   int val = *(const int*)p;
 *   return (unsigned long)val * 2654435761UL;
 * }
 * @endcode
 */
typedef unsigned long (*ccol_hashing_proc_t)(const void *ptr);

/* ========================================================================== */
/*                         MAP KEY-VALUE TYPES                                */
/* ========================================================================== */

/**
 * @brief Key or value pair for map types
 *
 * Generic structure for storing pointers and sizes. Used by all map
 * implementations (chmap, cbmap) to store keys and values of arbitrary
 * types and sizes.
 *
 * @note ptr points to actual data (which is copied into the map)
 * @note size includes null terminator for strings
 */
typedef struct cmap_pair {
  void *ptr;   /**< Pointer to data */
  size_t size; /**< Size of data in bytes */
} cmap_pair;

/**
 * @brief Iterator for map types
 *
 * Generic iterator structure used by all map implementations. Provides
 * access to current key-value pair during iteration.
 *
 * @note Pointers are valid until map is modified
 * @note Iterator must be destroyed when done (or goes out of scope)
 */
typedef struct cmap_iterator {
  cmap_pair *key_pair; /**< Pointer to current key */
  cmap_pair *val_pair; /**< Pointer to current value */
} cmap_iterator;

/* ========================================================================== */
/*                    MEMORY MANAGEMENT UTILITIES                             */
/* ========================================================================== */

/**
 * @brief Verify custom memory management procedures are valid
 *
 * Checks that all required function pointers are non-NULL if a custom
 * memory management structure is provided.
 *
 * @param mmgt_procs Memory management procedures to verify (or NULL)
 * @param err Optional pointer to receive error string
 * @return true if valid or NULL, false if invalid
 *
 * @note If mmgt_procs is NULL, returns true (will use default malloc/free)
 * @note All four functions must be provided if structure is non-NULL
 */
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

/**
 * @brief Populate container's memory management procedures
 *
 * Allocates and copies custom memory management procedures into the
 * container structure. Used during container creation.
 *
 * @param container Container structure to populate
 * @param mmgmt_procs Source memory management procedures (or NULL)
 * @param err Optional pointer to receive error string
 * @return true on success, false on allocation failure
 *
 * @note If mmgmt_procs is NULL, sets container->m_procs to NULL (use defaults)
 * @note Allocates memory for m_procs using the provided allocator
 */
#define ccol_populate_mem_mgmt_procs(container, mmgmt_procs, err)             \
  ({                                                                          \
    bool result = true;                                                       \
    if (mmgmt_procs) {                                                        \
      container->m_procs = mmgmt_procs->malloc(sizeof(ccol_memmgmt_procs_t)); \
      if (!container->m_procs) {                                              \
        if (err) {                                                            \
          *err = CCOL_ERR_STR(                                                \
              "Failed to allocate buffer for memory mgmt buffer");            \
        }                                                                     \
        result = false;                                                       \
      } else {                                                                \
        mem_cpy(container->m_procs, mmgmt_procs,                              \
                sizeof(ccol_memmgmt_procs_t));                                \
      }                                                                       \
    } else {                                                                  \
      container->m_procs = NULL;                                              \
    }                                                                         \
    result;                                                                   \
  })

/* ========================================================================== */
/*                         TYPE INTROSPECTION                                 */
/* ========================================================================== */

/**
 * @brief Check if type is an integral or floating-point type
 *
 * Uses C11 _Generic to determine if a value is a standard numeric type
 * (signed/unsigned integers or floating-point, with or without const).
 *
 * @param x Value to check
 * @return true if numeric type, false otherwise
 *
 * @note Supports: char, short, int, long, long long (signed/unsigned)
 * @note Supports: float, double, long double
 * @note Supports: const variants of all above types
 */
#if defined __clang__
#define is_integral_type(x)                                                 \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool result = _Generic((x),                                             \
        char: true,                                                         \
        short: true,                                                        \
        int: true,                                                          \
        long: true,                                                         \
        long long: true,                                                    \
        unsigned char: true,                                                \
        unsigned short: true,                                               \
        unsigned int: true,                                                 \
        unsigned long: true,                                                \
        unsigned long long: true,                                           \
        float: true,                                                        \
        double: true,                                                       \
        long double: true,                                                  \
        const char: true,                                                   \
        const short: true,                                                  \
        const int: true,                                                    \
        const long: true,                                                   \
        const long long: true,                                              \
        const unsigned char: true,                                          \
        const unsigned short: true,                                         \
        const unsigned int: true,                                           \
        const unsigned long: true,                                          \
        const unsigned long long: true,                                     \
        const float: true,                                                  \
        const double: true,                                                 \
        const long double: true,                                            \
        default: false);                                                    \
    _Pragma("GCC diagnostic pop");                                          \
    result;                                                                 \
  })
#else
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
#endif

/**
 * @brief Check if type is a pointer to integral or floating-point type
 *
 * Uses C11 _Generic to determine if a pointer points to a standard
 * numeric type.
 *
 * @param x Pointer to check
 * @return true if pointer to numeric type, false otherwise
 *
 * @note Supports pointers to all types checked by is_integral_type()
 */
#if defined __clang__
#define is_integral_ptr(x)                                                  \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool result = _Generic((x),                                             \
        char *: true,                                                       \
        short *: true,                                                      \
        int *: true,                                                        \
        long *: true,                                                       \
        long long *: true,                                                  \
        unsigned char *: true,                                              \
        unsigned short *: true,                                             \
        unsigned int *: true,                                               \
        unsigned long *: true,                                              \
        unsigned long long *: true,                                         \
        float *: true,                                                      \
        double *: true,                                                     \
        long double *: true,                                                \
        const char *: true,                                                 \
        const short *: true,                                                \
        const int *: true,                                                  \
        const long *: true,                                                 \
        const long long *: true,                                            \
        const unsigned char *: true,                                        \
        const unsigned short *: true,                                       \
        const unsigned int *: true,                                         \
        const unsigned long *: true,                                        \
        const unsigned long long *: true,                                   \
        const float *: true,                                                \
        const double *: true,                                               \
        const long double *: true,                                          \
        default: false);                                                    \
    result;                                                                 \
    _Pragma("GCC diagnostic pop");                                          \
  })
#else
#define is_integral_ptr(x)              \
  _Generic((x),                         \
      char *: true,                     \
      short *: true,                    \
      int *: true,                      \
      long *: true,                     \
      long long *: true,                \
      unsigned char *: true,            \
      unsigned short *: true,           \
      unsigned int *: true,             \
      unsigned long *: true,            \
      unsigned long long *: true,       \
      float *: true,                    \
      double *: true,                   \
      long double *: true,              \
      const char *: true,               \
      const short *: true,              \
      const int *: true,                \
      const long *: true,               \
      const long long *: true,          \
      const unsigned char *: true,      \
      const unsigned short *: true,     \
      const unsigned int *: true,       \
      const unsigned long *: true,      \
      const unsigned long long *: true, \
      const float *: true,              \
      const double *: true,             \
      const long double *: true,        \
      default: false)
#endif

/**
 * @brief Check if pointer points to signed integer type
 *
 * Uses C11 _Generic to determine if a pointer points to a signed
 * integer type. Used by BST map to determine key comparison behavior.
 *
 * @param _ptr Pointer to check
 * @return true if pointer to signed integer, false otherwise
 *
 * @note Returns true for: char*, short*, int*, long*, long long*
 * @note Returns true for const variants
 * @note Returns false for unsigned types and non-integers
 */
#define __is_signed_int_ptr(_ptr) \
  _Generic((_ptr),                \
      char *: true,               \
      short *: true,              \
      int *: true,                \
      long *: true,               \
      long long *: true,          \
      const char *: true,         \
      const short *: true,        \
      const int *: true,          \
      const long *: true,         \
      const long long *: true,    \
      default: false)

/**
 * @brief Check if data is a char pointer (string)
 *
 * Uses C11 _Generic to determine if data is a pointer to char type.
 * Handles both signed and unsigned char pointers.
 *
 * @param data Value to check
 * @return true if char pointer, false otherwise
 *
 * @note Clang version suppresses unreachable code warnings
 * @note Treats unsigned char* as string type
 * @note Returns false for char arrays (use is_char_array() for those)
 */
#if defined __clang__
#define is_char_ptr(data)                                                   \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool result = _Generic((data),                                          \
        char *: true,                                                       \
        const char *: true,                                                 \
        unsigned char *: true,                                              \
        const unsigned char *: true,                                        \
        default: false);                                                    \
    _Pragma("GCC diagnostic pop");                                          \
    result;                                                                 \
  })

/**
 * @brief Check if data is a char array (not pointer to pointer)
 *
 * Distinguishes between char arrays (char arr[]) and char pointers (char*).
 *
 * @param data Value to check
 * @return true if char array, false if pointer or other type
 *
 * @note Uses nested _Generic to check if &data is char**
 * @note Useful for determining string storage semantics
 */
#define is_char_array(data)                   \
  (is_char_ptr((data)) && _Generic((&(data)), \
       char **: false,                        \
       const char **: false,                  \
       unsigned char **: false,               \
       const unsigned char **: false,         \
       default: true))
#else
#define is_char_ptr(data)          \
  _Generic((data),                 \
      char *: true,                \
      const char *: true,          \
      unsigned char *: true,       \
      const unsigned char *: true, \
      default: false)

#define is_char_array(data)                   \
  (is_char_ptr((data)) && _Generic((&(data)), \
       char **: false,                        \
       const char **: false,                  \
       unsigned char **: false,               \
       const unsigned char **: false,         \
       default: true))
#endif

typedef enum ccollections_data_type {
  ccol_char = 0,
  ccol_short,
  ccol_int,
  ccol_long,
  ccol_long_long,
  ccol_unsigned_char,
  ccol_unsigned_short,
  ccol_unsigned_int,
  ccol_unsigned_long,
  ccol_unsigned_long_long,
  ccol_float,
  ccol_double,
  ccol_long_double,
  ccol_pointer,
  ccol_string,
  ccol_other_types,
} ccol_data_type;

#if defined __clang__
#define _determine_non_special_data_type(var)                               \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    ccol_data_type result = _Generic((var),                                 \
        char: ccol_char,                                                    \
        short: ccol_short,                                                  \
        int: ccol_int,                                                      \
        long: ccol_long,                                                    \
        long long: ccol_long_long,                                          \
        unsigned char: ccol_unsigned_char,                                  \
        unsigned short: ccol_unsigned_short,                                \
        unsigned int: ccol_unsigned_int,                                    \
        unsigned long: ccol_unsigned_long,                                  \
        unsigned long long: ccol_unsigned_long_long,                        \
        float: ccol_float,                                                  \
        double: ccol_double,                                                \
        long double: ccol_long_double,                                      \
        const char: ccol_char,                                              \
        const short: ccol_short,                                            \
        const int: ccol_int,                                                \
        const long: ccol_long,                                              \
        const long long: ccol_long_long,                                    \
        const unsigned char: ccol_unsigned_char,                            \
        const unsigned short: ccol_unsigned_short,                          \
        const unsigned int: ccol_unsigned_int,                              \
        const unsigned long: ccol_unsigned_long,                            \
        const unsigned long long: ccol_unsigned_long_long,                  \
        const float: ccol_float,                                            \
        const double: ccol_double,                                          \
        const long double: ccol_long_double,                                \
        default: ccol_other_types);                                         \
    _Pragma("GCC diagnostic pop");                                          \
    result;                                                                 \
  })
#else
#define _determine_non_special_data_type(var)            \
  _Generic((var),                                        \
      char: ccol_char,                                   \
      short: ccol_short,                                 \
      int: ccol_int,                                     \
      long: ccol_long,                                   \
      long long: ccol_long_long,                         \
      unsigned char: ccol_unsigned_char,                 \
      unsigned short: ccol_unsigned_short,               \
      unsigned int: ccol_unsigned_int,                   \
      unsigned long: ccol_unsigned_long,                 \
      unsigned long long: ccol_unsigned_long_long,       \
      float: ccol_float,                                 \
      double: ccol_double,                               \
      long double: ccol_long_double,                     \
      const char: ccol_char,                             \
      const short: ccol_short,                           \
      const int: ccol_int,                               \
      const long: ccol_long,                             \
      const long long: ccol_long_long,                   \
      const unsigned char: ccol_unsigned_char,           \
      const unsigned short: ccol_unsigned_short,         \
      const unsigned int: ccol_unsigned_int,             \
      const unsigned long: ccol_unsigned_long,           \
      const unsigned long long: ccol_unsigned_long_long, \
      const float: ccol_float,                           \
      const double: ccol_double,                         \
      const long double: ccol_long_double,               \
      default: ccol_other_types)
#endif

#define determine_ccol_data_type(data)                                 \
  ({                                                                   \
    ccol_data_type r = ccol_other_types;                               \
    if (is_char_array(data)) {                                         \
      r = ccol_string;                                                 \
    } else if (is_char_ptr(data)) {                                    \
      r = ccol_string;                                                 \
    } else {                                                           \
      if (__builtin_classify_type((data)) == 5 && /* is a pointer */   \
          sizeof((data)) == sizeof(uintptr_t)) {  /* other pointers */ \
        r = ccol_pointer;                                              \
      } else {                                                         \
        r = _determine_non_special_data_type(data);                    \
      }                                                                \
    }                                                                  \
    r;                                                                 \
  })

/* ========================================================================== */
/*                    MAP PAIR POPULATION UTILITY                             */
/* ========================================================================== */

/**
 * @brief Populate a cmap_pair from data of any type
 *
 * Automatically determines the correct way to populate a cmap_pair based
 * on the data type. Handles strings (char arrays and pointers) specially
 * to include null terminators.
 *
 * @param pair Pointer to cmap_pair to populate
 * @param data Data to store (can be value, array, or pointer)
 *
 * @note For char arrays: stores pointer to array, size includes null terminator
 * @note For char pointers: dereferences to get string, size includes null
 * terminator
 * @note For other types: stores pointer to data, size is sizeof(data)
 * @note GCC array-bounds warning is suppressed for char pointer dereferencing
 *
 * Example:
 * @code
 * cmap_pair kp, vp;
 * int key = 42;
 * char value[] = "hello";
 * _populate_cmap_pair(&kp, key);     // kp.ptr = &key, kp.size = 4
 * _populate_cmap_pair(&vp, value);   // vp.ptr = value, vp.size = 6
 * @endcode
 */
#define _populate_cmap_pair(pair, data)                     \
  do {                                                      \
    if (is_char_array(data)) {                              \
      pair->ptr = (char *)&(data);                          \
      pair->size = strlen((char *)pair->ptr) + 1;           \
    } else if (is_char_ptr(data)) {                         \
      char *_ptr = (char *)&data;                           \
      _Pragma("GCC diagnostic push");                       \
      _Pragma("GCC diagnostic ignored \"-Warray-bounds\""); \
      pair->ptr = *((char **)_ptr);                         \
      _Pragma("GCC diagnostic pop");                        \
      pair->size = strlen((char *)pair->ptr) + 1;           \
    } else {                                                \
      pair->ptr = &(data);                                  \
      pair->size = sizeof((data));                          \
    }                                                       \
  } while (0)

/* ========================================================================== */
/*                         UTILITY MACROS                                     */
/* ========================================================================== */

/**
 * @brief Return minimum of two values
 *
 * Evaluates both arguments and returns the smaller value.
 *
 * @param a First value
 * @param b Second value
 * @return The smaller of a and b
 *
 * @warning Arguments may be evaluated multiple times
 */
#define ccol_min(a, b) ((a) < (b) ? (a) : (b))

/**
 * @brief Return maximum of two values
 *
 * Evaluates both arguments and returns the larger value.
 *
 * @param a First value
 * @param b Second value
 * @return The larger of a and b
 *
 * @warning Arguments may be evaluated multiple times
 */
#define ccol_max(a, b) ((a) > (b) ? (a) : (b))

/**
 * @brief Type-safe comparison macro
 *
 * Compares two values of the same type and returns standard comparison result.
 * Casts pointers to the specified type before comparison.
 *
 * @param ptr1 Pointer to first value
 * @param ptr2 Pointer to second value
 * @param T Type to cast to
 * @return Negative if *ptr1 < *ptr2, 0 if equal, positive if *ptr1 > *ptr2
 *
 * @note Uses three-way comparison: (a > b) - (a < b)
 * @note Result is -1, 0, or 1 for integer types
 */
#define ccol_typed_cmp(ptr1, ptr2, T) \
  ({                                  \
    T var1 = *(T *)(ptr1);            \
    T var2 = *(T *)(ptr2);            \
    (var1 > var2) - (var1 < var2);    \
  })

/* ========================================================================== */
/*                    OPTIMIZED UTILITY FUNCTIONS                             */
/* ========================================================================== */

/**
 * @brief Find nearest power of two greater than or equal to input
 *
 * Searches a precomputed table of powers of two (2^0 through 2^63) and
 * returns the smallest power of two that is >= input. If input exceeds
 * the largest power of two (2^63), returns uint64_invalid_size to indicate
 * an error condition.
 *
 * @param input Value to round up to power of two
 * @return Nearest power of two >= input, or uint64_invalid_size if too large
 *
 * @note O(1) lookup
 * @note Returns 1 for input 0 or 1
 * @note Returns uint64_invalid_size if input > 2^63
 * @note Useful for capacity calculations in dynamic containers
 *
 * Example:
 * @code
 * find_nearest_gte_power_of_two(5)   -> 8
 * find_nearest_gte_power_of_two(16)  -> 16
 * find_nearest_gte_power_of_two(100) -> 128
 * find_nearest_gte_power_of_two(UINT64_MAX) -> uint64_invalid_size
 * @endcode
 */
size_t find_nearest_gte_power_of_two(size_t input);

/**
 * @brief Optimized memory copy for small and large buffers
 *
 * Uses direct assignments for small sizes (1-8 bytes) and falls back to
 * memcpy for larger buffers. Small copies use packed structs for optimal
 * performance and avoid function call overhead.
 *
 * @param dst Destination pointer (must not overlap with src)
 * @param src Source pointer
 * @param n Number of bytes to copy
 *
 * @note For n <= 32: Uses direct uint8/16/32/64 assignments
 * @note For n > 32: Falls back to standard memcpy
 * @note Does not handle overlapping regions (use memmove for that)
 * @note Inlined for optimal performance
 *
 * @warning Behavior undefined if src and dst overlap
 *
 * Example:
 * @code
 * int src = 42;
 * int dst;
 * mem_cpy(&dst, &src, sizeof(int));  // Optimized for 4 bytes
 * @endcode
 */
void mem_cpy(void *dst, const void *src, size_t n);

/**
 * @brief Optimized memory zeroing for small and large buffers
 *
 * Uses direct zero assignments for small sizes (1-8 bytes) and falls back
 * to memset for larger buffers. Small zeros use packed structs for optimal
 * performance and avoid function call overhead.
 *
 * @param dst Destination pointer to zero
 * @param n Number of bytes to zero
 *
 * @note For n <= 32: Uses direct zero assignments to uint8/16/32/64
 * @note For n > 32: Falls back to memset(dst, 0, n)
 * @note Inlined for optimal performance
 *
 * Example:
 * @code
 * int array[100];
 * mem_zero(array, sizeof(array));  // Zeros entire array
 * @endcode
 */
void mem_zero(void *dst, size_t n);
