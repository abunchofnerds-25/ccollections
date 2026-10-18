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

/* Everything declared from here to the end of this header belongs to the
 * public Application Binary Interface (ABI) of libccollections, and the
 * shared library exports all of it. The library is built with
 * -fvisibility=hidden, so a function or object outside these blocks stays
 * internal: it isn't in the dynamic symbol table, an application linking
 * against the library can't interpose it, and a symbol of the same name in
 * that application can't collide with it. */
#pragma GCC visibility push(default)

/**
 * @file cvector.h
 * @brief Dynamic array (vector) container that changes its size
 * automatically
 *
 * A generic dynamic array that grows as you add elements and shrinks as you
 * remove them. It stores elements of one fixed size, which you give when you
 * create it.
 *
 * Main features:
 * - Automatic capacity control (grows by 2x, shrinks by 0.5x)
 * - Minimum capacity of 4 elements
 * - Type-inferred macros for the common operations
 * - Support for custom memory management
 * - Amortized O(1) push and pop operations
 */

/** @brief Opaque vector structure */
typedef struct cvector cvector;

/** @brief Pointer to a vector (the handle type) */
typedef cvector *cvec;

/* ========================================================================== */
/*                         CORE VECTOR FUNCTIONS                              */
/* ========================================================================== */

/**
 * @brief Create a vector with custom memory management
 *
 * Creates a new vector that stores elements of the size you give. It starts
 * with a minimum capacity of 4 elements and changes its capacity when it
 * needs to.
 *
 * @param elem_size Size of each element in bytes. It must be more than 0
 * and not more than SIZE_MAX / 4, because a larger value overflows the
 * allocation of the first 4-element backing buffer.
 * @param mmgmt_procs Custom memory management procedures, or NULL to use
 * the default malloc and free
 * @param err Optional pointer that receives an error string on failure.
 * Pass NULL to ignore the error string.
 *
 * @return Pointer to the new vector, or NULL on failure
 *
 * @note The first capacity is 4 elements
 * @note The capacity doubles when the vector is full and you add one more
 * element, and it halves when less than 1/4 of the capacity is in use.
 * @note You must destroy the vector with cvector_destroy() after use
 *
 * @see cvector_create
 * @see cvector_destroy
 */
cvec cvector_create_full(size_t elem_size, ccol_memmgmt_procs_t *mmgmt_procs,
                         char **err);

/**
 * @brief Create a vector with default memory management
 *
 * A convenience wrapper that creates a vector using the standard malloc and
 * free.
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
 * Gives the memory management procedures struct of this vector.
 *
 * @param v Vector to query
 *
 * @return Pointer to the memory management procedures, or NULL when the
 * vector uses the default procedures
 *
 * @note It asserts when v is NULL
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
 * @brief Destroy a vector and set the pointer to NULL
 *
 * Frees every resource of the vector, the data buffer included, and then
 * sets the vector pointer to NULL.
 *
 * @param v Vector to destroy. It is set to NULL once the vector is
 * destroyed. It must be a modifiable lvalue, such as a variable or an
 * element of an array, and the macro evaluates it exactly once.
 *
 * @note Calling it with a NULL pointer is safe and does nothing
 * @note When the vector holds pointers to heap buffers, those elements are
 * not freed one by one, so free the element data first if you need to.
 */
#define cvector_destroy(v)    \
  _ccol_cvector_destroy_impl( \
      v, _ccol_uniq(__ccol_cvector_destroy_slot, __COUNTER__))

/* Internal: the body of cvector_destroy. slot is a name from _ccol_uniq(),
 * so the macro can nest inside the argument of another destroy macro and
 * still be -Wshadow clean. The argument is evaluated exactly once. */
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
 * Adds a new element at the end of the vector and doubles the capacity when
 * the vector is full. The element is copied in with an assignment that is
 * optimized for the element size.
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
 * @note The element is copied into the vector
 * @note The capacity doubles when the vector is full (a scaling factor of 2)
 * @note It asserts when v is NULL
 * @note new_elem may point into v's own backing buffer, for example a
 *       pointer that cvector_data_ptr(v) or cvector_at(v, i) gave: a growth
 *       of the capacity that this call starts doesn't invalidate such a
 *       pointer.
 *
 * @see cvector_pop_back
 * @see cvec_push
 */
ccol_retval_t cvector_push_back(cvec v, const void *new_elem);

/**
 * @brief Remove and return the last element from the vector
 *
 * Removes the last element of the vector and copies it to the target
 * buffer. The capacity halves when less than 1/4 of it is in use;
 * the minimum capacity is 4.
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
 * @note The element is copied to target_elem
 * @note The capacity halves when less than 1/4 of it is in use, down to the
 * minimum of 4
 * @note It asserts when v is NULL
 *
 * @see cvector_push_back
 * @see cvec_pop
 */
ccol_retval_t cvector_pop_back(cvec v, void *target_elem);

/**
 * @brief Access an element at a specific index
 *
 * Gives a pointer to the element at the index you give, which you can use to
 * read the element or to change it in place.
 *
 * @param v Vector to access
 * @param index Zero-based index of the element to access
 *
 * @return Pointer to the element at the index, or NULL when the index is
 * out of bounds
 *
 * @note O(1) complexity
 * @note It gives NULL when index >= elem_count, and also when the vector is
 * empty.
 * @note The pointer is valid only until the size of the vector changes
 * @note It asserts when v is NULL
 *
 * @see cvec_at
 * @see cvector_elem_count
 */
void *cvector_at(cvec v, size_t index);

/**
 * @brief Get the number of elements in the vector
 *
 * Gives how many elements the vector holds right now.
 *
 * @param v Vector to query
 *
 * @return Number of elements in the vector
 *
 * @note O(1) complexity
 * @note It asserts when v is NULL
 *
 * @see cvec_size
 */
size_t cvector_elem_count(cvec v);

/**
 * @brief Clear all elements and reset capacity
 *
 * Removes every element of the vector and then tries to shrink the capacity
 * back to the minimum of 4 elements. If that reallocation fails the
 * capacity stays as it is, but the element count is set to 0 anyway.
 *
 * @param v Vector to reset
 *
 * @note The element count becomes 0
 * @note The capacity goes back to the minimum of 4 when the reallocation
 * succeeds
 * @note The elements aren't freed one by one, so do that first if you need
 * to.
 * @note It asserts when v is NULL
 *
 * @see cvec_reset
 */
