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
#include <string.h>

/**
 * @file chashmap.h
 * @brief Hash map (dictionary) with automatic resizing and separate chaining
 *
 * Provides a hash map implementation with:
 * - Automatic dynamic resizing based on load factor
 * - Separate chaining using linked lists for collision resolution
 * - Doubly-linked list for maintaining insertion order and iteration
 * - Custom hashing function support
 * - Type-safe macros for common operations
 * - DJB2 hash function as default
 *
 * Key characteristics:
 * - Minimum bucket array size: 63 (always power-of-2 minus 1)
 * - Scale factor: 4x (grows to 4x size, shrinks to 0.25x size)
 * - Scale up threshold: (bucket_count + 1) * 1.5 elements
 * - Scale down threshold: (bucket_count + 1) / 8 elements
 * - Average complexity: O(1) for insert/get/delete (with good hash function)
 * - Worst case: O(n) per bucket for collision chains (where n is chain length)
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
 * Creates a new hash map with specified initial bucket count, custom memory
 * management, and custom hashing function. The bucket array size is rounded
 * up to the nearest power-of-2 minus 1 (minimum 63).
 *
 * @param initial_bucket_array_size Initial number of buckets (minimum 64,
 * rounded to power-of-2 - 1)
 * @param mmgmt_procs Custom memory management procedures, or NULL for default
 * malloc/free
 * @param custom_hashing_proc Custom hash function, or NULL for default DJB2
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created hash map, or NULL on failure
 *
 * @note Actual bucket size is max(63,
 * nearest_power_of_2(initial_bucket_array_size) - 1)
 * @note Default hash function is DJB2: hash = 5381; hash = ((hash << 5) + hash)
 * + byte
 * @note Custom hash function receives void* pointer to key data
 * @note Each bucket uses a linked list for collision resolution
 * @note Map must be destroyed with chmap_destroy() when done
 *
 * @see chmap_create
 * @see chmap_create_mp
 * @see chmap_create_ch
 * @see chmap_destroy
 */
chmap chmap_create_full(size_t initial_bucket_array_size,
                        ccol_memmgmt_procs_t *mmgmt_procs,
                        ccol_hashing_proc_t custom_hashing_proc, char **err);

/**
 * @brief Create a hash map with default settings
 *
 * Convenience wrapper for chmap_create_full() with default memory management
 * and default DJB2 hashing.
 *
 * @param initial_bucket_array_size Initial number of buckets
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created hash map, or NULL on failure
 */
static inline __attribute__((always_inline)) chmap
chmap_create(size_t initial_bucket_array_size, char **err) {
  return chmap_create_full(initial_bucket_array_size, NULL, NULL, err);
}

/**
 * @brief Create a hash map with custom memory management
 *
 * Convenience wrapper for chmap_create_full() with custom memory management
 * but default DJB2 hashing.
 *
 * @param initial_bucket_array_size Initial number of buckets
 * @param mmgmt_procs Custom memory management procedures
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created hash map, or NULL on failure
 */
static inline __attribute__((always_inline)) chmap
chmap_create_mp(size_t initial_bucket_array_size,
                ccol_memmgmt_procs_t *mmgmt_procs, char **err) {
  return chmap_create_full(initial_bucket_array_size, mmgmt_procs, NULL, err);
}

/**
 * @brief Create a hash map with custom hashing function
 *
 * Convenience wrapper for chmap_create_full() with custom hashing function
 * but default memory management.
 *
 * @param initial_bucket_array_size Initial number of buckets
 * @param custom_hashing_proc Custom hash function
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created hash map, or NULL on failure
 */
