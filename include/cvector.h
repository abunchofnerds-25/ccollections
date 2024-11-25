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
#include <csort.h>

/**
 * @file cvector.h
 * @brief Dynamic array (vector) container with automatic resizing
 *
 * Provides a generic dynamic array container that automatically grows and
 * shrinks as elements are added or removed. The vector stores elements of a
 * fixed size specified at creation time.
 *
 * Key features:
 * - Automatic capacity management (grows by 2x, shrinks by 0.5x)
 * - Minimum capacity of 4 elements
 * - Type-safe macros for common operations
 * - Custom memory management support
 * - O(1) amortized push/pop operations
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
 * Creates a new vector container that stores elements of the specified size.
 * The vector starts with a minimum capacity of 4 elements and automatically
 * resizes as needed.
 *
 * @param elem_size Size of each element in bytes (must be > 0)
 * @param mmgmt_procs Custom memory management procedures, or NULL to use
 * default malloc/free
 * @param err Optional pointer to receive error string on failure (pass NULL to
 * ignore)
 *
 * @return Pointer to newly created vector, or NULL on failure
 *
 * @note Initial capacity is 4 elements
 * @note Capacity doubles when full + 1, halves when < 1/4 filled
 * @note The vector must be destroyed with cvector_destroy() when done
 *
 * @see cvector_create
 * @see cvector_destroy
 */
cvec cvector_create_full(size_t elem_size, ccol_memmgmt_procs_t *mmgmt_procs,
                         char **err);

/**
 * @brief Create a vector with default memory management
 *
 * Convenience wrapper function that creates a vector using standard
 * malloc/free.
 *
 * @param elem_size Size of each element in bytes
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created vector, or NULL on failure
 */
static inline __attribute__((always_inline)) cvec
cvector_create(size_t elem_size, char **err) {
  return cvector_create_full(elem_size, NULL, err);
}

/**
 * @brief Get the memory management procedures for a vector
 *
 * Returns the memory management procedures structure used by this vector.
 *
 * @param v Vector to query
 *
 * @return Pointer to memory management procedures, or NULL if using default
 */
ccol_memmgmt_procs_t *cvector_get_mprocs(cvec v);

/**
 * @brief Destroy a vector (internal function)
 *
 * @param v Vector to destroy
 *
 * @warning Do not call directly - use cvector_destroy() macro instead
 */
void __cvector_destroy(cvec v);

/**
 * @brief Destroy a vector and set pointer to NULL
 *
 * Frees all resources associated with the vector including the data buffer.
 * The vector pointer is automatically set to NULL after destruction.
 *
 * @param v Vector to destroy (will be set to NULL after destruction)
 *
 * @note Safe to call with NULL pointer (no-op)
 * @note Does not free individual elements when the vector itself was created to
 * contain pointers to buffers allocated from the heap - caller must free
 * element data first if needed
 */
#define cvector_destroy(v)  \
  do {                      \
    if (v) {                \
      __cvector_destroy(v); \
      v = NULL;             \
    }                       \
  } while (0)

/**
 * @brief Append an element to the end of the vector
 *
 * Adds a new element to the end of the vector. If the vector is at capacity,
 * it automatically grows by doubling its capacity. The element is copied into
 * the vector using an optimized assignment based on element size.
 *
 * @param v Vector to append to
 * @param new_elem Pointer to element to append (must not be NULL)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if new_elem is NULL
 * @return ccol_container_full if vector has reached max_elem_count
 * @return ccol_not_enough_memory if capacity expansion fails
 *
 * @note Amortized O(1) complexity
 * @note Element is copied into the vector
 * @note Capacity doubles when full (2x scaling factor)
 * @note Will assert if v is NULL
 *
 * @see cvector_pop_back
 * @see cvec_push
 */
ccol_retval_t cvector_push_back(cvec v, const void *new_elem);

