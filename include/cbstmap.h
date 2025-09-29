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

#include <citerators.h>
#include <common.h>

/**
 * @file cbstmap.h
 * @brief Self-balancing binary search tree map (AVL tree) with sorted keys
 *
 * Provides an ordered map implementation using an AVL tree with:
 * - Automatic height balancing via rotations
 * - Iterative implementations (no recursion, stack-safe)
 * - O(log n) insert, delete, and search operations
 * - In-order iteration (sorted key order)
 * - Custom comparison function support
 * - Type-safe macros for common operations with signed/unsigned key handling
 *
 * Key characteristics:
 * - AVL tree maintains balance factor |height(left) - height(right)| ≤ 1
 * - Single and double rotations for rebalancing
 * - Keys stored in sorted order (ascending)
 * - Iterator traverses in-order (left, root, right)
 * - All tree operations are iterative using explicit stacks
 * - Default comparison: memcmp for unsigned, proper signed comparison
 */

/**
 * @brief Check if a key type-variable pointer denotes a char-pointer key type
 *
 * Analogous to __is_signed_int_ptr: the key type-variable has type key_t*, so
 * for char* keys the type-variable has type char** — that is what this checks.
 */
#define __is_char_ptr_key(_ptr)     \
  _Generic((_ptr),                  \
      char **: true,                \
      const char **: true,          \
      unsigned char **: true,       \
      const unsigned char **: true, \
      default: false)

/** @brief Opaque binary search tree map structure */
typedef struct cbinarymap cbinarymap;

/** @brief Pointer to binary search tree map (handle type) */
typedef cbinarymap *cbmap;

/* ========================================================================== */
/*                         BST MAP CREATION                                   */
/* ========================================================================== */

/**
 * @brief Create a balanced BST map with full customization
 *
 * Creates a new self-balancing binary search tree map with specified key
 * signedness, custom memory management, and custom comparison function.
 *
 * @param keys_are_signed_ints If true, treat integer keys as signed for
 * comparison
 * @param mmgmt_procs Custom memory management procedures, or NULL for default
 * malloc/free
 * @param custom_comparison_proc Custom comparison function, or NULL for default
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created BST map, or NULL on failure
 *
 * @note Default comparison for unsigned: memcmp(key1, key2, size)
 * @note Default comparison for signed: proper signed integer comparison (1, 2,
 * 4, 8 bytes only, when used for other sizes the behaviour is undefined)
 * @note Custom comparison receives void* pointers to key data
 * @note Map maintains AVL balance: |height(left) - height(right)| ≤ 1
 * @note All operations are O(log n) for balanced tree
 * @note Map must be destroyed with cbmap_destroy() when done
 *
 * @see cbmap_create
 * @see cbmap_create_mp
 * @see cbmap_create_ch
 * @see cbmap_destroy
 */
cbmap cbmap_create_full(bool keys_are_signed_ints, bool keys_are_strings,
                        ccol_memmgmt_procs_t *mmgmt_procs,
                        ccol_comparison_proc_t custom_comparison_proc,
                        char **err);

/**
 * @brief Create a BST map with default settings
 *
 * Convenience wrapper for cbmap_create_full() with default memory management
 * and default comparison (based on key signedness).
 *
 * @param keys_are_signed_ints If true, treat integer keys as signed
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created BST map, or NULL on failure
 */
static inline __attribute__((always_inline)) cbmap
cbmap_create(bool keys_are_signed_ints, char **err) {
  return cbmap_create_full(keys_are_signed_ints, false, NULL, NULL, err);
}

/**
 * @brief Create a BST map with custom memory management
 *
 * Convenience wrapper for cbmap_create_full() with custom memory management
 * but default comparison.
 *
 * @param keys_are_signed_ints If true, treat integer keys as signed
 * @param mmgmt_procs Custom memory management procedures
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created BST map, or NULL on failure
 */
static inline __attribute__((always_inline)) cbmap cbmap_create_mp(
    bool keys_are_signed_ints, ccol_memmgmt_procs_t *mmgmt_procs, char **err) {
  return cbmap_create_full(keys_are_signed_ints, false, mmgmt_procs, NULL, err);
}

