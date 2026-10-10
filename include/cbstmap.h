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

#include "citerators.h"
#include "common.h"

/* Everything that this header declares from here to its end is part of the
 * public Application Binary Interface (ABI) of libccollections, and the shared
 * library exports all of it. Because the library itself is built with
 * -fvisibility=hidden, a function or an object must be covered by one of
 * these blocks: if no block covers it, it stays internal to the library and
 * is absent from the dynamic symbol table, so the application that links
 * against the library cannot interpose it, and cannot collide with it through
 * a symbol of the same name. */
#pragma GCC visibility push(default)

/**
 * @file cbstmap.h
 * @brief Self-balancing binary search tree map (AVL tree) with sorted keys
 *
 * Gives an ordered map that uses an AVL tree. The map has these properties:
 * - It balances its own height with rotations
 * - Every operation is iterative, with no recursion, so the stack is safe
 * - Insert, delete and search operations are O(log n)
 * - In-order iteration gives the keys in sorted order
 * - The caller can give a custom comparison function
 * - Type-inferred macros do the common operations, for signed, unsigned,
 *   floating-point and string key types
 *
 * Key characteristics:
 * - The AVL tree keeps the balance factor |height(left) - height(right)| <= 1
 * - The tree uses single and double rotations to balance itself
 * - The tree keeps the keys in sorted order (ascending)
 * - The iterator walks the tree in order (left, root, right)
 * - All tree operations are iterative and use fixed-size stacks, with no
 *   recursion and no heap memory to walk or balance the tree
 * - The default comparison is a signed or unsigned integer comparison for
 *   integers, a numeric comparison for floating-point types, a
 *   strcmp-equivalent comparison for strings, and a raw memcmp for any other
 *   key type
 */

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
 * Creates a new self-balancing binary search tree map with the key type,
 * custom memory management and custom comparison function that the caller
 * gives.
 *
 * @param key_type Type of the keys that the map stores (it selects the
 * default comparison strategy)
 * @param mmgmt_procs Custom memory management procedures, or NULL for the
 * default malloc/free
 * @param custom_comparison_proc Custom comparison function, or NULL for the
 * default
 * @param err Optional pointer that receives an error string on failure
 *
 * @return Pointer to the new BST map, or NULL on failure
 *
 * @note Default comparison for ccol_char: native `char` comparison, signed
 * or unsigned to match the `char` type of this platform
 * @note Default comparison for ccol_signed_char and for
 * short/int/long/long_long: a correct signed integer comparison, which does
 * not depend on the `char` signedness of this platform. Note that
 * ccol_signed_char is a scalar `signed char` or its typedef `int8_t`, while
 * a `signed char *` string key is ccol_string
 * @note Default comparison for the unsigned integer types and ccol_pointer:
 * a correct unsigned integer comparison
 * @note Default comparison for ccol_float/double/long_double: a correct
 * floating-point comparison, in which a NaN key sorts as greater than every
 * non-NaN key and equal to every other NaN key. This total order is well
 * defined, so the caller can find, update and delete a NaN key, and a NaN
 * key never collides with an unrelated key
 * @note Default comparison for ccol_string: lexicographic byte comparison
 * @note For ccol_other_types (for example a struct key type) the default
 * comparison, used when the caller gives no custom_comparison_proc, is a raw
 * memcmp of the representation. That memcmp compares indeterminate padding
 * bytes, and it compares each pointer member by address, not by the object
 * that the pointer points at, so give custom_comparison_proc for a struct key
 * @note A custom comparison gets void* pointers to the key data
 * @note The map keeps the AVL balance: abs(height(left) - height(right)) <= 1
 * @note All operations are O(log n) for a balanced tree
 * @note The caller must destroy the map with cbmap_destroy() after use
 *
 * @see cbmap_create
 * @see cbmap_create_mp
 * @see cbmap_create_ch
 * @see cbmap_destroy
 */
cbmap cbmap_create_full(ccol_data_type key_type,
                        ccol_memmgmt_procs_t *mmgmt_procs,
                        ccol_comparison_proc_t custom_comparison_proc,
                        char **err);

/**
 * @brief Create a BST map with default settings
 *
 * Convenience wrapper for cbmap_create_full() that uses the default memory
 * management and the default comparison for the key type.
 *
 * @param key_type Type of the keys that the map stores
 * @param err Optional pointer that receives an error string on failure
 *
 * @return Pointer to the new BST map, or NULL on failure
 */
static inline __attribute__((always_inline)) cbmap
cbmap_create(ccol_data_type key_type, char **err) {
  return cbmap_create_full(key_type, NULL, NULL, err);
}

/**
 * @brief Create a BST map with custom memory management
 *
 * Convenience wrapper for cbmap_create_full() that uses custom memory
 * management and the default comparison.
 *
 * @param key_type Type of the keys that the map stores
 * @param mmgmt_procs Custom memory management procedures
 * @param err Optional pointer that receives an error string on failure
 *
 * @return Pointer to the new BST map, or NULL on failure
 */
static inline __attribute__((always_inline)) cbmap cbmap_create_mp(
    ccol_data_type key_type, ccol_memmgmt_procs_t *mmgmt_procs, char **err) {
  return cbmap_create_full(key_type, mmgmt_procs, NULL, err);
}

/**
 * @brief Create a BST map with custom comparison function
 *
 * Convenience wrapper for cbmap_create_full() that uses a custom comparison
 * function and the default memory management.
 *
 * @param key_type Type of the keys that the map stores
 * @param custom_comparison_proc Custom comparison function
 * @param err Optional pointer that receives an error string on failure
 *
 * @return Pointer to the new BST map, or NULL on failure
 *
 * @note custom_comparison_proc sets only the order of the keys; key_type
 * sets the width that the map expects for a fixed-width key and the
 * alignment of each stored key.
 */
