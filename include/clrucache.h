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

#include <common.h>
#include <stdbool.h>

/**
 * @file clrucache.h
 * @brief Thread-safe generic LRU cache with optional remote source integration
 *
 * An LRU cache backed by a hash map (O(1) lookup) and a doubly-linked list
 * (O(1) eviction). All operations are fully thread-safe via a single global
 * mutex and per-entry condition variables.
 *
 * Key concurrency guarantees:
 * - Multiple getters for the same uncached key coalesce: only one remote fetch
 *   executes; all others block and receive the same result once it completes.
 * - Getters block while a setter is in progress for the same key, so they
 *   always read a consistent value.
 * - Multiple setters for the same key are serialized.
 *
 * Set semantics: the remote setter (if provided) is always called before the
 * cache is updated. If the remote call fails the cache is not updated and
 * clrucache_set_full() returns ccol_unexpected_failure.
 *
 * Limitations / caller responsibilities:
 * - The eviction callback is invoked while holding the cache mutex and MUST
 *   NOT call back into the cache (deadlock).
 * - clrucache_destroy() should only be called once all other threads have
 *   stopped using the cache.
 */

/* ========================================================================== */
/*                         CALLBACK TYPES                                     */
/* ========================================================================== */

/**
 * @brief Remote getter callback
 *
 * Called when the requested key is not in the cache and a remote source
 * exists. On success the implementation must heap-allocate the value data,
 * assign it to val->ptr, and set val->size to the byte length. The cache
 * takes ownership of val->ptr and will free it. On failure the callback
 * must leave val->ptr as NULL and return false.
 *
 * @param key  Key pair (ptr + size)
 * @param val  Output pair to populate on success (ptr + size)
 * @return true on success, false on failure
 */
typedef bool (*clru_remote_getter_t)(const cmap_pair *key, cmap_pair *val);

/**
 * @brief Remote setter callback
 *
 * Attempts to persist a key-value pair to the remote source.
 *
 * @param key  Key pair (ptr + size)
 * @param val  Value pair (ptr + size)
 * @return true on success, false on failure
 */
typedef bool (*clru_remote_setter_t)(const cmap_pair *key,
                                     const cmap_pair *val);

/**
 * @brief Eviction callback
 *
 * Invoked synchronously (while the cache mutex is held) when an entry is
 * evicted to make room for a new one. Must not call back into the cache.
 *
 * @param key  Evicted key pair (ptr + size)
 * @param val  Evicted value pair (ptr + size)
 */
typedef void (*clru_eviction_cb_t)(const cmap_pair *key, const cmap_pair *val);

/* ========================================================================== */
/*                         OPAQUE TYPE                                        */
/* ========================================================================== */

/** @brief Opaque LRU cache structure */
typedef struct clrucache clrucache;

/** @brief Handle type (pointer to opaque struct) */
typedef clrucache *clru_cache;

/* ========================================================================== */
/*                         CREATION / DESTRUCTION                             */
/* ========================================================================== */

/**
 * @brief Create an LRU cache
 *
 * @param capacity     Maximum number of live entries before eviction occurs
 * @param key_type     ccol_data_type of keys (from determine_ccol_data_type)
 * @param val_type     ccol_data_type of values (from determine_ccol_data_type)
 * @param getter       Remote getter (may be NULL)
 * @param setter       Remote setter (may be NULL). If non-NULL, it is called
 *                     synchronously before each cache update.
 * @param eviction_cb  Called on eviction (may be NULL)
 * @param mprocs       Custom allocator, or NULL for malloc/free
 * @param err          Optional: set to error string on failure
 * @return New cache handle, or NULL on failure
 */
clru_cache clrucache_create_full(size_t capacity, ccol_data_type key_type,
                                 ccol_data_type val_type,
                                 clru_remote_getter_t getter,
                                 clru_remote_setter_t setter,
                                 clru_eviction_cb_t eviction_cb,
                                 ccol_memmgmt_procs_t *mprocs, char **err);