/**
 * @brief Create a BST map with custom comparison function
 *
 * Convenience wrapper for cbmap_create_full() with custom comparison function
 * but default memory management.
 *
 * @param keys_are_signed_ints If true, treat integer keys as signed (ignored if
 * custom_comparison_proc provided)
 * @param custom_comparison_proc Custom comparison function
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created BST map, or NULL on failure
 *
 * @note keys_are_signed_ints is ignored when custom_comparison_proc is provided
 */
static inline __attribute__((always_inline)) cbmap
cbmap_create_ch(bool keys_are_signed_ints,
                ccol_comparison_proc_t custom_comparison_proc, char **err) {
  return cbmap_create_full(keys_are_signed_ints, false, NULL,
                           custom_comparison_proc, err);
}

/* ========================================================================== */
/*                         BST MAP OPERATIONS                                 */
/* ========================================================================== */

/**
 * @brief Get the number of elements in the map
 *
 * Returns the current number of key-value pairs stored in the BST map.
 *
 * @param cbm BST map to query
 *
 * @return Number of elements in the map
 *
 * @note Will assert if cbm is NULL
 * @note O(1) complexity
 */
size_t cbmap_elem_count(cbmap cbm);

/**
 * @brief Clear all elements from the map
 *
 * Removes all key-value pairs from the map using iterative post-order
 * traversal. The map becomes empty but remains usable.
 *
 * @param cbm BST map to reset
 *
 * @return ccol_success on success
 *
 * @note O(n) complexity
 * @note All nodes are destroyed, keys and values freed
 * @note Map structure remains valid for reuse
 * @note Will assert if cbm is NULL
 *
 * @see cbmap_destroy
 */
ccol_retval_t cbmap_reset(cbmap cbm);

/**
 * @brief Insert or update a key-value pair
 *
 * Inserts a new key-value pair into the BST, or updates the value if the key
 * already exists. Automatically rebalances the tree using AVL rotations to
 * maintain height balance. Uses iterative insertion with explicit stack.
 *
 * @param cbm BST map to insert into
 * @param key_pair Key to insert (ptr and size must be valid)
 * @param val_pair Value to insert (ptr and size must be valid)
 *
 * @return ccol_success on success
 * @return ccol_container_full if max_elem_count reached
 * @return ccol_not_enough_memory if allocation fails
 *
 * @note O(log n) average and worst case (due to balancing)
 * @note Key and value data are copied (not referenced)
 * @note If key exists, only value is updated (key remains unchanged)
 * @note If update changes value size, memory is reallocated
 * @note Tree is rebalanced bottom-up after insertion
 * @note Uses single or double rotations as needed (left, right, left-right,
 * right-left)
 * @note Will assert if cbm is NULL
 *
 * @see cbmap_get_elem_copy
 * @see cbmap_get_elem_ref
 * @see cbmap_delete_elem
 */
ccol_retval_t cbmap_insert_elem(cbmap cbm, const cmap_pair *key_pair,
                                const cmap_pair *val_pair);

/**
 * @brief Get a copy of the value associated with a key
 *
 * Retrieves a copy of the value for the specified key into the provided buffer.
 * Performs binary search through the tree.
 *
 * @param cbm BST map to search
 * @param key_pair Key to look up
 * @param target_buf Buffer to receive value copy
 * @param target_buf_size Size of target buffer
 *
 * @return ccol_success if key found and value copied
 * @return ccol_invalid_args if buffer size doesn't match value size
 * @return ccol_key_not_found if key does not exist
 *
 * @note O(log n) complexity (binary search)
 * @note Requires exact size match (unlike chmap_get_elem_copy)
 * @note Will assert if cbm is NULL
 *
 * @see cbmap_get_elem_ref
 * @see cbmap_insert_elem
 */
ccol_retval_t cbmap_get_elem_copy(cbmap cbm, const cmap_pair *key_pair,
                                  void *target_buf, size_t target_buf_size);