static inline __attribute__((always_inline)) cbmap
cbmap_create_ch(ccol_data_type key_type,
                ccol_comparison_proc_t custom_comparison_proc, char **err) {
  return cbmap_create_full(key_type, NULL, custom_comparison_proc, err);
}

/* ========================================================================== */
/*                         BST MAP OPERATIONS                                 */
/* ========================================================================== */

/**
 * @brief Get the number of elements in the map
 *
 * Returns the number of key-value pairs that the BST map stores now.
 *
 * @param cbm BST map to query
 *
 * @return Number of elements in the map
 *
 * @note The function asserts if cbm is NULL
 * @note O(1) complexity
 */
size_t cbmap_elem_count(cbmap cbm);

/**
 * @brief Clear all elements from the map
 *
 * Removes all the key-value pairs from the map with an iterative post-order
 * walk of the tree. The map becomes empty, but the caller can go on using it.
 *
 * @param cbm BST map to reset
 *
 * @return ccol_success on success
 *
 * @note O(n) complexity
 * @note The function destroys all the nodes and frees the keys and the values
 * @note The map structure stays valid, so the caller can use it again
 * @note The function asserts if cbm is NULL
 *
 * @see cbmap_destroy
 */
ccol_retval_t cbmap_reset(cbmap cbm);

/**
 * @brief Insert or update a key-value pair
 *
 * Inserts a new key-value pair into the BST, or updates the value if the key
 * is already in the map, and then balances the tree again with AVL rotations
 * to keep the height balance. The insert is iterative and uses an explicit
 * stack.
 *
 * @param cbm BST map to insert into
 * @param key_pair Key to insert (ptr and size must be valid)
 * @param val_pair Value to insert (ptr and size must be valid)
 *
 * @return ccol_success on success
 * @return ccol_key_already_present if the key is already in the map; the
 * function updates its value correctly and does not change the key itself
 * @return ccol_container_full if the map holds ccol_max_elem_count elements
 * @return ccol_not_enough_memory if an allocation fails
 * @return ccol_invalid_args in three cases: key_pair or val_pair is NULL;
 * one of the two pairs has a NULL ptr and a non-zero size; or the key_pair
 * size does not match the size of a fixed-width key type (see
 * ccol_fixed_width_data_type_size)
 *
 * @note O(log n) in the average case and in the worst case, because of the
 * balance work
 * @note A size of 0 is correct for the key or the value, with or without a
 * ptr, and stores an empty key or an empty value. Note that
 * chmap_insert_elem rejects a key or a value of size zero
 * @note The function copies the key data and the value data instead of
 * pointing at the data of the caller
 * @note If the key is already in the map, the function updates only the
 * value and does not change the key
 * @note If the update changes the size of the value, the function allocates
 * new memory for it
 * @note The function balances the tree from the bottom up after the insert
 * @note The function uses single or double rotations as needed (left, right,
 * left-right, right-left)
 * @note The function asserts if cbm is NULL
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
 * Copies the value of the given key into the buffer of the caller, after a
 * binary search through the tree.
 *
 * @param cbm BST map to search
 * @param key_pair Key to look up
 * @param target_buf Buffer that receives the copy of the value
 * @param target_buf_size Size of the target buffer
 *
 * @return ccol_success if the function finds the key and copies the value
 * @return ccol_invalid_args in five cases: key_pair is NULL; key_pair has a
 * NULL ptr and a non-zero size; target_buf is NULL with a non-zero
 * target_buf_size; the buffer size does not match the size of the value; or
 * the key_pair size does not match the size of a fixed-width key type (see
 * ccol_fixed_width_data_type_size)
 * @return ccol_key_not_found if the key is not in the map
 *
 * @note O(log n) complexity, because the function does a binary search
 * @note The two sizes must match exactly, while chmap_get_elem_copy does
 * not need an exact match
 * @note The function asserts if cbm is NULL
 *
 * @see cbmap_get_elem_ref
 * @see cbmap_insert_elem
 */
ccol_retval_t cbmap_get_elem_copy(cbmap cbm, const cmap_pair *key_pair,
                                  void *target_buf, size_t target_buf_size);

/**
 * @brief Get a reference to the value associated with a key
 *
 * Gets a pointer to the value pair structure of the given key, which stays
 * valid until an insert or a delete changes the map.
 *
 * @param cbm BST map to search
 * @param key_pair Key to look up
 * @param val_pair Output parameter that receives the pointer to the value
 * pair
 *
 * @return ccol_success if the function finds the key
 * @return ccol_key_not_found if the key is not in the map
 * @return ccol_invalid_args in three cases: key_pair or val_pair is NULL;
 * key_pair has a NULL ptr and a non-zero size; or the key_pair size does not
 * match the size of a fixed-width key type (see
 * ccol_fixed_width_data_type_size)
 *
 * @note O(log n) complexity, because the function does a binary search
 * @note An insert or a delete makes the returned pointer invalid
 * @note Do not free the returned pointer, because the map owns it
 * @note The returned cmap_pair is the accessor of the map for that entry: it
 * describes the value and is not part of the value. Its target is
 * const-qualified: the caller can read through val_pair->ptr and write to the
 * bytes that it points at, within val_pair->size, but an assignment to
 * val_pair->ptr or to val_pair->size is a compile error. The pointer and the
 * size describe one another, and the map cannot own a pointer that it did not
 * allocate. To replace a value, call cbmap_insert_elem()
 * @note The out-parameter is a const cmap_pair **, which is why the caller
 * must declare its own variable as const cmap_pair *: the address of a plain
 * cmap_pair * does not compile
 * @note The function asserts if cbm is NULL
 *
 * @see cbmap_get_elem_copy
 * @see cbmap_insert_elem
 */
ccol_retval_t cbmap_get_elem_ref(cbmap cbm, const cmap_pair *key_pair,
                                 const cmap_pair **val_pair);

