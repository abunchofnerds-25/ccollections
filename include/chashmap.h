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
#include <string.h>

/**
 * @file chashmap.h
 * @brief Hash map (dictionary) with automatic resizing and dual implementation
 * strategy
 *
 * Provides a hash map implementation with automatic selection between two
 * strategies:
 *
 * **Open-addressing** (used when both key and value are integral types ≤8
 * bytes):
 * - Compact 17-byte slots (8-byte key + 8-byte value + 1-byte metadata)
 * - Linear probing with Fibonacci hashing for integers
 * - Load factor thresholds: 0.70 (grow) / 0.25 (shrink)
 * - Zero allocations per entry (contiguous array)
 * - Good cache locality and memory efficiency
 * - Excludes long double (can be >8 bytes on some architectures)
 *
 * **Separate chaining** (used for non-integral types or types >8 bytes):
 * - Linked lists for collision resolution
 * - Small String Optimization (SSO): 23-byte inline storage for keys/values
 * - Doubly-linked list for maintaining insertion order and iteration
 * - Minimum bucket array size: 64 (always power-of-2)
 * - Scale factor: 4x (grows to 4x size, shrinks to 0.25x size)
 * - Scale up threshold: (bucket_count + 1) * 1.5 elements
 * - Scale down threshold: (bucket_count + 1) / 8 elements
 *
 * Common features:
 * - Automatic implementation selection based on key and value types
 * - Custom hashing function support (default: XXHash64 for buffers, Fibonacci
 * for integers)
 * - Type-safe macros for common operations
 * - Average complexity: O(1) for insert/get/delete
 *
 * Implementation selection examples:
 * - int→int, long→double, float→uint32_t: Open-addressing
 * - string→int, int→string, string→string, int→long double: Separate chaining
 */

/** @brief Default initial bucket array size */
#define DEFAULT_INITIAL_BUCKET_ARRAY_SIZE 64

/** @brief Opaque hash map structure */
typedef struct chashmap chashmap;

/** @brief Pointer to hash map (handle type) */
typedef chashmap *chmap;

/* ========================================================================== */
/*                         HASH MAP CREATION                                  */
/* ========================================================================== */

/**
 * @brief Create a hash map with full customization
 *
 * Creates a new hash map with specified initial bucket count, key and value
 * types, custom memory management, and custom hashing function. The
 * implementation strategy (open-addressing vs separate chaining) is
 * automatically selected based on the key and value types.
 *
 * @param initial_bucket_array_size Initial number of buckets (minimum 64)
 * @param key_type Type of keys (determines implementation strategy)
 * @param val_type Type of values (determines implementation strategy)
 * @param mmgmt_procs Custom memory management procedures, or NULL for default
 * malloc/free
 * @param custom_hashing_proc Custom hash function, or NULL for default
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created hash map, or NULL on failure
 *
 * @note Implementation selection:
 *       - Open-addressing: Both key and value are integral types ≤8 bytes
 *       - Separate chaining: Either key or value is non-integral or >8 bytes
 * @note Integral types: char, short, int, long, long long (signed/unsigned),
 * float, double
 * @note Excludes long double from open-addressing (can be 10-16 bytes)
 * @note Open-addressing uses Fibonacci hashing for integers, XXHash64 for
 * buffers
 * @note Separate chaining default is XXHash64 for all types
 * @note Map must be destroyed with chmap_destroy() when done
 *
 * @see chmap_create
 * @see chmap_create_mp
 * @see chmap_create_ch
 * @see chmap_destroy
 */
chmap chmap_create_full(size_t initial_bucket_array_size,
                        ccol_data_type key_type, ccol_data_type val_type,
                        ccol_memmgmt_procs_t *mmgmt_procs,
                        ccol_hashing_proc_t custom_hashing_proc, char **err);

/**
 * @brief Create a hash map with default settings
 *
 * Convenience wrapper for chmap_create_full() with default memory management
 * and default hashing (Fibonacci for integral keys, XXHash64 for others).
 *
 * @param initial_bucket_array_size Initial number of buckets
 * @param key_type Type of keys (determines implementation strategy)
 * @param val_type Type of values (determines implementation strategy)
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created hash map, or NULL on failure
 *
 * @note See chmap_create_full() for implementation selection details
 */