/**
 * @brief Get a reference to the value associated with a key
 *
 * Retrieves a pointer to the value pair structure for the specified key.
 * The returned pointer is valid until the map is modified (insert/delete).
 *
 * @param cbm BST map to search
 * @param key_pair Key to look up
 * @param val_pair Output parameter to receive pointer to value pair
 *
 * @return ccol_success if key found
 * @return ccol_key_not_found if key does not exist
 *
 * @note O(log n) complexity (binary search)
 * @note Returned pointer is invalidated by insert/delete operations
 * @note Do not free the returned pointer - it's owned by the map
 * @note Can modify value in-place, but do not change size
 * @note Will assert if cbm is NULL
 *
 * @see cbmap_get_elem_copy
 * @see cbmap_insert_elem
 */
ccol_retval_t cbmap_get_elem_ref(cbmap cbm, const cmap_pair *key_pair,
                                 cmap_pair **val_pair);

/**
 * @brief Delete a key-value pair from the map
 *
 * Removes the specified key and its associated value from the BST.
 * Automatically rebalances the tree using AVL rotations. Uses iterative
 * deletion with explicit stack.
 *
 * For nodes with two children, replaces with:
 * - Right subtree minimum if right is deeper or equal height
 * - Left subtree maximum if left is deeper
 *
 * @param cbm BST map to delete from
 * @param key_pair Key to delete
 *
 * @return ccol_success if key found and deleted
 * @return ccol_key_not_found if key does not exist
 *
 * @note O(log n) complexity (search + rebalancing)
 * @note Frees memory allocated for key and value
 * @note Tree is rebalanced bottom-up after deletion
 * @note Uses iterative extreme node detachment for two-child case
 * @note Will assert if cbm is NULL
 *
 * @see cbmap_insert_elem
 * @see cbmap_reset
 */
ccol_retval_t cbmap_delete_elem(cbmap cbm, const cmap_pair *key_pair);

/* ========================================================================== */
/*                         BST MAP ITERATION                                  */
/* ========================================================================== */

/**
 * @brief Begin in-order iteration over the BST map
 *
 * Creates an iterator positioned at the leftmost (smallest key) node.
 * Iteration proceeds in sorted key order (ascending) using in-order
 * traversal (left, root, right).
 *
 * @param cbm BST map to iterate over
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to iterator, or NULL if map is empty or allocation fails
 *
 * @note Iterates in sorted key order (in-order traversal)
 * @note Iterator uses a stack to track path through tree
 * @note Iterator must be destroyed with cbmap_iter_destroy()
 * @note Modifying map during iteration invalidates the iterator
 * @note Will assert if cbm is NULL
 * @note Returns NULL if map is empty (not an error)
 *
 * @see cbmap_begin (macro wrapper)
 * @see cbmap_iter_next
 * @see cbmap_iter_destroy
 */
cmap_iterator *cbmap_begin_iter(cbmap cbm, char **err);

void __cbmap_iterator_destroy(cmap_iterator *iter);

/* ========================================================================== */
/*                         BST MAP DESTRUCTION                                */
/* ========================================================================== */

/**
 * @brief Destroy a BST map (internal function)
 *
 * @param cbm BST map to destroy
 *
 * @warning Do not call directly - use cbmap_destroy() macro instead
 */
void __cbmap_destroy(cbmap cbm);

/**
 * @brief Destroy a BST map and set pointer to NULL
 *
 * Frees all resources associated with the BST map including all keys,
 * values, and tree nodes using iterative post-order traversal.
 *
 * @param cbm BST map to destroy (will be set to NULL)
 *
 * @note Safe to call with NULL
 * @note Frees all key and value data
 * @note Uses iterative post-order traversal (no recursion)
 */
#define cbmap_destroy(cbm)  \
  do {                      \
    __cbmap_destroy((cbm)); \
    cbm = NULL;             \
  } while (0)

/**
 * @brief Internal cleanup function for automatic BST map destruction
 *
 * @param cbm Pointer to BST map pointer
 *
 * @note Used by _ccol_destructor attribute
 * @warning Do not call directly
 */
static inline void ___cbmap_destroy(cbmap *cbm) {
  if (*cbm) {
    __cbmap_destroy(*cbm);
    *cbm = NULL;
  }
}

/* ========================================================================== */
/*                    TYPE-SAFE CONVENIENCE MACROS                            */
/* ========================================================================== */

