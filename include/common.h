/*
MIT License

Copyright (c) 2026 - A bunch of nerds

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
 * - Threading primitives (mutex, rwlock, condition variables, one-time
 *   initialization, thread creation/join, thread-local storage keys, fork
 *   handler registration)
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
#include <ctype.h>
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
#define mutex_destroy(m) pthread_mutex_destroy(&(m))

/** @brief Initialize a mutex with default attributes */
#define mutex_init(m) pthread_mutex_init(&(m), NULL)

/** @brief Lock a mutex (blocking) */
#define mutex_lock(m) pthread_mutex_lock(&(m))

/** @brief Unlock a mutex */
#define mutex_unlock(m) pthread_mutex_unlock(&(m))

/** @brief Read-write lock type (wraps pthread_rwlock_t) */
#define rw_lock_t pthread_rwlock_t

/** @brief Destroy a read-write lock */
#define rw_lock_destroy(a) pthread_rwlock_destroy(&(a))

/** @brief Initialize a read-write lock with default attributes */
#define rw_lock_init(a) pthread_rwlock_init(&(a), NULL)

/** @brief Acquire write lock (exclusive access) */
#define rw_lock_wrlock(a) pthread_rwlock_wrlock(&(a))

/** @brief Acquire read lock (shared access) */
#define rw_lock_rdlock(a) pthread_rwlock_rdlock(&(a))

/** @brief Release read-write lock */
#define rw_lock_unlock(a) pthread_rwlock_unlock(&(a))

/** @brief Condition variable type (wraps pthread_cond_t) */
#define cond_var_t pthread_cond_t

/** @brief Condition variable attributes type (wraps pthread_cond_attr_t) */
#define cond_var_attr_t pthread_condattr_t

/** @brief Destroy condition variable attributes */
#define cond_var_attr_destroy(ca) pthread_condattr_destroy(&(ca))

/** @brief Initialize condition variable attributes */
#define cond_var_attr_init(ca) pthread_condattr_init(&(ca))

/** @brief Set the clock type of condition variable attributes */
#define cond_var_attr_setclock(ca, clk) pthread_condattr_setclock(&(ca), clk)

/** @brief Destroy a condition variable */
#define cond_var_destroy(c) pthread_cond_destroy(&(c))

/** @brief Initialize a condition variable with default attributes */
#define cond_var_init(c) pthread_cond_init(&(c), NULL)

/** @brief Initialize a condition variable with custom attributes */
#define cond_var_init_ca(c, ca) pthread_cond_init(&(c), &(ca))

/** @brief Wait on condition variable (releases mutex while waiting) */
#define cond_var_wait(c, m) pthread_cond_wait(&(c), &(m))

/** @brief Timed wait on condition variable with absolute timeout */
#define cond_var_timedwait(c, m, t) pthread_cond_timedwait(&(c), &(m), &(t))

/** @brief Signal one waiting thread on condition variable */
#define cond_var_signal(c) pthread_cond_signal(&(c))

/** @brief Signal all waiting threads on condition variable */
#define cond_var_broadcast(c) pthread_cond_broadcast(&(c))

/** @brief Thread ID type (wraps pthread_t) */
#define thread_id_t pthread_t

/** @brief Get current thread ID */
#define get_thread_id pthread_self

/**
 * @brief Create a thread running fn(arg), storing its handle in handle
 *
 * Every current call site passes default (NULL) attributes and ignores the
 * return value beyond a success/failure check, so this wrapper hides the
 * attributes argument, mirroring how mutex_init()/cond_var_init() already
 * hide their own NULL-attributes argument.
 */
#define thread_create(handle, fn, arg) \
  pthread_create(&(handle), NULL, (fn), (arg))

/** @brief Block until the thread identified by handle has terminated */
#define thread_join(handle) pthread_join((handle), NULL)

/** @brief One-time-initialization guard type (wraps pthread_once_t) */
#define once_flag_t pthread_once_t

/**
 * @brief Static initializer for a once_flag_t (wraps PTHREAD_ONCE_INIT)
 *
 * Unlike mutex/cond/rwlock, a one-time-init flag is a trivial, universally
 * representable "not yet run" state, not a real synchronization object, so
 * it is the one primitive in this file that may still use a static/constant
 * initializer; it is what makes lazily initializing everything else
 * possible without a chicken-and-egg problem.
 */