void cvector_reset(cvec v);

/**
 * @brief Reserve capacity for future elements
 *
 * Allocates capacity in advance for at least new_capacity_count elements,
 * rounded up to the nearest power of two with a minimum of 4. When you know
 * the final size up front, a reservation saves you a lot of reallocations.
 *
 * @param v Vector to reserve capacity for
 * @param new_capacity_count Minimum number of elements to reserve space for
 *
 * @return true when the reservation succeeds, false on failure
 *
 * @note The real capacity is rounded up to the nearest power of two that is
 * 4 or more
 * @note The capacity never shrinks when new_capacity is less than or equal
 * to the current capacity
 * @note It gives true at once when the vector already has enough capacity
 * @note It gives false when new_capacity > ccol_max_elem_count
 * @note It asserts when v is NULL
 *
 * @see cvec_reserve
 *
 * Example:
 * @code
 * cvec v = cvector_create(sizeof(int), NULL);
 * cvector_reserve(v, 100);  // Allocates 128 elements (the next power of 2)
 * // You can push 100 elements or more now with no reallocation
 * @endcode
 */
bool cvector_reserve(cvec v, size_t new_capacity_count);

/**
 * @brief Append an array of elements to the vector
 *
 * Appends several elements from a C array to the end of the vector in one
 * go, growing the capacity with at most one reallocation when it has to.
 *
 * @param v Vector to append to
 * @param arr_ptr Pointer to the array of elements to append. It is only
 * read through, so it can point at a const array.
 * @param elem_count Number of elements in the array
 *
 * @return true on success, false on failure
 *
 * @note It is cheaper than many push_back calls
 * @note The whole range is copied in one operation, not element by
 * element.
 * @note It can grow the capacity with cvector_reserve
 * @note It gives false when the result is more than ccol_max_elem_count
 * @note It gives false when elem_count causes an overflow
 * @note It asserts when v is NULL
 * @note arr_ptr may point into v's own backing buffer, for example a
 *       pointer that cvector_data_ptr(v) or cvector_at(v, i) gave: a growth
 *       of the capacity that this call starts doesn't invalidate such a
 *       pointer.
 * @note The source and destination byte ranges may overlap, for example
 *       when arr_ptr aliases v's buffer and its elem_count reads past the
 *       current element count of v, into capacity that the vector has
 *       reserved already. The copy is safe when the ranges overlap.
 * @note An aliasing source range has to stay inside the reserved capacity
 *       of v. When arr_ptr points into v's backing buffer and its elem_count
 *       reads past the end of that buffer's reservation, it gives false and
 *       leaves v unchanged. Only an aliasing source range can be bounded
 *       like this, because an array of the caller passed as a bare pointer
 *       (as every C array is) has no extent that this function can see.
 *
 * @see cvector_append_cvector
 * @see cvec_append_array
 *
 * Example:
 * @code
 * int arr[] = {1, 2, 3, 4, 5};
 * cvec v = cvector_create(sizeof(int), NULL);
 * cvector_append_array(v, arr, 5);  // Adds all 5 elements in one call
 * @endcode
 */
bool cvector_append_array(cvec v, const void *arr_ptr, size_t elem_count);

/**
 * @brief Append all elements from one vector to another
 *
 * Copies all the elements of v_from to the end of v_to in one go. Both
 * vectors must have the same element size.
 *
 * @param v_to Destination vector to append to
 * @param v_from Source vector to copy the elements from
 *
 * @return true on success, false on failure
 *
 * @note Both vectors must have the same elem_size
 * @note v_from is not changed
 * @note The capacity of v_to can grow
 * @note It gives false when the result is more than ccol_max_elem_count
 * @note It asserts when one of the vectors is NULL, and also when the two
 * elem_size values differ.
 *
 * @see cvector_append_array
 * @see cvec_append_cvec
 *
 * Example:
 * @code
 * cvec v1 = cvector_create(sizeof(int), NULL);
 * cvec v2 = cvector_create(sizeof(int), NULL);
 * // ... fill v1 and v2 ...
 * cvector_append_cvector(v1, v2);  // v1 has all the elements of v2 too
 * @endcode
 */
bool cvector_append_cvector(cvec v_to, cvec v_from);

/**
 * @brief Get pointer to the underlying data array
 *
 * Gives a direct pointer to the vector's internal data buffer, which is
 * handy when you pass the vector to a function that expects a C array.
 *
 * @param v Vector to get the data pointer from
 *
 * @return Pointer to the internal data array
 *
 * @warning The pointer becomes invalid after any operation that can change
 * the size of the vector
 * @warning Don't free this pointer, the vector owns it.
 * @note It asserts when v is NULL
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
 * Works together with the _ccol_destructor attribute to clean up on its own
 * when a variable goes out of scope: it destroys the vector and sets the
 * pointer to NULL.
 *
 * @param cv Pointer to the vector pointer
 *
 * @note It is meant for __attribute__((cleanup))
 * @note Calling it with NULL, or with a pointer to NULL, is safe
 * @note It's an internal helper; prefer the cvector_destroy() macro.
 *
 * Example:
 * @code
 * {
 *   cvec v _ccol_destructor(___cvector_destroy) = cvector_create(sizeof(int),
 *                                                 NULL);
 *   // ... use the vector ...
 * } // The vector is destroyed here, at the end of the scope
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
 * A linear scan that gives the zero-based index of the first element equal
 * to *elem. With a NULL cmp the elements are compared with memcmp over the
 * element size, so equality is byte-wise.
 *
 * @param v    Vector to search. It must not be NULL.
 * @param elem Pointer to the value to search for. It must not be NULL.
 * @param cmp  Comparison function, or NULL to use memcmp
 *
 * @return Zero-based index of the first match, or ccol_invalid_size when
 * nothing matches or when elem is NULL.
 *
 * @note O(n) complexity
 * @note It asserts when v is NULL
 * @note With a NULL cmp a struct that has padding bytes can compare wrong
 */
