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
 * @param elem_size Size of each element in bytes (must be > 0, and must not
 * exceed SIZE_MAX / 4, since the initial 4-element backing buffer allocation
 * would otherwise overflow)
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
 *
 * @note Will assert if v is NULL
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
 * @note new_elem may safely alias into v's own backing buffer (for example a
 *       pointer returned by cvector_data_ptr(v) or cvector_at(v, i)); any
 *       capacity expansion this call triggers will not invalidate it
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
 * @note arr_ptr may safely alias into v's own backing buffer (for example a
 *       pointer returned by cvector_data_ptr(v) or cvector_at(v, i)); any
 *       capacity expansion this call triggers will not invalidate it
 * @note The source and destination byte ranges are allowed to overlap (for
 *       example when arr_ptr aliases v's own buffer and elem_count reads
 *       past v's current element count into already-reserved capacity);
 *       the copy is overlap-safe
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
/*                         ITERATOR */
/* ========================================================================== */

/**
 * @brief Find the index of the first occurrence of an element
 *
 * Performs a linear scan and returns the zero-based index of the first element
 * that compares equal to *elem. When cmp is NULL the comparison is done with
 * memcmp over the element size (byte-wise equality).
 *
 * @param v    Vector to search (must not be NULL)
 * @param elem Pointer to the value to search for (must not be NULL)
 * @param cmp  Comparison function, or NULL to use memcmp
 *
 * @return Zero-based index of the first match, or ccol_invalid_size if not
 * found or elem is NULL
 *
 * @note O(n) complexity
 * @note Asserts if v is NULL
 * @note Structs with padding bytes may not compare correctly when cmp is NULL
 *
 * @see cvec_find
 */
size_t cvector_find(cvec v, const void *elem, ccol_comparison_proc_t cmp);

/**
 * @brief Create an iterator positioned at the first element.
 *
 * Returns a @c cmap_iterator* whose @c key_pair->ptr points to the internal
 * index field and whose @c val_pair->ptr points directly into the vector's
 * buffer.  The @c _direct_ptr flag is set to @c true so the unified accessor
 * macros (@c ccol_iter_key_ptr / @c ccol_iter_val_ptr) bypass the map SSO
 * path and return typed pointers straight into the buffer.
 *
 * @param v    Vector to iterate
 * @param err  Optional pointer to receive an error string on failure
 *
 * @return Pointer to a @c cmap_iterator, or NULL if the vector is empty or
 *         allocation fails
 *
 * @note The iterator is destroyed automatically when cvec_iter_next() reaches
 *       the end, or call ccol_iter_destroy() to abort early.
 * @note Modifying the vector during iteration invalidates the iterator.
 * @note A NULL v is treated the same as an empty vector (returns NULL, not
 * an error); this is intentional, not merely permissive, matching
 * chashmap_begin_iter()/cbmap_begin_iter()'s identical NULL-tolerance, so a
 * lazily-created container field left uninitialized because nothing has
 * been inserted into it yet can be iterated directly without every caller
 * needing its own NULL guard first
 *
 * @see cvec_begin
 * @see ccol_iter_next
 */
cmap_iterator *cvector_begin_iter(cvec v, char **err);

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
 * @note Type variable is named v##__ccol_val_type_var and used internally by
 * macros
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
#define cvec_declare(v, type)                                             \
  size_t *v##__ccol_key_type_var                                          \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  type *v##__ccol_val_type_var                                            \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  cvec v                       /* deliberately not initialized to NULL */

#define cvec_declare_scoped(v, type)                             \
  size_t *v##__ccol_key_type_var __attribute__((unused)) = NULL; \
  type *v##__ccol_val_type_var __attribute__((unused)) = NULL;   \
  cvec v _ccol_destructor(___cvector_destroy) = NULL

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
#define cvec_redeclare(v, type)                                  \
  size_t *v##__ccol_key_type_var __attribute__((unused)) = NULL; \
  type *v##__ccol_val_type_var __attribute__((unused)) = NULL

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
    v = cvector_create(sizeof(*v##__ccol_val_type_var), &err);           \
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
#define cvec_init_mp(v, mprocs)                                               \
  do {                                                                        \
    char *err = NULL;                                                         \
    v = cvector_create_full(sizeof(*v##__ccol_val_type_var), (mprocs), &err); \
    if (!(v)) {                                                               \
      fatal_err("cvec_init_mp('%s'): %s", #v, err ? err : "unknown error");   \
    }                                                                         \
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
 * Type-safe wrapper for cvector_push_back(). new_elem is converted to v's
 * declared element type via ordinary C assignment/initialization (exactly as
 * `T tmp = new_elem;` would do) before being copied into the vector, then
 * that converted copy is what's actually pushed. Calls fatal_err() on
 * cvector_push_back() failure.
 *
 * @param v Vector to push to
 * @param new_elem Element value to push
 *
 * @note Terminates program on failure
 * @note For rvalues (literals, expressions with no addressable storage),
 * cvec_push_rvalue() is equivalent; both macros behave identically today
 * since neither takes new_elem's address directly any more
 * @note new_elem may safely alias into v's own backing buffer (for example
 * cvec_at(v, i)): its value is read into a private, non-aliasing temporary
 * before v is ever touched, so a capacity expansion this call triggers can
 * never invalidate it
 * @note new_elem's value is converted to v's declared element type the same
 * way a plain C assignment would (e.g. an int literal pushed into a
 * cvec_construct(v, long) converts to the long value, and a float pushed
 * into a cvec_construct(v, int) truncates to an int the same way `int x =
 * some_float;` would); new_elem's raw bit pattern is never copied verbatim
 * into a differently-typed element, even when the two types happen to share
 * the same size. A new_elem whose type cannot be implicitly converted to v's
 * declared element type at all (e.g. two unrelated struct types, however
 * identically laid out) is a compile error at this point, not a silently
 * reinterpreted value
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
/* The result-holding local below is deliberately named __cvec_push_r, not the
 * far more tempting plain 'r' (this project's own house style for a retval
 * local): 'r' is declared in the SAME statement whose initializer embeds the
 * caller's own new_elem expression via macro substitution, and C's
 * declarator-scope rule ("the scope of an identifier begins right after its
 * own declarator", i.e. before the initializer is even evaluated; the same
 * rule that makes `int x = x;` a self-reference, not a copy of an outer x)
 * means a caller who happens to name their own pushed variable 'r' would
 * silently take the address of this macro's own not-yet-initialized 'r'
 * instead of their own, reproduced as real, silent data corruption with
 * zero compiler warnings at any optimization level before this was renamed.
 * A name this unlikely to ever collide with a real caller identifier is the
 * only practical fix available to a non-hygienic C macro. The same reasoning
 * applies to __cvec_push_val below it: it is declared in its own statement
 * (not nested inside new_elem's own expansion), so it has no declarator-scope
 * hazard of its own, but it is still named with the double-underscore prefix
 * C reserves for implementation use, so no conforming caller-supplied
 * identifier can ever collide with it either.
 *
 * __cvec_push_val itself is not merely a hygiene device: it is what fixes a
 * real, previously-shipped bug. This macro used to take &(new_elem) directly
 * and hand that raw pointer straight to cvector_push_back(), guarded only by
 * a sizeof() _Static_assert. That assert caught a differently-SIZED new_elem
 * (preventing an out-of-bounds read) but did nothing for a differently-TYPED
 * new_elem of the *same* size (e.g. pushing a `float` into a
 * cvec_construct(v, int)): the assert passed (sizeof(float) ==
 * sizeof(int) on every mainstream platform) and cvector_push_back() copied
 * the float's raw 4-byte IEEE-754 bit pattern into the vector's storage
 * verbatim, so reading it back as int produced the float's reinterpreted
 * bits (e.g. 1077936128 for 3.0f), not the value 3 an ordinary C assignment
 * would have produced; silent data corruption with zero compiler
 * diagnostics at any optimization level. Declaring __cvec_push_val as v's
 * own declared element type and initializing it from new_elem routes that
 * conversion through the C compiler's own assignment rules instead of a raw
 * byte copy, which is what makes the result correct for every implicitly
 * convertible new_elem type and a compile error for every genuinely
 * incompatible one; it also makes the sizeof()-only _Static_assert both
 * unnecessary (a mismatched-size new_elem can no longer cause an
 * out-of-bounds read, since __cvec_push_val is always exactly v's own
 * element size) and actively wrong (it would reject a legitimate, safe
 * conversion like an int literal into a cvec_construct(v, long)), so it was
 * removed rather than kept alongside the fix. */
#define cvec_push(v, new_elem)                                    \
  do {                                                            \
    typeof(*v##__ccol_val_type_var) __cvec_push_val = (new_elem); \
    ccol_retval_t __cvec_push_r =                                 \
        cvector_push_back((v), (const void *)&__cvec_push_val);   \
    if (__cvec_push_r != ccol_success) {                          \
      fatal_err("cvec_push('%s'): r: %d (%s)", #v, __cvec_push_r, \
                ccol_retval_to_str(__cvec_push_r));               \
    }                                                             \
  } while (0)

/**
 * @brief Push an rvalue element onto the vector (type-safe)
 *
 * Type-safe wrapper for cvector_push_back() that can handle rvalue expressions
 * that cannot be directly addressed. Creates a temporary, typed as v's own
 * declared element type and initialized from new_elem via a GNU C compound
 * literal, and pushes that. Calls fatal_err() on cvector_push_back() failure.
 *
 * @param v Vector to push to
 * @param new_elem Element value to push (can be rvalue)
 *
 * @note Terminates program on failure
 * @note Can push literal values and expressions
 * @note Uses GNU C compound literal extension
 * @note new_elem's value is converted to v's declared element type the same
 * way a plain C assignment would (e.g. an unsuffixed int literal like 5
 * pushed into a cvec_construct(v, long) converts to the long value 5, and a
 * float expression pushed into a cvec_construct(v, int) truncates to an int
 * the same way `int x = some_float;` would); new_elem's raw bit pattern is
 * never copied verbatim into a differently-typed element, even when the two
 * types happen to share the same size. A new_elem whose type cannot be
 * implicitly converted to v's declared element type at all is a compile
 * error at this point, not a silently reinterpreted value
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
/* See cvec_push's own comment just above it for why this local is named
 * __cvec_push_rvalue_r rather than plain 'r': new_elem's expansion sits
 * inside this same declaration's initializer (now doubly so, since it is
 * also the compound literal's own initializer), so a caller-supplied
 * expression containing the bare identifier 'r' would otherwise resolve to
 * this macro's own not-yet-initialized local instead of the caller's.
 *
 * The compound literal itself is typed as v's own declared element type
 * (typeof(*v##__ccol_val_type_var)), not typeof(new_elem); mirrors
 * cvec_find's own compound literal a few macros down, for the identical
 * reason. Typing it as new_elem's own type used to be a real, shipped bug:
 * it made the compound literal's initialization a same-type copy (or, for a
 * literal like 5, an int-to-int copy) instead of a genuine conversion to v's
 * element type, so a same-sized-but-differently-typed new_elem (e.g. a
 * `float` pushed into a cvec_construct(v, int)) had its raw bit pattern
 * copied into the vector verbatim rather than being converted the way a
 * plain C assignment would; reading it back produced the float's
 * reinterpreted bits (e.g. 1077936128 for 3.0f), not the value 3, with zero
 * compiler diagnostics at any optimization level. The sizeof()-based
 * _Static_assert this macro used to carry only ever caught a differently-
 * SIZED new_elem (preventing an out-of-bounds read past a too-small compound
 * literal); it did nothing for this same-size-different-type case, and gave
 * a false impression of type safety while doing so. Typing the compound
 * literal as v's own element type fixes both problems at once: the literal
 * is always exactly v's element size (no out-of-bounds read is possible
 * regardless of new_elem's own size), and its initializer goes through the
 * C compiler's real conversion rules instead of a raw byte copy, so the
 * _Static_assert became both unnecessary and, for a legitimate conversion
 * like an int literal into a cvec_construct(v, long), actively wrong (it
 * would have rejected a now-safe, correct push); it was removed rather than
 * kept alongside the fix. */
#define cvec_push_rvalue(v, new_elem)                         \
  do {                                                        \
    ccol_retval_t __cvec_push_rvalue_r = cvector_push_back(   \
        (v), &(typeof(*v##__ccol_val_type_var)){(new_elem)}); \
    if (__cvec_push_rvalue_r != ccol_success) {               \
      fatal_err("cvec_push_rvalue('%s'): r: %d (%s)", #v,     \
                __cvec_push_rvalue_r,                         \
                ccol_retval_to_str(__cvec_push_rvalue_r));    \
    }                                                         \
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
/* Both locals below use the __cvec_pop_ prefix rather than the more obvious
 * '_tmp'/'r': v's own expansion (e.g. a vector variable a caller happened to
 * name '_tmp') appears inside _tmp's own later use as an address-of target,
 * and r's declaration/initializer has the identical hazard cvec_push's own
 * comment documents in full. A name this specific to this one macro is
 * exceedingly unlikely to ever collide with a real caller identifier. */
#define cvec_pop(v)                                                         \
  ({                                                                        \
    typeof(*v##__ccol_val_type_var) __cvec_pop_tmp;                         \
    ccol_retval_t __cvec_pop_r = cvector_pop_back((v), &__cvec_pop_tmp);    \
    if (__cvec_pop_r != ccol_success) {                                     \
      fatal_err(                                                            \
          "cvec_pop('%s'): r: %d (%s)%s", #v, __cvec_pop_r,                 \
          ccol_retval_to_str(__cvec_pop_r),                                 \
          __cvec_pop_r == ccol_container_empty ? "; vector is empty" : ""); \
    }                                                                       \
    __cvec_pop_tmp;                                                         \
  })

/**
 * @brief Terminates via fatal_err() when ptr is NULL (used by cvec_at)
 *
 * Internal helper, not meant to be called directly. Kept as a plain function
 * so the diagnostic-formatting logic itself lives in one place; cvec_at is
 * the one that wraps it in a statement expression (to evaluate index exactly
 * once), and that wrapping still composes correctly both as an rvalue and as
 * an assignment target and does not trip -Wunused-value when the result is
 * discarded, since the outer expression is still a plain pointer dereference.
 */
static inline __attribute__((always_inline)) void *_cvec_at_checked(
    void *ptr, const char *vec_name, size_t index, size_t elem_count) {
  if (!ptr) {
    fatal_err("cvec_at('%s'): index %lu out of bounds (size: %lu)", vec_name,
              (unsigned long)index, (unsigned long)elem_count);
  }
  return ptr;
}

/**
 * @brief Access element at index (type-safe, returns reference)
 *
 * Type-safe wrapper for cvector_at() that returns the element reference (not
 * pointer). Automatically casts to the correct type. Terminates the program
 * via fatal_err() if index is out of bounds, matching every other type-safe
 * macro's "aborting convenience API" contract; use cvec_at_ptr for a
 * non-terminating, NULL-on-out-of-bounds alternative.
 *
 * @param v Vector to access
 * @param index Zero-based index of element; evaluated exactly once, so a
 * side-effecting expression (e.g. cvec_at(vec, i++)) is safe to pass
 *
 * @return Element reference at index
 *
 * @note Returns reference, not pointer
 * @note Terminates the program via fatal_err() if index is out of bounds
 *
 * @see cvector_at
 * @see cvec_at_ptr
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push_rvalue(vec, 42);
 * int val = cvec_at(vec, 0);  // val == 42
 * cvec_at(vec, 0) = 5;        // now the first element is 5
 * @endcode
 */
/* index is captured into a local (__cvec_at_index) exactly once, before
 * being handed to both cvector_at() and _cvec_at_checked()'s own
 * diagnostic-only re-read. Passing (index) directly to both, as this macro
 * used to, evaluates it twice as two separate arguments of the same
 * _cvec_at_checked() call (unsequenced relative to each other), which is
 * undefined behavior for any side-effecting index expression (an
 * indeterminate final value for e.g. a bare `i++`, and in practice each
 * loop iteration advancing the index variable by 2 instead of 1, since both
 * evaluations' increments apply). Reproduced directly: `cvec_at(vec, j++)`
 * in an ordinary loop advanced j by 2 per call and walked off the end of
 * the vector, hitting the out-of-bounds fatal_err() below despite the call
 * site looking correct; GCC's own -Wsequence-point flags the pre-fix
 * expansion. __cvec_at_index uses the same double-underscore,
 * macro-specific naming this header already relies on elsewhere (e.g.
 * __cvec_push_val) so no real caller identifier can plausibly collide with
 * it. The statement expression still composes as an lvalue: `*ptr_expr` is
 * an lvalue regardless of whether ptr_expr's own computation used a
 * statement expression, so `cvec_at(v, i) = x;` keeps working. */
#define cvec_at(v, index)                                                   \
  (*(typeof(*v##__ccol_val_type_var) *)({                                   \
    size_t __cvec_at_index = (index);                                       \
    _cvec_at_checked(cvector_at((v), __cvec_at_index), #v, __cvec_at_index, \
                     cvector_elem_count(v));                                \
  }))

/**
 * @brief Access element at index (type-safe, returns pointer or NULL)
 *
 * Type-safe wrapper for cvector_at() that returns a pointer to the element,
 * or NULL if index is out of bounds. Unlike cvec_at, does not terminate the
 * program on an out-of-bounds index.
 *
 * @param v Vector to access
 * @param index Zero-based index of element
 *
 * @return Pointer to element at index, or NULL if index is out of bounds
 *
 * @note Returns NULL if index is out of bounds (does not terminate)
 * @note Returned pointer is only valid until the vector is resized
 *
 * @see cvector_at
 * @see cvec_at
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push_rvalue(vec, 42);
 * int *ptr = cvec_at_ptr(vec, 0);
 * if (ptr) {
 *   *ptr = 5; // Modify in-place
 * }
 * @endcode
 */
#define cvec_at_ptr(v, index) \
  ((typeof(*v##__ccol_val_type_var) *)(cvector_at((v), (index))))

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
 * @param new_capacity_count Minimum number of elements to reserve; evaluated
 * exactly once (including on the failure path), so a side-effecting
 * expression is safe to pass
 *
 * @note Terminates program if reservation fails
 *
 * @see cvector_reserve
 */
/* new_capacity_count is captured into __cvec_reserve_count exactly once,
 * before either use (the real cvector_reserve() call and the diagnostic
 * message), for the same reason documented at length next to cvec_push's own
 * internal locals just above: a macro parameter embedded via textual
 * substitution into more than one place in the expansion is evaluated once
 * per place it appears, so a side-effecting new_capacity_count (e.g. a
 * function call reading from a queue, or a non-deterministic value) used to
 * be evaluated a second time solely to build the fatal_err() message on the
 * failure path; silently reporting a different value than was actually
 * passed to cvector_reserve(), and duplicating any real side effect right
 * before the process aborts. */
#define cvec_reserve(v, new_capacity_count)                             \
  do {                                                                  \
    size_t __cvec_reserve_count = (new_capacity_count);                 \
    if (!cvector_reserve((v), __cvec_reserve_count)) {                  \
      fatal_err(                                                        \
          "cvec_reserve('%s'): failed to reserve %lu elements; out of " \
          "memory?",                                                    \
          #v, (unsigned long)__cvec_reserve_count);                     \
    }                                                                   \
  } while (0)

/**
 * @brief Append array of elements (type-safe wrapper with error handling)
 *
 * Type-safe wrapper for cvector_append_array() that calls fatal_err() on
 * failure.
 *
 * @param v Vector to append to
 * @param arr_ptr Pointer to array of elements
 * @param elem_count Number of elements in array; evaluated exactly once
 * (including on the failure path), so a side-effecting expression is safe to
 * pass
 *
 * @note Terminates program if append fails
 *
 * @see cvector_append_array
 */
/* elem_count is captured into __cvec_append_array_count exactly once, before
 * either use, for the identical reason documented next to cvec_reserve's own
 * matching fix just above: embedding the same macro parameter into both the
 * real call and the fatal_err() diagnostic evaluates it twice on the failure
 * path otherwise. */
#define cvec_append_array(v, arr_ptr, elem_count)                           \
  do {                                                                      \
    size_t __cvec_append_array_count = (elem_count);                        \
    if (!cvector_append_array((v), (arr_ptr), __cvec_append_array_count)) { \
      fatal_err(                                                            \
          "cvec_append_array('%s'): failed to append %lu elements; out of " \
          "memory?",                                                        \
          #v, (unsigned long)__cvec_append_array_count);                    \
    }                                                                       \
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
 * @brief Type-correct csort_item_getter_proc_t adapter for cvector_at()
 *
 * cvector_at() is declared to take a cvec (i.e. cvector *) as its first
 * parameter, not void *, while csort_item_getter_proc_t requires exactly
 * void *(*)(void *, size_t). Passing cvector_at() to csort_sort() via a
 * function pointer cast to csort_item_getter_proc_t calls it through a
 * pointer to an incompatible function type, which is undefined behavior per
 * C11 6.3.2.3p8 regardless of cvec and void * sharing identical
 * representation on every mainstream ABI (confirmed via Clang's
 * -fsanitize=function, which flags every such call). This adapter has the
 * exact csort_item_getter_proc_t signature itself, so it can be handed to
 * csort_sort() directly with no cast at all, closing the UB.
 */
static inline __attribute__((always_inline)) void *_cvec_sort_getter(
    void *collection, size_t index) {
  return cvector_at((cvec)collection, index);
}

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
 * @note Calls fatal_err() if comparison_proc is NULL (for cvec_sort, this
 * means no default comparator is available for the vector's element type;
 * pass an explicit comparison function to this macro directly instead)
 * @note Calls fatal_err() if the sort's internal temporary buffer could not
 * be allocated, matching every other mutating type-safe macro in this header
 * (cvec_push, cvec_reserve, cvec_append_array, ...); the vector is left
 * completely unsorted (and unmodified) in that case, so this never silently
 * hands back a partially- or un-sorted vector as if nothing had gone wrong
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
#define cvector_sort_with_comparison_proc(v, comparison_proc)                \
  do {                                                                       \
    if (!(v)) {                                                              \
      fatal_err("cvector_sort_with_comparison_proc('%s'): vector is NULL",   \
                #v);                                                         \
    }                                                                        \
    /* Evaluated into a plain pointer variable, rather than tested via       \
     * !(comparison_proc) directly, so that a caller passing a bare named    \
     * comparator function (as opposed to a variable already holding one,    \
     * e.g. cvec_sort's own use of this macro) does not trip -Werror=address \
     * ("the address of 'X' will always evaluate as 'true'"); GCC/Clang      \
     * can prove a *named function's* address is never NULL, but not a       \
     * pointer variable's, even one initialised from that exact same         \
     * function. This also means comparison_proc's own expression is         \
     * evaluated exactly once, matching this header's existing single-       \
     * evaluation convention for a macro's other arguments. */               \
    ccol_comparison_proc_t __cvec_sort_cmp = (comparison_proc);              \
    if (!__cvec_sort_cmp) {                                                  \
      fatal_err(                                                             \
          "cvector_sort_with_comparison_proc('%s'): comparison_proc is "     \
          "NULL (no default comparator is available for this element "       \
          "type; pass an explicit comparison function to "                   \
          "cvector_sort_with_comparison_proc instead of using cvec_sort)",   \
          #v);                                                               \
    }                                                                        \
    if (!csort_sort((v), cvector_elem_count((v)),                            \
                    sizeof(*(v##__ccol_val_type_var)), _cvec_sort_getter,    \
                    __cvec_sort_cmp, cvector_get_mprocs((v)))) {             \
      fatal_err(                                                             \
          "cvector_sort_with_comparison_proc('%s'): out of memory; vector "  \
          "left unsorted",                                                   \
          #v);                                                               \
    }                                                                        \
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
 * @note Supported types depend on csort library defaults; calls fatal_err()
 * with a clear diagnostic if the element type has no default comparator
 * (use cvector_sort_with_comparison_proc directly with an explicit
 * comparison function for such a type)
 * @note Calls fatal_err() on allocation failure, like every other mutating
 * type-safe macro in this header; see cvector_sort_with_comparison_proc
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
#define cvec_sort(v)                                                  \
  do {                                                                \
    ccol_comparison_proc_t comparison_proc =                          \
        csort_get_default_comparison_proc(*(v##__ccol_val_type_var)); \
    cvector_sort_with_comparison_proc(v, comparison_proc);            \
  } while (0)

/**
 * @brief Find the first occurrence of an element by value (type-safe)
 *
 * Type-safe wrapper for cvector_find() that accepts a value (including
 * rvalues and literals) and compares using memcmp (byte-wise equality).
 * Use cvector_find() directly when a custom comparator is needed.
 *
 * @param v    Vector to search
 * @param elem Element value to search for
 *
 * @return Zero-based index of the first match, or ccol_invalid_size if not
 * found
 *
 * @note O(n) complexity
 * @note Uses memcmp for comparison (byte-wise equality)
 * @note Structs with padding bytes may not compare correctly
 *
 * @see cvector_find
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push_rvalue(vec, 10);
 * cvec_push_rvalue(vec, 20);
 * cvec_push_rvalue(vec, 30);
 * size_t idx = cvec_find(vec, 20);  // idx == 1
 * size_t nf  = cvec_find(vec, 99);  // nf == ccol_invalid_size
 * @endcode
 */
#define cvec_find(v, elem) \
  cvector_find((v), &(typeof(*(v##__ccol_val_type_var))){(elem)}, NULL)