/**
 * @brief Destroy a cache (internal - use clru_destroy() macro)
 *
 * Evicts all remaining entries (calling the eviction callback for each) and
 * frees all memory. Caller must ensure no other thread is blocked inside the
 * cache.
 */
void __clrucache_destroy(clru_cache cache);

/**
 * @brief Copy a value directly into a caller-provided buffer (internal —
 *        use clru_get() macro instead)
 *
 * Unlike clrucache_get_full(), this function performs no heap allocation: it
 * copies the stored value into buf while holding the cache mutex and returns.
 * buf_size must be >= the stored value size; the clru_get() macro satisfies
 * this by passing sizeof(*val_ptr), which always matches ValT.
 */
ccol_retval_t __clrucache_get_into(clru_cache cache, const cmap_pair *key_pair,
                                   void *buf, size_t buf_size);

/* ========================================================================== */
/*                         OPERATIONS                                         */
/* ========================================================================== */

/**
 * @brief Get a value by key
 *
 * On success, heap-allocates a copy of the stored value (using standard
 * malloc), assigns it to val_out->ptr, and sets val_out->size to the stored
 * byte size. The caller is responsible for freeing val_out->ptr with free().
 *
 * On a cache miss, if a remote getter was provided it is called once (other
 * concurrent getters for the same key block until it completes). The fetched
 * value is stored in the cache and returned to all waiters.
 *
 * Blocks if a setter is in progress for this key (both sync and async modes
 * block until the cache is in a consistent state).
 *
 * @param cache     Cache handle
 * @param key_pair  Key to look up
 * @param val_out   On success: val_out->ptr is a heap-allocated copy of the
 *                  value (caller must free() it); val_out->size is the stored
 *                  value size
 * @return ccol_success, ccol_key_not_found, ccol_invalid_args, or
 *         ccol_not_enough_memory
 */
ccol_retval_t clrucache_get_full(clru_cache cache, const cmap_pair *key_pair,
                                 cmap_pair *val_out);

/**
 * @brief Set a value by key
 *
 * If a remote setter was provided at construction, it is called first. On
 * success the cache is updated. On failure ccol_unexpected_failure is returned
 * and the cache is left unchanged for that key.
 *
 * Multiple setters for the same key are serialized. Concurrent getters for
 * the same key block until the set completes (cache is consistent).
 *
 * @param cache    Cache handle
 * @param key_pair Key to set
 * @param val_pair Value to associate with the key
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_unexpected_failure (sync remote failure)
 */
ccol_retval_t clrucache_set_full(clru_cache cache, const cmap_pair *key_pair,
                                 const cmap_pair *val_pair);

/* ========================================================================== */
/*                         DESTROY MACRO                                      */
/* ========================================================================== */

static inline void ___clrucache_destroy(clru_cache *cp) {
  if (cp && *cp) {
    __clrucache_destroy(*cp);
    *cp = NULL;
  }
}

#define clru_destroy(name)       \
  do {                           \
    __clrucache_destroy((name)); \
    (name) = NULL;               \
  } while (0)

/* ========================================================================== */
/*                    TYPE-TRACKING DECLARE / INIT / CONSTRUCT                */
/* ========================================================================== */

/**
 * @brief Declare an uninitialized cache variable with type tracking
 *
 * Must be followed by clru_init() before use.
 */
#define clru_declare(name, KeyT, ValT)                                    \
  typeof(KeyT) *name##__clru_key_type_var __attribute__((unused)) = NULL; \
  typeof(ValT) *name##__clru_val_type_var __attribute__((unused)) = NULL; \
  clru_cache name

/**
 * @brief Declare with automatic cleanup on scope exit
 */
#define clru_declare_scoped(name, KeyT, ValT)                             \
  typeof(KeyT) *name##__clru_key_type_var __attribute__((unused)) = NULL; \
  typeof(ValT) *name##__clru_val_type_var __attribute__((unused)) = NULL; \
  clru_cache name _ccol_destructor(___clrucache_destroy)