size_t cvector_find(cvec v, const void *elem, ccol_comparison_proc_t cmp);

/* cvector_begin_iter() is declared in citerators.h (included above), since
 * the ccol_begin() macro there needs all three iterator functions in every
 * translation unit that uses it. */

/* ========================================================================== */
/*                         TYPE-INFERRED CONVENIENCE MACROS */
/* ========================================================================== */

/**
 * @brief Declare an uninitialized vector variable
 *
 * Declares a vector variable 'v' together with a type variable that the
 * macros use for type safety. You have to initialize the vector before
 * using it.
 *
 * @param v Name of the vector variable to declare
 * @param type Element type for the vector
 *
 * @note Initialize the vector with cvec_init() or cvec_construct() before
 * use
 * @note The type variable is named v##__ccol_val_type_var and the macros
 * use it internally.
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
#define cvec_declare(v, type)                                           \
  size_t *v##__ccol_key_type_var                                        \
      __attribute__((unused)); /* not initialized to NULL on purpose */ \
  __typeof__(type) *v##__ccol_val_type_var                              \
      __attribute__((unused)); /* not initialized to NULL on purpose */ \
  cvec v                       /* not initialized to NULL on purpose */

#define cvec_declare_scoped(v, type)                                       \
  size_t *v##__ccol_key_type_var __attribute__((unused)) = NULL;           \
  __typeof__(type) *v##__ccol_val_type_var __attribute__((unused)) = NULL; \
  cvec v _ccol_destructor(___cvector_destroy) = NULL

/**
 * @brief Turn on the type-inferred macros for a vector in a local scope
 *
 * Declares the type variable that the type-inferred macros need. Use it
 * when the vector was created without cvec_declare or cvec_construct.
 *
 * @param v Name of the vector variable
 * @param type Type of the elements in the vector
 *
 * @note Use it when you have a cvec from another scope and want
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
 * Initializes a vector that cvec_declare() declared, and stops the program
 * with _ccol_fatal_err() when the initialization fails.
 *
 * @param v Vector variable to initialize
 *
 * @note It calls _ccol_fatal_err() when the initialization fails
 * @note Declare the vector with cvec_declare() first
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
#define cvec_init(v)                                                        \
  do {                                                                      \
    char *__ccol_cvec_err = NULL;                                           \
    v = cvector_create(sizeof(*v##__ccol_val_type_var), &__ccol_cvec_err);  \
    if (!(v)) {                                                             \
      _ccol_fatal_err("cvec_init('%s'): %s", #v,                            \
                      __ccol_cvec_err ? __ccol_cvec_err : "unknown error"); \
    }                                                                       \
  } while (0)

/**
 * @brief Initialize a declared vector with custom memory management
 *
 * Initializes a vector with custom memory management procedures, and stops
 * the program with _ccol_fatal_err() when the initialization fails.
 *
 * @param v Vector variable to initialize
 * @param mprocs Pointer to the custom memory management procedures
 *
 * @note It calls _ccol_fatal_err() when the initialization fails
 * @note Declare the vector with cvec_declare() first
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
    char *__ccol_cvec_err = NULL;                                           \
    v = cvector_create_full(sizeof(*v##__ccol_val_type_var), (mprocs),      \
                            &__ccol_cvec_err);                              \
    if (!(v)) {                                                             \
      _ccol_fatal_err("cvec_init_mp('%s'): %s", #v,                         \
                      __ccol_cvec_err ? __ccol_cvec_err : "unknown error"); \
    }                                                                       \
  } while (0)

/**
 * @brief Declare and initialize a vector in one step
 *
 * A convenience macro that does cvec_declare() and cvec_init() together,
 * and stops the program with _ccol_fatal_err() when the initialization
 * fails.
 *
 * @param v Name of the vector variable to create
 * @param type Element type for the vector
 *
 * @note It calls _ccol_fatal_err() when the initialization fails
 * @note It is the same as: cvec_declare(v, type); cvec_init(v);
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
 * A convenience macro that does cvec_declare() and cvec_init_mp() together,
 * and stops the program with _ccol_fatal_err() when the initialization
 * fails.
 *
 * @param v Name of the vector variable to create
 * @param type Element type for the vector
 * @param mprocs Pointer to the custom memory management procedures
 *
 * @note It calls _ccol_fatal_err() when the initialization fails
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
 * The type-inferred wrapper for cvector_destroy().
 *
 * @param v Vector to destroy. It is set to NULL once the vector is
 * destroyed.
 *
 * @note Calling it with a NULL pointer is safe and does nothing
 *
 * @see cvector_destroy
 */
#define cvec_destroy(v) cvector_destroy(v)

/**
 * @brief The declared element type of v, without its top-level qualifiers.
 *
 * An internal helper of cvec_push, cvec_pop and cvec_find; don't use it
 * directly.
 *
 * Each of those macros writes to a temporary of the element type. cvec_pop
 * hands the temporary's address to cvector_pop_back(), and the other two
 * copy a value into it and then clear its padding with
 * _ccol_clear_padding(). For that write the temporary can't be const, even
 * for an element type like `const char *const`. _ccol_unqual_type() of
 * common.h gives such a type and leaves an array element type as it is. It
 * evaluates nothing, so v is never evaluated here.
 *
 * @param v Name of the vector variable
 */
#define _cvec_unqual_elem_type(v) _ccol_unqual_type(*v##__ccol_val_type_var)

/* True when the declared element type of v is an array type. Lvalue
 * conversion through the comma operator turns an array into a pointer and
 * leaves every other type compatible with itself. */
