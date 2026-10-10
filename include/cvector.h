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
#include "csort.h"

/* Everything declared from here to the end of this header is part of the
 * public Application Binary Interface (ABI) of libccollections, and the shared
 * library exports all of it. The library is built with -fvisibility=hidden,
 * so a function or object that is not inside one of these blocks stays
 * internal to the library: it is absent from the dynamic symbol table, the
 * application that links against the library cannot interpose it, and a
 * symbol with the same name in that application cannot collide with it. */
#pragma GCC visibility push(default)

/**
 * @file cvector.h
 * @brief Dynamic array (vector) container that changes its size
 * automatically
 *
 * This header gives a generic dynamic array container that grows when the
 * caller adds elements and shrinks when the caller removes them. The vector
 * stores elements of a fixed size, which the caller gives at creation time.
 *
 * Key features:
 * - Automatic capacity control (grows by 2x, shrinks by 0.5x)
 * - Minimum capacity of 4 elements
 * - Type-inferred macros for the common operations
 * - Support for custom memory management
 * - Amortized O(1) push and pop operations
 */

/** @brief Opaque vector structure */
typedef struct cvector cvector;

/** @brief Pointer to vector (handle type) */
typedef cvector *cvec;

/* ========================================================================== */
/*                         CORE VECTOR FUNCTIONS                              */
/* ========================================================================== */

/**
 * @brief Create a vector with custom memory management
 *
 * This function creates a new vector container that stores elements of the
 * size that the caller gives. The vector starts with a minimum capacity of 4
 * elements and changes its capacity automatically when necessary.
 *
 * @param elem_size Size of each element in bytes. The value must be more
 * than 0 and must not be more than SIZE_MAX / 4, because a larger value makes
 * the allocation of the first 4-element backing buffer overflow.
 * @param mmgmt_procs Custom memory management procedures, or NULL to use
 * the default malloc and free
 * @param err Optional pointer that receives an error string on failure.
 * Pass NULL to ignore the error string.
 *
 * @return Pointer to the new vector, or NULL on failure
 *
 * @note The first capacity is 4 elements
 * @note The capacity doubles when the vector is full and the caller adds
 * one more element, and it halves when less than 1/4 of the capacity is full.
 * @note The caller must destroy the vector with cvector_destroy() after use
 *
 * @see cvector_create
 * @see cvector_destroy
 */
cvec cvector_create_full(size_t elem_size, ccol_memmgmt_procs_t *mmgmt_procs,
                         char **err);

/**
 * @brief Create a vector with default memory management
 *
 * This convenience wrapper creates a vector that uses the standard malloc
 * and free.
 *
 * @param elem_size Size of each element in bytes
 * @param err Optional pointer that receives an error string on failure
 *
 * @return Pointer to the new vector, or NULL on failure
 */
static inline __attribute__((always_inline)) cvec
cvector_create(size_t elem_size, char **err) {
  return cvector_create_full(elem_size, NULL, err);
}

/**
 * @brief Get the memory management procedures for a vector
 *
 * This function gives the memory management procedures struct of this
 * vector.
 *
 * @param v Vector to query
 *
 * @return Pointer to the memory management procedures, or NULL when the
 * vector uses the default procedures
 *
 * @note This function asserts when v is NULL
 */
ccol_memmgmt_procs_t *cvector_get_mprocs(cvec v);

/**
 * @brief Destroy a vector (internal function)
 *
 * @param v Vector to destroy
 *
 * @warning Do not call this function directly. Use the cvector_destroy()
 * macro instead.
 */
void __cvector_destroy(cvec v);

/**
 * @brief Destroy a vector and set pointer to NULL
 *
 * This macro frees all the resources of the vector, including the data
 * buffer, and then sets the vector pointer to NULL.
 *
 * @param v Vector to destroy. The macro sets it to NULL after it destroys
 * the vector. It must be a modifiable lvalue, such as a variable or an
 * element of an array, and the macro evaluates it exactly once.
 *
 * @note A call with a NULL pointer is safe and does nothing
 * @note When the vector holds pointers to heap buffers, the macro does not
 * free those elements one by one, so the caller must free the element data
 * first when necessary.
 */
#define cvector_destroy(v)    \
  _ccol_cvector_destroy_impl( \
      v, _ccol_uniq(__ccol_cvector_destroy_slot, __COUNTER__))

/* Internal: the body of cvector_destroy. slot is a name from _ccol_uniq(),
 * so the macro nests inside the argument of another destroy macro and stays
 * -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_cvector_destroy_impl(v, slot) \
  do {                                      \
    __typeof__(v) *slot = &(v);             \
    if (*slot) {                            \
      __cvector_destroy(*slot);             \
      *slot = NULL;                         \
    }                                       \
  } while (0)

/**
 * @brief Append an element to the end of the vector
 *
 * This function adds a new element to the end of the vector, doubling the
 * capacity automatically when the vector is full. It copies the element into
 * the vector with an assignment that the library optimizes for the size of
 * the element.
 *
 * @param v Vector to append to
 * @param new_elem Pointer to the element to append. It must not be NULL.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args when new_elem is NULL
 * @return ccol_container_full when the vector is at ccol_max_elem_count
 * @return ccol_not_enough_memory when the capacity cannot grow
 *
 * @note Amortized O(1) complexity
 * @note The function copies the element into the vector
 * @note The capacity doubles when the vector is full (a scaling factor of 2)
 * @note This function asserts when v is NULL
 * @note new_elem can safely alias into the backing buffer of v, for example
 *       a pointer that cvector_data_ptr(v) or cvector_at(v, i) gives: a
 *       capacity growth that this call starts does not invalidate such a
 *       pointer.
 *
 * @see cvector_pop_back
 * @see cvec_push
 */
ccol_retval_t cvector_push_back(cvec v, const void *new_elem);

/**
 * @brief Remove and return the last element from the vector
 *
 * This function removes the last element from the vector and copies it to
 * the target buffer. The vector halves its capacity automatically when less
 * than 1/4 of the capacity is full; the minimum capacity is 4.
 *
 * @param v Vector to pop from
 * @param target_elem Pointer to the buffer that receives the element. It
 * must not be NULL.
 *
 * @return ccol_success on success
 * @return ccol_invalid_args when target_elem is NULL
 * @return ccol_container_empty when the vector is empty
 *
 * @note O(1) complexity
 * @note The function copies the element to target_elem
 * @note The capacity halves when less than 1/4 of it is full, down to the
 * minimum of 4
 * @note This function asserts when v is NULL
 *
 * @see cvector_push_back
 * @see cvec_pop
 */
ccol_retval_t cvector_pop_back(cvec v, void *target_elem);

/**
 * @brief Access an element at a specific index
 *
 * This function gives a pointer to the element at the index that the caller
 * gives. The caller can use the pointer to read the element or to change it
 * in place.
 *
 * @param v Vector to access
 * @param index Zero-based index of the element to access
 *
 * @return Pointer to the element at the index, or NULL when the index is
 * out of bounds
 *
 * @note O(1) complexity
 * @note The function gives NULL when index >= elem_count, and also when the
 * vector is empty.
 * @note The pointer stays valid only until the size of the vector changes
 * @note This function asserts when v is NULL
 *
 * @see cvec_at
 * @see cvector_elem_count
 */
void *cvector_at(cvec v, size_t index);