/**
 * @brief Initialize a previously declared cache variable
 *
 * @param name       Cache variable (must have been declared with clru_declare)
 * @param capacity   Maximum live entries before eviction
 * @param getter     Remote getter (may be NULL)
 * @param setter     Remote setter (may be NULL)
 * @param evict_cb   Eviction callback (may be NULL)
 */
#define clru_init(name, capacity, getter, setter, evict_cb)               \
  do {                                                                    \
    char *_clru_err = NULL;                                               \
    (name) = clrucache_create_full(                                       \
        (capacity), determine_ccol_data_type(*name##__clru_key_type_var), \
        determine_ccol_data_type(*name##__clru_val_type_var), (getter),   \
        (setter), (evict_cb), NULL, &_clru_err);                          \
    if (!(name)) {                                                        \
      fatal_err("clru_init('%s'): %s", #name,                             \
                _clru_err ? _clru_err : "unknown error");                 \
    }                                                                     \
  } while (0)

/**
 * @brief Declare and initialize in one step (most common usage)
 *
 * Example:
 * @code
 * clru_construct(my_cache, int, double, 128, NULL, NULL, NULL);
 * int k = 42; double v = 3.14;
 * clru_set(my_cache, k, v);
 * double out;
 * clru_get(my_cache, k, &out, sizeof(out));
 * clru_destroy(my_cache);
 * @endcode
 */
#define clru_construct(name, KeyT, ValT, capacity, getter, setter, evict_cb) \
  typeof(KeyT) *name##__clru_key_type_var __attribute__((unused)) = NULL;    \
  typeof(ValT) *name##__clru_val_type_var __attribute__((unused)) = NULL;    \
  clru_cache name = NULL;                                                    \
  do {                                                                       \
    char *_clru_err = NULL;                                                  \
    (name) = clrucache_create_full(                                          \
        (capacity), determine_ccol_data_type(*name##__clru_key_type_var),    \
        determine_ccol_data_type(*name##__clru_val_type_var), (getter),      \
        (setter), (evict_cb), NULL, &_clru_err);                             \
    if (!(name)) {                                                           \
      fatal_err("clru_construct('%s'): %s", #name,                           \
                _clru_err ? _clru_err : "unknown error");                    \
    }                                                                        \
  } while (0)

/**
 * @brief Declare, initialize, and auto-destroy on scope exit
 */
#define clru_construct_scoped(name, KeyT, ValT, capacity, getter, setter,  \
                              evict_cb)                                    \
  typeof(KeyT) *name##__clru_key_type_var __attribute__((unused)) = NULL; \
  typeof(ValT) *name##__clru_val_type_var __attribute__((unused)) = NULL; \
  clru_cache name _ccol_destructor(___clrucache_destroy) = NULL;          \
  do {                                                                    \
    char *_clru_err = NULL;                                               \
    (name) = clrucache_create_full(                                       \
        (capacity), determine_ccol_data_type(*name##__clru_key_type_var), \
        determine_ccol_data_type(*name##__clru_val_type_var), (getter),   \
        (setter), (evict_cb), NULL, &_clru_err);                          \
    if (!(name)) {                                                        \
      fatal_err("clru_construct_scoped('%s'): %s", #name,                 \
                _clru_err ? _clru_err : "unknown error");                 \
    }                                                                     \
  } while (0)

/**
 * @brief Restore type-tracking variables for a cache passed across scopes
 *
 * Example:
 * @code
 * void use_cache(clru_cache c) {
 *   clru_redeclare(c, int, double);
 *   double out;
 *   clru_get(c, 42, &out, sizeof(out));
 * }
 * @endcode
 */
#define clru_redeclare(name, KeyT, ValT)                                  \
  typeof(KeyT) *name##__clru_key_type_var __attribute__((unused)) = NULL; \
  typeof(ValT) *name##__clru_val_type_var __attribute__((unused)) = NULL

/* ========================================================================== */
/*                    TYPE-SAFE GET / SET MACROS                              */
/* ========================================================================== */

/**
 * @brief Get a value by key (type-safe convenience wrapper)
 *
 * For non-char* val types: copies the value into *val_ptr and frees the
 * internal heap allocation. For char* val types: transfers ownership of the
 * heap-allocated string to *(char **)val_ptr; the caller must free() it.
 *
 * When passing the cache across scopes (e.g. into a thread function), declare
 * a local alias and use clru_redeclare() before calling clru_get() so that
 * the val-type detection works correctly.
 *
 * @param name     Cache variable (must have been declared with clru_construct
 *                 or clru_declare, or restored with clru_redeclare)
 * @param key      Key expression (lvalue or literal string)
 * @param val_ptr  Pointer to the value type (ValT *), NOT cmap_pair *.
 *                 For non-char* val types: e.g. double * for a double-valued
 *                 cache; the value is copied in and the internal allocation
 *                 freed — no heap visible to the caller.
 *                 For char* val types: pointer to a char* variable; ownership
 *                 of the heap-allocated string is transferred to the caller,
 *                 who must free() it.
 * @return ccol_retval_t (ccol_success or ccol_key_not_found etc.)
 *
 * Example:
 * @code
 * int k = 5; int v = 0;
 * if (clru_get(cache, k, &v) == ccol_success) { ... }
 *
 * char *str = NULL;
 * if (clru_get(str_cache, k, &str) == ccol_success) { free(str); }
 * @endcode
 */
#define clru_get(name, key, val_ptr)                                         \
  ({                                                                         \
    __typeof__(key) _clru_k = (key);                                         \
    cmap_pair _clru_kp = {};                                                 \
    _populate_cmap_pair(&_clru_kp, _clru_k);                                 \
    ccol_retval_t _clru_r;                                                   \
    if (is_char_ptr(*(name##__clru_val_type_var))) {                         \
      /* char* path: heap-allocate a copy and transfer ownership to caller */\
      cmap_pair _clru_vout = {};                                              \
      _clru_r = clrucache_get_full((name), &_clru_kp, &_clru_vout);          \
      if (_clru_r == ccol_success && _clru_vout.ptr) {                       \
        /* Write the heap pointer via memcpy to avoid strict-aliasing        \
         * violations when val_ptr is not char**. */                         \
        void *_clru_str_out = _clru_vout.ptr;                                \
        memcpy((val_ptr), &_clru_str_out, sizeof(void *));                   \
      }                                                                      \
    } else {                                                                 \
      /* Non-char* path: copy directly into *val_ptr — no heap allocation */ \
      _clru_r = __clrucache_get_into((name), &_clru_kp,                      \
                                     (val_ptr), sizeof(*(val_ptr)));         \
    }                                                                        \
    _clru_r;                                                                 \
  })

/**
 * @brief Set a value by key (type-safe convenience wrapper)
 *
 * Uses the policy chosen at construction (clru_set_sync / clru_set_async).
 *
 * @param name   Cache variable
 * @param key    Key expression
 * @param val    Value expression
 * @return ccol_retval_t
 *
 * Example:
 * @code
 * int k = 5; double v = 2.71;
 * clru_set(cache, k, v);
 * @endcode
 */
#define clru_set(name, key, val)                           \
  ({                                                       \
    __typeof__(key) _clru_k = (key);                       \
    __typeof__(val) _clru_v = (val);                       \
    cmap_pair _clru_kp = {};                               \
    cmap_pair _clru_vp = {};                               \
    _populate_cmap_pair(&_clru_kp, _clru_k);               \
    _populate_cmap_pair(&_clru_vp, _clru_v);               \
    clrucache_set_full((name), &_clru_kp, &_clru_vp);      \
  })

/**
 * @brief Return the number of live entries currently in the cache
 */
size_t clrucache_size(clru_cache cache);

/**
 * @brief Return the capacity of the cache
 */
size_t clrucache_capacity(clru_cache cache);