/**
 * @brief Remove and return the last element from the vector
 *
 * Removes the last element from the vector and copies it to the target buffer.
 * If the vector becomes less than 1/4 full, it automatically shrinks by halving
 * its capacity (minimum capacity is 4).
 *
 * @param v Vector to pop from
 * @param target_elem Pointer to buffer to receive the element (must not be
 * NULL)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if target_elem is NULL
 * @return ccol_container_empty if vector is empty
 *
 * @note O(1) complexity
 * @note Element is copied to target_elem
 * @note Capacity halves when < 1/4 full (down to minimum of 4)
 * @note Will assert if v is NULL
 *
 * @see cvector_push_back
 * @see cvec_pop
 */
ccol_retval_t cvector_pop_back(cvec v, void *target_elem);

/**
 * @brief Access an element at a specific index
 *
 * Returns a pointer to the element at the specified index. The pointer can
 * be used to read or modify the element in-place.
 *
 * @param v Vector to access
 * @param index Zero-based index of element to access
 *
 * @return Pointer to element at index, or NULL if index is out of bounds
 *
 * @note O(1) complexity
 * @note Returns NULL if index >= elem_count or vector is empty
 * @note Returned pointer is only valid until vector is resized
 * @note Will assert if v is NULL
 *
 * @see cvec_at
 * @see cvector_elem_count
 */
void *cvector_at(cvec v, size_t index);

/**
 * @brief Get the number of elements in the vector
 *
 * Returns the current number of elements stored in the vector.
 *
 * @param v Vector to query
 *
 * @return Number of elements in the vector
 *
 * @note O(1) complexity
 * @note Will assert if v is NULL
 *
 * @see cvec_size
 */
size_t cvector_elem_count(cvec v);

/**
 * @brief Clear all elements and reset capacity
 *
 * Removes all elements from the vector and attempts to shrink the capacity
 * back to the minimum (4 elements). If reallocation fails, the capacity
 * remains unchanged but the element count is still reset to 0.
 *
 * @param v Vector to reset
 *
 * @note Element count becomes 0
 * @note Capacity reset to minimum (4) if reallocation succeeds
 * @note Does not free individual elements - caller must do this first if needed
 * @note Will assert if v is NULL
 *
 * @see cvec_reset
 */
void cvector_reset(cvec v);

/**
 * @brief Reserve capacity for future elements
 *
 * Pre-allocates capacity to hold at least new_capacity_count elements,
 * rounded up to the nearest power of two (minimum 4). Helps avoid multiple
 * reallocations when the final size is known in advance.
 *
 * @param v Vector to reserve capacity for
 * @param new_capacity_count Minimum number of elements to reserve space for
 *
 * @return true if reservation succeeded, false on failure
 *
 * @note Actual capacity will be rounded up to nearest power of two >= 4
 * @note Does not shrink capacity if new_capacity <= current capacity
 * @note Returns true immediately if already have enough capacity
 * @note Returns false if new_capacity > max_elem_count
 * @note Will assert if v is NULL
 *
 * @see cvec_reserve
 *
 * Example:
 * @code
 * cvec v = cvector_create(sizeof(int), NULL);
 * cvector_reserve(v, 100);  // Pre-allocate for 128 elements (next power of 2)
 * // Now can push 100+ elements without reallocation
 * @endcode
 */
bool cvector_reserve(cvec v, size_t new_capacity_count);

/**
 * @brief Append an array of elements to the vector
 *
 * Efficiently appends multiple elements from a C array to the end of the
 * vector. Automatically handles capacity expansion and may trigger a single
 * reallocation if needed.
 *
 * @param v Vector to append to
 * @param arr_ptr Pointer to array of elements to append
 * @param elem_count Number of elements in the array
 *
 * @return true on success, false on failure
 *
 * @note More efficient than multiple push_back calls
 * @note Uses optimized mem_cpy for bulk copying
 * @note May trigger capacity expansion via cvector_reserve
 * @note Returns false if result would exceed max_elem_count
 * @note Returns false if elem_count would cause overflow
 * @note Will assert if v is NULL
 *
 * @see cvector_append_cvector
 * @see cvec_append_array
 *
 * Example:
 * @code
 * int arr[] = {1, 2, 3, 4, 5};
 * cvec v = cvector_create(sizeof(int), NULL);
 * cvector_append_array(v, arr, 5);  // Add all 5 elements at once
 * @endcode
 */
bool cvector_append_array(cvec v, void *arr_ptr, size_t elem_count);