static inline __attribute__((always_inline)) chmap
chmap_create(size_t initial_bucket_array_size, ccol_data_type key_type,
             ccol_data_type val_type, char **err) {
  return chmap_create_full(initial_bucket_array_size, key_type, val_type, NULL,
                           NULL, err);
}

/**
 * @brief Create a hash map with custom memory management
 *
 * Convenience wrapper for chmap_create_full() with custom memory management
 * but default hashing.
 *
 * @param initial_bucket_array_size Initial number of buckets
 * @param key_type Type of keys (determines implementation strategy)
 * @param val_type Type of values (determines implementation strategy)
 * @param mmgmt_procs Custom memory management procedures
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created hash map, or NULL on failure
 *
 * @note See chmap_create_full() for implementation selection details
 */
static inline __attribute__((always_inline)) chmap chmap_create_mp(
    size_t initial_bucket_array_size, ccol_data_type key_type,
    ccol_data_type val_type, ccol_memmgmt_procs_t *mmgmt_procs, char **err) {
  return chmap_create_full(initial_bucket_array_size, key_type, val_type,
                           mmgmt_procs, NULL, err);
}

/**
 * @brief Create a hash map with custom hashing function
 *
 * Convenience wrapper for chmap_create_full() with custom hashing function
 * but default memory management.
 *
 * @param initial_bucket_array_size Initial number of buckets
 * @param key_type Type of keys (determines implementation strategy)
 * @param val_type Type of values (determines implementation strategy)
 * @param custom_hashing_proc Custom hash function
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created hash map, or NULL on failure
 *
 * @note See chmap_create_full() for implementation selection details
 */
static inline __attribute__((always_inline)) chmap
chmap_create_ch(size_t initial_bucket_array_size, ccol_data_type key_type,
                ccol_data_type val_type,
                ccol_hashing_proc_t custom_hashing_proc, char **err) {
  return chmap_create_full(initial_bucket_array_size, key_type, val_type, NULL,
                           custom_hashing_proc, err);
}

/* ========================================================================== */
/*                         HASH MAP OPERATIONS                                */
/* ========================================================================== */

/**
 * @brief Get the number of elements in the map
 *
 * Returns the current number of key-value pairs stored in the hash map.
 *
 * @param chm Hash map to query
 *
 * @return Number of elements in the map
 *
 * @note Will assert if chm is NULL
 * @note O(1) complexity
 */
size_t chmap_elem_count(chmap chm);

/**
 * @brief Clear all elements and optionally resize the map
 *
 * Removes all key-value pairs from the map and destroys all internal data
 * structures. Optionally resizes the bucket/slot array to a new size.
 *
 * @param chm Hash map to reset
 * @param new_bucket_array_size New capacity (0 to keep current size)
 *
 * @return ccol_success on success
 * @return ccol_not_enough_memory if resize fails (elements still cleared)
 *
 * @note All elements are destroyed regardless of return value
 * @note If new_bucket_array_size is 0, array size remains unchanged
 * @note If new_bucket_array_size < 64, it's set to 64
 * @note Otherwise rounded to nearest_power_of_2(new_bucket_array_size)
 * @note Will assert if chm is NULL
 *
 * @see chmap_destroy
 */
ccol_retval_t chmap_reset(chmap chm, size_t new_bucket_array_size);

/**
 * @brief Insert or update a key-value pair (upsert)
 *
 * Inserts a new key-value pair into the map, or updates the value if the key
 * already exists. Automatically resizes the map if load factor exceeds
 * threshold. Keys and values are copied into the map.
 *
 * @param chm Hash map to insert into
 * @param key_pair Key to insert (ptr and size must be valid)
 * @param val_pair Value to insert (ptr and size must be valid)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if any pointer is NULL or size is 0
 * @return ccol_container_full if max_elem_count reached
 * @return ccol_not_enough_memory if allocation fails
 *
 * @note O(1) average complexity
 * @note Open-addressing: O(n) worst case for linear probing
 * @note Separate chaining: O(n) worst case per bucket (where n is chain length)
 * @note Key and value data are copied (not referenced)
 * @note If key exists, only value is updated (key remains unchanged)
 * @note Open-addressing: Triggers resize at 0.70 load factor
 * @note Separate chaining: Triggers resize if elem_count >= (bucket_count + 1)
 * * 1.5
 * @note Scale factor is 2x for open-addressing, 4x for separate chaining
 *
 * @see chmap_get_elem_copy
 * @see chmap_get_elem_ref
 * @see chmap_delete_elem
 */