/**
 * @brief Enable type-safe macros for an existing BST map
 *
 * Declares type variables needed for type-safe macro operations when using
 * a BST map that was created in another scope.
 *
 * @param hm_name BST map variable name
 * @param key_t Key type
 * @param val_t Value type
 *
 * Example:
 * @code
 * void process(cbmap map) {
 *   cbmap_redeclare(map, int, char*);
 *   cbmap_insert(map, 42, "hello");
 * }
 * @endcode
 */
#define cbmap_redeclare(hm_name, key_t, val_t)                                \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL

/**
 * @brief Declare an uninitialized BST map variable
 *
 * Declares a BST map variable and associated type variables for type-safe
 * macro operations. The map must be initialized before use.
 *
 * @param hm_name BST map variable name
 * @param key_t Key type
 * @param val_t Value type
 *
 * @note Map must be initialized with cbmap_init*() before use
 *
 * @see cbmap_init
 * @see cbmap_construct
 */
#define cbmap_declare(hm_name, key_t, val_t)                              \
  typeof(key_t) *hm_name##__ccol_key_type_var                             \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  typeof(val_t) *hm_name##__ccol_val_type_var                             \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  cbmap hm_name                /* deliberately not initialized to NULL */

#define cbmap_declare_scoped(hm_name, key_t, val_t)                           \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name _ccol_destructor(___cbmap_destroy) = NULL

/**
 * @brief Initialize a BST map with defaults
 *
 * Initializes a previously declared BST map with default settings.
 * Automatically determines key signedness from type variable.
 *
 * @param hm_name BST map variable to initialize
 *
 * @note Terminates program on failure
 * @note Uses default memory management
 * @note Automatically detects signed vs unsigned keys
 *
 * @see cbmap_declare
 * @see cbmap_construct
 */
#define cbmap_init(hm_name)                                                 \
  do {                                                                      \
    char *err = NULL;                                                       \
    hm_name = cbmap_create_full(                                            \
        __is_signed_int_ptr(hm_name##__ccol_key_type_var),                  \
        __is_char_ptr_key(hm_name##__ccol_key_type_var), NULL, NULL, &err); \
    if (!hm_name) {                                                         \
      fatal_err("Failed to create BST map '%s': %s", #hm_name,              \
                err ? err : "unknown error");                               \
    }                                                                       \
  } while (0)

/**
 * @brief Initialize a BST map with custom memory management
 *
 * Initializes a previously declared BST map with custom memory management.
 *
 * @param hm_name BST map variable to initialize
 * @param mmgmt_procs Custom memory management procedures
 *
 * @note Terminates program on failure
 * @note Automatically detects signed vs unsigned keys
 */
#define cbmap_init_mp(hm_name, mmgmt_procs)                                  \
  do {                                                                       \
    char *err = NULL;                                                        \
    hm_name =                                                                \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__ccol_key_type_var), \
                          __is_char_ptr_key(hm_name##__ccol_key_type_var),   \
                          (mmgmt_procs), NULL, &err);                        \
    if (!hm_name) {                                                          \
      fatal_err("Failed to create BST map '%s': %s", #hm_name,               \
                err ? err : "unknown error");                                \
    }                                                                        \
  } while (0)

/**
 * @brief Initialize a BST map with custom comparison
 *
 * Initializes a previously declared BST map with custom comparison function.
 *
 * @param hm_name BST map variable to initialize
 * @param custom_comparison_proc Custom comparison function
 *
 * @note Terminates program on failure
 * @note Uses default memory management
 */
#define cbmap_init_cc(hm_name, custom_comparison_proc)                       \
  do {                                                                       \
    char *err = NULL;                                                        \
    hm_name =                                                                \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__ccol_key_type_var), \
                          __is_char_ptr_key(hm_name##__ccol_key_type_var),   \
                          NULL, (custom_comparison_proc), &err);             \
    if (!hm_name) {                                                          \
      fatal_err("Failed to create BST map '%s': %s", #hm_name,               \
                err ? err : "unknown error");                                \
    }                                                                        \
  } while (0)

/**
 * @brief Initialize a BST map with full customization
 *
 * Initializes a previously declared BST map with custom memory management
 * and custom comparison.
 *
 * @param hm_name BST map variable to initialize
 * @param mmgmt_procs Custom memory management procedures
 * @param custom_comparison_proc Custom comparison function
 *
 * @note Terminates program on failure
 */
#define cbmap_init_full(hm_name, mmgmt_procs, custom_comparison_proc)        \
  do {                                                                       \
    char *err = NULL;                                                        \
    hm_name =                                                                \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__ccol_key_type_var), \
                          __is_char_ptr_key(hm_name##__ccol_key_type_var),   \
                          (mmgmt_procs), (custom_comparison_proc), &err);    \
    if (!hm_name) {                                                          \
      fatal_err("Failed to create BST map '%s': %s", #hm_name,               \
                err ? err : "unknown error");                                \
    }                                                                        \
  } while (0)