#define ONCE_INIT PTHREAD_ONCE_INIT

/** @brief Run fn exactly once across all callers racing the same flag */
#define call_once(flag, fn) pthread_once(&(flag), (fn))

/** @brief Thread-local storage key type (wraps pthread_key_t) */
#define thread_ls_key_t pthread_key_t

/** @brief Create a thread-local storage key with an optional destructor */
#define thread_ls_key_create(key, destructor) \
  pthread_key_create(&(key), (destructor))

/** @brief Delete a thread-local storage key */
#define thread_ls_key_delete(key) pthread_key_delete((key))

/** @brief Set the calling thread's value for a thread-local storage key */
#define thread_ls_set(key, val) pthread_setspecific((key), (val))

/** @brief Get the calling thread's value for a thread-local storage key */
#define thread_ls_get(key) pthread_getspecific((key))

/** @brief Register prepare/parent/child handlers to run around fork(2) */
#define at_fork(prepare, parent, child) \
  pthread_atfork((prepare), (parent), (child))

/**
 * @brief Get a thread's name (wraps pthread_getname_np)
 *
 * Non-portable (Apple/BSD only); used solely by clogger.c's platform
 * fallback branches for systems without a /proc-based thread name lookup.
 */
#define get_thread_name_np(thread, buf, len) \
  pthread_getname_np((thread), (buf), (len))

/**
 * @brief Get a thread's 64-bit numeric id (wraps pthread_threadid_np)
 *
 * Non-portable (Apple-only); used solely by clogger.c's platform fallback
 * branch for systems without a syscall-based thread id lookup.
 */
#define get_thread_id_np(thread, out_id) pthread_threadid_np((thread), (out_id))

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
#define fatal_err(_err_fmt, ...)                                        \
  do {                                                                  \
    fprintf(stderr, "%s:%d: fatal: " _err_fmt "\n", __FILE__, __LINE__, \
            ##__VA_ARGS__);                                             \
    ccol_assert(false);                                                 \
  } while (0)

/* ========================================================================== */
/*                         MEMORY MANAGEMENT                                  */
/* ========================================================================== */

/**
 * @brief Allocate memory (default: malloc)
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
#define mem_alloc(size) malloc((size))

/**
 * @brief Allocate and zero-initialize memory (default: calloc)
 * @param elem_count Number of elements
 * @param elem_size Size of each element
 * @return Pointer to allocated memory, or NULL on failure
 */
#define mem_calloc(elem_count, elem_size) calloc((elem_count), (elem_size))

/**
 * @brief Reallocate memory (default: realloc)
 * @param ptr Existing pointer to reallocate
 * @param new_size New size in bytes
 * @return Pointer to reallocated memory, or NULL on failure
 */
#define mem_realloc(ptr, new_size) realloc((ptr), (new_size))

/**
 * @brief Free memory (default: free)
 * @param ptr Pointer to free
 */
#define mem_free(ptr) free((ptr))

/**
 * @brief Allocate memory using custom or default allocator
 * @param m_procs Memory management procedures (or NULL for default)
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
#define _mem_alloc(m_procs, size) \
  (m_procs) ? (m_procs)->malloc((size)) : mem_alloc((size))

/**
 * @brief Allocate zeroed memory using custom or default allocator
 * @param m_procs Memory management procedures (or NULL for default)
 * @param e_count Number of elements
 * @param e_size Size of each element
 * @return Pointer to allocated memory, or NULL on failure
 */
#define _mem_calloc(m_procs, e_count, e_size)        \
  (m_procs) ? (m_procs)->calloc((e_count), (e_size)) \
            : mem_calloc((e_count), (e_size))

/**
 * @brief Reallocate memory using custom or default allocator
 * @param m_procs Memory management procedures (or NULL for default)
 * @param ptr Existing pointer
 * @param new_size New size in bytes
 * @return Pointer to reallocated memory, or NULL on failure
 */
#define _mem_realloc(m_procs, ptr, new_size)        \
  (m_procs) ? (m_procs)->realloc((ptr), (new_size)) \
            : mem_realloc((ptr), (new_size))

/**
 * @brief Free memory using custom or default allocator
 * @param m_procs Memory management procedures (or NULL for default)
 * @param ptr Pointer to free
 */
#define _mem_free(m_procs, ptr) \
  (m_procs) ? (m_procs)->free((ptr)) : mem_free((ptr))

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
  /* Every enumerator below is given an explicit value, deliberately, even
   * though C would auto-increment them the same way if left implicit:
   * ccol_success == 0 is a load-bearing invariant real call sites depend on
   * (some check "== 0" directly rather than always spelling out
   * ccol_success), and an implicit-value list silently renumbers every
   * later entry (including ccol_success itself) the moment a new
   * enumerator is inserted anywhere but the very end; exactly the
   * regression that occurred here once, caught via a cbstmap test
   * failure that had nothing to do with cbstmap at all. Pin every value
   * explicitly so a future addition cannot reintroduce that class of bug. */
  ccol_unexpected_failure = -18,          /**< Unexpected/unknown error */
  ccol_http_connection_failed = -17,      /**< TCP connection to the server
                                             could not be established */
  ccol_http_host_resolution_failed = -16, /**< DNS or hostname resolution
                                             failed */
  ccol_http_tls_handshake_failed = -15,   /**< TLS/SSL handshake with the server
                                            failed */
  ccol_http_tls_cert_verification_failed = -14, /**< Peer TLS certificate
                                                   could not be verified */
  ccol_http_tls_cert_load_failed = -13, /**< A local certificate/key/CA-bundle
                                          file could not be read or was
                                          malformed */
  ccol_http_too_many_redirects = -12,   /**< HTTP redirect limit was exceeded */
  ccol_http_invalid_url = -11,      /**< URL is malformed or uses an unsupported
                                      scheme */
  ccol_http_transfer_aborted = -10, /**< Network send/receive error or
                                      streaming callback aborted */
  ccol_msg_too_large = -9,       /**< Message data exceeded the configured size
                                   limit */
  ccol_container_empty = -8,     /**< Container has no elements */
  ccol_container_full = -7,      /**< Container at maximum capacity */
  ccol_timed_out = -6,           /**< Operation timed out */
  ccol_not_permitted = -5,       /**< Operation not allowed in current state */
  ccol_invalid_args = -4,        /**< Invalid arguments provided */
  ccol_key_not_found = -3,       /**< Key does not exist in map */
  ccol_key_already_present = -2, /**< Key already exists (for update
                                    operations) */
  ccol_not_enough_memory = -1,   /**< Memory allocation failed */
  ccol_success = 0               /**< Operation succeeded */
} ccol_retval_t;