/**
 * @brief Delete a key-value pair from the map
 *
 * Removes the given key and its value from the BST and then balances the
 * tree again with AVL rotations. The delete is iterative and uses an
 * explicit stack.
 *
 * A node with two children gets one of these two replacements:
 * - The minimum of the right subtree, when the right side is deeper than the
 *   left side or the two sides have the same height
 * - The maximum of the left subtree, when the left side is deeper
 *
 * @param cbm BST map to delete from
 * @param key_pair Key to delete
 *
 * @return ccol_success if the function finds the key and deletes it
 * @return ccol_key_not_found if the key is not in the map
 * @return ccol_invalid_args in three cases: key_pair is NULL; key_pair has a
 * NULL ptr and a non-zero size; or the key_pair size does not match the size
 * of a fixed-width key type (see ccol_fixed_width_data_type_size)
 *
 * @note O(log n) complexity for the search and the balance work
 * @note The function frees the memory of the key and of the value
 * @note The function balances the tree from the bottom up after the delete
 * @note For a node with two children, the function detaches the extreme node
 * iteratively
 * @note The function asserts if cbm is NULL
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
 * Creates an iterator at the leftmost node, which holds the smallest key.
 * The iterator walks the keys in sorted order (ascending), walking the tree
 * in order (left, root, right).
 *
 * @param cbm BST map to iterate over
 * @param err Optional pointer that receives an error string on failure
 *
 * @return Pointer to the iterator, or NULL if the map is empty or if an
 * allocation fails
 *
 * @note The iterator walks the keys in sorted order, in an in-order walk
 * @note The iterator uses a stack to keep the path through the tree
 * @note The caller must destroy the iterator with cbmap_iter_destroy()
 * @note A change to the map during the walk makes the iterator invalid
 * @note The function treats a NULL cbm in the same way as an empty map: it
 * returns NULL, and this is not an error. The library does this on purpose,
 * so that a map field that stays uninitialized because the caller has put
 * nothing into it can be iterated directly, with no NULL guard of the
 * caller's own first
 * @note The function returns NULL if the map is empty; this is not an error
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
 * @warning Do not call this function directly. Use the cbmap_destroy() macro
 */
void __cbmap_destroy(cbmap cbm);

/**
 * @brief Destroy a BST map and set pointer to NULL
 *
 * Frees all the resources of the BST map, including all the keys, all the
 * values and all the tree nodes, with an iterative post-order walk of the
 * tree.
 *
 * @param cbm BST map to destroy (the macro sets it to NULL)
 *
 * @note A call with NULL is safe
 * @note The macro frees all the key data and all the value data
 * @note The macro walks the tree in post-order, with no recursion
 * @note The macro evaluates cbm exactly once, and cbm must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define cbmap_destroy(cbm)      \
  _ccol_cbmap_destroy_impl(cbm, \
                           _ccol_uniq(__ccol_cbmap_destroy_slot, __COUNTER__))

/* Internal: the body of cbmap_destroy. slot is a name from _ccol_uniq(), so
 * the macro nests inside the argument of another destroy macro and stays
 * -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_cbmap_destroy_impl(cbm, slot) \
  do {                                      \
    __typeof__(cbm) *slot = &(cbm);         \
    __cbmap_destroy(*slot);                 \
    *slot = NULL;                           \
  } while (0)

/**
 * @brief Internal cleanup function for automatic BST map destruction
 *
 * @param cbm Pointer to BST map pointer
 *
 * @note The _ccol_destructor attribute uses this function
 * @warning Do not call this function directly
 */
static inline void ___cbmap_destroy(cbmap *cbm) {
  if (cbm && *cbm) {
    __cbmap_destroy(*cbm);
    *cbm = NULL;
  }
}

/* ========================================================================== */
/*                    TYPE-INFERRED CONVENIENCE MACROS */
/* ========================================================================== */

/**
 * @brief Turn on the type-inferred macros for an existing BST map
 *
 * Declares the type variables that the type-inferred macros need, for a BST
 * map that another scope created.
 *
 * @param hm_name BST map variable name
 * @param key_t Key type
 * @param val_t Value type
 *
 * Example:
 * @code
 * void process(cbmap map) {
 *   cbmap_redeclare(map, int, char*);
 *   int key = 42;
 *   cbmap_insert(map, key, "hello");
 * }
 * @endcode
 */
#define cbmap_redeclare(hm_name, key_t, val_t)                              \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = \
      NULL;                                                                 \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL

/**
 * @brief Declare an uninitialized BST map variable
 *
 * Declares a BST map variable together with the type variables that the
 * type-inferred macros need. The caller must initialize the map before use.
 *
 * @param hm_name BST map variable name
 * @param key_t Key type
 * @param val_t Value type
 *
 * @note The caller must initialize the map with cbmap_init*() before use
 *
 * @see cbmap_init
 * @see cbmap_construct
 */
#define cbmap_declare(hm_name, key_t, val_t)                              \
  __typeof__(key_t) *hm_name##__ccol_key_type_var                         \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  __typeof__(val_t) *hm_name##__ccol_val_type_var                         \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  cbmap hm_name                /* deliberately not initialized to NULL */

#define cbmap_declare_scoped(hm_name, key_t, val_t)                         \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = \
      NULL;                                                                 \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = \
      NULL;                                                                 \
  cbmap hm_name _ccol_destructor(___cbmap_destroy) = NULL

/**
 * @brief Initialize a BST map with defaults
 *
 * Initializes a BST map that the caller declared before, with the default
 * settings and with the key type taken from the type variable.
 *
 * @param hm_name BST map variable to initialize
 *
 * @note The macro stops the program on failure
 * @note The macro uses the default memory management
 * @note The macro finds the key type by itself (signedness, float, string,
 * struct)
 *
 * @see cbmap_declare
 * @see cbmap_construct
 */