/**
 * @brief Declare and initialize a BST map with defaults
 *
 * Combines declaration and initialization with default settings.
 *
 * @param hm_name BST map variable name
 * @param key_t Key type
 * @param val_t Value type
 *
 * @note Terminates program on failure
 * @note Automatically detects signed vs unsigned keys
 *
 * Example:
 * @code
 * cbmap_construct(sorted_map, int, char*);
 * cbmap_insert(sorted_map, 42, "hello");
 * cbmap_insert(sorted_map, 10, "world");
 * // Iteration will visit keys in order: 10, 42
 * for (cbmap_iter_declare(sorted_map, it) = cbmap_begin(sorted_map);
 *      it; it = cbmap_iter_next(it)) {
 *   // Keys visited in sorted order
 * }
 * cbmap_destroy(sorted_map);
 * @endcode
 */
#define cbmap_construct(hm_name, key_t, val_t)                                \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name /* _ccol_destructor(___cbmap_destroy) = NULL; */ = NULL;      \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name = cbmap_create_full(                                              \
        __is_signed_int_ptr(hm_name##__ccol_key_type_var),                    \
        __is_char_ptr_key(hm_name##__ccol_key_type_var), NULL, NULL, &err);   \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create BST map '%s': %s", #hm_name,                \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

#define cbmap_construct_scoped(hm_name, key_t, val_t)                         \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name _ccol_destructor(___cbmap_destroy);                           \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name = cbmap_create_full(                                              \
        __is_signed_int_ptr(hm_name##__ccol_key_type_var),                    \
        __is_char_ptr_key(hm_name##__ccol_key_type_var), NULL, NULL, &err);   \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create BST map '%s': %s", #hm_name,                \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

/**
 * @brief Declare and initialize a BST map with custom memory management
 *
 * Combines declaration and initialization with custom memory management.
 *
 * @param hm_name BST map variable name
 * @param key_t Key type
 * @param val_t Value type
 * @param mmgmt_procs Custom memory management procedures
 *
 * @note Terminates program on failure
 */
#define cbmap_construct_mp(hm_name, key_t, val_t, mmgmt_procs)                \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name /* _ccol_destructor(___cbmap_destroy) = NULL; */ = NULL;      \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name =                                                                 \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__ccol_key_type_var),  \
                          __is_char_ptr_key(hm_name##__ccol_key_type_var),    \
                          (mmgmt_procs), NULL, &err);                         \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create BST map '%s': %s", #hm_name,                \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

#define cbmap_construct_mp_scoped(hm_name, key_t, val_t, mmgmt_procs)         \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name _ccol_destructor(___cbmap_destroy) = NULL;                    \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name =                                                                 \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__ccol_key_type_var),  \
                          __is_char_ptr_key(hm_name##__ccol_key_type_var),    \
                          (mmgmt_procs), NULL, &err);                         \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create BST map '%s': %s", #hm_name,                \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

/**
 * @brief Declare and initialize a BST map with custom comparison
 *
 * Combines declaration and initialization with custom comparison function.
 *
 * @param hm_name BST map variable name
 * @param key_t Key type
 * @param val_t Value type
 * @param custom_comparison_proc Custom comparison function
 *
 * @note Terminates program on failure
 */
#define cbmap_construct_cc(hm_name, key_t, val_t, custom_comparison_proc)     \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name /* _ccol_destructor(___cbmap_destroy) = NULL; */ = NULL;      \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name =                                                                 \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__ccol_key_type_var),  \
                          __is_char_ptr_key(hm_name##__ccol_key_type_var),    \
                          NULL, (custom_comparison_proc), &err);              \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create BST map '%s': %s", #hm_name,                \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

#define cbmap_construct_cc_scoped(hm_name, key_t, val_t,                      \
                                  custom_comparison_proc)                     \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name _ccol_destructor(___cbmap_destroy) = NULL;                    \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name =                                                                 \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__ccol_key_type_var),  \
                          __is_char_ptr_key(hm_name##__ccol_key_type_var),    \
                          NULL, (custom_comparison_proc), &err);              \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create BST map '%s': %s", #hm_name,                \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

/**
 * @brief Declare and initialize a BST map with full customization
 *
 * Combines declaration and initialization with custom memory management
 * and custom comparison.
 *
 * @param hm_name BST map variable name
 * @param key_t Key type
 * @param val_t Value type
 * @param mmgmt_procs Custom memory management procedures
 * @param custom_comparison_proc Custom comparison function
 *
 * @note Terminates program on failure
 */
#define cbmap_construct_full(hm_name, key_t, val_t, mmgmt_procs,              \
                             custom_comparison_proc)                          \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name /* _ccol_destructor(___cbmap_destroy) = NULL; */ = NULL;      \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name =                                                                 \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__ccol_key_type_var),  \
                          __is_char_ptr_key(hm_name##__ccol_key_type_var),    \
                          (mmgmt_procs), (custom_comparison_proc), &err);     \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create BST map '%s': %s", #hm_name,                \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