/**
 * @brief Append all elements from one vector to another
 *
 * Efficiently copies all elements from v_from to the end of v_to.
 * Both vectors must have the same element size.
 *
 * @param v_to Destination vector to append to
 * @param v_from Source vector to copy elements from
 *
 * @return true on success, false on failure
 *
 * @note Both vectors must have the same elem_size
 * @note v_from remains unchanged
 * @note May trigger capacity expansion in v_to
 * @note Returns false if result would exceed max_elem_count
 * @note Will assert if either vector is NULL or elem_size mismatch
 *
 * @see cvector_append_array
 * @see cvec_append_cvec
 *
 * Example:
 * @code
 * cvec v1 = cvector_create(sizeof(int), NULL);
 * cvec v2 = cvector_create(sizeof(int), NULL);
 * // ... populate v1 and v2 ...
 * cvector_append_cvector(v1, v2);  // v1 now contains all of v2's elements
 * @endcode
 */
bool cvector_append_cvector(cvec v_to, cvec v_from);

/**
 * @brief Get pointer to the underlying data array
 *
 * Returns a direct pointer to the vector's internal data buffer. Useful
 * for passing the vector to functions that expect C arrays.
 *
 * @param v Vector to get data pointer from
 *
 * @return Pointer to internal data array
 *
 * @warning Pointer becomes invalid after any operation that may resize
 * @warning Do not free this pointer - it's managed by the vector
 * @note Will assert if v is NULL
 *
 * @see cvec_data_ptr
 *
 * Example:
 * @code
 * cvec v = cvector_create(sizeof(int), NULL);
 * // ... add elements ...
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
 * Helper function used with _ccol_destructor attribute for automatic
 * cleanup when variables go out of scope. Destroys the vector and sets
 * the pointer to NULL.
 *
 * @param cv Pointer to vector pointer
 *
 * @note Designed for use with __attribute__((cleanup))
 * @note Safe to call with NULL or pointer to NULL
 * @note This is an internal helper - prefer using cvector_destroy() macro
 *
 * Example:
 * @code
 * {
 *   cvec v _ccol_destructor(___cvector_destroy) = cvector_create(sizeof(int),
 *                                                 NULL);
 *   // ... use vector ...
 * } // Automatically destroyed when leaving scope
 * @endcode
 */
static inline void ___cvector_destroy(cvec *cv) {
  if (*cv) {
    __cvector_destroy(*cv);
    *cv = NULL;
  }
}

/* ========================================================================== */
/*                         TYPE-SAFE CONVENIENCE MACROS                       */
/* ========================================================================== */

/**
 * @brief Declare an uninitialized vector variable
 *
 * Declares a vector variable 'v' and an associated type variable used for
 * type safety in macro operations. The vector must be initialized before use.
 *
 * @param v Name of the vector variable to declare
 * @param type Element type for the vector
 *
 * @note Vector must be initialized with cvec_init() or cvec_construct() before
 * use
 * @note Type variable is named v##__cvec_type_var and used internally by macros
 *
 * @see cvec_init
 * @see cvec_construct
 *
 * Example:
 * @code
 * cvec_declare(my_vec, int);
 * cvec_init(my_vec);
 * // ... use vector ...
 * cvec_destroy(my_vec);
 * @endcode
 */
#define cvec_declare(v, type) \
  type *v##__cvec_type_var;   \
  cvec v

#define cvec_declare_scoped(v, type) \
  type *v##__cvec_type_var;          \
  cvec v _ccol_destructor(___cvector_destroy)

/**
 * @brief Enable type-safe macros for a vector in local scope
 *
 * Declares the type variable needed for type-safe macro operations when
 * the vector was created without using cvec_declare or cvec_construct.
 *
 * @param v Vector variable name
 * @param type Type of elements in the vector
 *
 * @note Use this when you have a cvec from another scope but want type-safe
 * access
 *
 * Example:
 * @code
 * void process(cvec vec) {
 *   cvec_redeclare(vec, int);
 *   int val = cvec_at(vec, 0);
 * }
 * @endcode
 */
#define cvec_redeclare(v, type) \
  type *v##__cvec_type_var __attribute__((unused)) = NULL