ccol_retval_t chmap_insert_elem(chmap chm, const cmap_pair *key_pair,
                                const cmap_pair *val_pair);

/**
 * @brief Get a copy of the value associated with a key
 *
 * Retrieves a copy of the value for the specified key into the provided buffer.
 * Copies min(value_size, target_buf_size) bytes to handle size mismatches.
 *
 * @param chm Hash map to search
 * @param key_pair Key to look up
 * @param target_buf Buffer to receive value copy
 * @param target_buf_size Size of target buffer
 *
 * @return ccol_success if key found and value copied
 * @return ccol_invalid_args if any pointer is NULL or size is 0
 * @return ccol_key_not_found if key does not exist
 *
 * @note O(1) average complexity
 * @note Open-addressing: O(n) worst case for linear probing
 * @note Separate chaining: O(n) worst case per bucket (where n is chain length)
 * @note Copies min(actual_value_size, target_buf_size) bytes
 * @note Safe to use with undersized buffers (partial copy)
 *
 * @see chmap_get_elem_ref
 * @see chmap_insert_elem
 */
ccol_retval_t chmap_get_elem_copy(chmap chm, const cmap_pair *key_pair,
                                  void *target_buf, size_t target_buf_size);

/**
 * @brief Get a reference to the value associated with a key
 *
 * Retrieves a pointer to the value pair structure for the specified key.
 * The returned pointer is valid until the map is modified
 * (insert/delete/resize).
 *
 * @param chm Hash map to search
 * @param key_pair Key to look up
 * @param val_pair Output parameter to receive pointer to value pair
 *
 * @return ccol_success if key found
 * @return ccol_invalid_args if any pointer is NULL or key size is 0
 * @return ccol_key_not_found if key does not exist
 *
 * @note O(1) average complexity
 * @note Open-addressing: O(n) worst case for linear probing
 * @note Separate chaining: O(n) worst case per bucket (where n is chain length)
 * @note Returned pointer is invalidated by insert/delete/resize operations
 * @note Do not free the returned pointer - it's owned by the map
 * @note Can modify value in-place, but do not change size
 *
 * @see chmap_get_elem_copy
 * @see chmap_insert_elem
 */
ccol_retval_t chmap_get_elem_ref(chmap chm, const cmap_pair *key_pair,
                                 cmap_pair **val_pair);

/**
 * @brief Delete a key-value pair from the map
 *
 * Removes the specified key and its associated value from the map.
 * Automatically resizes down if load factor drops below threshold.
 *
 * @param chm Hash map to delete from
 * @param key_pair Key to delete
 *
 * @return ccol_success if key found and deleted
 * @return ccol_invalid_args if any pointer is NULL or key size is 0
 * @return ccol_key_not_found if key does not exist
 *
 * @note O(1) average complexity
 * @note Open-addressing: O(n) worst case for linear probing, uses tombstones
 * @note Separate chaining: O(n) worst case per bucket (where n is chain length)
 * @note Frees memory allocated for key and value
 * @note Open-addressing: Triggers resize at 0.25 load factor
 * @note Separate chaining: Triggers resize if elem_count < (bucket_count + 1) /
 * 8
 * @note Scale factor is 0.5x for open-addressing, 0.25x for separate chaining
 * @note Won't resize below minimum threshold
 *
 * @see chmap_insert_elem
 * @see chmap_reset
 */
ccol_retval_t chmap_delete_elem(chmap chm, const cmap_pair *key_pair);

/* ========================================================================== */
/*                         HASH MAP ITERATION                                 */
/* ========================================================================== */