#define cbmap_construct_full_scoped(hm_name, key_t, val_t, mmgmt_procs,       \
                                    custom_comparison_proc)                   \
  typeof(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = NULL; \
  typeof(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL; \
  cbmap hm_name _ccol_destructor(___cbmap_destroy) = NULL;                    \
  do {                                                                        \
    char *err = NULL;                                                         \
    hm_name =                                                                 \
        cbmap_create_full(__is_signed_int_ptr(hm_name##__ccol_key_type_var),  \
                          __is_char_ptr_key(hm_name##__ccol_key_type_var),    \
                          (mmgmt_procs), (custom_comparison_proc), &err);     \
    if (!hm_name) {                                                           \
      fatal_err("Failed to create BST map '%s': %s", #hm_name,                \
                err ? err : "unknown error");                                 \
    }                                                                         \
  } while (0)

/* ========================================================================== */
/*                    TYPE-SAFE OPERATION MACROS                              */
/* ========================================================================== */

/**
 * @brief Insert a key-value pair (type-safe)
 *
 * Type-safe wrapper for cbmap_insert_elem() that automatically handles
 * type conversion and cmap_pair creation. Calls fatal_err() on failure.
 *
 * @param hm_name BST map to insert into
 * @param key Key to insert
 * @param val Value to associate with key
 *
 * @note Terminates program on failure
 * @note Automatically takes address of key and value
 * @note Handles both value types and string types correctly
 * @note Tree is automatically rebalanced after insertion
 *
 * @see cbmap_insert_elem
 * @see cbmap_get
 * @see cbmap_remove
 */
#define cbmap_insert(hm_name, key, val)                               \
  do {                                                                \
    cmap_pair *key_pair = &(cmap_pair){};                             \
    cmap_pair *val_pair = &(cmap_pair){};                             \
    _populate_cmap_pair(key_pair, (key));                             \
    _populate_cmap_pair(val_pair, (val));                             \
    ccol_retval_t r = cbmap_insert_elem(hm_name, key_pair, val_pair); \
    if (r != ccol_success && r != ccol_key_already_present) {         \
      _ccol_dump_key_to_stderr(key_pair->ptr, key_pair->size);        \
      fatal_err("cbmap_insert('%s'): r: %d (%s)", #hm_name, r,        \
                ccol_retval_to_str(r));                               \
    }                                                                 \
  } while (0)

/**
 * @brief Remove a key-value pair (type-safe)
 *
 * Type-safe wrapper for cbmap_delete_elem() that returns the result code.
 *
 * @param hm_name BST map to remove from
 * @param key Key to remove
 *
 * @return ccol_success if removed, ccol_key_not_found if not found
 *
 * @note Does not terminate on key_not_found
 * @note Frees memory for key and value
 * @note Tree is automatically rebalanced after deletion
 *
 * @see cbmap_delete_elem
 * @see cbmap_insert
 */
#define cbmap_remove(hm_name, key)                          \
  ({                                                        \
    cmap_pair *key_pair = &(cmap_pair){};                   \
    _populate_cmap_pair(key_pair, (key));                   \
    ccol_retval_t r = cbmap_delete_elem(hm_name, key_pair); \
    r;                                                      \
  })

/**
 * @brief Get value by key (type-safe, returns value)
 *
 * Type-safe wrapper for cbmap_get_elem_ref() that returns the actual value.
 * Calls fatal_err() if key not found or size mismatch.
 *
 * @param hm_name BST map to search
 * @param key Key to look up
 *
 * @return Value associated with key
 *
 * @note Terminates program if key not found
 * @note Terminates program if value size doesn't match type size
 * @note Returns value, not pointer
 * @note For strings, returns the char* itself
 * @note O(log n) search time
 *
 * @see cbmap_get_ptr
 * @see cbmap_insert
 */
#define cbmap_get(hm_name, key)                                             \
  ({                                                                        \
    typeof(*hm_name##__ccol_val_type_var) *val = NULL;                      \
    cmap_pair *key_pair = &(cmap_pair){};                                   \
    cmap_pair *val_pair = NULL;                                             \
    _populate_cmap_pair(key_pair, (key));                                   \
    ccol_retval_t r = cbmap_get_elem_ref(hm_name, key_pair, &val_pair);     \
    if (r != ccol_success) {                                                \
      _ccol_dump_key_to_stderr(key_pair->ptr, key_pair->size);              \
      fatal_err("cbmap_get('%s'): r: %d (%s)", #hm_name, r,                 \
                ccol_retval_to_str(r));                                     \
    }                                                                       \
    if (is_char_ptr(*hm_name##__ccol_val_type_var)) {                       \
      val = (typeof(*hm_name##__ccol_val_type_var) *)&(val_pair->ptr);      \
    } else if (val_pair->size != sizeof(*val)) {                            \
      fatal_err(                                                            \
          "cbmap_get('%s'): value size mismatch - stored: %lu bytes, "      \
          "requested: %lu bytes; wrong type or missing cbmap_redeclare()?", \
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
 * Type-safe wrapper for cbmap_get_elem_ref() that returns a pointer to the
 * value, or NULL if key not found. Unlike cbmap_get(), does not terminate
 * on key_not_found.
 *
 * @param hm_name BST map to search
 * @param key Key to look up
 *
 * @return Pointer to value, or NULL if key not found
 *
 * @note Returns NULL if key not found (does not terminate)
 * @note Terminates program if value size doesn't match type size
 * @note Returns pointer to value for in-place modification
 * @note Pointer invalidated by insert/delete operations
 * @note For strings, returns pointer to the char* itself
 * @note O(log n) search time
 *
 * @see cbmap_get
 * @see cbmap_get_elem_ref
 */
#define cbmap_get_ptr(hm_name, key)                                           \
  ({                                                                          \
    typeof(*hm_name##__ccol_val_type_var) *val = NULL;                        \
    cmap_pair *key_pair = &(cmap_pair){};                                     \
    cmap_pair *val_pair = NULL;                                               \
    _populate_cmap_pair(key_pair, (key));                                     \
    ccol_retval_t r = cbmap_get_elem_ref(hm_name, key_pair, &val_pair);       \
    if (r == ccol_success) {                                                  \
      if (is_char_ptr(*hm_name##__ccol_val_type_var)) {                       \
        val = (typeof(*hm_name##__ccol_val_type_var) *)&(val_pair->ptr);      \
      } else if (val_pair->size != sizeof(*val)) {                            \
        fatal_err(                                                            \
            "cbmap_get_ptr('%s'): value size mismatch - stored: %lu bytes, "  \
            "requested: %lu bytes; wrong type or missing cbmap_redeclare()?", \
            #hm_name, (unsigned long)val_pair->size,                          \
            (unsigned long)sizeof(*val));                                     \
      } else {                                                                \
        val = (typeof(*hm_name##__ccol_val_type_var) *)(val_pair->ptr);       \
      }                                                                       \
    }                                                                         \
    val;                                                                      \
  })