#define cbmap_init(hm_name)                                                  \
  do {                                                                       \
    char *__ccol_cbmap_err = NULL;                                           \
    hm_name = cbmap_create_full(                                             \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var), NULL,  \
        NULL, &__ccol_cbmap_err);                                            \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create BST map '%s': %s", #hm_name,          \
                     __ccol_cbmap_err ? __ccol_cbmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Initialize a BST map with custom memory management
 *
 * Initializes a BST map that the caller declared before, with custom memory
 * management.
 *
 * @param hm_name BST map variable to initialize
 * @param mmgmt_procs Custom memory management procedures
 *
 * @note The macro stops the program on failure
 * @note The macro finds the key type by itself (signedness, float, string,
 * struct)
 */
#define cbmap_init_mp(hm_name, mmgmt_procs)                                  \
  do {                                                                       \
    char *__ccol_cbmap_err = NULL;                                           \
    hm_name = cbmap_create_full(                                             \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        (mmgmt_procs), NULL, &__ccol_cbmap_err);                             \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create BST map '%s': %s", #hm_name,          \
                     __ccol_cbmap_err ? __ccol_cbmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Initialize a BST map with custom comparison
 *
 * Initializes a BST map that the caller declared before, with a custom
 * comparison function.
 *
 * @param hm_name BST map variable to initialize
 * @param custom_comparison_proc Custom comparison function
 *
 * @note The macro stops the program on failure
 * @note The macro uses the default memory management
 */
#define cbmap_init_cc(hm_name, custom_comparison_proc)                       \
  do {                                                                       \
    char *__ccol_cbmap_err = NULL;                                           \
    hm_name = cbmap_create_full(                                             \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var), NULL,  \
        (custom_comparison_proc), &__ccol_cbmap_err);                        \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create BST map '%s': %s", #hm_name,          \
                     __ccol_cbmap_err ? __ccol_cbmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Initialize a BST map with full customization
 *
 * Initializes a BST map that the caller declared before, with custom memory
 * management and a custom comparison.
 *
 * @param hm_name BST map variable to initialize
 * @param mmgmt_procs Custom memory management procedures
 * @param custom_comparison_proc Custom comparison function
 *
 * @note The macro stops the program on failure
 */
#define cbmap_init_full(hm_name, mmgmt_procs, custom_comparison_proc)        \
  do {                                                                       \
    char *__ccol_cbmap_err = NULL;                                           \
    hm_name = cbmap_create_full(                                             \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        (mmgmt_procs), (custom_comparison_proc), &__ccol_cbmap_err);         \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create BST map '%s': %s", #hm_name,          \
                     __ccol_cbmap_err ? __ccol_cbmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Declare and initialize a BST map with defaults
 *
 * Declares and initializes a BST map in one step, with the default settings.
 *
 * @param hm_name BST map variable name
 * @param key_t Key type
 * @param val_t Value type
 *
 * @note The macro stops the program on failure
 * @note The macro finds the key type by itself (signedness, float, string,
 * struct)
 * @note For a struct key_t, the macro uses a raw memcmp order (see
 * cbmap_create_full), so use cbmap_construct_cc for a struct key type
 *
 * Example:
 * @code
 * cbmap_construct(sorted_map, int, char*);
 * int key = 42;
 * cbmap_insert(sorted_map, key, "hello");
 * key = 10;
 * cbmap_insert(sorted_map, key, "world");
 * // The walk visits the keys in this order: 10, 42
 * ccol_for_each(sorted_map, it, {
 *   printf("%d -> %s\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
 * });
 * cbmap_destroy(sorted_map);
 * @endcode
 */
#define cbmap_construct(hm_name, key_t, val_t)                               \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  cbmap hm_name /* _ccol_destructor(___cbmap_destroy) = NULL; */ = NULL;     \
  do {                                                                       \
    char *__ccol_cbmap_err = NULL;                                           \
    hm_name = cbmap_create_full(                                             \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var), NULL,  \
        NULL, &__ccol_cbmap_err);                                            \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create BST map '%s': %s", #hm_name,          \
                     __ccol_cbmap_err ? __ccol_cbmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

#define cbmap_construct_scoped(hm_name, key_t, val_t)                        \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  cbmap hm_name _ccol_destructor(___cbmap_destroy) = NULL;                   \
  do {                                                                       \
    char *__ccol_cbmap_err = NULL;                                           \
    hm_name = cbmap_create_full(                                             \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var), NULL,  \
        NULL, &__ccol_cbmap_err);                                            \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create BST map '%s': %s", #hm_name,          \
                     __ccol_cbmap_err ? __ccol_cbmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Declare and initialize a BST map with custom memory management
 *
 * Declares and initializes a BST map in one step, with custom memory
 * management.
 *
 * @param hm_name BST map variable name
 * @param key_t Key type
 * @param val_t Value type
 * @param mmgmt_procs Custom memory management procedures
 *
 * @note The macro stops the program on failure
 */
#define cbmap_construct_mp(hm_name, key_t, val_t, mmgmt_procs)               \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  cbmap hm_name /* _ccol_destructor(___cbmap_destroy) = NULL; */ = NULL;     \
  do {                                                                       \
    char *__ccol_cbmap_err = NULL;                                           \
    hm_name = cbmap_create_full(                                             \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        (mmgmt_procs), NULL, &__ccol_cbmap_err);                             \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create BST map '%s': %s", #hm_name,          \
                     __ccol_cbmap_err ? __ccol_cbmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

#define cbmap_construct_mp_scoped(hm_name, key_t, val_t, mmgmt_procs)        \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  cbmap hm_name _ccol_destructor(___cbmap_destroy) = NULL;                   \
  do {                                                                       \
    char *__ccol_cbmap_err = NULL;                                           \
    hm_name = cbmap_create_full(                                             \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        (mmgmt_procs), NULL, &__ccol_cbmap_err);                             \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create BST map '%s': %s", #hm_name,          \
                     __ccol_cbmap_err ? __ccol_cbmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Declare and initialize a BST map with custom comparison
 *
 * Declares and initializes a BST map in one step, with a custom comparison
 * function.
 *
 * @param hm_name BST map variable name
 * @param key_t Key type
 * @param val_t Value type
 * @param custom_comparison_proc Custom comparison function
 *
 * @note The macro stops the program on failure
 */
#define cbmap_construct_cc(hm_name, key_t, val_t, custom_comparison_proc)    \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  cbmap hm_name /* _ccol_destructor(___cbmap_destroy) = NULL; */ = NULL;     \
  do {                                                                       \
    char *__ccol_cbmap_err = NULL;                                           \
    hm_name = cbmap_create_full(                                             \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var), NULL,  \
        (custom_comparison_proc), &__ccol_cbmap_err);                        \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create BST map '%s': %s", #hm_name,          \
                     __ccol_cbmap_err ? __ccol_cbmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

#define cbmap_construct_cc_scoped(hm_name, key_t, val_t,                     \
                                  custom_comparison_proc)                    \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  cbmap hm_name _ccol_destructor(___cbmap_destroy) = NULL;                   \
  do {                                                                       \
    char *__ccol_cbmap_err = NULL;                                           \
    hm_name = cbmap_create_full(                                             \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var), NULL,  \
        (custom_comparison_proc), &__ccol_cbmap_err);                        \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create BST map '%s': %s", #hm_name,          \
                     __ccol_cbmap_err ? __ccol_cbmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Declare and initialize a BST map with full customization
 *
 * Declares and initializes a BST map in one step, with custom memory
 * management and a custom comparison.
 *
 * @param hm_name BST map variable name
 * @param key_t Key type
 * @param val_t Value type
 * @param mmgmt_procs Custom memory management procedures
 * @param custom_comparison_proc Custom comparison function
 *
 * @note The macro stops the program on failure
 */
#define cbmap_construct_full(hm_name, key_t, val_t, mmgmt_procs,             \
                             custom_comparison_proc)                         \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  cbmap hm_name /* _ccol_destructor(___cbmap_destroy) = NULL; */ = NULL;     \
  do {                                                                       \
    char *__ccol_cbmap_err = NULL;                                           \
    hm_name = cbmap_create_full(                                             \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        (mmgmt_procs), (custom_comparison_proc), &__ccol_cbmap_err);         \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create BST map '%s': %s", #hm_name,          \
                     __ccol_cbmap_err ? __ccol_cbmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

#define cbmap_construct_full_scoped(hm_name, key_t, val_t, mmgmt_procs,      \
                                    custom_comparison_proc)                  \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  cbmap hm_name _ccol_destructor(___cbmap_destroy) = NULL;                   \
  do {                                                                       \
    char *__ccol_cbmap_err = NULL;                                           \
    hm_name = cbmap_create_full(                                             \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        (mmgmt_procs), (custom_comparison_proc), &__ccol_cbmap_err);         \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create BST map '%s': %s", #hm_name,          \
                     __ccol_cbmap_err ? __ccol_cbmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/* ========================================================================== */
/*                    TYPE-INFERRED OPERATION MACROS */
/* ========================================================================== */

/**
 * @brief Insert a key-value pair (type-inferred)
 *
 * Type-inferred wrapper for cbmap_insert_elem(), which builds the cmap_pair
 * for the key and the cmap_pair for the value from the expressions of the
 * caller and calls ccol_fatal_err() on failure.
 *
 * The macro converts key and val to the declared key type and the declared
 * value type of the map, in the same way as a plain C assignment, and stores
 * the converted copies; it never reinterprets the bytes of an expression of
 * another type, and an expression that has no implicit conversion to the
 * declared type is a compile error. A character-pointer key type or value
 * type keeps the pointer type of the expression instead, so a const char *
 * needs no cast.
 *
 * @param hm_name BST map to insert into
 * @param key Key to insert
 * @param val Value for that key
 *
 * @note The macro stops the program on failure
 * @note The macro takes the address of the key and of the value by itself
 * @note The macro handles a value type and a string type correctly
 * @note The map balances the tree again after the insert
 *
 * @note A NULL key or value of a character-pointer type is not a string, so
 * the macro stops the program with ccol_invalid_args for it
 *
 * @see cbmap_insert_elem
 * @see cbmap_get
 * @see cbmap_remove
 */
#define cbmap_insert(hm_name, key, __ccol_cbmap_val)        \
  _ccol_cbmap_insert_impl(                                  \
      hm_name, (key), (__ccol_cbmap_val),                   \
      _ccol_uniq(__ccol_cbmap_insert_key_tmp, __COUNTER__), \
      _ccol_uniq(__ccol_cbmap_insert_val_tmp, __COUNTER__), \
      _ccol_uniq(__ccol_cbmap_insert_kp, __COUNTER__),      \
      _ccol_uniq(__ccol_cbmap_insert_vp, __COUNTER__),      \
      _ccol_uniq(__ccol_cbmap_insert_r, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_cbmap_insert_impl(                                              \
    hm_name, key, __ccol_cbmap_val, __ccol_cbmap_key_tmp,                     \
    __ccol_cbmap_val_tmp, __ccol_cbmap_kp, __ccol_cbmap_vp, __ccol_cbmap_r)   \
  do {                                                                        \
    /* Each temporary below holds a private copy of the expression that the   \
     * caller wrote. _ccol_declared_or_own_type() gives that copy the         \
     * DECLARED key type or value type of the tree, so the initialization is  \
     * a plain C assignment and it converts the value through the conversion  \
     * rules of the compiler. cvec_push() and chmap_insert() use the same     \
     * construct for the same reason.                                         \
     *                                                                        \
     * A temporary of __typeof__(key) instead would copy the RAW BYTES of the \
     * caller expression into the tree whenever the two types happen to have  \
     * the same width. A float that goes into a cbmap_construct(m, char *,    \
     * int) would store the bit pattern of that float, and a later            \
     * cbmap_get() would report 1069547520 for 1.5f. A key is worse: a float  \
     * key of 2.0f in an int-keyed tree is then unreachable by any int        \
     * lookup for the life of the tree. The widths match, so no size check    \
     * anywhere can see it, and no compiler diagnostic fires at any           \
     * optimization level. An expression whose type has no implicit           \
     * conversion to the declared type is a compile error here instead.       \
     *                                                                        \
     * A narrower expression is the other half of the same rule. A short      \
     * that goes into an int-valued tree becomes an int here, so the node     \
     * always holds exactly sizeof(ValT) bytes. Without the conversion the    \
     * node would hold two bytes, the insert would report success, and the    \
     * value-size check of cbmap_get() would stop the process later, at an    \
     * unrelated call site that did nothing wrong.                            \
     *                                                                        \
     * A declared type that is a character pointer keeps the type of the      \
     * caller expression instead; read _ccol_declared_or_own_type() in        \
     * common.h for why that case must not convert.                           \
     *                                                                        \
     * The copy also settles the lifetime question that                       \
     * _populate_cmap_pair() raises. That helper takes the address of its     \
     * argument from inside one of its own nested blocks, so a compound       \
     * literal that the caller writes would be created there and would die    \
     * with that block. These temporaries hold their own copy for the whole   \
     * call instead. The macro still evaluates the expression of the caller   \
     * exactly once, and an rvalue is as acceptable as an lvalue.             \
     */                                                                       \
    _ccol_declared_or_own_type(hm_name##__ccol_key_type_var, (key))           \
        __ccol_cbmap_key_tmp = (key);                                         \
    _ccol_clear_padding(&__ccol_cbmap_key_tmp);                               \
    _ccol_declared_or_own_type(hm_name##__ccol_val_type_var,                  \
                               (__ccol_cbmap_val)) __ccol_cbmap_val_tmp =     \
        (__ccol_cbmap_val);                                                   \
    cmap_pair *__ccol_cbmap_kp = &(cmap_pair){};                              \
    cmap_pair *__ccol_cbmap_vp = &(cmap_pair){};                              \
    _populate_cmap_pair(__ccol_cbmap_kp, __ccol_cbmap_key_tmp);               \
    _populate_cmap_pair(__ccol_cbmap_vp, __ccol_cbmap_val_tmp);               \
    /* A pair with no pointer comes only from a NULL character pointer,       \
     * which is not a string. The raw layer of this module accepts a pair     \
     * of size 0 as an empty key or value, so the macro refuses it here,      \
     * with the code that chmap_insert() reports for the same argument.       \
     * For every other type the pointer is the address of a temporary and     \
     * the test folds away. */                                                \
    ccol_retval_t __ccol_cbmap_r =                                            \
        (!__ccol_cbmap_kp->ptr || !__ccol_cbmap_vp->ptr)                      \
            ? ccol_invalid_args                                               \
            : cbmap_insert_elem(hm_name, __ccol_cbmap_kp, __ccol_cbmap_vp);   \
    if (__ccol_cbmap_r != ccol_success &&                                     \
        __ccol_cbmap_r != ccol_key_already_present) {                         \
      _ccol_dump_key_to_stderr(__ccol_cbmap_kp->ptr, __ccol_cbmap_kp->size);  \
      ccol_fatal_err("cbmap_insert('%s'): r: %d (%s)", #hm_name,              \
                     __ccol_cbmap_r, ccol_retval_to_str(__ccol_cbmap_r));     \
    }                                                                         \
  } while (0)

/**
 * @brief Remove a key-value pair (type-inferred)
 *
 * Type-inferred wrapper for cbmap_delete_elem() that returns the result code.
 *
 * @param hm_name BST map to remove from
 * @param key Key to remove
 *
 * @return ccol_success if the macro removes the key, or ccol_key_not_found
 * if the key is not in the map
 *
 * @note The macro does not stop the program on ccol_key_not_found
 * @note The macro frees the memory of the key and of the value
 * @note The map balances the tree again after the delete
 *
 * @note A NULL key of a character-pointer type is not a string, so the
 * macro returns ccol_invalid_args for it
 *
 * @see cbmap_delete_elem
 * @see cbmap_insert
 */
#define cbmap_remove(hm_name, key)                                          \
  _ccol_cbmap_remove_impl(                                                  \
      hm_name, (key), _ccol_uniq(__ccol_cbmap_remove_key_tmp, __COUNTER__), \
      _ccol_uniq(__ccol_cbmap_remove_kp, __COUNTER__),                      \
      _ccol_uniq(__ccol_cbmap_remove_r, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_cbmap_remove_impl(hm_name, key, __ccol_cbmap_key_tmp,          \
                                __ccol_cbmap_kp, __ccol_cbmap_r)             \
  ({                                                                         \
    /* The temporary carries the DECLARED key type of the tree.              \
     * See cbmap_insert() for why the lookup key goes through a              \
     * conversion and not through a raw byte copy. */                        \
    _ccol_declared_or_own_type(hm_name##__ccol_key_type_var, (key))          \
        __ccol_cbmap_key_tmp = (key);                                        \
    _ccol_clear_padding(&__ccol_cbmap_key_tmp);                              \
    cmap_pair *__ccol_cbmap_kp = &(cmap_pair){};                             \
    _populate_cmap_pair(__ccol_cbmap_kp, __ccol_cbmap_key_tmp);              \
    ccol_retval_t __ccol_cbmap_r =                                           \
        !__ccol_cbmap_kp->ptr ? ccol_invalid_args                            \
                              : cbmap_delete_elem(hm_name, __ccol_cbmap_kp); \
    __ccol_cbmap_r;                                                          \
  })

/**
 * @brief Get value by key (type-inferred, returns value)
 *
 * Type-inferred wrapper for cbmap_get_elem_ref() that returns the value
 * itself. It calls ccol_fatal_err() if the key is not in the map, and also
 * calls ccol_fatal_err() if the two sizes do not match.
 *
 * @param hm_name BST map to search
 * @param key Key to look up
 *
 * @return The value of that key
 *
 * @note The macro stops the program if the key is not in the map
 * @note The macro stops the program if the size of the stored value does not
 * match the size of the declared value type
 * @note The macro returns the value, not a pointer
 * @note For a string, the macro returns the char* itself
 * @note The search takes O(log n) time
 *
 * @note A NULL key of a character-pointer type is not a string, so the
 * macro stops the program with ccol_invalid_args for it
 *
 * @see cbmap_get_ptr
 * @see cbmap_insert
 */
#define cbmap_get(hm_name, key)                                           \
  _ccol_cbmap_get_impl(hm_name, (key),                                    \
                       _ccol_uniq(__ccol_cbmap_get_key_tmp, __COUNTER__), \
                       _ccol_uniq(__ccol_cbmap_get_val, __COUNTER__),     \
                       _ccol_uniq(__ccol_cbmap_get_kp, __COUNTER__),      \
                       _ccol_uniq(__ccol_cbmap_get_vp, __COUNTER__),      \
                       _ccol_uniq(__ccol_cbmap_get_r, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_cbmap_get_impl(hm_name, key, __ccol_cbmap_key_tmp,               \
                             __ccol_cbmap_val, __ccol_cbmap_kp,                \
                             __ccol_cbmap_vp, __ccol_cbmap_r)                  \
  ({                                                                           \
    /* The temporary carries the DECLARED key type of the tree. See            \
     * cbmap_insert() for why the lookup key goes through a conversion and     \
     * not through a raw byte copy. A key that the tree stored through that    \
     * conversion is only ever found again by a lookup that makes the same     \
     * one. */                                                                 \
    _ccol_declared_or_own_type(hm_name##__ccol_key_type_var, (key))            \
        __ccol_cbmap_key_tmp = (key);                                          \
    _ccol_clear_padding(&__ccol_cbmap_key_tmp);                                \
    __typeof__(*hm_name##__ccol_val_type_var) *__ccol_cbmap_val = NULL;        \
    cmap_pair *__ccol_cbmap_kp = &(cmap_pair){};                               \
    const cmap_pair *__ccol_cbmap_vp = NULL;                                   \
    _populate_cmap_pair(__ccol_cbmap_kp, __ccol_cbmap_key_tmp);                \
    ccol_retval_t __ccol_cbmap_r =                                             \
        !__ccol_cbmap_kp->ptr                                                  \
            ? ccol_invalid_args                                                \
            : cbmap_get_elem_ref(hm_name, __ccol_cbmap_kp, &__ccol_cbmap_vp);  \
    if (__ccol_cbmap_r != ccol_success) {                                      \
      _ccol_dump_key_to_stderr(__ccol_cbmap_kp->ptr, __ccol_cbmap_kp->size);   \
      ccol_fatal_err("cbmap_get('%s'): r: %d (%s)", #hm_name, __ccol_cbmap_r,  \
                     ccol_retval_to_str(__ccol_cbmap_r));                      \
    }                                                                          \
    if (ccol_is_char_ptr(*hm_name##__ccol_val_type_var)) {                     \
      __ccol_cbmap_val = (__typeof__(*hm_name##__ccol_val_type_var) *)&(       \
          __ccol_cbmap_vp->ptr);                                               \
    } else if (__ccol_cbmap_vp->size != sizeof(*__ccol_cbmap_val)) {           \
      ccol_fatal_err(                                                          \
          "cbmap_get('%s'): value size mismatch - stored: %lu bytes, "         \
          "requested: %lu bytes; wrong type or missing cbmap_redeclare()?",    \
          #hm_name, (unsigned long)__ccol_cbmap_vp->size,                      \
          (unsigned long)sizeof(*__ccol_cbmap_val));                           \
    } else {                                                                   \
      __ccol_cbmap_val =                                                       \
          (__typeof__(*hm_name##__ccol_val_type_var) *)(__ccol_cbmap_vp->ptr); \
    }                                                                          \
    *__ccol_cbmap_val;                                                         \
  })

/* Gives p without a change for every value type but a string; for a map with
 * string values, it gives a pointer whose target is const-qualified.
 *
 * A map that stores strings owns the bytes and keeps its own {ptr, size}
 * accessor for the node in step with those bytes. The only char* object in
 * the whole map is the ptr field of that accessor, which is why
 * cbmap_get_ptr() can only give back the address of that field for a map
 * with string values. A store of a different char* through that address
 * replaces the pointer while the size goes on describing the older string,
 * and it hands the map a pointer that the map does not own and whose
 * lifetime the map cannot control. Without the const,
 * cbmap_get_elem_copy() and every read of an iterator value then walk the
 * new buffer, which can be much shorter, for the length of the older string.
 * AddressSanitizer reports a global-buffer-overflow read of the old length
 * on the first such read.
 *
 * The const makes that store a compile error. The caller can read the
 * stored char* through the returned pointer, and can write to the string
 * bytes that it points at, within the stored length. To replace a string
 * value, call cbmap_insert(), which frees the old bytes and then copies the
 * new bytes into storage that the map owns. */
#define _ccol_cbmap_value_ptr_result(hm_name, p)                               \
  _Generic(*hm_name##__ccol_val_type_var,                                      \
      char *: (__typeof__(*hm_name##__ccol_val_type_var) const *)(p),          \
      const char *: (__typeof__(*hm_name##__ccol_val_type_var) const *)(p),    \
      signed char *: (__typeof__(*hm_name##__ccol_val_type_var) const *)(p),   \
      const signed char *: (__typeof__(*hm_name##__ccol_val_type_var)          \
                                const *)(p),                                   \
      unsigned char *: (__typeof__(*hm_name##__ccol_val_type_var) const *)(p), \
      const unsigned char *: (__typeof__(*hm_name##__ccol_val_type_var)        \
                                  const *)(p),                                 \
      default: (p))

/**
 * @brief Get pointer to value by key (type-inferred, returns pointer or NULL)
 *
 * Type-inferred wrapper for cbmap_get_elem_ref() that returns a pointer to
 * the value, or NULL if the key is not in the map. Note that cbmap_get()
 * stops the program in that case, while this macro does not.
 *
 * @param hm_name BST map to search
 * @param key Key to look up
 *
 * @return Pointer to the value, or NULL if the key is not in the map
 *
 * @note The macro returns NULL if the key is not in the map, and does not
 * stop the program
 * @note The macro stops the program if the size of the value does not match
 * the size of the type
 * @note The macro returns a pointer to the value, so the caller can change
 * the value in place
 * @note An insert or a delete makes the pointer invalid
 * @note For a char* value type, the target of the pointer is const-qualified
 * (char *const *): the caller can read the stored string and edit its bytes
 * in place, within the stored length, but a replacement of the pointer
 * itself is a compile error. The map owns the string bytes and keeps its own
 * accessor in step with them, so a replacement of the pointer would leave
 * the length of the accessor describing the older string. To replace a
 * string value, call cbmap_insert(), which frees the old bytes and copies
 * the new bytes into storage that the map owns
 * @note The search takes O(log n) time
 *
 * @note A NULL key of a character-pointer type is not a string, so the
 * macro gives NULL for it
 *
 * @see cbmap_get
 * @see cbmap_get_elem_ref
 * @see cbmap_insert
 *
 * Example:
 * @code
 * cbmap_construct(map, int, int);
 * int key = 42, value = 100;
 * cbmap_insert(map, key, value);
 * int* ptr = cbmap_get_ptr(map, key);
 * if (ptr) {
 *   *ptr = 200; // Change the value in place
 * }
 *
 * cbmap_construct(names, int, char *);
 * cbmap_insert(names, key, "hello");
 * char *const *sptr = cbmap_get_ptr(names, key);
 * if (sptr) {
 *   printf("%s\n", *sptr);   // Read the stored string
 *   (*sptr)[0] = 'j';        // Edit its bytes in place: "jello"
 *   cbmap_insert(names, key, "a longer replacement");  // Replace the string
 * }
 * @endcode
 */
#define cbmap_get_ptr(hm_name, key)                                          \
  _ccol_cbmap_get_ptr_impl(                                                  \
      hm_name, (key), _ccol_uniq(__ccol_cbmap_get_ptr_key_tmp, __COUNTER__), \
      _ccol_uniq(__ccol_cbmap_get_ptr_val, __COUNTER__),                     \
      _ccol_uniq(__ccol_cbmap_get_ptr_kp, __COUNTER__),                      \
      _ccol_uniq(__ccol_cbmap_get_ptr_vp, __COUNTER__),                      \
      _ccol_uniq(__ccol_cbmap_get_ptr_r, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_cbmap_get_ptr_impl(hm_name, key, __ccol_cbmap_key_tmp,          \
                                 __ccol_cbmap_val, __ccol_cbmap_kp,           \
                                 __ccol_cbmap_vp, __ccol_cbmap_r)             \
  ({                                                                          \
    /* The temporary carries the DECLARED key type of the tree. See           \
     * cbmap_insert() for why the lookup key goes through a conversion and    \
     * not through a raw byte copy. A key that the tree stored through that   \
     * conversion is only ever found again by a lookup that makes the same    \
     * one. */                                                                \
    _ccol_declared_or_own_type(hm_name##__ccol_key_type_var, (key))           \
        __ccol_cbmap_key_tmp = (key);                                         \
    _ccol_clear_padding(&__ccol_cbmap_key_tmp);                               \
    __typeof__(*hm_name##__ccol_val_type_var) *__ccol_cbmap_val = NULL;       \
    cmap_pair *__ccol_cbmap_kp = &(cmap_pair){};                              \
    const cmap_pair *__ccol_cbmap_vp = NULL;                                  \
    _populate_cmap_pair(__ccol_cbmap_kp, __ccol_cbmap_key_tmp);               \
    ccol_retval_t __ccol_cbmap_r =                                            \
        !__ccol_cbmap_kp->ptr                                                 \
            ? ccol_invalid_args                                               \
            : cbmap_get_elem_ref(hm_name, __ccol_cbmap_kp, &__ccol_cbmap_vp); \
    if (__ccol_cbmap_r == ccol_success) {                                     \
      if (ccol_is_char_ptr(*hm_name##__ccol_val_type_var)) {                  \
        __ccol_cbmap_val = (__typeof__(*hm_name##__ccol_val_type_var) *)&(    \
            __ccol_cbmap_vp->ptr);                                            \
      } else if (__ccol_cbmap_vp->size != sizeof(*__ccol_cbmap_val)) {        \
        ccol_fatal_err(                                                       \
            "cbmap_get_ptr('%s'): value size mismatch - stored: %lu bytes, "  \
            "requested: %lu bytes; wrong type or missing cbmap_redeclare()?", \
            #hm_name, (unsigned long)__ccol_cbmap_vp->size,                   \
            (unsigned long)sizeof(*__ccol_cbmap_val));                        \
      } else {                                                                \
        __ccol_cbmap_val =                                                    \
            (__typeof__(*hm_name##__ccol_val_type_var) *)(__ccol_cbmap_vp     \
                                                              ->ptr);         \
      }                                                                       \
    }                                                                         \
    _ccol_cbmap_value_ptr_result(hm_name, __ccol_cbmap_val);                  \
  })

#pragma GCC visibility pop