/**
 * @brief Begin iteration over the hash map
 *
 * Creates an iterator positioned at the first element. For separate chaining
 * maps, iteration follows insertion order via the doubly-linked list. For
 * open-addressing maps, iteration follows slot order.
 *
 * @param chm Hash map to iterate over
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to iterator, or NULL if map is empty or allocation fails
 *
 * @note Separate chaining: Iterates in insertion order via doubly-linked list
 * @note Open-addressing: Iterates in slot order (not insertion order)
 * @note Iterator must be destroyed with chmap_iter_destroy() or will
 * auto-destroy at end
 * @note Modifying map during iteration invalidates the iterator
 * @note Will assert if chm is NULL
 * @note Returns NULL if map is empty (not an error)
 *
 * @see chmap_begin (macro wrapper)
 * @see chmap_iter_next
 * @see chmap_iter_destroy
 */
cmap_iterator *chashmap_begin_iter(chmap chm, char **err);

void __chmap_iterator_destroy(cmap_iterator *iter);

/* ========================================================================== */
/*                         HASH MAP DESTRUCTION                               */
/* ========================================================================== */

/**
 * @brief Destroy a hash map (internal function)
 *
 * @param chm Hash map to destroy
 *
 * @warning Do not call directly - use chmap_destroy() macro instead
 */
void __chmap_destroy(chmap chm);

/**
 * @brief Internal cleanup function for automatic map destruction
 *
 * @param chm Pointer to hash map pointer
 *
 * @note Used by _ccol_destructor attribute
 * @warning Do not call directly
 */
static inline void ___chmap_destroy(chmap *chm) {
  if (*chm) {
    __chmap_destroy(*chm);
    *chm = NULL;
  }
}

/**
 * @brief Destroy a hash map and set pointer to NULL
 *
 * Frees all resources associated with the hash map including all keys,
 * values, buckets, and internal structures.
 *
 * @param chm Hash map to destroy (will be set to NULL)
 *
 * @note Safe to call with NULL
 * @note Frees all key and value data
 * @note Destroys all bucket collision chains
 */
#define chmap_destroy(chm)  \
  do {                      \
    __chmap_destroy((chm)); \
    chm = NULL;             \
  } while (0)

/* ========================================================================== */
/*                    TYPE-SAFE CONVENIENCE MACROS                            */
/* ========================================================================== */

/**
 * @brief Enable type-safe macros for an existing hash map
 *
 * Declares type variables needed for type-safe macro operations when using
 * a hash map that was created in another scope.
 *
 * @param hm_name Hash map variable name
 * @param key_t Key type
 * @param val_t Value type
 *
 * Example:
 * @code
 * void process(chmap map) {
 *   chmap_redeclare(map, int, char*);
 *   chmap_insert(map, 42, "hello");
 * }
 * @endcode
 */
#define chmap_redeclare(hm_name, key_t, val_t)                                \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL

/**
 * @brief Declare an uninitialized hash map variable
 *
 * Declares a hash map variable and associated type variables for type-safe
 * macro operations. The map must be initialized before use.
 *
 * @param hm_name Hash map variable name
 * @param key_t Key type
 * @param val_t Value type
 *
 * @note Map must be initialized with chmap_init*() before use
 *
 * @see chmap_init
 * @see chmap_construct
 */
#define chmap_declare(hm_name, key_t, val_t)                                  \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */

#define chmap_declare_scoped(hm_name, key_t, val_t)                           \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name _ccol_destructor(___chmap_destroy)

/**
 * @brief Initialize a hash map with full customization
 *
 * Initializes a previously declared hash map with custom memory management
 * and custom hashing. Calls fatal_err() on failure. Key and value types are
 * automatically detected from the type variables created by chmap_declare.
 *
 * @param hm_name Hash map variable to initialize (must be declared)
 * @param mmgmt_procs Custom memory management procedures
 * @param custom_hashing_proc Custom hash function
 *
 * @note Terminates program on failure
 * @note Uses DEFAULT_INITIAL_BUCKET_ARRAY_SIZE (64)
 * @note Automatically detects key and value types to select implementation
 *
 * @see chmap_declare
 * @see chmap_construct_full
 */