/**
 * @brief Get the number of elements in the vector
 *
 * This function gives the current number of elements in the vector.
 *
 * @param v Vector to query
 *
 * @return Number of elements in the vector
 *
 * @note O(1) complexity
 * @note This function asserts when v is NULL
 *
 * @see cvec_size
 */
size_t cvector_elem_count(cvec v);

/**
 * @brief Clear all elements and reset capacity
 *
 * This function removes all the elements from the vector and then tries to
 * shrink the capacity back to the minimum of 4 elements. When the
 * reallocation fails, the capacity stays the same, but the function sets the
 * element count to 0 all the same.
 *
 * @param v Vector to reset
 *
 * @note The element count becomes 0
 * @note The capacity goes back to the minimum of 4 when the reallocation
 * succeeds
 * @note The function does not free the elements one by one, so the caller
 * must do this first when necessary.
 * @note This function asserts when v is NULL
 *
 * @see cvec_reset
 */
void cvector_reset(cvec v);

/**
 * @brief Reserve capacity for future elements
 *
 * This function allocates, in advance, capacity for new_capacity_count
 * elements or more, rounded up to the nearest power of two with a minimum
 * of 4. A reservation prevents many reallocations when you know the final
 * size in advance.
 *
 * @param v Vector to reserve capacity for
 * @param new_capacity_count Minimum number of elements to reserve space for
 *
 * @return true when the reservation succeeds, false on failure
 *
 * @note The function rounds the real capacity up to the nearest power of
 * two that is 4 or more
 * @note The function does not shrink the capacity when new_capacity is less
 * than or equal to the current capacity
 * @note The function gives true immediately when the vector already has
 * enough capacity
 * @note The function gives false when new_capacity > ccol_max_elem_count
 * @note This function asserts when v is NULL
 *
 * @see cvec_reserve
 *
 * Example:
 * @code
 * cvec v = cvector_create(sizeof(int), NULL);
 * cvector_reserve(v, 100);  // Allocate 128 elements (the next power of 2)
 * // Now you can push 100 elements or more with no reallocation
 * @endcode
 */
bool cvector_reserve(cvec v, size_t new_capacity_count);

/**
 * @brief Append an array of elements to the vector
 *
 * This function efficiently appends several elements from a C array to the
 * end of the vector. It grows the capacity automatically, with at most one
 * reallocation when necessary.
 *
 * @param v Vector to append to
 * @param arr_ptr Pointer to the array of elements to append. The function
 * only reads through it, so it can point at a const array.
 * @param elem_count Number of elements in the array
 *
 * @return true on success, false on failure
 *
 * @note This function is more efficient than many push_back calls
 * @note The function copies the whole range in one operation instead of
 * element by element.
 * @note The function can grow the capacity with cvector_reserve
 * @note The function gives false when the result is more than
 * ccol_max_elem_count
 * @note The function gives false when elem_count causes an overflow
 * @note This function asserts when v is NULL
 * @note arr_ptr can safely alias into the backing buffer of v, for example
 *       a pointer that cvector_data_ptr(v) or cvector_at(v, i) gives: a
 *       capacity growth that this call starts does not invalidate such a
 *       pointer.
 * @note The source and the destination byte ranges can overlap, for example
 *       when arr_ptr aliases the buffer of v and its elem_count reads past
 *       the current element count of v, into capacity that the vector has
 *       already reserved. The copy is safe when the ranges overlap.
 * @note A source range that aliases must stay inside the reserved capacity
 *       of v. When arr_ptr points into the backing buffer of v and its
 *       elem_count reads past the end of the reservation of that buffer, the
 *       function gives false and leaves v unchanged. Only a source range
 *       that aliases can have this bound, because an array of the caller
 *       passed as a bare pointer (as every C array is) has no extent that
 *       this function can see.
 *
 * @see cvector_append_cvector
 * @see cvec_append_array
 *
 * Example:
 * @code
 * int arr[] = {1, 2, 3, 4, 5};
 * cvec v = cvector_create(sizeof(int), NULL);
 * cvector_append_array(v, arr, 5);  // Add all the 5 elements in one call
 * @endcode
 */
bool cvector_append_array(cvec v, const void *arr_ptr, size_t elem_count);

/**
 * @brief Append all elements from one vector to another
 *
 * This function efficiently copies all the elements of v_from to the end of
 * v_to. Both vectors must have the same element size.
 *
 * @param v_to Destination vector to append to
 * @param v_from Source vector to copy the elements from
 *
 * @return true on success, false on failure
 *
 * @note Both vectors must have the same elem_size
 * @note v_from stays unchanged
 * @note The function can grow the capacity of v_to
 * @note The function gives false when the result is more than
 * ccol_max_elem_count
 * @note This function asserts when one of the vectors is NULL, and also
 * when the two elem_size values are different.
 *
 * @see cvector_append_array
 * @see cvec_append_cvec
 *
 * Example:
 * @code
 * cvec v1 = cvector_create(sizeof(int), NULL);
 * cvec v2 = cvector_create(sizeof(int), NULL);
 * // ... fill v1 and v2 ...
 * cvector_append_cvector(v1, v2);  // v1 now has all the elements of v2
 * @endcode
 */
bool cvector_append_cvector(cvec v_to, cvec v_from);

/**
 * @brief Get pointer to the underlying data array
 *
 * This function gives a direct pointer to the internal data buffer of the
 * vector, which is useful when you pass the vector to a function that
 * expects a C array.
 *
 * @param v Vector to get the data pointer from
 *
 * @return Pointer to the internal data array
 *
 * @warning The pointer becomes invalid after an operation that can change
 * the size of the vector
 * @warning Do not free this pointer; the vector owns it.
 * @note This function asserts when v is NULL
 *
 * @see cvec_data_ptr
 *
 * Example:
 * @code
 * cvec v = cvector_create(sizeof(int), NULL);
 * // ... add the elements ...
 * int *arr = cvector_data_ptr(v);
 * for (size_t i = 0; i < cvector_elem_count(v); i++) {
 *   printf("%d ", arr[i]);
 * }
 * @endcode
 */
void *cvector_data_ptr(cvec v);

/**
 * @brief Destroy vector and set pointer to NULL (cleanup helper)
 *
 * This helper works with the _ccol_destructor attribute to clean up
 * automatically when a variable goes out of scope: it destroys the vector
 * and sets the pointer to NULL.
 *
 * @param cv Pointer to the vector pointer
 *
 * @note The library designed this function for __attribute__((cleanup))
 * @note A call with NULL, or with a pointer to NULL, is safe
 * @note This is an internal helper; prefer the cvector_destroy() macro.
 *
 * Example:
 * @code
 * {
 *   cvec v _ccol_destructor(___cvector_destroy) = cvector_create(sizeof(int),
 *                                                 NULL);
 *   // ... use the vector ...
 * } // The vector is destroyed automatically at the end of the scope
 * @endcode
 */
static inline void ___cvector_destroy(cvec *cv) {
  if (cv && *cv) {
    __cvector_destroy(*cv);
    *cv = NULL;
  }
}

/* ========================================================================== */
/*                         ITERATOR */
/* ========================================================================== */