#define _cvec_elem_is_array(v)             \
  (!__builtin_types_compatible_p(          \
      __typeof__(*v##__ccol_val_type_var), \
      __typeof__((void)0, *v##__ccol_val_type_var)))

/* Declares tmp, a modifiable temporary of v's element type, and stores elem
 * into it, evaluating elem exactly once. cvec_push and cvec_find use it.
 *
 * For an element type that isn't an array elem is stored by assignment,
 * which converts it to the element type exactly like `T tmp = elem;` does.
 * An array can't be assigned, so for an array element type we store elem
 * (an array or a string literal) into arg, a pointer of elem's decayed type,
 * fill tmp with zeros, and copy the bytes of that array into tmp up to the
 * smaller of the two sizes.
 *
 * __builtin_choose_expr picks the target of the assignment and of the copy
 * at compile time, but the compiler still checks the arm it doesn't pick,
 * so both arms have to be well formed for every element type. They are,
 * because arg is a plain const void * when the element type isn't an array.
 * For an array element type a pointer as elem is a compile error, since the
 * macro can't see the size of the object it points to. */
/* elem as an operand of __typeof__ and sizeof, which only the array path of
 * _cvec_elem_load reads. For an array element type this is elem itself,
 * since that path needs its array type and size. For any other element type
 * it is the constant 0: there every use is short-circuited or sits in the
 * arm of __builtin_choose_expr that isn't chosen, so any valid expression
 * will do, and elem can't appear because C allows neither operator on a
 * bit-field. Neither operator evaluates its operand, so elem is still
 * evaluated only once. */
#define _cvec_elem_operand(v, elem) \
  __builtin_choose_expr(_cvec_elem_is_array(v), (elem), 0)

#define _cvec_elem_load(v, elem, tmp, arg)                                  \
  _Static_assert(                                                           \
      !_cvec_elem_is_array(v) ||                                            \
          !__builtin_types_compatible_p(                                    \
              __typeof__(_cvec_elem_operand(v, elem)),                      \
              __typeof__((void)0, _cvec_elem_operand(v, elem))),            \
      "a vector whose element type is an array takes an array or a string " \
      "literal as the element value");                                      \
  _cvec_unqual_elem_type(v) tmp;                                            \
  __typeof__(__builtin_choose_expr(_cvec_elem_is_array(v),                  \
                                   ((void)0, _cvec_elem_operand(v, elem)),  \
                                   (const void *)0)) arg;                   \
  *__builtin_choose_expr(_cvec_elem_is_array(v), &arg, &tmp) = (elem);      \
  __builtin_choose_expr(                                                    \
      _cvec_elem_is_array(v),                                               \
      (void)(memset(&tmp, 0, sizeof(tmp)),                                  \
             memcpy(&tmp, (const void *)arg,                                \
                    sizeof(_cvec_elem_operand(v, elem)) < sizeof(tmp)       \
                        ? sizeof(_cvec_elem_operand(v, elem))               \
                        : sizeof(tmp))),                                    \
      (void)0)

/**
 * @brief Push an element onto the vector (type-inferred)
 *
 * The type-inferred wrapper for cvector_push_back(). new_elem is converted
 * to the declared element type of v with an ordinary C assignment, exactly
 * like `T tmp = new_elem;`, and that converted value is copied into the
 * vector. It calls _ccol_fatal_err() when cvector_push_back() fails.
 *
 * @param v Vector to push to
 * @param new_elem The element: a variable, a literal or any expression,
 * including a struct value that a function returns.
 *
 * @note It stops the program on failure
 * @note For an array element type such as char[64], new_elem is an array or
 * a string literal: the element is filled with zeros and the bytes of
 * new_elem are copied into it, up to the smaller of the two sizes. For such
 * a type a pointer as new_elem is a compile error, because the macro can't
 * see the size of the object it points to.
 * @note The address of new_elem is never taken. The value is first copied
 * into a temporary of the element type, so a literal, a computed expression
 * and a struct value that a function returns work as well as a variable.
 * @note new_elem may alias into v's own backing buffer, for example
 * cvec_at(v, i). The value is read into a private temporary that doesn't
 * alias before v is touched, so a growth of the capacity that this call
 * starts can never make the value wrong.
 * @note With GCC 11 or later every padding byte of the temporary is set to
 * zero before the push, so the vector stores a struct element with zero
 * padding, which cvec_find relies on. With a compiler that has no
 * __builtin_clear_padding, such as Clang, the padding bytes of the stored
 * element are unspecified.
 * @note The value of new_elem is converted to v's declared element type the
 * same way a plain C assignment converts it. For example an int literal
 * going into a cvec_construct(v, long) becomes a long value, and a float
 * going into a cvec_construct(v, int) truncates to an int, as in
 * `int x = some_float;`. The raw bit pattern of new_elem is never copied
 * into an element of a different type, even when the two types have the
 * same size. A new_elem whose type has no implicit conversion to v's
 * declared element type, such as an unrelated struct type with the same
 * layout, is a compile error here; such a value is never quietly
 * reinterpreted.
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
/* The local below that holds the result is called __cvec_push_r on purpose.
 * 'r' is the usual name for a retval local in this project, but it isn't
 * safe here. Through macro substitution the declaration of 'r' is the SAME
 * statement whose initializer has the caller's new_elem expression in it,
 * and in C the scope of an identifier starts right after its own
 * declarator, so before the initializer is evaluated; that's the same rule
 * that makes `int x = x;` refer to itself instead of copying an outer x. If
 * the local were called 'r', a caller pushing its own variable named 'r'
 * would get the address of this macro's local, which has no value yet,
 * instead of the address of its own variable. That's silent data corruption
 * with no compiler warning at any optimization level, and a non-hygienic C
 * macro has only one practical defence against it: a name that can't
 * collide with a real identifier of a caller.
 *
 * The same goes for __cvec_push_val below it. That local has a statement of
 * its own and new_elem isn't expanded inside it, so it has no
 * declarator-scope hazard of its own, but its name also carries the
 * double-underscore prefix that C reserves for the implementation, so no
 * conforming identifier of a caller can collide with it.
 *
 * __cvec_push_val is more than hygiene though, it's what makes the stored
 * value right. Think of a macro that hands &(new_elem) straight to
 * cvector_push_back() with only a sizeof() _Static_assert as a guard. The
 * assert catches a new_elem of a different SIZE and prevents an
 * out-of-bounds read, but it catches nothing for a new_elem of a different
 * TYPE with the SAME size, such as a `float` going into a
 * cvec_construct(v, int), since sizeof(float) == sizeof(int) on every
 * mainstream platform. cvector_push_back() would then copy the float's raw
 * 4-byte IEEE-754 bit pattern into the vector's storage, and reading that
 * element as an int gives the reinterpreted float bits (1077936128 for
 * 3.0f, for example) instead of the 3 that an ordinary C assignment gives.
 * That's silent data corruption again, with no compiler diagnostic at any
 * optimization level.
 *
 * __cvec_push_val has v's declared element type and new_elem as its
 * initializer, so the conversion follows the compiler's assignment rules
 * and not a raw byte copy. That is right for every new_elem type with an
 * implicit conversion and a compile error for every type that is really
 * incompatible. So a sizeof()-only _Static_assert isn't needed: a new_elem
 * of a different size can't cause an out-of-bounds read, because
 * __cvec_push_val always has exactly v's element size. Such an assert would
 * also be wrong, since it refuses a legal and safe conversion like an int
 * literal going into a cvec_construct(v, long), so this macro doesn't have
 * one.
 *
 * new_elem initializes __cvec_push_val with `=` and never with a brace
 * initializer, so a struct rvalue (the result of a function that returns
 * the element type) is accepted. A brace initializer would use that value
 * to initialize the struct's first MEMBER, and the code wouldn't compile. */
#define cvec_push(v, new_elem)                                        \
  _ccol_cvec_push_impl(v, (new_elem),                                 \
                       _ccol_uniq(__ccol_cvec_push_val, __COUNTER__), \
                       _ccol_uniq(__ccol_cvec_push_arg, __COUNTER__), \
                       _ccol_uniq(__ccol_cvec_push_r, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro can nest inside the argument of any public
 * macro, itself included. */
#define _ccol_cvec_push_impl(v, new_elem, __cvec_push_val, __cvec_push_arg, \
                             __cvec_push_r)                                 \
  do {                                                                      \
    _cvec_elem_load(v, new_elem, __cvec_push_val, __cvec_push_arg);         \
    _ccol_clear_padding(&__cvec_push_val);                                  \
    ccol_retval_t __cvec_push_r =                                           \
        cvector_push_back((v), (const void *)&__cvec_push_val);             \
    if (__cvec_push_r != ccol_success) {                                    \
      _ccol_fatal_err("cvec_push('%s'): r: %d (%s)", #v, __cvec_push_r,     \
                      ccol_retval_to_str(__cvec_push_r));                   \
    }                                                                       \
  } while (0)

/**
 * @brief Pop an element from the vector (type-inferred)
 *
 * The type-inferred wrapper for cvector_pop_back(), which gives the popped
 * element as a value. It calls _ccol_fatal_err() on failure.
 *
 * @param v Vector to pop from
 *
 * @return The value of the popped element
 *
 * @note It stops the program on failure
 * @note It gives a value, not a pointer
 * @note It uses the GNU C statement expression extension
 * @note A vector whose element type is an array, such as char[32], is
 *       refused at compile time because C can't give an array as a value;
 *       pop such an element into a buffer with cvector_pop_back()
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
 * names '_tmp' and 'r' aren't safe. v is expanded inside the later use of
 * _tmp as the operand of an address-of operator, which goes wrong when a
 * caller names a vector variable '_tmp', for example. The declaration and the
 * initializer of r have the same hazard, and the comment above cvec_push
 * explains it in full. A name that belongs to this one macro alone is very
 * unlikely to collide with a real identifier of a caller. */
#define cvec_pop(v)                                                    \
  _ccol_cvec_pop_impl(v, _ccol_uniq(__ccol_cvec_pop_tmp, __COUNTER__), \
                      _ccol_uniq(__ccol_cvec_pop_r, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro can nest inside the argument of any public
 * macro, itself included. */
#define _ccol_cvec_pop_impl(v, __cvec_pop_tmp, __cvec_pop_r)                \
  ({                                                                        \
    _Static_assert(!_cvec_elem_is_array(v),                                 \
                   "cvec_pop cannot give an array element as a value; "     \
                   "pop it into a buffer with cvector_pop_back()");         \
    _cvec_unqual_elem_type(v) __cvec_pop_tmp;                               \
    ccol_retval_t __cvec_pop_r = cvector_pop_back((v), &__cvec_pop_tmp);    \
    if (__cvec_pop_r != ccol_success) {                                     \
      _ccol_fatal_err(                                                      \
          "cvec_pop('%s'): r: %d (%s)%s", #v, __cvec_pop_r,                 \
          ccol_retval_to_str(__cvec_pop_r),                                 \
          __cvec_pop_r == ccol_container_empty ? "; vector is empty" : ""); \
    }                                                                       \
    __cvec_pop_tmp;                                                         \
  })

/**
 * @brief Stops the program with _ccol_fatal_err() when ptr is NULL. cvec_at
 * uses this helper.
 *
 * An internal helper; don't call it directly. It is a plain function so
 * that the code formatting the diagnostic lives in one place. cvec_at puts
 * it inside a statement expression that evaluates index exactly once. That
 * statement expression works both as an rvalue and as an assignment target,
 * and it doesn't trip -Wunused-value when the result is thrown away, since
 * the outer expression is a plain pointer dereference.
 */
static inline __attribute__((always_inline)) void *_cvec_at_checked(
    void *ptr, cvec v, const char *vec_name, size_t index) {
  if (__builtin_expect(!ptr, 0)) {
    /* We read the element count only here, on the failure path, so a lookup
     * that succeeds costs one call to cvector_at() and nothing else. */
    _ccol_fatal_err("cvec_at('%s'): index %lu out of bounds (size: %lu)",
                    vec_name, (unsigned long)index,
                    (unsigned long)cvector_elem_count(v));
  }
  return ptr;
}

/**
 * @brief Access element at index (type-inferred, returns reference)
 *
 * The type-inferred wrapper for cvector_at() that gives the element itself
 * rather than a pointer, cast to the right type for you. It stops the
 * program with _ccol_fatal_err() when the index is out of bounds, which is
 * the same "aborting convenience API" contract that every other
 * type-inferred macro has. If you'd rather get NULL than an abort for an
 * out-of-bounds index, use cvec_at_ptr.
 *
 * @param v Vector to access
 * @param index Zero-based index of the element. It is evaluated exactly
 * once, so an expression with a side effect like cvec_at(vec, i++) is safe
 * to pass.
 *
 * @return Element reference at the index
 *
 * @note It gives a reference, not a pointer
 * @note It stops the program with _ccol_fatal_err() when the index is out
 * of bounds
 *
 * @see cvector_at
 * @see cvec_at_ptr
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push(vec, 42);
 * int val = cvec_at(vec, 0);  // val == 42
 * cvec_at(vec, 0) = 5;        // the first element is 5 from here on
 * @endcode
 */
/* index goes into the local __cvec_at_index exactly once, before it is
 * handed to cvector_at() and to the re-read that _cvec_at_checked() does
 * only for the diagnostic. Writing (index) directly in both places
 * evaluates the expression twice, as two arguments of one
 * _cvec_at_checked() call with no sequence point between them, which is
 * undefined behavior for an index expression with a side effect: a bare
 * `i++` ends up with an indeterminate value. In practice both increments
 * happen, so every loop iteration moves the index variable by 2 instead of
 * 1. `cvec_at(vec, j++)` in an ordinary loop then moves j by 2 per call,
 * walks off the end of the vector and hits the out-of-bounds
 * _ccol_fatal_err() below, while the call site looks perfectly fine. GCC's
 * -Wsequence-point warns about that expansion. __cvec_at_index follows the
 * same one-macro double-underscore naming this header uses elsewhere (for
 * example __cvec_push_val), so no real identifier of a caller can collide
 * with it. The statement expression still works as an lvalue, because
 * `*ptr_expr` is an lvalue even when a statement expression computes
 * ptr_expr, and that's why `cvec_at(v, i) = x;` is a valid assignment
 * target. */
#define cvec_at(v, index) \
  _ccol_cvec_at_impl(v, (index), _ccol_uniq(__ccol_cvec_at_index, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro can nest inside the argument of any public
 * macro, itself included. */
#define _ccol_cvec_at_impl(v, index, __cvec_at_index)           \
  (*(__typeof__(*v##__ccol_val_type_var) *)({                   \
    size_t __cvec_at_index = (index);                           \
    _cvec_at_checked(cvector_at((v), __cvec_at_index), (v), #v, \
                     __cvec_at_index);                          \
  }))

/**
 * @brief Access element at index (type-inferred, returns pointer or NULL)
 *
 * The type-inferred wrapper for cvector_at() that gives a pointer to the
 * element, or NULL when the index is out of bounds. Unlike cvec_at it
 * doesn't stop the program for such an index.
 *
 * @param v Vector to access
 * @param index Zero-based index of the element
 *
 * @return Pointer to the element at the index, or NULL when the index is
 * out of bounds
 *
 * @note It gives NULL for an out-of-bounds index instead of stopping the
 * program.
 * @note The pointer is valid only until the size of the vector changes
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
 * The type-inferred wrapper for cvector_elem_count().
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
 * The type-inferred wrapper for cvector_reset().
 *
 * @param v Vector to reset
 *
 * @see cvector_reset
 */
#define cvec_reset(v) cvector_reset((v))

/**
 * @brief Reserve capacity (a type-inferred wrapper with error handling)
 *
 * The type-inferred wrapper for cvector_reserve() that calls
 * _ccol_fatal_err() on failure.
 *
 * @param v Vector to reserve capacity for
 * @param new_capacity_count Minimum number of elements to reserve. It is
 * evaluated exactly once, on the failure path too, so an expression with a
 * side effect is safe to pass.
 *
 * @note It stops the program when the reservation fails
 *
 * @see cvector_reserve
 */
/* new_capacity_count goes into __cvec_reserve_count exactly once, before
 * both of its uses (the real cvector_reserve() call and the diagnostic
 * message). The comment next to cvec_push's internal locals above has the
 * full reason: a macro parameter that textual substitution puts in more
 * than one place of the expansion is evaluated once for every place it
 * shows up. Without the local, a new_capacity_count with a side effect
 * (say a function call that reads from a queue, or a value that isn't
 * deterministic) would be evaluated a second time just to build the
 * _ccol_fatal_err() message on the failure path. The message would then
 * report a different value from the one cvector_reserve() got, and the
 * second evaluation would repeat any real side effect right before the
 * process aborts. */
#define cvec_reserve(v, new_capacity_count)        \
  _ccol_cvec_reserve_impl(v, (new_capacity_count), \
                          _ccol_uniq(__ccol_cvec_reserve_count, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro can nest inside the argument of any public
 * macro, itself included. */
#define _ccol_cvec_reserve_impl(v, new_capacity_count, __cvec_reserve_count) \
  do {                                                                       \
    size_t __cvec_reserve_count = (new_capacity_count);                      \
    if (!cvector_reserve((v), __cvec_reserve_count)) {                       \
      _ccol_fatal_err(                                                       \
          "cvec_reserve('%s'): failed to reserve %lu elements; out of "      \
          "memory?",                                                         \
          #v, (unsigned long)__cvec_reserve_count);                          \
    }                                                                        \
  } while (0)

/**
 * @brief Append an array of elements (a type-inferred wrapper with error
 * handling)
 *
 * The type-inferred wrapper for cvector_append_array() that calls
 * _ccol_fatal_err() on failure.
 *
 * @param v Vector to append to
 * @param arr_ptr Pointer to the array of elements. It can point at a const
 * array.
 * @param elem_count Number of elements in the array. It is evaluated
 * exactly once, on the failure path too, so an expression with a side
 * effect is safe to pass.
 *
 * @note It stops the program when the append fails. One such failure is an
 * arr_ptr that aliases v's backing buffer and whose elem_count reads past
 * the end of that buffer's reservation; see cvector_append_array.
 *
 * @see cvector_append_array
 */
/* elem_count goes into __cvec_append_array_count exactly once, before both
 * uses, for the same reason as the matching local of cvec_reserve above:
 * one macro parameter in both the real call and the _ccol_fatal_err()
 * diagnostic gets evaluated twice on the failure path. */
#define cvec_append_array(v, arr_ptr, elem_count) \
  _ccol_cvec_append_array_impl(                   \
      v, (arr_ptr), (elem_count),                 \
      _ccol_uniq(__ccol_cvec_append_array_count, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro can nest inside the argument of any public
 * macro, itself included. */
#define _ccol_cvec_append_array_impl(v, arr_ptr, elem_count,                \
                                     __cvec_append_array_count)             \
  do {                                                                      \
    size_t __cvec_append_array_count = (elem_count);                        \
    if (!cvector_append_array((v), (arr_ptr), __cvec_append_array_count)) { \
      _ccol_fatal_err(                                                      \
          "cvec_append_array('%s'): failed to append %lu elements; out of " \
          "memory?",                                                        \
          #v, (unsigned long)__cvec_append_array_count);                    \
    }                                                                       \
  } while (0)

/**
 * @brief Append a vector to a vector (a type-inferred wrapper with error
 * handling)
 *
 * The type-inferred wrapper for cvector_append_cvector() that calls
 * _ccol_fatal_err() on failure.
 *
 * @param v_to Destination vector
 * @param v_from Source vector
 *
 * @note It stops the program when the append fails
 *
 * @see cvector_append_cvector
 */
#define cvec_append_cvec(v_to, v_from)                                         \
  do {                                                                         \
    if (!cvector_append_cvector((v_to), (v_from))) {                           \
      _ccol_fatal_err("cvec_append_cvec('%s' <- '%s'): out of memory?", #v_to, \
                      #v_from);                                                \
    }                                                                          \
  } while (0)

/**
 * @brief Get the data pointer (a type-inferred wrapper)
 *
 * The type-inferred wrapper for cvector_data_ptr().
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
 * An internal helper. Don't call it directly, use one of the two macros. It
 * sorts v's elements in place with a stable mergesort that addresses each
 * element straight from the backing array, and allocates its temporary
 * buffer with v's allocator.
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
 * Sorts the vector in place through the csort library with a custom
 * comparison function, which must follow the usual comparator convention
 * and give a value less than 0, equal to 0 or more than 0.
 *
 * @param v Vector to sort. It is evaluated exactly once.
 * @param comparison_proc Comparison function for the sort. It is evaluated
 * exactly once.
 *
 * @note It sorts in place with a stable mergesort algorithm
 * @note O(n log n) time complexity, O(n) space complexity
 * @note It asserts when v is NULL
 * @note The sort itself is csort's
 * @note It calls _ccol_fatal_err() when comparison_proc is NULL. The
 * comparison procedure comes from an expression of the caller whose value
 * can't in general be known before the program runs, so this check stays a
 * run-time check. cvec_sort's element-type check is a compile-time
 * _Static_assert instead, so a cvec_sort call never gets to this
 * diagnostic.
 * @note It calls _ccol_fatal_err() when the sort's internal temporary
 * buffer can't be allocated, like every other type-inferred macro in this
 * header that changes a vector (cvec_push, cvec_reserve, cvec_append_array
 * and the rest). The vector is then left completely unsorted and unchanged,
 * so you never get back an unsorted or half-sorted vector as if nothing
 * went wrong.
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
#define cvector_sort_with_comparison_proc(v, comparison_proc)                 \
  do {                                                                        \
    /* v is evaluated exactly once, into this local, before comparison_proc   \
     * is, and both the NULL check and the sort read the local. The type is   \
     * spelled with __typeof__ because a caller can name its own variable     \
     * cvec and hide the typedef that way. */                                 \
    __typeof__(v) __cvec_sort_v = (v);                                        \
    if (!__cvec_sort_v) {                                                     \
      _ccol_fatal_err(                                                        \
          "cvector_sort_with_comparison_proc('%s'): vector is NULL", #v);     \
    }                                                                         \
    /* comparison_proc goes into a plain pointer variable instead of being    \
     * tested with !(comparison_proc) directly, because a caller can pass a   \
     * comparator function by its bare name rather than a variable that       \
     * holds one (cvec_sort uses this macro with such a variable). For a bare \
     * name the pointer variable avoids -Werror=address ("the address of 'X'  \
     * will always evaluate as 'true'"): GCC and Clang can prove that the     \
     * address of a named function is never NULL, but they can't prove it     \
     * for a pointer variable, not even one initialized from that same        \
     * function. The variable also makes comparison_proc evaluated exactly    \
     * once, which is the convention of this header: a macro evaluates each   \
     * of its arguments once. */                                              \
    ccol_comparison_proc_t __cvec_sort_cmp = (comparison_proc);               \
    if (!__cvec_sort_cmp) {                                                   \
      _ccol_fatal_err(                                                        \
          "cvector_sort_with_comparison_proc('%s'): comparison_proc is "      \
          "NULL; pass a real comparison procedure",                           \
          #v);                                                                \
    }                                                                         \
    if (!_cvector_sort(__cvec_sort_v, __cvec_sort_cmp)) {                     \
      _ccol_fatal_err(                                                        \
          "cvector_sort_with_comparison_proc('%s'): out of memory; vector "   \
          "left unsorted",                                                    \
          #v);                                                                \
    }                                                                         \
  } while (0)

/**
 * @brief Sort a vector with the default comparison for its type
 *
 * Sorts the vector in place with the default comparison function that the
 * csort library has for the element type; the type decides which
 * comparator that is.
 *
 * An element type with no default comparison procedure is a compile-time
 * error rather than a run-time one. The macro carries a _Static_assert that
 * names the vector and points at cvector_sort_with_comparison_proc, so such
 * a call never builds and can never abort a running program. Every standard
 * integer type and floating type sorts. Every enumeration sorts too, since
 * an enumerated type is compatible with one of the standard integer types
 * and the comparator of that compatible type gets picked. A char *, a
 * const char *, a signed char * and an unsigned char * all sort as
 * NUL-terminated strings in strcmp() order; uint8_t * is an unsigned char *
 * and sorts the same way. Sort a vector of pointers to binary buffers with
 * cvector_sort_with_comparison_proc. A bool sorts false before true. A
 * struct, a union, a pointer to anything other than char and a fixed-size
 * char array have no default comparison procedure, so those types are
 * rejected; sort such a vector with cvector_sort_with_comparison_proc and an
 * explicit comparison procedure.
 *
 * @param v Vector to sort
 *
 * @note It sorts in place with a stable mergesort algorithm
 * @note O(n log n) time complexity, O(n) space complexity
 * @note It uses the default comparison for the element type
 * @note In a char * vector a NULL element comes before every non-NULL
 * string and is equal only to another NULL, so a vector with NULL elements
 * sorts without any special handling from the caller.
 * @note The code doesn't compile when the element type has no default
 * comparison procedure, and the diagnostic names the vector and the
 * supported alternative.
 * @note It calls _ccol_fatal_err() when an allocation fails, like every
 * other type-inferred macro in this header that changes a vector. See
 * cvector_sort_with_comparison_proc.
 *
 * @see cvector_sort_with_comparison_proc
 * @see cvec_find. It behaves differently on purpose for a type that has no
 * default comparison procedure: cvec_find falls back to byte-wise equality
 * instead of refusing to compile.
 *
 * Example:
 * @code
 * cvec_construct(vec, int);
 * cvec_push(vec, 3);
 * cvec_push(vec, 1);
 * cvec_push(vec, 2);
 * cvec_sort(vec);  // vec is [1, 2, 3] after this
 * @endcode
 */
/* The _Static_assert turns "this element type has no default comparator"
 * into a diagnostic at the call site, where without it this would be a
 * run-time abort. Whether a type has a default comparator depends on the
 * type alone, so the answer is known at compile time. The association lists
 * of ___csort_has_default_comparison_proc() are an exact copy of the lists
 * of csort_get_default_comparison_proc(), so once the assertion holds, the
 * selector below gives a real comparison procedure and the NULL check
 * inside cvector_sort_with_comparison_proc can never fire for a cvec_sort()
 * caller. The predicate gets the same *(v##__ccol_val_type_var) lvalue as
 * the selector, so both answer the question for one identical type. */
#define cvec_sort(v)                                                         \
  do {                                                                       \
    _Static_assert(                                                          \
        ___csort_has_default_comparison_proc(*(v##__ccol_val_type_var)),     \
        "cvec_sort(" #v                                                      \
        "): this vector's element type has no default comparison procedure " \
        "(a struct, a union, a pointer to anything other than char and a "   \
        "fixed-size char array all have none). Sort this vector with "       \
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
 * The type-inferred wrapper for cvector_find(), which takes a value,
 * including an rvalue or a literal. The needle is converted to the vector's
 * declared element type and the elements are compared with that type's
 * default comparison procedure, so equality is the type's equality and not
 * the equality of its object representation. For example -0.0 and 0.0 are
 * one value, a long double compares by value and not through the padding
 * bytes its representation has on some targets, and a char * element
 * matches on the string content, not on the pointer identity.
 *
 * We read a char *, a signed char * or an unsigned char * element as a
 * NUL-terminated string (uint8_t * is an unsigned char *), which the
 * comparison reads with strcmp. Don't search a vector of pointers to binary
 * buffers with this macro, because the comparison reads past the end of a
 * buffer that has no NUL byte. For such a vector, do one of these:
 * - Use cvector_find() with a comparator that compares the pointers.
 * - Declare the element type as void *, whose byte-wise equality is the
 *   identity of the pointer.
 *
 * For an array element type such as char[64], elem is an array or a string
 * literal: the macro's copy is filled with zeros up to the element size and
 * then gets the bytes of elem, up to the smaller of the two sizes, just as
 * cvec_push stores such a value. For an array element type a pointer as
 * elem is a compile error, because the macro can't see the size of the
 * object it points to.
 *
 * A char * element can be NULL, and so can the needle. The default string
 * comparison procedure puts NULL before every non-NULL string and makes
 * NULL equal only to another NULL, so searching for NULL in a sparse
 * char * vector gives the first empty slot. Nothing is dereferenced.
 *
 * A struct, a union, a pointer to a type other than char and a fixed-size
 * char array have no default comparison procedure. For those the comparison
 * is byte-wise equality over the element's full representation, padding
 * bytes of a struct included. With GCC 11 or later the macro zeroes the
 * padding bytes of its own copy of elem, and cvec_push does the same for
 * each element it stores, so a struct that went in through cvec_push is
 * found with any value whose members are equal, whatever the padding bytes
 * hold.
 *
 * An element that went in through cvector_push_back(),
 * cvector_append_array() or an assignment through cvec_at keeps whatever
 * padding bytes the caller wrote. With a compiler that has no
 * __builtin_clear_padding, such as Clang, the padding bytes of the macro's
 * copies are unspecified, and you should search for a padded struct with
 * cvector_find() and a comparator that reads the members.
 *
 * cvec_find compiles for every element type and cvec_sort is different on
 * purpose: an order can't come from the bytes of an element the way
 * equality can, so cvec_sort has no fallback and refuses such an element
 * type at compile time.
 *
 * Use cvector_find() directly when you need a custom comparator, or when
 * you want byte-wise equality for a type that has a default comparison
 * procedure.
 *
 * @param v    Vector to search
 * @param elem Element value to search for
 *
 * @return Zero-based index of the first match, or ccol_invalid_size when
 * nothing matches.
 *
 * @note O(n) complexity
 * @note It compares with the element type's default comparison procedure
 * @note A NULL char * element, and a NULL needle for a char * vector, are
 * ordered without dereferencing either of them.
 * @note It falls back to byte-wise equality for a type that has no default
 * comparison procedure. A struct with padding bytes compares right only
 * when both copies carry zero padding, as described above.
 * @note It compiles for every element type; cvec_sort doesn't.
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
 * _ccol_uniq(), so the macro can nest inside the argument of any public
 * macro, itself included. */
#define _ccol_cvec_find_impl(v, elem, __cvec_find_needle, __cvec_find_arg) \
  ({                                                                       \
    _cvec_elem_load(v, elem, __cvec_find_needle, __cvec_find_arg);         \
    _ccol_clear_padding(&__cvec_find_needle);                              \
    cvector_find(                                                          \
        (v), (const void *)&__cvec_find_needle,                            \
        csort_get_default_comparison_proc(*(v##__ccol_val_type_var)));     \
  })

#pragma GCC visibility pop