#define chmap_init_full(hm_name, mmgmt_procs, custom_hashing_proc) \
  do {                                                             \
    char *err = NULL;                                              \
    hm_name = chmap_create_full(                                   \
        DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                         \
        determine_ccol_data_type(*hm_name##__ccol_key_type_var),   \
        determine_ccol_data_type(*hm_name##__ccol_val_type_var),   \
        (mmgmt_procs), (custom_hashing_proc), &err);               \
    if (!hm_name) {                                                \
      fatal_err("Failed to create hash map '%s': %s", #hm_name,    \
                err ? err : "unknown error");                      \
    }                                                              \
  } while (0)

/**
 * @brief Declare and initialize a hash map with full customization
 *
 * Combines declaration and initialization with custom memory management
 * and custom hashing. Calls fatal_err() on failure. Key and value types
 * are specified explicitly and determine the implementation strategy.
 *
 * @param hm_name Hash map variable name
 * @param key_t Key type
 * @param val_t Value type
 * @param mmgmt_procs Custom memory management procedures
 * @param custom_hashing_proc Custom hash function
 *
 * @note Terminates program on failure
 * @note Uses DEFAULT_INITIAL_BUCKET_ARRAY_SIZE (64)
 * @note Implementation automatically selected based on key_t and val_t
 *
 * @see chmap_init_full
 */
#define chmap_construct_full(hm_name, key_t, val_t, mmgmt_procs,              \
                             custom_hashing_proc)                             \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;              \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name = chmap_create_full(                                              \
        DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                                    \
        determine_ccol_data_type(*hm_name##__ccol_key_type_var),              \
        determine_ccol_data_type(*hm_name##__ccol_val_type_var),              \
        (mmgmt_procs), (custom_hashing_proc), &err);                          \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create hash map '%s': %s", #hm_name,               \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

#define chmap_construct_full_scoped(hm_name, key_t, val_t, mmgmt_procs,       \
                                    custom_hashing_proc)                      \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name _ccol_destructor(___chmap_destroy) = NULL;                    \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name = chmap_create_full(                                              \
        DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                                    \
        determine_ccol_data_type(*hm_name##__ccol_key_type_var),              \
        determine_ccol_data_type(*hm_name##__ccol_val_type_var),              \
        (mmgmt_procs), (custom_hashing_proc), &err);                          \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create hash map '%s': %s", #hm_name,               \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

/**
 * @brief Initialize a hash map with defaults
 *
 * Initializes a previously declared hash map with default settings.
 * Key and value types are automatically detected from the type variables.
 *
 * @param hm_name Hash map variable to initialize
 *
 * @note Terminates program on failure
 * @note Uses default memory management and automatic hashing selection
 * @note Implementation automatically selected based on detected types
 */
#define chmap_init(hm_name)                                             \
  do {                                                                  \
    char *err = NULL;                                                   \
    hm_name = chmap_create(                                             \
        DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                              \
        determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        determine_ccol_data_type(*hm_name##__ccol_val_type_var), &err); \
    if (!hm_name) {                                                     \
      fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                err ? err : "unknown error");                           \
    }                                                                   \
  } while (0)

/**
 * @brief Declare and initialize a hash map with defaults
 *
 * Combines declaration and initialization with default settings.
 * Implementation strategy is automatically selected based on key and value
 * types.
 *
 * @param hm_name Hash map variable name
 * @param key_t Key type
 * @param val_t Value type
 *
 * @note Terminates program on failure
 * @note Open-addressing used for: int→int, long→double, float→uint32_t, etc.
 * @note Separate chaining used for: string→int, int→string, string→string, etc.
 *
 * Example:
 * @code
 * chmap_construct(ages, char*, int);      // string→int: separate chaining
 * chmap_insert(ages, "Alice", 30);
 * chmap_insert(ages, "Bob", 25);
 * int age = chmap_get(ages, "Alice");     // age == 30
 * chmap_destroy(ages);
 *
 * chmap_construct(counters, int, int);    // int→int: open-addressing
 * chmap_insert(counters, 1, 100);
 * int count = chmap_get(counters, 1);     // count == 100
 * chmap_destroy(counters);
 * @endcode
 */
#define chmap_construct(hm_name, key_t, val_t)                                \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;              \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name = chmap_create(                                                   \
        DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                                    \
        determine_ccol_data_type(*hm_name##__ccol_key_type_var),              \
        determine_ccol_data_type(*hm_name##__ccol_val_type_var), &err);       \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create hash map '%s': %s", #hm_name,               \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

#define chmap_construct_scoped(hm_name, key_t, val_t)                         \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name _ccol_destructor(___chmap_destroy) = NULL;                    \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name = chmap_create(                                                   \
        DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                                    \
        determine_ccol_data_type(*hm_name##__ccol_key_type_var),              \
        determine_ccol_data_type(*hm_name##__ccol_val_type_var), &err);       \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create hash map '%s': %s", #hm_name,               \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

/**
 * @brief Initialize a hash map with custom memory management
 *
 * Initializes a previously declared hash map with custom memory management.
 * Key and value types are automatically detected from the type variables.
 *
 * @param hm_name Hash map variable to initialize
 * @param mmgmt_procs Custom memory management procedures
 *
 * @note Terminates program on failure
 * @note Uses default hashing (automatic selection based on types)
 */
#define chmap_init_mp(hm_name, mmgmt_procs)                      \
  do {                                                           \
    char *err = NULL;                                            \
    hm_name = chmap_create_mp(                                   \
        DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                       \
        determine_ccol_data_type(*hm_name##__ccol_key_type_var), \
        determine_ccol_data_type(*hm_name##__ccol_val_type_var), \
        (mmgmt_procs), &err);                                    \
    if (!hm_name) {                                              \
      fatal_err("Failed to create hash map '%s': %s", #hm_name,  \
                err ? err : "unknown error");                    \
    }                                                            \
  } while (0)

/**
 * @brief Declare and initialize a hash map with custom memory management
 *
 * Combines declaration and initialization with custom memory management.
 *
 * @param hm_name Hash map variable name
 * @param key_t Key type
 * @param val_t Value type
 * @param mmgmt_procs Custom memory management procedures
 *
 * @note Terminates program on failure
 */
#define chmap_construct_mp(hm_name, key_t, val_t, mmgmt_procs)                \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;              \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name = chmap_create_mp(                                                \
        DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                                    \
        determine_ccol_data_type(*hm_name##__ccol_key_type_var),              \
        determine_ccol_data_type(*hm_name##__ccol_val_type_var),              \
        (mmgmt_procs), &err);                                                 \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create hash map '%s': %s", #hm_name,               \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

#define chmap_construct_mp_scoped(hm_name, key_t, val_t, mmgmt_procs)         \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name _ccol_destructor(___chmap_destroy) = NULL;                    \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name = chmap_create_mp(                                                \
        DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                                    \
        determine_ccol_data_type(*hm_name##__ccol_key_type_var),              \
        determine_ccol_data_type(*hm_name##__ccol_val_type_var),              \
        (mmgmt_procs), &err);                                                 \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create hash map '%s': %s", #hm_name,               \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

/**
 * @brief Initialize a hash map with custom hashing
 *
 * Initializes a previously declared hash map with custom hashing function.
 *
 * @param hm_name Hash map variable to initialize
 * @param custom_hashing_proc Custom hash function
 *
 * @note Terminates program on failure
 * @note Uses default memory management
 */
#define chmap_init_ch(hm_name, custom_hashing_proc)              \
  do {                                                           \
    char *err = NULL;                                            \
    hm_name = chmap_create_ch(                                   \
        DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                       \
        determine_ccol_data_type(*hm_name##__ccol_key_type_var), \
        determine_ccol_data_type(*hm_name##__ccol_val_type_var), \
        (custom_hashing_proc), &err);                            \
    if (!hm_name) {                                              \
      fatal_err("Failed to create hash map '%s': %s", #hm_name,  \
                err ? err : "unknown error");                    \
    }                                                            \
  } while (0)

/**
 * @brief Declare and initialize a hash map with custom hashing
 *
 * Combines declaration and initialization with custom hashing function.
 *
 * @param hm_name Hash map variable name
 * @param key_t Key type
 * @param val_t Value type
 * @param custom_hashing_proc Custom hash function
 *
 * @note Terminates program on failure
 */
#define chmap_construct_ch(hm_name, key_t, val_t, custom_hashing_proc)        \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;              \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name = chmap_create_ch(                                                \
        DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                                    \
        determine_ccol_data_type(*hm_name##__ccol_key_type_var),              \
        determine_ccol_data_type(*hm_name##__ccol_val_type_var),              \
        (custom_hashing_proc), &err);                                         \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create hash map '%s': %s", #hm_name,               \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

#define chmap_construct_ch_scoped(hm_name, key_t, val_t, custom_hashing_proc) \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name _ccol_destructor(___chmap_destroy) = NULL;                    \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name = chmap_create_ch(                                                \
        DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                                    \
        determine_ccol_data_type(*hm_name##__ccol_key_type_var),              \
        determine_ccol_data_type(*hm_name##__ccol_val_type_var),              \
        (custom_hashing_proc), &err);                                         \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create hash map '%s': %s", #hm_name,               \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

/* ========================================================================== */
/*                    TYPE-SAFE OPERATION MACROS                              */
/* ========================================================================== */

/**
 * @brief Insert a key-value pair (type-safe)
 *
 * Type-safe wrapper for chmap_insert_elem() that automatically handles
 * type conversion and cmap_pair creation. Calls fatal_err() on failure.
 *
 * @param hm_name Hash map to insert into
 * @param key Key to insert
 * @param val Value to associate with key
 *
 * @note Terminates program on failure
 * @note Automatically takes address of key and value
 * @note Handles both value types and string types correctly
 *
 * @see chmap_insert_elem
 * @see chmap_get
 * @see chmap_remove
 *
 * Example:
 * @code
 * chmap_construct(map, int, char*);
 * chmap_insert(map, 42, "hello");
 * chmap_insert(map, 100, "world");
 * @endcode
 */
#define chmap_insert(hm_name, key, val)                               \
  do {                                                                \
    cmap_pair *key_pair = &(cmap_pair){};                             \
    cmap_pair *val_pair = &(cmap_pair){};                             \
    _populate_cmap_pair(key_pair, (key));                             \
    _populate_cmap_pair(val_pair, (val));                             \
    ccol_retval_t r = chmap_insert_elem(hm_name, key_pair, val_pair); \
    if (r != ccol_success && r != ccol_key_already_present) {         \
      _ccol_dump_key_to_stderr(key_pair->ptr, key_pair->size);        \
      fatal_err("chmap_insert('%s'): r: %d (%s)", #hm_name, r,        \
                ccol_retval_to_str(r));                               \
    }                                                                 \
  } while (0)

/**
 * @brief Remove a key-value pair (type-safe)
 *
 * Type-safe wrapper for chmap_delete_elem() that returns the result code.
 *
 * @param hm_name Hash map to remove from
 * @param key Key to remove
 *
 * @return ccol_success if removed, ccol_key_not_found if not found
 *
 * @note Does not terminate on key_not_found
 * @note Frees memory for key and value
 *
 * @see chmap_delete_elem
 * @see chmap_insert
 *
 * Example:
 * @code
 * ccol_retval_t r = chmap_remove(map, 42);
 * if (r == ccol_key_not_found) {
 *   printf("Key not found\n");
 * }
 * @endcode
 */
#define chmap_remove(hm_name, key)                          \
  ({                                                        \
    cmap_pair *key_pair = &(cmap_pair){};                   \
    _populate_cmap_pair(key_pair, (key));                   \
    ccol_retval_t r = chmap_delete_elem(hm_name, key_pair); \
    r;                                                      \
  })

/**
 * @brief Get value by key (type-safe, returns value)
 *
 * Type-safe wrapper for chmap_get_elem_ref() that returns the actual value.
 * Calls fatal_err() if key not found or size mismatch.
 *
 * @param hm_name Hash map to search
 * @param key Key to look up
 *
 * @return Value associated with key
 *
 * @note Terminates program if key not found
 * @note Terminates program if value size doesn't match type size
 * @note Returns value, not pointer
 * @note For strings, returns the char* itself
 *
 * @see chmap_get_ptr
 * @see chmap_insert
 *
 * Example:
 * @code
 * chmap_construct(map, int, char*);
 * chmap_insert(map, 42, "hello");
 * char* val = chmap_get(map, 42); // val == "hello"
 * @endcode
 */
#define chmap_get(hm_name, key)                                             \
  ({                                                                        \
    typeof(*hm_name##__ccol_val_type_var) *val = NULL;                      \
    cmap_pair *key_pair = &(cmap_pair){};                                   \
    cmap_pair *val_pair = NULL;                                             \
    _populate_cmap_pair(key_pair, (key));                                   \
    ccol_retval_t r = chmap_get_elem_ref(hm_name, key_pair, &val_pair);     \
    if (r != ccol_success) {                                                \
      _ccol_dump_key_to_stderr(key_pair->ptr, key_pair->size);              \
      fatal_err("chmap_get('%s'): r: %d (%s)", #hm_name, r,                 \
                ccol_retval_to_str(r));                                     \
    }                                                                       \
    if (is_char_ptr(*hm_name##__ccol_val_type_var)) {                       \
      val = (typeof(*hm_name##__ccol_val_type_var) *)&(val_pair->ptr);      \
    } else if (val_pair->size != sizeof(*val)) {                            \
      fatal_err(                                                            \
          "chmap_get('%s'): value size mismatch — stored: %lu bytes, "      \
          "requested: %lu bytes; wrong type or missing chmap_redeclare()?", \
          #hm_name, (unsigned long)val_pair->size,                          \
          (unsigned long)sizeof(*val));                                     \
    } else {                                                                \
      val = (typeof(*hm_name##__ccol_val_type_var) *)(val_pair->ptr);       \
    }                                                                       \
    *val;                                                                   \
  })

/**
 * @brief Get pointer to value by key (type-safe, returns pointer or NULL)
 *
 * Type-safe wrapper for chmap_get_elem_ref() that returns a pointer to the
 * value, or NULL if key not found. Unlike chmap_get(), does not terminate
 * on key_not_found.
 *
 * @param hm_name Hash map to search
 * @param key Key to look up
 *
 * @return Pointer to value, or NULL if key not found
 *
 * @note Returns NULL if key not found (does not terminate)
 * @note Terminates program if value size doesn't match type size
 * @note Returns pointer to value for in-place modification
 * @note Pointer will get invalidated by later insert/delete/resize operations
 * @note For strings, returns pointer to the char* itself
 *
 * @see chmap_get
 * @see chmap_get_elem_ref
 *
 * Example:
 * @code
 * chmap_construct(map, int, int);
 * chmap_insert(map, 42, 100);
 * int* ptr = chmap_get_ptr(map, 42);
 * if (ptr) {
 *   *ptr = 200; // Modify in-place
 * }
 * @endcode
 */
#define chmap_get_ptr(hm_name, key)                                           \
  ({                                                                          \
    typeof(*hm_name##__ccol_val_type_var) *val = NULL;                        \
    cmap_pair *key_pair = &(cmap_pair){};                                     \
    cmap_pair *val_pair = NULL;                                               \
    _populate_cmap_pair(key_pair, (key));                                     \
    ccol_retval_t r = chmap_get_elem_ref(hm_name, key_pair, &val_pair);       \
    if (r == ccol_success) {                                                  \
      if (is_char_ptr(*hm_name##__ccol_val_type_var)) {                       \
        val = (typeof(*hm_name##__ccol_val_type_var) *)&(val_pair->ptr);      \
      } else if (val_pair->size != sizeof(*val)) {                            \
        fatal_err(                                                            \
            "chmap_get_ptr('%s'): value size mismatch — stored: %lu bytes, "  \
            "requested: %lu bytes; wrong type or missing chmap_redeclare()?", \
            #hm_name, (unsigned long)val_pair->size,                          \
            (unsigned long)sizeof(*val));                                     \
      } else {                                                                \
        val = (typeof(*hm_name##__ccol_val_type_var) *)(val_pair->ptr);       \
      }                                                                       \
    }                                                                         \
    val;                                                                      \
  })

#include <citerators.h>