/** Returns a string literal for @p r, suitable for use in fatal_err() messages.
 */
static inline const char *ccol_retval_to_str(ccol_retval_t r) {
  switch (r) {
    case ccol_success:
      return "ccol_success";
    case ccol_not_enough_memory:
      return "ccol_not_enough_memory";
    case ccol_key_already_present:
      return "ccol_key_already_present";
    case ccol_key_not_found:
      return "ccol_key_not_found";
    case ccol_invalid_args:
      return "ccol_invalid_args";
    case ccol_not_permitted:
      return "ccol_not_permitted";
    case ccol_timed_out:
      return "ccol_timed_out";
    case ccol_container_full:
      return "ccol_container_full";
    case ccol_container_empty:
      return "ccol_container_empty";
    case ccol_msg_too_large:
      return "ccol_msg_too_large";
    case ccol_http_connection_failed:
      return "ccol_http_connection_failed";
    case ccol_http_host_resolution_failed:
      return "ccol_http_host_resolution_failed";
    case ccol_http_tls_handshake_failed:
      return "ccol_http_tls_handshake_failed";
    case ccol_http_tls_cert_verification_failed:
      return "ccol_http_tls_cert_verification_failed";
    case ccol_http_tls_cert_load_failed:
      return "ccol_http_tls_cert_load_failed";
    case ccol_http_too_many_redirects:
      return "ccol_http_too_many_redirects";
    case ccol_http_invalid_url:
      return "ccol_http_invalid_url";
    case ccol_http_transfer_aborted:
      return "ccol_http_transfer_aborted";
    case ccol_unexpected_failure:
      return "ccol_unexpected_failure";
    default:
      return "unknown";
  }
}

/**
 * Hex-dumps @p size bytes at @p data to stderr in xxd-like format (offset,
 * hex columns, printable-ASCII sidebar). Called automatically by cbmap/chmap
 * macros before a fatal_err() on a failed insert/lookup, so the offending key
 * is visible even when it is opaque binary data.
 */