static inline __attribute__((always_inline)) chmap
chmap_create_ch(size_t initial_bucket_array_size,
                ccol_hashing_proc_t custom_hashing_proc, char **err) {
  return chmap_create_full(initial_bucket_array_size, NULL, custom_hashing_proc,
                           err);
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
 * structures. Optionally resizes the bucket array to a new size.
 *
 * @param chm Hash map to reset
 * @param new_bucket_array_size New bucket count (0 to keep current size)
 *
 * @return ccol_success on success
 * @return ccol_not_enough_memory if resize fails (elements still cleared)
 *
 * @note All elements are destroyed regardless of return value
 * @note If new_bucket_array_size is 0, bucket array size remains unchanged
 * @note If new_bucket_array_size < 64, it's set to 63
 * @note Otherwise rounded to nearest_power_of_2(new_bucket_array_size) - 1
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
 * @note O(1) average, O(n) worst case per bucket (where n is chain length)
 * @note Key and value data are copied (not referenced)
 * @note If key exists, only value is updated (key remains unchanged)
 * @note If update changes value size, memory is reallocated
 * @note Triggers resize if elem_count >= (bucket_count + 1) * 1.5
 * @note Scale factor is 4x on resize
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
 * @note O(1) average, O(n) worst case per bucket (where n is chain length)
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
 * @note O(1) average, O(n) worst case per bucket (where n is chain length)
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
 * @note O(1) average, O(n) worst case per bucket (where n is chain length)
 * @note Frees memory allocated for key and value
 * @note Triggers resize if elem_count < (bucket_count + 1) / 8
 * @note Won't resize below minimum threshold (scale_factor * (64 - 1))
 * @note Scale factor is 0.25x on resize down
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
 * Creates an iterator positioned at the first element in insertion order.
 * The hash map maintains a doubly-linked list of all elements for iteration
 * independent of hash bucket organization.
 *
 * @param chm Hash map to iterate over
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to iterator, or NULL if map is empty or allocation fails
 *
 * @note Iterates in insertion order, not hash order
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

/**
 * @brief Begin iteration with automatic error handling
 *
 * Macro wrapper around chashmap_begin_iter() that calls fatal_err() on failure.
 *
 * @param chm Hash map to iterate over
 *
 * @return Iterator positioned at first element, or NULL if map is empty
 *
 * @note Terminates program on allocation failure
 * @note Returns NULL if map is empty (normal case)
 */
#define chmap_begin(chm)                                  \
  ({                                                      \
    char *err;                                            \
    cmap_iterator *iter = chashmap_begin_iter(chm, &err); \
    if (err != NULL) {                                    \
      fatal_err("failed to create iterator: %s", err);    \
    }                                                     \
    iter;                                                 \
  })

/**
 * @brief Declare a type-safe iterator for a hash map
 *
 * Declares an iterator variable with automatic type tracking and cleanup.
 * The iterator has an automatic destructor attribute that cleans it up
 * when it goes out of scope.
 *
 * @param chm Hash map variable name (used to infer key/value types)
 * @param iter Iterator variable name
 *
 * @note Iterator is automatically destroyed when going out of scope
 * @note Use with chmap_construct or chmap_declare to set up type variables
 * @note Type variables are used by chmap_iter_key_ptr and chmap_iter_val_ptr
 *
 * Example:
 * @code
 * chmap_construct(my_map, int, char*);
 * // ... populate map ...
 * for (chmap_iter_declare(my_map, it) = chmap_begin(my_map);
 *      it; it = chmap_iter_next(it)) {
 *   // use it->key_pair and it->val_pair
 * }
 * @endcode
 */
#define chmap_iter_declare(chm, iter)                             \
  typeof(*chm##__chm_key_type_var) *iter##__chm_iter_key_type_var \
      __attribute__((unused)) = NULL;                             \
  typeof(*chm##__chm_val_type_var) *iter##__chm_iter_val_type_var \
      __attribute__((unused)) = NULL;                             \
  cmap_iterator *iter _ccol_destructor(___chmap_iterator_destroy)

/**
 * @brief Advance iterator to next element
 *
 * Moves the iterator to the next element in insertion order. If the end is
 * reached, automatically destroys the iterator and returns NULL.
 *
 * @param iter Current iterator position
 *
 * @return Iterator at next position, or NULL if end reached
 *
 * @note Automatically destroys iterator when returning NULL
 * @note Do not access iterator after it returns NULL
 * @note O(1) complexity (follows linked list)
 *
 * @see chashmap_begin_iter
 * @see chmap_iter_destroy
 */
cmap_iterator *chmap_iter_next(cmap_iterator *iter);

/**
 * @brief Get typed pointer to iterator's key
 *
 * Returns a properly typed pointer to the current key. Handles both
 * char* (string) keys and value keys differently.
 *
 * @param iter Iterator variable
 *
 * @return Const pointer to key value
 *
 * @note For string keys (char*), returns pointer to the char* itself
 * @note For other keys, returns pointer to the key data
 * @note Type is inferred from iterator type variables
 */
#define chmap_iter_key_ptr(iter)                       \
  ({                                                   \
    const typeof(*iter##__chm_iter_key_type_var) *key; \
    if (is_char_ptr(*iter##__chm_iter_key_type_var)) { \
      key = (typeof(key))(&it->key_pair->ptr);         \
    } else {                                           \
      key = (typeof(key))(it->key_pair->ptr);          \
    }                                                  \
    key;                                               \
  })

/**
 * @brief Get typed pointer to iterator's value
 *
 * Returns a properly typed pointer to the current value. Handles both
 * char* (string) values and value types differently.
 *
 * @param iter Iterator variable
 *
 * @return Pointer to value
 *
 * @note For string values (char*), returns pointer to the char* itself
 * @note For other values, returns pointer to the value data
 * @note Type is inferred from iterator type variables
 * @note Value can be modified in-place (but don't change size)
 */
#define chmap_iter_val_ptr(iter)                       \
  ({                                                   \
    typeof(*iter##__chm_iter_val_type_var) *val;       \
    if (is_char_ptr(*iter##__chm_iter_val_type_var)) { \
      val = (typeof(val))(&it->val_pair->ptr);         \
    } else {                                           \
      val = (typeof(val))(it->val_pair->ptr);          \
    }                                                  \
    val;                                               \
  })

/**
 * @brief Destroy an iterator (internal function)
 *
 * @param iter Iterator to destroy
 *
 * @warning Do not call directly - use chmap_iter_destroy() macro instead
 */
void __chmap_iterator_destroy(cmap_iterator *iter);

/**
 * @brief Destroy an iterator and set pointer to NULL
 *
 * Frees the iterator structure. Safe to call with NULL.
 *
 * @param iter Iterator to destroy (will be set to NULL)
 *
 * @note Safe to call with NULL
 * @note Iterator is automatically destroyed by chmap_iter_next() at end
 * @note Iterator with _ccol_destructor attribute auto-destroys out of scope
 */
#define chmap_iter_destroy(iter)      \
  do {                                \
    __chmap_iterator_destroy((iter)); \
    iter = NULL;                      \
  } while (0)

/**
 * @brief Internal cleanup function for automatic iterator destruction
 *
 * @param iter Pointer to iterator pointer
 *
 * @note Used by _ccol_destructor attribute
 * @warning Do not call directly
 */
static inline void ___chmap_iterator_destroy(cmap_iterator **iter) {
  if (*iter) {
    __chmap_iterator_destroy(*iter);
    *iter = NULL;
  }
}

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
#define chmap_destroy(chm) \
  do {                     \
    __chmap_destroy(chm);  \
    chm = NULL;            \
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
 *   chmap_enable_local_macros(map, int, char*);
 *   chmap_insert(map, 42, "hello");
 * }
 * @endcode
 */
#define chmap_enable_local_macros(hm_name, key_t, val_t)                     \
  typeof(key_t) *hm_name##__chm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__chm_val_type_var __attribute__((unused)) = NULL

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
#define chmap_declare(hm_name, key_t, val_t)                                 \
  typeof(key_t) *hm_name##__chm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__chm_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */

/**
 * @brief Initialize a hash map with full customization
 *
 * Initializes a previously declared hash map with custom memory management
 * and custom hashing. Calls fatal_err() on failure.
 *
 * @param hm_name Hash map variable to initialize (must be declared)
 * @param mmgmt_procs Custom memory management procedures
 * @param custom_hashing_proc Custom hash function
 *
 * @note Terminates program on failure
 * @note Uses DEFAULT_INITIAL_BUCKET_ARRAY_SIZE (64)
 *
 * @see chmap_declare
 * @see chmap_construct_full
 */
#define chmap_init_full(hm_name, mmgmt_procs, custom_hashing_proc)       \
  do {                                                                   \
    char *err = NULL;                                                    \
    hm_name = chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,       \
                                mmgmt_procs, custom_hashing_proc, &err); \
    if (!hm_name) {                                                      \
      fatal_err("%s", err);                                              \
    }                                                                    \
  } while (0)

/**
 * @brief Declare and initialize a hash map with full customization
 *
 * Combines declaration and initialization with custom memory management
 * and custom hashing. Calls fatal_err() on failure.
 *
 * @param hm_name Hash map variable name
 * @param key_t Key type
 * @param val_t Value type
 * @param mmgmt_procs Custom memory management procedures
 * @param custom_hashing_proc Custom hash function
 *
 * @note Terminates program on failure
 * @note Uses DEFAULT_INITIAL_BUCKET_ARRAY_SIZE (64)
 *
 * @see chmap_init_full
 */
#define chmap_construct_full(hm_name, key_t, val_t, mmgmt_procs,             \
                             custom_hashing_proc)                            \
  typeof(key_t) *hm_name##__chm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__chm_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;             \
  do {                                                                       \
    char *err = NULL;                                                        \
    hm_name = chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,           \
                                mmgmt_procs, custom_hashing_proc, &err);     \
    if (!hm_name) {                                                          \
      fatal_err("%s", err);                                                  \
    }                                                                        \
  } while (0)

/**
 * @brief Initialize a hash map with defaults
 *
 * Initializes a previously declared hash map with default settings.
 *
 * @param hm_name Hash map variable to initialize
 *
 * @note Terminates program on failure
 * @note Uses default memory management and DJB2 hashing
 */
#define chmap_init(hm_name)                                          \
  do {                                                               \
    char *err = NULL;                                                \
    hm_name = chmap_create(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, &err); \
    if (!hm_name) {                                                  \
      fatal_err("%s", err);                                          \
    }                                                                \
  } while (0)

/**
 * @brief Declare and initialize a hash map with defaults
 *
 * Combines declaration and initialization with default settings.
 *
 * @param hm_name Hash map variable name
 * @param key_t Key type
 * @param val_t Value type
 *
 * @note Terminates program on failure
 *
 * Example:
 * @code
 * chmap_construct(ages, char*, int);
 * chmap_insert(ages, "Alice", 30);
 * chmap_insert(ages, "Bob", 25);
 * int age = chmap_get(ages, "Alice"); // age == 30
 * chmap_destroy(ages);
 * @endcode
 */
#define chmap_construct(hm_name, key_t, val_t)                               \
  typeof(key_t) *hm_name##__chm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__chm_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;             \
  do {                                                                       \
    char *err = NULL;                                                        \
    hm_name = chmap_create(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, &err);         \
    if (!hm_name) {                                                          \
      fatal_err("%s", err);                                                  \
    }                                                                        \
  } while (0)

/**
 * @brief Initialize a hash map with custom memory management
 *
 * Initializes a previously declared hash map with custom memory management.
 *
 * @param hm_name Hash map variable to initialize
 * @param mmgmt_procs Custom memory management procedures
 *
 * @note Terminates program on failure
 * @note Uses default DJB2 hashing
 */
#define chmap_init_mp(hm_name, mmgmt_procs)                                    \
  do {                                                                         \
    char *err = NULL;                                                          \
    hm_name =                                                                  \
        chmap_create_mp(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, mmgmt_procs, &err); \
    if (!hm_name) {                                                            \
      fatal_err("%s", err);                                                    \
    }                                                                          \
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
#define chmap_construct_mp(hm_name, key_t, val_t, mmgmt_procs)                 \
  typeof(key_t) *hm_name##__chm_key_type_var __attribute__((unused)) = NULL;   \
  typeof(val_t) *hm_name##__chm_val_type_var __attribute__((unused)) = NULL;   \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;               \
  do {                                                                         \
    char *err = NULL;                                                          \
    hm_name =                                                                  \
        chmap_create_mp(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, mmgmt_procs, &err); \
    if (!hm_name) {                                                            \
      fatal_err("%s", err);                                                    \
    }                                                                          \
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
    hm_name = chmap_create_ch(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, \
                              custom_hashing_proc, &err);        \
    if (!hm_name) {                                              \
      fatal_err("%s", err);                                      \
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
#define chmap_construct_ch(hm_name, key_t, val_t, custom_hashing_proc)       \
  typeof(key_t) *hm_name##__chm_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__chm_val_type_var __attribute__((unused)) = NULL; \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;             \
  do {                                                                       \
    char *err = NULL;                                                        \
    hm_name = chmap_create_ch(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,             \
                              custom_hashing_proc, &err);                    \
    if (!hm_name) {                                                          \
      fatal_err("%s", err);                                                  \
    }                                                                        \
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
    _populate_cmap_pair(key_pair, key);                               \
    _populate_cmap_pair(val_pair, val);                               \
    ccol_retval_t r = chmap_insert_elem(hm_name, key_pair, val_pair); \
    if (r != ccol_success) {                                          \
      fatal_err("Failed to insert elem - r: %d", r);                  \
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
    _populate_cmap_pair(key_pair, key);                     \
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
#define chmap_get(hm_name, key)                                              \
  ({                                                                         \
    typeof(*hm_name##__chm_val_type_var) *val = NULL;                        \
    cmap_pair *key_pair = &(cmap_pair){};                                    \
    cmap_pair *val_pair = NULL;                                              \
    _populate_cmap_pair(key_pair, key);                                      \
    ccol_retval_t r = chmap_get_elem_ref(hm_name, key_pair, &val_pair);      \
    if (r != ccol_success) {                                                 \
      fatal_err("Failed to get elem ref - r: %d", r);                        \
    }                                                                        \
    if (is_char_ptr(*hm_name##__chm_val_type_var)) {                         \
      val = (typeof(*hm_name##__chm_val_type_var) *)&(val_pair->ptr);        \
    } else if (val_pair->size != sizeof(*val)) {                             \
      fatal_err(                                                             \
          "Failed to get elem ref - val_pair->size: %lu - sizeof(val): %lu", \
          (unsigned long)val_pair->size, (unsigned long)sizeof(val));        \
    } else {                                                                 \
      val = (typeof(*hm_name##__chm_val_type_var) *)(val_pair->ptr);         \
    }                                                                        \
    *val;                                                                    \
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
 * @note Pointer invalidated by insert/delete/resize operations
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
#define chmap_get_ptr(hm_name, key)                                            \
  ({                                                                           \
    typeof(*hm_name##__chm_val_type_var) *val = NULL;                          \
    cmap_pair *key_pair = &(cmap_pair){};                                      \
    cmap_pair *val_pair = NULL;                                                \
    _populate_cmap_pair(key_pair, key);                                        \
    ccol_retval_t r = chmap_get_elem_ref(hm_name, key_pair, &val_pair);        \
    if (r == ccol_success) {                                                   \
      if (is_char_ptr(*hm_name##__chm_val_type_var)) {                         \
        val = (typeof(*hm_name##__chm_val_type_var) *)&(val_pair->ptr);        \
      } else if (val_pair->size != sizeof(*val)) {                             \
        fatal_err(                                                             \
            "Failed to get elem ref - val_pair->size: %lu - sizeof(val): %lu", \
            (unsigned long)val_pair->size, (unsigned long)sizeof(val));        \
      } else {                                                                 \
        val = (typeof(*hm_name##__chm_val_type_var) *)(val_pair->ptr);         \
      }                                                                        \
    }                                                                          \
    val;                                                                       \
  })