/**
 * @brief Find the index of the first occurrence of an element
 *
 * This function does a linear scan and gives the zero-based index of the
 * first element that is equal to *elem. When cmp is NULL, the function
 * compares with memcmp over the element size, which is byte-wise equality.
 *
 * @param v    Vector to search. It must not be NULL.
 * @param elem Pointer to the value to search for. It must not be NULL.
 * @param cmp  Comparison function, or NULL to use memcmp
 *
 * @return Zero-based index of the first match, or ccol_invalid_size when the
 * function finds no match or when elem is NULL.
 *
 * @note O(n) complexity
 * @note This function asserts when v is NULL
 * @note A struct with padding bytes can compare incorrectly when cmp is
 * NULL
 *
 * @see cvec_find
 */
size_t cvector_find(cvec v, const void *elem, ccol_comparison_proc_t cmp);

/**
 * @brief Create an iterator positioned at the first element.
 *
 * This function gives a @c cmap_iterator* whose @c key_pair->ptr points to
 * the internal index field and whose @c val_pair->ptr points directly into
 * the buffer of the vector. The function sets the @c _direct_ptr flag to
 * true, so the unified accessor macros (@c ccol_iter_key_ptr and
 * @c ccol_iter_val_ptr) skip the map SSO path and give typed pointers
 * straight into the buffer.
 *
 * @param v    Vector to iterate
 * @param err  Optional pointer that receives an error string on failure
 *
 * @return Pointer to a @c cmap_iterator, or NULL when the vector is empty or
 *         when the allocation fails.
 *
 * @note cvec_iter_next() destroys the iterator automatically when it
 *       reaches the end. Call ccol_iter_destroy() to stop before the end.
 * @note A change to the vector during the iteration invalidates the
 *       iterator.
 * @note The function treats a NULL v in the same way as an empty vector: it
 * gives NULL, not an error. This behaviour is deliberate and is the same
 * NULL tolerance that chashmap_begin_iter() and cbmap_begin_iter() have. A
 * container field can stay uninitialized because the caller inserted
 * nothing into it, and the caller can iterate such a field directly without
 * a NULL guard of its own.
 *
 * @see cvec_begin
 * @see ccol_iter_next
 */
cmap_iterator *cvector_begin_iter(cvec v, char **err);

/* ========================================================================== */
/*                         TYPE-INFERRED CONVENIENCE MACROS */
/* ========================================================================== */

/**
 * @brief Declare an uninitialized vector variable
 *
 * This macro declares a vector variable 'v' together with a type variable
 * that the macros use for type safety. The caller must initialize the vector
 * before use.
 *
 * @param v Name of the vector variable to declare
 * @param type Element type for the vector
 *
 * @note The caller must initialize the vector with cvec_init() or with
 * cvec_construct() before use
 * @note The name of the type variable is v##__ccol_val_type_var, which the
 * macros use internally.
 *
 * @see cvec_init
 * @see cvec_construct
 *
 * Example:
 * @code
 * cvec_declare(my_vec, int);
 * cvec_init(my_vec);
 * // ... use the vector ...
 * cvec_destroy(my_vec);
 * @endcode
 */
#define cvec_declare(v, type)                                             \
  size_t *v##__ccol_key_type_var                                          \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  __typeof__(type) *v##__ccol_val_type_var                                \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  cvec v                       /* deliberately not initialized to NULL */

#define cvec_declare_scoped(v, type)                                       \
  size_t *v##__ccol_key_type_var __attribute__((unused)) = NULL;           \
  __typeof__(type) *v##__ccol_val_type_var __attribute__((unused)) = NULL; \
  cvec v _ccol_destructor(___cvector_destroy) = NULL

/**
 * @brief Turn on the type-inferred macros for a vector in a local scope
 *
 * This macro declares the type variable that the type-inferred macros need.
 * Use it when the caller created the vector without cvec_declare or
 * cvec_construct.
 *
 * @param v Name of the vector variable
 * @param type Type of the elements in the vector
 *
 * @note Use this macro when you have a cvec from another scope and you want
 * type-inferred access to it
 *
 * Example:
 * @code
 * void process(cvec vec) {
 *   cvec_redeclare(vec, int);
 *   int val = cvec_at(vec, 0);
 * }
 * @endcode
 */
#define cvec_redeclare(v, type)                                  \
  size_t *v##__ccol_key_type_var __attribute__((unused)) = NULL; \
  __typeof__(type) *v##__ccol_val_type_var __attribute__((unused)) = NULL

/**
 * @brief Initialize a declared vector, with error handling
 *
 * This macro initializes a vector that cvec_declare() declared, and stops
 * the program with ccol_fatal_err() when the initialization fails.
 *
 * @param v Vector variable to initialize
 *
 * @note The macro calls ccol_fatal_err() when the initialization fails
 * @note The caller must declare the vector with cvec_declare() first
 * @note The vector uses the default memory management (malloc and free)
 *
 * @see cvec_declare
 * @see cvec_construct
 * @see cvec_init_mp
 *
 * Example:
 * @code
 * cvec_declare(my_vec, int);
 * cvec_init(my_vec);  // Stops the program on failure
 * @endcode
 */