/**
 * @brief Initialize a declared vector (with error handling)
 *
 * Initializes a vector that was previously declared with cvec_declare().
 * Terminates the program with fatal_err() if initialization fails.
 *
 * @param v Vector variable to initialize
 *
 * @note Calls fatal_err() on initialization failure
 * @note Vector must have been declared with cvec_declare()
 * @note Uses default memory management (malloc/free)
 *
 * @see cvec_declare
 * @see cvec_construct
 * @see cvec_init_mp
 *
 * Example:
 * @code
 * cvec_declare(my_vec, int);
 * cvec_init(my_vec);  // Terminates on failure
 * @endcode
 */
#define cvec_init(v)                                                     \
  do {                                                                   \
    char *err = NULL;                                                    \
    v = cvector_create(sizeof(*v##__cvec_type_var), &err);               \
    if (!(v)) {                                                          \
      fatal_err("cvec_init('%s'): %s", #v, err ? err : "unknown error"); \
    }                                                                    \
  } while (0)

/**
 * @brief Initialize a declared vector with custom memory management
 *
 * Initializes a vector with custom memory management procedures.
 * Terminates the program with fatal_err() if initialization fails.
 *
 * @param v Vector variable to initialize
 * @param mprocs Pointer to custom memory management procedures
 *
 * @note Calls fatal_err() on initialization failure
 * @note Vector must have been declared with cvec_declare()
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
#define cvec_init_mp(v, mprocs)                                             \
  do {                                                                      \
    char *err = NULL;                                                       \
    v = cvector_create_full(sizeof(*v##__cvec_type_var), (mprocs), &err);   \
    if (!(v)) {                                                             \
      fatal_err("cvec_init_mp('%s'): %s", #v, err ? err : "unknown error"); \
    }                                                                       \
  } while (0)

/**
 * @brief Declare and initialize a vector in one step
 *
 * Convenience macro that combines cvec_declare() and cvec_init().
 * Terminates the program with fatal_err() if initialization fails.
 *
 * @param v Name of the vector variable to create
 * @param type Element type for the vector
 *
 * @note Calls fatal_err() on initialization failure
 * @note Equivalent to: cvec_declare(v, type); cvec_init(v);
 *
 * @see cvec_declare
 * @see cvec_init
 * @see cvec_construct_mp
 *
 * Example:
 * @code
 * cvec_construct(my_vec, int);
 * cvec_push_rvalue(my_vec, 42);
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
 * Convenience macro that combines cvec_declare() and cvec_init_mp().
 * Terminates the program with fatal_err() if initialization fails.
 *
 * @param v Name of the vector variable to create
 * @param type Element type for the vector
 * @param mprocs Pointer to custom memory management procedures
 *
 * @note Calls fatal_err() on initialization failure
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
 * @brief Destroy a vector and set pointer to NULL (type-safe wrapper)
 *
 * Type-safe wrapper for cvector_destroy().
 *
 * @param v Vector to destroy (will be set to NULL after destruction)
 *
 * @note Safe to call with NULL pointer (no-op)
 *
 * @see cvector_destroy
 */
#define cvec_destroy(v) cvector_destroy(v)

/**
 * @brief Push an element onto the vector (type-safe)
 *
 * Type-safe wrapper for cvector_push_back() that automatically takes the
 * address of the element. Calls fatal_err() on failure.
 *
 * @param v Vector to push to
 * @param new_elem Element value to push (lvalue)
 *
 * @note Terminates program on failure
 * @note Element must be an addressable lvalue
 * @note For rvalues, use cvec_push_rvalue()
 *
 * @see cvec_push_rvalue
 * @see cvector_push_back
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * int x = 42;
 * cvec_push(vec, x);
 * @endcode
 */
#define cvec_push(v, new_elem)                                                \
  do {                                                                        \
    ccol_retval_t r = cvector_push_back((v), (const void *)&(new_elem));      \
    if (r != ccol_success) {                                                  \
      fatal_err("cvec_push('%s'): r: %d (%s)", #v, r, ccol_retval_to_str(r)); \
    }                                                                         \
  } while (0)

/**
 * @brief Push an rvalue element onto the vector (type-safe)
 *
 * Type-safe wrapper for cvector_push_back() that can handle rvalue expressions
 * that cannot be directly addressed. Creates a temporary compound literal.
 * Calls fatal_err() on failure.
 *
 * @param v Vector to push to
 * @param new_elem Element value to push (can be rvalue)
 *
 * @note Terminates program on failure
 * @note Can push literal values and expressions
 * @note Uses GNU C compound literal extension
 *
 * @see cvec_push
 * @see cvector_push_back
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push_rvalue(vec, 42);
 * cvec_push_rvalue(vec, x + y);
 * @endcode
 */
#define cvec_push_rvalue(v, new_elem)                                          \
  do {                                                                         \
    ccol_retval_t r = cvector_push_back((v), &(typeof(new_elem)){(new_elem)}); \
    if (r != ccol_success) {                                                   \
      fatal_err("cvec_push_rvalue('%s'): r: %d (%s)", #v, r,                   \
                ccol_retval_to_str(r));                                        \
    }                                                                          \
  } while (0)

/**
 * @brief Pop an element from the vector (type-safe)
 *
 * Type-safe wrapper for cvector_pop_back() that returns the popped element
 * as a value. Calls fatal_err() on failure.
 *
 * @param v Vector to pop from
 *
 * @return The popped element value
 *
 * @note Terminates program on failure
 * @note Returns value, not pointer
 * @note Uses GNU C statement expression extension
 *
 * @see cvector_pop_back
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push_rvalue(vec, 42);
 * int val = cvec_pop(vec);  // val == 42
 * @endcode
 */
#define cvec_pop(v)                                                           \
  ({                                                                          \
    typeof(*v##__cvec_type_var) _tmp;                                         \
    ccol_retval_t r = cvector_pop_back((v), &_tmp);                           \
    if (r != ccol_success) {                                                  \
      fatal_err("cvec_pop('%s'): r: %d (%s)%s", #v, r, ccol_retval_to_str(r), \
                r == ccol_container_empty ? " — vector is empty" : "");       \
    }                                                                         \
    _tmp;                                                                     \
  })

/**
 * @brief Access element at index (type-safe, returns reference)
 *
 * Type-safe wrapper for cvector_at() that returns the element reference (not
 * pointer). Automatically casts to the correct type.
 *
 * @param v Vector to access
 * @param index Zero-based index of element
 *
 * @return Element reference at index
 *
 * @note Returns reference, not pointer
 * @note No bounds checking - will cause a SIGSEGV, if index out of bounds
 *
 * @see cvector_at
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push_rvalue(vec, 42);
 * int val = cvec_at(vec, 0);  // val == 42
 * cvec_at(vec, 0) = 5;        // now the first element is 5
 * @endcode
 */
#define cvec_at(v, index) \
  *(typeof(*v##__cvec_type_var) *)(cvector_at((v), (index)))

/**
 * @brief Get the number of elements (type-safe wrapper)
 *
 * Type-safe wrapper for cvector_elem_count().
 *
 * @param v Vector to query
 *
 * @return Number of elements in the vector
 *
 * @see cvector_elem_count
 */
#define cvec_size(v) cvector_elem_count((v))

/**
 * @brief Clear all elements and reset capacity (type-safe wrapper)
 *
 * Type-safe wrapper for cvector_reset().
 *
 * @param v Vector to reset
 *
 * @see cvector_reset
 */
#define cvec_reset(v) cvector_reset((v))

/**
 * @brief Reserve capacity (type-safe wrapper with error handling)
 *
 * Type-safe wrapper for cvector_reserve() that calls fatal_err() on failure.
 *
 * @param v Vector to reserve capacity for
 * @param new_capacity_count Minimum number of elements to reserve
 *
 * @note Terminates program if reservation fails
 *
 * @see cvector_reserve
 */
#define cvec_reserve(v, new_capacity_count)                              \
  do {                                                                   \
    if (!cvector_reserve((v), (new_capacity_count))) {                   \
      fatal_err(                                                         \
          "cvec_reserve('%s'): failed to reserve %lu elements — out of " \
          "memory?",                                                     \
          #v, (size_t)(new_capacity_count));                             \
    }                                                                    \
  } while (0)

/**
 * @brief Append array of elements (type-safe wrapper with error handling)
 *
 * Type-safe wrapper for cvector_append_array() that calls fatal_err() on
 * failure.
 *
 * @param v Vector to append to
 * @param arr_ptr Pointer to array of elements
 * @param elem_count Number of elements in array
 *
 * @note Terminates program if append fails
 *
 * @see cvector_append_array
 */
#define cvec_append_array(v, arr_ptr, elem_count)                            \
  do {                                                                       \
    if (!cvector_append_array((v), (arr_ptr), (elem_count))) {               \
      fatal_err(                                                             \
          "cvec_append_array('%s'): failed to append %lu elements — out of " \
          "memory?",                                                         \
          #v, (size_t)(elem_count));                                         \
    }                                                                        \
  } while (0)

/**
 * @brief Append vector to vector (type-safe wrapper with error handling)
 *
 * Type-safe wrapper for cvector_append_cvector() that calls fatal_err() on
 * failure.
 *
 * @param v_to Destination vector
 * @param v_from Source vector
 *
 * @note Terminates program if append fails
 *
 * @see cvector_append_cvector
 */
#define cvec_append_cvec(v_to, v_from)                                   \
  do {                                                                   \
    if (!cvector_append_cvector((v_to), (v_from))) {                     \
      fatal_err("cvec_append_cvec('%s' <- '%s'): out of memory?", #v_to, \
                #v_from);                                                \
    }                                                                    \
  } while (0)

/**
 * @brief Get data pointer (type-safe wrapper)
 *
 * Type-safe wrapper for cvector_data_ptr().
 *
 * @param v Vector to get data pointer from
 *
 * @return Pointer to internal data array
 *
 * @see cvector_data_ptr
 */
#define cvec_data_ptr(v) cvector_data_ptr((v))

/**
 * @brief Sort vector using custom comparison function
 *
 * Sorts the vector in-place using the csort library with a custom comparison
 * function. The comparison function should follow the standard comparator
 * convention (return <0, 0, or >0).
 *
 * @param v Vector to sort
 * @param comparison_proc Comparison function for sorting
 *
 * @note Sorts in-place using stable mergesort algorithm
 * @note O(n log n) time complexity, O(n) space complexity
 * @note Will assert if v is NULL
 * @note Uses csort for sorting
 *
 * @see cvec_sort
 *
 * Example:
 * @code
 * int compare_ints(const void *a, const void *b) {
 *   return *(int*)a - *(int*)b;
 * }
 * cvec_construct(vec, int);
 * // ... add elements ...
 * cvector_sort_with_comparison_proc(vec, compare_ints);
 * @endcode
 */
#define cvector_sort_with_comparison_proc(v, comparison_proc)               \
  do {                                                                      \
    if (!(v)) {                                                             \
      fatal_err("cvector_sort_with_comparison_proc('%s'): vector is NULL",  \
                #v);                                                        \
    }                                                                       \
    csort_sort((v), cvector_elem_count((v)), sizeof(*(v##__cvec_type_var)), \
               (csort_item_getter_proc_t)cvector_at, (comparison_proc),     \
               cvector_get_mprocs((v)));                                    \
  } while (0)

/**
 * @brief Sort vector using default comparison for type
 *
 * Sorts the vector in-place using the default comparison function for the
 * element type. The default comparator is obtained from the csort library
 * based on the type.
 *
 * @param v Vector to sort
 *
 * @note Sorts in-place using stable mergesort algorithm
 * @note O(n log n) time complexity, O(n) space complexity
 * @note Uses default comparison for the element type
 * @note Supported types depend on csort library defaults
 *
 * @see cvector_sort_with_comparison_proc
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push_rvalue(vec, 3);
 * cvec_push_rvalue(vec, 1);
 * cvec_push_rvalue(vec, 2);
 * cvec_sort(vec);  // vec is now [1, 2, 3]
 * @endcode
 */
#define cvec_sort(v)                                              \
  do {                                                            \
    ccol_comparison_proc_t comparison_proc =                      \
        csort_get_default_comparison_proc(*(v##__cvec_type_var)); \
    cvector_sort_with_comparison_proc(v, comparison_proc);        \
  } while (0)