static inline void _ccol_dump_key_to_stderr(const void *data, size_t size) {
  const unsigned char *p = (const unsigned char *)data;
  fprintf(stderr, "Key dump (%zu byte%s):\n", size, size == 1 ? "" : "s");
  for (size_t i = 0; i < size; i += 16) {
    fprintf(stderr, "  %08zx  ", i);
    for (size_t j = 0; j < 16; j++) {
      if (i + j < size)
        fprintf(stderr, "%02x ", p[i + j]);
      else
        fprintf(stderr, "   ");
      if (j == 7) fprintf(stderr, " ");
    }
    fprintf(stderr, " |");
    for (size_t j = 0; j < 16 && i + j < size; j++)
      fprintf(stderr, "%c", isprint(p[i + j]) ? (char)p[i + j] : '.');
    fprintf(stderr, "|\n");
  }
}

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
typedef void *(*ccol_malloc_t)(size_t size);

/**
 * @brief Custom free function pointer type
 * @param ptr Pointer to free
 */
typedef void (*ccol_free_t)(void *ptr);

/**
 * @brief Custom calloc function pointer type
 * @param elem_count Number of elements
 * @param elem_size Size of each element
 * @return Pointer to allocated memory, or NULL on failure
 */
typedef void *(*ccol_calloc_t)(size_t elem_count, size_t elem_size);

/**
 * @brief Custom realloc function pointer type
 * @param ptr Existing pointer
 * @param size New size in bytes
 * @return Pointer to reallocated memory, or NULL on failure
 */