#define cvec_init(v)                                                       \
  do {                                                                     \
    char *__ccol_cvec_err = NULL;                                          \
    v = cvector_create(sizeof(*v##__ccol_val_type_var), &__ccol_cvec_err); \
    if (!(v)) {                                                            \
      ccol_fatal_err("cvec_init('%s'): %s", #v,                            \
                     __ccol_cvec_err ? __ccol_cvec_err : "unknown error"); \
    }                                                                      \
  } while (0)

/**
 * @brief Initialize a declared vector with custom memory management
 *
 * This macro initializes a vector with custom memory management procedures
 * and stops the program with ccol_fatal_err() when the initialization fails.
 *
 * @param v Vector variable to initialize
 * @param mprocs Pointer to the custom memory management procedures
 *
 * @note The macro calls ccol_fatal_err() when the initialization fails
 * @note The caller must declare the vector with cvec_declare() first
 *
 * @see cvec_init
 * @see cvec_declare
 *
 * Example:
 * @code
 * ccol_memmgmt_procs_t my_mprocs = { ... };
 * cvec_declare(my_vec, int);
 * cvec_init_mp(my_vec, &my_mprocs);
 * @endcode
 */
#define cvec_init_mp(v, mprocs)                                            \
  do {                                                                     \
    char *__ccol_cvec_err = NULL;                                          \
    v = cvector_create_full(sizeof(*v##__ccol_val_type_var), (mprocs),     \
                            &__ccol_cvec_err);                             \
    if (!(v)) {                                                            \
      ccol_fatal_err("cvec_init_mp('%s'): %s", #v,                         \
                     __ccol_cvec_err ? __ccol_cvec_err : "unknown error"); \
    }                                                                      \
  } while (0)

/**
 * @brief Declare and initialize a vector in one step
 *
 * This convenience macro combines cvec_declare() and cvec_init(), and stops
 * the program with ccol_fatal_err() when the initialization fails.
 *
 * @param v Name of the vector variable to create
 * @param type Element type for the vector
 *
 * @note The macro calls ccol_fatal_err() when the initialization fails
 * @note The macro is equal to: cvec_declare(v, type); cvec_init(v);
 *
 * @see cvec_declare
 * @see cvec_init
 * @see cvec_construct_mp
 *
 * Example:
 * @code
 * cvec_construct(my_vec, int);
 * cvec_push(my_vec, 42);
 * cvec_destroy(my_vec);
 * @endcode
 */
#define cvec_construct(v, type) \
  cvec_declare(v, type);        \
  cvec_init(v)

#define cvec_construct_scoped(v, type) \
  cvec_declare_scoped(v, type);        \
  cvec_init(v)

/**
 * @brief Declare and initialize a vector with custom memory management
 *
 * This convenience macro combines cvec_declare() and cvec_init_mp(), and
 * stops the program with ccol_fatal_err() when the initialization fails.
 *
 * @param v Name of the vector variable to create
 * @param type Element type for the vector
 * @param mprocs Pointer to the custom memory management procedures
 *
 * @note The macro calls ccol_fatal_err() when the initialization fails
 *
 * @see cvec_construct
 * @see cvec_init_mp
 *
 * Example:
 * @code
 * ccol_memmgmt_procs_t my_mprocs = { ... };
 * cvec_construct_mp(my_vec, int, &my_mprocs);
 * @endcode
 */
#define cvec_construct_mp(v, type, mprocs) \
  cvec_declare(v, type);                   \
  cvec_init_mp(v, mprocs)

#define cvec_construct_mp_scoped(v, type, mprocs) \
  cvec_declare_scoped(v, type);                   \
  cvec_init_mp(v, mprocs)

/**
 * @brief Destroy a vector and set pointer to NULL (type-inferred wrapper)
 *
 * This macro is a type-inferred wrapper for cvector_destroy().
 *
 * @param v Vector to destroy. The macro sets it to NULL after it destroys
 * the vector.
 *
 * @note A call with a NULL pointer is safe and does nothing
 *
 * @see cvector_destroy
 */
#define cvec_destroy(v) cvector_destroy(v)

/**
 * @brief The declared element type of v, without its top-level qualifiers.
 *
 * This is an internal helper of cvec_push, cvec_pop and cvec_find; do not
 * use it directly.
 *
 * Each of those macros writes to a temporary of the element type: cvec_pop
 * gives the address of the temporary to cvector_pop_back(), and the other
 * macros copy a value into the temporary and then clear its padding with
 * _ccol_clear_padding(). The write needs a temporary that is not const, even
 * for an element type such as `const char *const`. The comma operator
 * applies an lvalue conversion, which removes the qualifiers, but that
 * conversion also changes an array element type into a pointer of a
 * different size, so an array element type keeps its own type.
 * __builtin_types_compatible_p() ignores top-level qualifiers and tells the
 * two cases apart, because an array is never compatible with the pointer
 * that it decays to. __typeof__() and __builtin_choose_expr() evaluate
 * nothing, so this macro never evaluates v.
 *
 * @param v Name of the vector variable
 */
#define _cvec_unqual_elem_type(v)                        \
  __typeof__(__builtin_choose_expr(                      \
      __builtin_types_compatible_p(                      \
          __typeof__(*v##__ccol_val_type_var),           \
          __typeof__((void)0, *v##__ccol_val_type_var)), \
      ((void)0, *v##__ccol_val_type_var), *v##__ccol_val_type_var))

/* True when the declared element type of v is an array type: lvalue
 * conversion through the comma operator turns an array into a pointer and
 * leaves every other type compatible with itself. */
#define _cvec_elem_is_array(v)             \
  (!__builtin_types_compatible_p(          \
      __typeof__(*v##__ccol_val_type_var), \
      __typeof__((void)0, *v##__ccol_val_type_var)))

/* This macro declares tmp, a modifiable temporary of the element type of v,
 * and stores elem into it, evaluating elem exactly once. cvec_push and
 * cvec_find use it.
 *
 * For an element type that is not an array, the macro stores elem by
 * assignment, which converts elem to the element type exactly as
 * `T tmp = elem;` does. Because an array cannot be assigned, for an array
 * element type the macro does these steps instead:
 * - It stores elem (an array or a string literal) into arg, a pointer of
 *   the decayed type of elem.
 * - It fills tmp with zeros.
 * - It copies the bytes of that array into tmp, up to the smaller of the
 *   two sizes.
 *
 * __builtin_choose_expr selects the target of the assignment and the copy
 * at compile time, but the compiler also checks the arm that it does not
 * select, so each arm must be well formed for every element type, and it is:
 * arg is a plain const void * when the element type is not an array. For an
 * array element type, a pointer as elem is a compile error, because the
 * macro cannot see the size of the object that it points to. */
#define _cvec_elem_load(v, elem, tmp, arg)                                     \
  _Static_assert(                                                              \
      !_cvec_elem_is_array(v) ||                                               \
          !__builtin_types_compatible_p(__typeof__(elem),                      \
                                        __typeof__((void)0, elem)),            \
      "a vector whose element type is an array takes an array or a string "    \
      "literal as the element value");                                         \
  _cvec_unqual_elem_type(v) tmp;                                               \
  __typeof__(__builtin_choose_expr(_cvec_elem_is_array(v), ((void)0, elem),    \
                                   (const void *)0)) arg;                      \
  *__builtin_choose_expr(_cvec_elem_is_array(v), &arg, &tmp) = (elem);         \
  __builtin_choose_expr(                                                       \
      _cvec_elem_is_array(v),                                                  \
      (void)(memset(&tmp, 0, sizeof(tmp)),                                     \
             memcpy(&tmp, (const void *)arg,                                   \
                    sizeof(elem) < sizeof(tmp) ? sizeof(elem) : sizeof(tmp))), \
      (void)0)

/**
 * @brief Push an element onto the vector (type-inferred)
 *
 * This macro is a type-inferred wrapper for cvector_push_back(). It converts
 * new_elem to the declared element type of v with an ordinary C assignment,
 * exactly as `T tmp = new_elem;` does, and then copies that converted value
 * into the vector. The macro calls ccol_fatal_err() when cvector_push_back()
 * fails.
 *
 * @param v Vector to push to
 * @param new_elem The element: a variable, a literal or any expression,
 * including a struct value that a function returns.
 *
 * @note The macro stops the program on failure
 * @note For an element type that is an array, such as char[64], new_elem
 * is an array or a string literal: the macro fills the element with zeros
 * and copies the bytes of new_elem into it, up to the smaller of the two
 * sizes. For such a type, a pointer as new_elem is a compile error, because
 * the macro cannot see the size of the object that it points to.
 * @note The macro never takes the address of new_elem; it first copies the
 * value into a temporary of the element type, so a literal, a computed
 * expression and a struct value that a function returns are as correct as
 * a variable.
 * @note new_elem can safely alias into the backing buffer of v, for example
 * cvec_at(v, i). The macro reads the value into a private temporary that
 * does not alias before it touches v, so a capacity growth that this call
 * starts can never make the value incorrect.
 * @note With GCC 11 or later, the macro sets every padding byte of its
 * temporary to zero before it pushes it, so the vector stores a struct
 * element with zero padding, which cvec_find relies on. With a compiler that
 * has no __builtin_clear_padding, such as Clang, the padding bytes of the
 * stored element are unspecified.
 * @note The macro converts the value of new_elem to the declared element
 * type of v in the same way as a plain C assignment. For example, an int
 * literal that goes into a cvec_construct(v, long) becomes a long value, and
 * a float that goes into a cvec_construct(v, int) truncates to an int, as in
 * `int x = some_float;`. The macro never copies the raw bit pattern of
 * new_elem into an element of a different type, even when the two types
 * have the same size. A new_elem whose type has no implicit conversion to
 * the declared element type of v, such as an unrelated struct type with the
 * same layout, is a compile error at this point; the macro never silently
 * reinterprets such a value.
 *
 * @see cvector_push_back
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * int x = 42;
 * cvec_push(vec, x);
 * cvec_push(vec, 7);
 * cvec_push(vec, x + 1);
 * @endcode
 */
/* The local below that holds the result is named __cvec_push_r on purpose.
 * The plain name 'r' is the usual name for a retval local in this project,
 * but it is not safe here: through macro substitution, the declaration of
 * 'r' is the SAME statement whose initializer contains the new_elem
 * expression of the caller. In C, the scope of an identifier starts directly
 * after its own declarator, so it starts before the compiler evaluates the
 * initializer; the same rule makes `int x = x;` a reference to itself
 * instead of a copy of an outer x. If the local were named 'r', a caller
 * that pushes its own variable named 'r' would get the address of this local
 * of the macro, which has no value yet, instead of the address of its own
 * variable. The result would be silent data corruption, with no compiler
 * warning at any optimization level. A non-hygienic C macro has only one
 * practical defence: a name that cannot collide with a real identifier of a
 * caller.
 *
 * The same reasoning applies to __cvec_push_val below it. That local has
 * its own statement, and the expansion of new_elem is not inside that
 * statement, so it has no declarator-scope hazard of its own; but its name
 * also has the double-underscore prefix that C reserves for the
 * implementation, so no conforming identifier of a caller can collide with
 * it.
 *
 * __cvec_push_val is more than a hygiene device: it makes the stored value
 * correct. Consider a macro that gives &(new_elem) directly to
 * cvector_push_back(), with only a sizeof() _Static_assert as a guard:
 * - The assert finds a new_elem of a different SIZE and prevents an
 *   out-of-bounds read.
 * - The assert finds nothing for a new_elem of a different TYPE with the
 *   SAME size, such as a `float` that goes into a
 *   cvec_construct(v, int).
 * - The assert passes, because sizeof(float) == sizeof(int) on every
 *   mainstream platform.
 * - cvector_push_back() then copies the raw 4-byte IEEE-754 bit pattern of
 *   the float into the storage of the vector.
 * - A read of that element as an int gives the reinterpreted bits of the
 *   float, for example 1077936128 for 3.0f, instead of the value 3 that an
 *   ordinary C assignment gives.
 *
 * The result is silent data corruption, with no compiler diagnostic at any
 * optimization level.
 *
 * The type of __cvec_push_val is the declared element type of v, and
 * new_elem is its initializer, so the conversion goes through the
 * assignment rules of the C compiler instead of a raw byte copy. The result
 * is correct for every new_elem type with an implicit conversion and a
 * compile error for every type that is really incompatible. A sizeof()-only
 * _Static_assert is therefore not necessary: a new_elem of a different size
 * cannot cause an out-of-bounds read, because __cvec_push_val always has
 * exactly the element size of v. Such an assert is also incorrect, because
 * it refuses a legal and safe conversion, such as an int literal that goes
 * into a cvec_construct(v, long). So this macro has no such assert.
 *
 * new_elem initializes __cvec_push_val with `=`, never with a brace
 * initializer, so the macro accepts a struct rvalue (the result of a
 * function that returns the element type). A brace initializer would use
 * that value as the initializer of the first MEMBER of the struct, and the
 * code would not compile. */
#define cvec_push(v, new_elem)                                        \
  _ccol_cvec_push_impl(v, (new_elem),                                 \
                       _ccol_uniq(__ccol_cvec_push_val, __COUNTER__), \
                       _ccol_uniq(__ccol_cvec_push_arg, __COUNTER__), \
                       _ccol_uniq(__ccol_cvec_push_r, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_cvec_push_impl(v, new_elem, __cvec_push_val, __cvec_push_arg, \
                             __cvec_push_r)                                 \
  do {                                                                      \
    _cvec_elem_load(v, new_elem, __cvec_push_val, __cvec_push_arg);         \
    _ccol_clear_padding(&__cvec_push_val);                                  \
    ccol_retval_t __cvec_push_r =                                           \
        cvector_push_back((v), (const void *)&__cvec_push_val);             \
    if (__cvec_push_r != ccol_success) {                                    \
      ccol_fatal_err("cvec_push('%s'): r: %d (%s)", #v, __cvec_push_r,      \
                     ccol_retval_to_str(__cvec_push_r));                    \
    }                                                                       \
  } while (0)

/**
 * @brief Pop an element from the vector (type-inferred)
 *
 * This macro is a type-inferred wrapper for cvector_pop_back() that gives
 * the popped element as a value. The macro calls ccol_fatal_err() on
 * failure.
 *
 * @param v Vector to pop from
 *
 * @return The value of the popped element
 *
 * @note The macro stops the program on failure
 * @note The macro gives a value, not a pointer
 * @note The macro uses the GNU C statement expression extension
 *
 * @see cvector_pop_back
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push(vec, 42);
 * int val = cvec_pop(vec);  // val == 42
 * @endcode
 */
/* Both locals below use the __cvec_pop_ prefix, because the more obvious
 * names '_tmp' and 'r' are not safe. The expansion of v appears inside the
 * later use of _tmp as the target of an address-of operator, which matters,
 * for example, when a caller names a vector variable '_tmp'; the declaration
 * and the initializer of r have the same hazard, which the comment above
 * cvec_push describes in full. A name that belongs to this one macro alone
 * is very unlikely to collide with a real identifier of a caller. */
#define cvec_pop(v)                                                    \
  _ccol_cvec_pop_impl(v, _ccol_uniq(__ccol_cvec_pop_tmp, __COUNTER__), \
                      _ccol_uniq(__ccol_cvec_pop_r, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_cvec_pop_impl(v, __cvec_pop_tmp, __cvec_pop_r)                \
  ({                                                                        \
    _cvec_unqual_elem_type(v) __cvec_pop_tmp;                               \
    ccol_retval_t __cvec_pop_r = cvector_pop_back((v), &__cvec_pop_tmp);    \
    if (__cvec_pop_r != ccol_success) {                                     \
      ccol_fatal_err(                                                       \
          "cvec_pop('%s'): r: %d (%s)%s", #v, __cvec_pop_r,                 \
          ccol_retval_to_str(__cvec_pop_r),                                 \
          __cvec_pop_r == ccol_container_empty ? "; vector is empty" : ""); \
    }                                                                       \
    __cvec_pop_tmp;                                                         \
  })

/**
 * @brief Stops the program with ccol_fatal_err() when ptr is NULL. cvec_at
 * uses this helper.
 *
 * This is an internal helper; do not call it directly. It is a plain
 * function so that the logic that formats the diagnostic lives in one
 * place. cvec_at puts this helper inside a statement expression, which
 * evaluates index exactly once. That statement expression composes
 * correctly both as an rvalue and as an assignment target, and it does not
 * trip -Wunused-value when the code discards the result, because the outer
 * expression is a plain pointer dereference.
 */
static inline __attribute__((always_inline)) void *_cvec_at_checked(
    void *ptr, cvec v, const char *vec_name, size_t index) {
  if (__builtin_expect(!ptr, 0)) {
    /* The element count is read here, on the failure path alone, so a lookup
     * that succeeds costs one call to cvector_at() and nothing more. */
    ccol_fatal_err("cvec_at('%s'): index %lu out of bounds (size: %lu)",
                   vec_name, (unsigned long)index,
                   (unsigned long)cvector_elem_count(v));
  }
  return ptr;
}

/**
 * @brief Access element at index (type-inferred, returns reference)
 *
 * This macro is a type-inferred wrapper for cvector_at() that gives the
 * element reference, not a pointer, cast to the correct type automatically.
 * It stops the program with ccol_fatal_err() when the index is out of
 * bounds, which is the same "aborting convenience API" contract that every
 * other type-inferred macro has. Use cvec_at_ptr when you want an
 * alternative that gives NULL for an index that is out of bounds instead of
 * stopping the program.
 *
 * @param v Vector to access
 * @param index Zero-based index of the element. The macro evaluates it
 * exactly once, so an expression with a side effect, such as
 * cvec_at(vec, i++), is safe to pass.
 *
 * @return Element reference at the index
 *
 * @note The macro gives a reference, not a pointer
 * @note The macro stops the program with ccol_fatal_err() when the index is
 * out of bounds
 *
 * @see cvector_at
 * @see cvec_at_ptr
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push(vec, 42);
 * int val = cvec_at(vec, 0);  // val == 42
 * cvec_at(vec, 0) = 5;        // the first element is now 5
 * @endcode
 */
/* The macro puts index into the local __cvec_at_index exactly once, before
 * it gives the index to cvector_at() and to the re-read that
 * _cvec_at_checked() makes for the diagnostic alone. A direct (index) in
 * both places evaluates the expression twice, as two arguments of one
 * _cvec_at_checked() call with no sequence point between them, which is
 * undefined behavior for an index expression with a side effect: a bare
 * `i++` gets an indeterminate final value. In practice both increments
 * apply, so each loop iteration advances the index variable by 2 instead of
 * by 1; for example, `cvec_at(vec, j++)` in an ordinary loop advances j by 2
 * for each call. The loop then walks off the end of the vector and reaches
 * the out-of-bounds ccol_fatal_err() below, although the call site looks
 * correct. The -Wsequence-point warning of GCC flags that expansion.
 * __cvec_at_index uses the same double-underscore naming, belonging to one
 * macro alone, that this header uses elsewhere (for example for
 * __cvec_push_val), so no real identifier of a caller can collide with it.
 * The statement expression composes as an lvalue all the same, because
 * `*ptr_expr` is an lvalue even when a statement expression computes
 * ptr_expr; this is why `cvec_at(v, i) = x;` is a valid assignment
 * target. */
#define cvec_at(v, index) \
  _ccol_cvec_at_impl(v, (index), _ccol_uniq(__ccol_cvec_at_index, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_cvec_at_impl(v, index, __cvec_at_index)           \
  (*(__typeof__(*v##__ccol_val_type_var) *)({                   \
    size_t __cvec_at_index = (index);                           \
    _cvec_at_checked(cvector_at((v), __cvec_at_index), (v), #v, \
                     __cvec_at_index);                          \
  }))

/**
 * @brief Access element at index (type-inferred, returns pointer or NULL)
 *
 * This macro is a type-inferred wrapper for cvector_at() that gives a
 * pointer to the element, or NULL when the index is out of bounds. Unlike
 * cvec_at, it does not stop the program for such an index.
 *
 * @param v Vector to access
 * @param index Zero-based index of the element
 *
 * @return Pointer to the element at the index, or NULL when the index is
 * out of bounds
 *
 * @note The macro gives NULL when the index is out of bounds instead of
 * stopping the program.
 * @note The pointer stays valid only until the size of the vector changes
 *
 * @see cvector_at
 * @see cvec_at
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push(vec, 42);
 * int *ptr = cvec_at_ptr(vec, 0);
 * if (ptr) {
 *   *ptr = 5; // Change the element in place
 * }
 * @endcode
 */
#define cvec_at_ptr(v, index) \
  ((__typeof__(*v##__ccol_val_type_var) *)(cvector_at((v), (index))))

/**
 * @brief Get the number of elements (type-inferred wrapper)
 *
 * This macro is a type-inferred wrapper for cvector_elem_count().
 *
 * @param v Vector to query
 *
 * @return Number of elements in the vector
 *
 * @see cvector_elem_count
 */
#define cvec_size(v) cvector_elem_count((v))

/**
 * @brief Clear all elements and reset capacity (type-inferred wrapper)
 *
 * This macro is a type-inferred wrapper for cvector_reset().
 *
 * @param v Vector to reset
 *
 * @see cvector_reset
 */
#define cvec_reset(v) cvector_reset((v))

/**
 * @brief Reserve capacity (a type-inferred wrapper with error handling)
 *
 * This macro is a type-inferred wrapper for cvector_reserve() that calls
 * ccol_fatal_err() on failure.
 *
 * @param v Vector to reserve capacity for
 * @param new_capacity_count Minimum number of elements to reserve. The
 * macro evaluates it exactly once, on the failure path too, so an
 * expression with a side effect is safe to pass.
 *
 * @note The macro stops the program when the reservation fails
 *
 * @see cvector_reserve
 */
/* The macro puts new_capacity_count into __cvec_reserve_count exactly once,
 * before both of its uses: the real cvector_reserve() call and the
 * diagnostic message. The comment next to the internal locals of cvec_push
 * above gives the reason in full: a macro parameter that textual
 * substitution embeds in more than one place in the expansion is evaluated
 * once for each place where it appears. Without the local, a
 * new_capacity_count with a side effect (such as a function call that reads
 * from a queue, or a value that is not deterministic) is evaluated a second
 * time, only to build the ccol_fatal_err() message on the failure path. The
 * message then reports a different value from the one that reached
 * cvector_reserve(), and the second evaluation repeats any real side effect
 * directly before the process aborts. */
#define cvec_reserve(v, new_capacity_count)        \
  _ccol_cvec_reserve_impl(v, (new_capacity_count), \
                          _ccol_uniq(__ccol_cvec_reserve_count, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_cvec_reserve_impl(v, new_capacity_count, __cvec_reserve_count) \
  do {                                                                       \
    size_t __cvec_reserve_count = (new_capacity_count);                      \
    if (!cvector_reserve((v), __cvec_reserve_count)) {                       \
      ccol_fatal_err(                                                        \
          "cvec_reserve('%s'): failed to reserve %lu elements; out of "      \
          "memory?",                                                         \
          #v, (unsigned long)__cvec_reserve_count);                          \
    }                                                                        \
  } while (0)

/**
 * @brief Append an array of elements (a type-inferred wrapper with error
 * handling)
 *
 * This macro is a type-inferred wrapper for cvector_append_array() that
 * calls ccol_fatal_err() on failure.
 *
 * @param v Vector to append to
 * @param arr_ptr Pointer to the array of elements. It can point at a const
 * array.
 * @param elem_count Number of elements in the array. The macro evaluates it
 * exactly once, on the failure path too, so an expression with a side
 * effect is safe to pass.
 *
 * @note The macro stops the program when the append fails. One such failure
 * is an arr_ptr that aliases the backing buffer of v and whose elem_count
 * reads past the end of the reservation of that buffer; see
 * cvector_append_array.
 *
 * @see cvector_append_array
 */
/* The macro puts elem_count into __cvec_append_array_count exactly once,
 * before both uses, for the same reason that the comment next to the
 * matching local of cvec_reserve above gives: the same macro parameter in
 * both the real call and the ccol_fatal_err() diagnostic is evaluated twice
 * on the failure path. */
#define cvec_append_array(v, arr_ptr, elem_count) \
  _ccol_cvec_append_array_impl(                   \
      v, (arr_ptr), (elem_count),                 \
      _ccol_uniq(__ccol_cvec_append_array_count, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_cvec_append_array_impl(v, arr_ptr, elem_count,                \
                                     __cvec_append_array_count)             \
  do {                                                                      \
    size_t __cvec_append_array_count = (elem_count);                        \
    if (!cvector_append_array((v), (arr_ptr), __cvec_append_array_count)) { \
      ccol_fatal_err(                                                       \
          "cvec_append_array('%s'): failed to append %lu elements; out of " \
          "memory?",                                                        \
          #v, (unsigned long)__cvec_append_array_count);                    \
    }                                                                       \
  } while (0)

/**
 * @brief Append a vector to a vector (a type-inferred wrapper with error
 * handling)
 *
 * This macro is a type-inferred wrapper for cvector_append_cvector() that
 * calls ccol_fatal_err() on failure.
 *
 * @param v_to Destination vector
 * @param v_from Source vector
 *
 * @note The macro stops the program when the append fails
 *
 * @see cvector_append_cvector
 */
#define cvec_append_cvec(v_to, v_from)                                        \
  do {                                                                        \
    if (!cvector_append_cvector((v_to), (v_from))) {                          \
      ccol_fatal_err("cvec_append_cvec('%s' <- '%s'): out of memory?", #v_to, \
                     #v_from);                                                \
    }                                                                         \
  } while (0)

/**
 * @brief Get the data pointer (a type-inferred wrapper)
 *
 * This macro is a type-inferred wrapper for cvector_data_ptr().
 *
 * @param v Vector to get the data pointer from
 *
 * @return Pointer to the internal data array
 *
 * @see cvector_data_ptr
 */
#define cvec_data_ptr(v) cvector_data_ptr((v))

/**
 * @brief The sort behind cvec_sort and cvector_sort_with_comparison_proc.
 *
 * This is an internal helper. Do not call it directly; use one of the two
 * macros. It sorts the elements of v in place with a stable mergesort that
 * addresses each element straight from the backing array, and allocates its
 * temporary buffer with the allocator of v.
 *
 * @param v The vector. It must not be NULL.
 * @param comparison_proc The comparator. It must not be NULL.
 *
 * @return true when v is sorted, false when the temporary buffer cannot be
 *         allocated. v is unchanged whenever the function gives false.
 */
bool _cvector_sort(cvec v, ccol_comparison_proc_t comparison_proc);

/**
 * @brief Sort a vector with a custom comparison function
 *
 * This macro sorts the vector in place, using the csort library with a
 * custom comparison function. The comparison function must obey the
 * standard comparator convention and give a value less than 0, equal to 0,
 * or more than 0.
 *
 * @param v Vector to sort. The macro evaluates it exactly once.
 * @param comparison_proc Comparison function for the sort. The macro
 * evaluates it exactly once.
 *
 * @note The macro sorts in place with a stable mergesort algorithm
 * @note O(n log n) time complexity, O(n) space complexity
 * @note The macro asserts when v is NULL
 * @note The macro uses csort for the sort
 * @note The macro calls ccol_fatal_err() when comparison_proc is NULL. A
 * comparison procedure comes here from an expression of the caller, whose
 * value is in general not knowable before the program runs, so this check
 * stays a run-time check. The element-type check of cvec_sort is a
 * compile-time _Static_assert instead, so a cvec_sort call never reaches
 * this diagnostic.
 * @note The macro calls ccol_fatal_err() when it cannot allocate the
 * internal temporary buffer of the sort, as every other type-inferred macro
 * in this header that changes a vector does (cvec_push, cvec_reserve,
 * cvec_append_array and the others). The vector then stays completely
 * unsorted and unchanged, so this macro never gives back a vector that is
 * unsorted, or sorted in part, as if nothing went wrong.
 *
 * @see cvec_sort
 *
 * Example:
 * @code
 * int compare_ints(const void *a, const void *b) {
 *   int x = *(const int *)a, y = *(const int *)b;
 *   return (x > y) - (x < y);
 * }
 * cvec_construct(vec, int);
 * // ... add the elements ...
 * cvector_sort_with_comparison_proc(vec, compare_ints);
 * @endcode
 */
#define cvector_sort_with_comparison_proc(v, comparison_proc)               \
  do {                                                                      \
    /* v is evaluated exactly once, into this local, before comparison_proc \
     * is, and the NULL check and the sort then read the local. The type is \
     * spelled with __typeof__, because a caller can name its own variable  \
     * cvec and so hide the typedef. */                                     \
    __typeof__(v) __cvec_sort_v = (v);                                      \
    if (!__cvec_sort_v) {                                                   \
      ccol_fatal_err(                                                       \
          "cvector_sort_with_comparison_proc('%s'): vector is NULL", #v);   \
    }                                                                       \
    /* The macro evaluates comparison_proc into a plain pointer variable    \
     * instead of testing !(comparison_proc) directly, because a caller can \
     * pass a comparator function by its bare name instead of a variable    \
     * that holds one (cvec_sort uses this macro with such a variable).     \
     * The pointer variable prevents -Werror=address for a bare name        \
     * ("the address of 'X' will always evaluate as 'true'"): GCC and       \
     * Clang can prove that the address of a named function is never        \
     * NULL, but they cannot prove it for a pointer variable, even for one  \
     * that the code initializes from that same function. The pointer       \
     * variable also means that the expression of comparison_proc is        \
     * evaluated exactly once, which obeys the convention of this header:   \
     * a macro evaluates each of its arguments once. */                     \
    ccol_comparison_proc_t __cvec_sort_cmp = (comparison_proc);             \
    if (!__cvec_sort_cmp) {                                                 \
      ccol_fatal_err(                                                       \
          "cvector_sort_with_comparison_proc('%s'): comparison_proc is "    \
          "NULL; pass a real comparison procedure",                         \
          #v);                                                              \
    }                                                                       \
    if (!_cvector_sort(__cvec_sort_v, __cvec_sort_cmp)) {                   \
      ccol_fatal_err(                                                       \
          "cvector_sort_with_comparison_proc('%s'): out of memory; vector " \
          "left unsorted",                                                  \
          #v);                                                              \
    }                                                                       \
  } while (0)

/**
 * @brief Sort a vector with the default comparison for its type
 *
 * This macro sorts the vector in place with the default comparison function
 * for the element type, which it gets from the csort library; the type
 * decides which comparator it gets.
 *
 * An element type with no default comparison procedure is a compile-time
 * error, not a run-time error: the macro carries a _Static_assert that
 * names the vector and points at cvector_sort_with_comparison_proc, so such
 * a call never builds and can never abort a program that runs. Every
 * standard integer type and floating type sorts. Every enumeration sorts
 * too, because an enumerated type is compatible with one of the standard
 * integer types, and the library chooses the comparator for that compatible
 * type. A char *, a const char *, a signed char * and an unsigned char *
 * all sort, as NUL-terminated strings in strcmp() order; uint8_t * is an
 * unsigned char * and sorts the same way. Sort a vector of pointers to
 * binary buffers with cvector_sort_with_comparison_proc. A bool, a struct,
 * a union, a pointer to anything other than char, and a fixed-size char
 * array have no default comparison procedure, so the macro rejects these
 * types; sort such a vector with cvector_sort_with_comparison_proc and an
 * explicit comparison procedure.
 *
 * @param v Vector to sort
 *
 * @note The macro sorts in place with a stable mergesort algorithm
 * @note O(n log n) time complexity, O(n) space complexity
 * @note The macro uses the default comparison for the element type
 * @note In a char * vector, a NULL element comes before every string that
 * is not NULL and is equal only to another NULL, so a vector that holds
 * NULL elements sorts without any special handling by the caller.
 * @note The code does not compile when the element type has no default
 * comparison procedure; the diagnostic names the vector and the alternative
 * that the library supports.
 * @note The macro calls ccol_fatal_err() when an allocation fails, as every
 * other type-inferred macro in this header that changes a vector does. See
 * cvector_sort_with_comparison_proc.
 *
 * @see cvector_sort_with_comparison_proc
 * @see cvec_find. Its behaviour for a type that has no default comparison
 * procedure is deliberately different: cvec_find falls back to byte-wise
 * equality instead of refusing to compile.
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push(vec, 3);
 * cvec_push(vec, 1);
 * cvec_push(vec, 2);
 * cvec_sort(vec);  // vec is now [1, 2, 3]
 * @endcode
 */
/* The _Static_assert turns "this element type has no default comparator"
 * into a diagnostic that the compiler issues at the call site; without the
 * assert, this condition is a run-time abort. The type alone decides
 * whether a type has a default comparator, so the answer is knowable at
 * compile time. The association lists of
 * ___csort_has_default_comparison_proc() are an exact copy of the lists of
 * csort_get_default_comparison_proc(), so when the assertion holds, the
 * selector below gives a real comparison procedure, and the NULL check
 * inside cvector_sort_with_comparison_proc can never fire for a cvec_sort()
 * caller. The predicate gets the same *(v##__ccol_val_type_var) lvalue that
 * the selector gets, so the two answer the question about one identical
 * type. */
#define cvec_sort(v)                                                         \
  do {                                                                       \
    _Static_assert(                                                          \
        ___csort_has_default_comparison_proc(*(v##__ccol_val_type_var)),     \
        "cvec_sort(" #v                                                      \
        "): this vector's element type has no default comparison procedure " \
        "(a bool, a struct, a union, a pointer to anything other than char " \
        "and a fixed-size char array all have none). Sort this vector with " \
        "cvector_sort_with_comparison_proc(" #v                              \
        ", your_comparison_proc) and an explicit comparison procedure "      \
        "instead.");                                                         \
    ccol_comparison_proc_t __ccol_cvec_sort_cmp =                            \
        csort_get_default_comparison_proc(*(v##__ccol_val_type_var));        \
    cvector_sort_with_comparison_proc(v, __ccol_cvec_sort_cmp);              \
  } while (0)

/**
 * @brief Find the first element with a given value (type-inferred)
 *
 * This macro is a type-inferred wrapper for cvector_find() that accepts a
 * value, including an rvalue or a literal. The macro converts the needle to
 * the declared element type of the vector and compares the elements with
 * the default comparison procedure of that type, so equality is the
 * equality of the type, not the equality of its object representation. For
 * example, -0.0 and 0.0 are one value; a long double compares by value, not
 * through the padding bytes that its representation has on some targets;
 * and a char * element matches on the content of the string, not on the
 * identity of the pointer.
 *
 * This library reads a char *, a signed char * or an unsigned char *
 * element as a NUL-terminated string (uint8_t * is an unsigned char *),
 * which the comparison reads with strcmp. Do not search a vector of
 * pointers to binary buffers with this macro, because the comparison reads
 * past the end of a buffer that has no NUL byte. For such a vector, do one
 * of these:
 * - Use cvector_find() with a comparator that compares the pointers.
 * - Declare the element type as void *, whose byte-wise equality is the
 *   identity of the pointer.
 *
 * For an element type that is an array, such as char[64], elem is an array
 * or a string literal: the macro fills its copy with zeros to the element
 * size and then copies the bytes of elem into the copy, up to the smaller
 * of the two sizes, in the same way as cvec_push stores such a value. For
 * an array element type, a pointer as elem is a compile error, because the
 * macro cannot see the size of the object that it points to.
 *
 * A char * element can be NULL, and so can the needle. The default string
 * comparison procedure puts NULL before every string that is not NULL and
 * makes NULL equal only to another NULL, so a search for NULL in a sparse
 * char * vector gives the first empty slot. The macro dereferences nothing.
 *
 * These element types have no default comparison procedure: a bool, a
 * struct, a union, a pointer to a type other than char, and a fixed-size
 * char array. For such a type, the comparison uses byte-wise equality over
 * the full representation of the element, including the padding bytes of a
 * struct. With GCC 11 or later, the macro sets the padding bytes of its own
 * copy of elem to zero, and cvec_push does the same for each element that
 * it stores, so the macro finds a struct that went in through cvec_push
 * with any value whose members are equal, whatever the value of the padding
 * bytes.
 *
 * An element that went in through cvector_push_back(),
 * cvector_append_array() or an assignment through cvec_at keeps the padding
 * bytes that the caller wrote. With a compiler that has no
 * __builtin_clear_padding, such as Clang, the padding bytes of the macro
 * copies are unspecified; in that case, search for a padded struct with
 * cvector_find() and a comparator that reads the members.
 *
 * cvec_find compiles for every element type, while cvec_sort differs on
 * purpose: an order cannot come from the bytes of an element in the way
 * that equality can, so cvec_sort has no fallback and refuses such an
 * element type at compile time.
 *
 * Use cvector_find() directly when you need a custom comparator, or when
 * you want byte-wise equality for a type that has a default comparison
 * procedure.
 *
 * @param v    Vector to search
 * @param elem Element value to search for
 *
 * @return Zero-based index of the first match, or ccol_invalid_size when
 * the macro finds no match.
 *
 * @note O(n) complexity
 * @note The macro compares with the default comparison procedure of the
 * element type
 * @note The macro orders a NULL char * element, and a NULL needle for a
 * char * vector, without dereferencing either of them.
 * @note The macro falls back to byte-wise equality for a type that has no
 * default comparison procedure. A struct with padding bytes compares
 * correctly only when both copies carry zero padding, as described above.
 * @note The macro compiles for every element type; cvec_sort does not.
 *
 * @see cvector_find
 * @see cvec_sort
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push(vec, 10);
 * cvec_push(vec, 20);
 * cvec_push(vec, 30);
 * size_t idx = cvec_find(vec, 20);  // idx == 1
 * size_t nf  = cvec_find(vec, 99);  // nf == ccol_invalid_size
 * @endcode
 */
#define cvec_find(v, elem)                                               \
  _ccol_cvec_find_impl(v, (elem),                                        \
                       _ccol_uniq(__ccol_cvec_find_needle, __COUNTER__), \
                       _ccol_uniq(__ccol_cvec_find_arg, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_cvec_find_impl(v, elem, __cvec_find_needle, __cvec_find_arg) \
  ({                                                                       \
    _cvec_elem_load(v, elem, __cvec_find_needle, __cvec_find_arg);         \
    _ccol_clear_padding(&__cvec_find_needle);                              \
    cvector_find(                                                          \
        (v), (const void *)&__cvec_find_needle,                            \
        csort_get_default_comparison_proc(*(v##__ccol_val_type_var)));     \
  })

#pragma GCC visibility pop