typedef void *(*ccol_realloc_t)(void *ptr, size_t size);

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
  ccol_malloc_t malloc;   /**< Custom malloc */
  ccol_free_t free;       /**< Custom free */
  ccol_calloc_t calloc;   /**< Custom calloc */
  ccol_realloc_t realloc; /**< Custom realloc */
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
  struct cmap_iterator *(*_next_fn)(struct cmap_iterator *); /**< Advance fn */
  void (*_free_fn)(struct cmap_iterator *);                  /**< Destroy fn */
  bool _direct_ptr; /**< true → val_pair->ptr IS the element (vec); false → map
                       SSO rules apply */
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
#define ccol_verify_memmgmt_procs(mmgt_procs, err)                         \
  ({                                                                       \
    bool result = true;                                                    \
    if ((mmgt_procs) && (!(mmgt_procs)->malloc || !(mmgt_procs)->calloc || \
                         !(mmgt_procs)->realloc || !(mmgt_procs)->free)) { \
      if ((err)) {                                                         \
        *(err) = CCOL_ERR_STR(                                             \
            "Detected at least one NULL memory management function");      \
      }                                                                    \
      result = false;                                                      \
    }                                                                      \
    result;                                                                \
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
#define ccol_populate_mem_mgmt_procs(container, mmgmt_procs, err)  \
  ({                                                               \
    bool result = true;                                            \
    if ((mmgmt_procs)) {                                           \
      (container)->m_procs =                                       \
          (mmgmt_procs)->malloc(sizeof(ccol_memmgmt_procs_t));     \
      if (!(container)->m_procs) {                                 \
        if ((err)) {                                               \
          *(err) = CCOL_ERR_STR(                                   \
              "Failed to allocate buffer for memory mgmt buffer"); \
        }                                                          \
        result = false;                                            \
      } else {                                                     \
        mem_cpy((container)->m_procs, (mmgmt_procs),               \
                sizeof(ccol_memmgmt_procs_t));                     \
      }                                                            \
    } else {                                                       \
      (container)->m_procs = NULL;                                 \
    }                                                              \
    result;                                                        \
  })

/**
 * @brief Bookkeeping for ccol_scoped_ptr / ccol_scoped_ptr_mp
 *
 * Not meant to be constructed directly; every field is populated by the
 * scoped_ptr macros themselves. Exists only so the cleanup callback can
 * recover, at scope-exit time, both which allocator freed the pointer and
 * the pointer's current value (which may have been reassigned after
 * declaration, e.g. via a later _mem_alloc call).
 */
typedef struct ccol_scoped_ptr_ctx_t {
  void **ptr_addr;               /**< Address of the guarded pointer var */
  ccol_memmgmt_procs_t *m_procs; /**< Allocator to free it with, or NULL */
} ccol_scoped_ptr_ctx_t;

/**
 * @brief Cleanup callback bound by ccol_scoped_ptr / ccol_scoped_ptr_mp
 * @param ctx Address of the hidden companion variable the macros declare
 */
static inline void _ccol_scoped_ptr_cleanup(ccol_scoped_ptr_ctx_t *ctx) {
  if (ctx->ptr_addr && *ctx->ptr_addr) {
    _mem_free(ctx->m_procs, *ctx->ptr_addr);
    *ctx->ptr_addr = NULL;
  }
}

/**
 * @brief Declare a raw pointer that is freed automatically at scope exit
 *
 * Declares `type *name`, initialised to NULL. Assign to it normally (e.g.
 * via _mem_alloc(mmgmt_procs, size), or plain malloc() if mmgmt_procs is
 * NULL); whichever value name holds when the enclosing scope ends is freed
 * with mmgmt_procs (or the default allocator, via mem_free(), if
 * mmgmt_procs is NULL).
 *
 * @param name Name of the pointer variable to declare
 * @param type Pointee type (e.g. char, struct foo)
 * @param mmgmt_procs Custom allocator used to free name's final value, or
 *                     NULL for the default allocator
 *
 * @note mmgmt_procs is stored by reference, not copied; it must remain
 *       valid for at least as long as name's own enclosing scope, which
 *       holds naturally whenever mmgmt_procs is itself a variable already
 *       in scope at the point of declaration
 * @note If name is reassigned mid-scope, only its final value is freed;
 *       an earlier value must be freed manually before reassigning it,
 *       exactly as with any other cleanup-attribute-based guard
 * @note Expands to two declarations, not one expression; use it as its
 *       own statement, the same constraint every *_construct_scoped macro
 *       in this library already has
 * @note Use ccol_scoped_ptr_release() to hand ownership out of the
 *       enclosing scope instead of having name freed automatically
 *
 * Example:
 * @code
 * void process(ccol_memmgmt_procs_t *mp) {
 *   ccol_scoped_ptr_mp(buf, char, mp);
 *   buf = _mem_alloc(mp, 128);
 *   if (!buf) return;
 *   // buf is freed via mp on every return path below this point
 * }
 * @endcode
 */
#define ccol_scoped_ptr_mp(name, type, mmgmt_procs)               \
  type *name = NULL;                                              \
  ccol_scoped_ptr_ctx_t name##__ccol_scoped_ctx _ccol_destructor( \
      _ccol_scoped_ptr_cleanup) = {(void **)&(name), (mmgmt_procs)}

/**
 * @brief Declare a raw pointer that is freed automatically at scope exit,
 *        using the default allocator
 *
 * Equivalent to ccol_scoped_ptr_mp(name, type, NULL); see that macro for
 * the full contract.
 */
#define ccol_scoped_ptr(name, type) ccol_scoped_ptr_mp(name, type, NULL)

/**
 * @brief Release ownership of a scoped pointer, preventing its auto-free
 *
 * Returns name's current value and sets name to NULL, so the pending
 * cleanup registered by ccol_scoped_ptr / ccol_scoped_ptr_mp becomes a
 * no-op at scope exit. Use this to hand ownership of the pointer out of
 * the enclosing scope (for example, as a function's return value) instead
 * of having it freed there.
 *
 * @param name A pointer previously declared with ccol_scoped_ptr /
 *             ccol_scoped_ptr_mp
 * @return name's value prior to release
 *
 * Example:
 * @code
 * char *build(ccol_memmgmt_procs_t *mp) {
 *   ccol_scoped_ptr_mp(buf, char, mp);
 *   buf = _mem_alloc(mp, 128);
 *   if (!buf) return NULL;
 *   // ... populate buf ...
 *   return ccol_scoped_ptr_release(buf); // caller now owns it
 * }
 * @endcode
 */
#define ccol_scoped_ptr_release(name)          \
  ({                                           \
    typeof(name) __ccol_released_ptr = (name); \
    (name) = NULL;                             \
    __ccol_released_ptr;                       \
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
        signed char: true,                                                  \
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
        const signed char: true,                                            \
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
      signed char: true,              \
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
      const signed char: true,        \
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
        signed char *: true,                                                \
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
        const signed char *: true,                                          \
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
      signed char *: true,              \
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
      const signed char *: true,        \
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
      signed char *: true,        \
      short *: true,              \
      int *: true,                \
      long *: true,               \
      long long *: true,          \
      const char *: true,         \
      const signed char *: true,  \
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
        signed char *: true,                                                \
        const signed char *: true,                                          \
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
       signed char **: false,                 \
       const signed char **: false,           \
       unsigned char **: false,               \
       const unsigned char **: false,         \
       default: true))
#else
#define is_char_ptr(data)          \
  _Generic((data),                 \
      char *: true,                \
      const char *: true,          \
      signed char *: true,         \
      const signed char *: true,   \
      unsigned char *: true,       \
      const unsigned char *: true, \
      default: false)

#define is_char_array(data)                   \
  (is_char_ptr((data)) && _Generic((&(data)), \
       char **: false,                        \
       const char **: false,                  \
       signed char **: false,                 \
       const signed char **: false,           \
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
        signed char: ccol_char,                                             \
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
        const signed char: ccol_char,                                       \
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
      signed char: ccol_char,                            \
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
      const signed char: ccol_char,                      \
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
    if (is_char_array((data))) {                                       \
      r = ccol_string;                                                 \
    } else if (is_char_ptr((data))) {                                  \
      r = ccol_string;                                                 \
    } else {                                                           \
      if (__builtin_classify_type((data)) == 5 && /* is a pointer */   \
          sizeof((data)) == sizeof(uintptr_t)) {  /* other pointers */ \
        r = ccol_pointer;                                              \
      } else {                                                         \
        r = _determine_non_special_data_type((data));                  \
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
    if (is_char_array((data))) {                            \
      (pair)->ptr = (char *)&(data);                        \
      (pair)->size = strlen((char *)(pair)->ptr) + 1;       \
    } else if (is_char_ptr((data))) {                       \
      char *_ptr = (char *)&(data);                         \
      _Pragma("GCC diagnostic push");                       \
      _Pragma("GCC diagnostic ignored \"-Warray-bounds\""); \
      (pair)->ptr = *((char **)_ptr);                       \
      _Pragma("GCC diagnostic pop");                        \
      (pair)->size = strlen((char *)(pair)->ptr) + 1;       \
    } else {                                                \
      (pair)->ptr = &(data);                                \
      (pair)->size = sizeof((data));                        \
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
    T var1 = *(T *)((ptr1));          \
    T var2 = *(T *)((ptr2));          \
    (var1 > var2) - (var1 < var2);    \
  })

/* ========================================================================== */
/*                    OPTIMIZED UTILITY FUNCTIONS                             */
/* ========================================================================== */

/**
 * @brief Find nearest power of two that is greater than or equal to input
 *
 * Searches a precomputed table of powers of two (2^0 through 2^63) and
 * returns the smallest power of two that is >= input. If input exceeds
 * the largest power of two (2^63), returns ccol_invalid_size to indicate
 * an error condition.
 *
 * @param input Value to round up to power of two
 * @return Nearest power of two >= input, or ccol_invalid_size if too large
 *
 * @note O(1) lookup
 * @note Returns 1 for input 0 or 1
 * @note Returns ccol_invalid_size if input > 2^63
 * @note Useful for capacity calculations in dynamic containers
 *
 * Example:
 * @code
 * find_nearest_gte_power_of_two(5)   -> 8
 * find_nearest_gte_power_of_two(16)  -> 16
 * find_nearest_gte_power_of_two(100) -> 128
 * find_nearest_gte_power_of_two(UINT64_MAX) -> ccol_invalid_size
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

/**
 * @brief A strdup variant that uses the custom memory management procs
 *
 * Duplicates the input string using the custom memory allocation function and
 * returns it
 *
 * @param mp Pointer to customer memory management procs
 * @param input The input string
 * @return The pointer to the new buffer containing the copy of input or NULL if
 * memory allocation fails
 *
 * Example:
 * @code
 * char* new_copy = ccol_strdup(mp, "hello");
 * @endcode
 */
static inline __attribute__((always_inline)) char *ccol_strdup(
    ccol_memmgmt_procs_t *mp, const char *input) {
  size_t len = strlen(input) + 1;  // The '\0' at the end
  char *result = (char *)_mem_alloc(mp, len * sizeof(char));
  if (result) {
    mem_cpy(result, input, len);
  }
  return result;
}
