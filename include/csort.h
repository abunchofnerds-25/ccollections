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

#include "common.h"

/* Everything this header declares from here to the end belongs to the public
 * Application Binary Interface (ABI) of libccollections, and the shared
 * library exports all of it. The library is built with -fvisibility=hidden,
 * so a function or an object outside these blocks stays internal: it isn't
 * in the dynamic symbol table, an application linking against the library
 * can't interpose it, and a symbol of the same name in that application
 * can't collide with it. */
#pragma GCC visibility push(default)

/**
 * @file csort.h
 * @brief Generic sort library with an iterative mergesort
 *
 * A type-generic sort with:
 * - An iterative mergesort (stable, no recursion)
 * - O(n log n) worst-case time
 * - O(n) space for the temporary buffer
 * - Default comparison functions for all the standard C types
 * - Support for a custom comparison function
 * - Support for custom memory management
 * - Type-inferred selection of the comparison function with _Generic
 *
 * Everything goes through function pointers, so it can sort any kind of
 * collection (an array, a vector or a custom container) as long as the
 * caller gives a getter function and a comparison function.
 *
 * The mergesort is iterative (bottom-up) and not recursive, so a large data
 * set can't overflow the stack.
 */

/**
 * @brief Function pointer type that gets an element from a collection
 *
 * The getter hides how a collection stores its elements, so the sort can
 * work with any kind of collection.
 *
 * @param collection Pointer to the collection being sorted
 * @param index Zero-based index of the element to get
 *
 * @return Pointer to the element at the given index
 *
 * @note The pointer must stay valid for the whole sort
 * @note For a C array: return &array[index]
 * @note For a vector: return cvector_at(vec, index)
 *
 * Example implementations:
 * @code
 * // For a plain C array of ints
 * void *int_array_getter(void *collection, size_t index) {
 *     return &((int*)collection)[index];
 * }
 *
 * // For a vector (cvector.h already has this one)
 * void *vector_getter(void *collection, size_t index) {
 *     return cvector_at((cvec)collection, index);
 * }
 * @endcode
 */
typedef void *(*csort_item_getter_proc_t)(void *collection, size_t index);

/* ========================================================================== */
/*                         INTERNAL DECLARATIONS                              */
/* ========================================================================== */

/**
 * @brief Default comparison function for C strings. Do not call it
 *        directly.
 *
 * The leading underscore marks it as internal. Get to it through
 * csort_get_default_comparison_proc(), which picks it from the type of its
 * argument. Its name and signature may change in any release.
 *
 * Compares two stored string pointers with strcmp(). Both arguments must be
 * pointers to char pointers (char**).
 *
 * A stored pointer can be NULL. NULL sorts before every non-NULL string, the
 * empty string included, and is equal only to another NULL. That keeps the
 * result a real total order over the whole char* type, so a collection with
 * NULL elements sorts and searches without any special handling by the
 * caller, and a NULL needle is answered without being dereferenced. Without
 * this rule a NULL element would reach strcmp() as a null pointer.
 *
 * @param first Pointer to the first char*, that is a char**. It must not be
 * NULL itself, because it is the address of the stored pointer.
 * @param second Pointer to the second char*, that is a char**, with the
 * same requirement.
 *
 * @return A negative value when *first < *second; zero when the two are
 * equal, including when both are NULL; a positive value when
 * *first > *second.
 *
 * @note For two non-NULL strings it behaves like strcmp()
 * @note A non-NULL string must be null-terminated
 * @note NULL is the least value and is never dereferenced.
 *
 * @see strcmp
 */
int _csort_default_string_comparison_proc(const void *first,
                                          const void *second);

/**
 * @brief The internal mergesort. Do not call it directly.
 *
 * An iterative (bottom-up) mergesort without recursion, so the stack can't
 * overflow. It uses a temporary buffer that it allocates with the memory
 * management procedures the caller gives.
 *
 * Properties of the algorithm:
 * - Stable: equal elements keep their relative order.
 * - O(n log n) time in every case: worst, average and best
 * - O(n) space for the temporary merge buffer
 * - Iterative, so there is no recursion and no stack depth to worry about
 *
 * @param col Pointer to the collection to sort
 * @param length Number of elements in the collection
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function that gets the element at an index (required).
 * @param comparison_proc Function that compares two elements (required).
 * @param mprocs Memory management procedures used to allocate the buffer.
 * NULL selects the default procedures.
 *
 * @return true when the whole collection is sorted, and also for the
 * trivial cases that have nothing to do: a col of NULL, a length of 0 or 1,
 * and an elem_size of 0. false when length is more than
 * ccol_max_elem_count, when mprocs is not NULL and doesn't hold all four
 * function pointers, when the temporary merge buffer can't be allocated,
 * and when length * elem_size overflows size_t. In each of these cases the
 * collection is left completely unchanged, because no merge pass has
 * started.
 *
 * @note Use the csort_sort() macro instead of calling this directly.
 * @note It asserts when getter_proc or comparison_proc is NULL, except when
 * col is NULL or length is 0 or 1: then the trivial-success path gives true
 * without looking at either procedure.
 * @note A length of 0 or 1 gives true at once
 * @note An elem_size of 0 gives true immediately, without calling
 * getter_proc or comparison_proc and without allocating a temporary buffer,
 * since a zero-size element has no bytes for those procedures to read and
 * none for a merge pass to move.
 * @note A length above ccol_max_elem_count (2^63 on a 64-bit size_t) is
 * rejected. Every other container in this library, cvector and chashmap for
 * example, has the same cap, and it is there so the internal count of
 * bottom-up merge passes can never overflow.
 * @note A non-NULL mprocs must hold malloc, free, calloc and realloc, the
 * same contract that ccol_memmgmt_procs_t itself documents. An incomplete
 * mprocs is rejected with false right before the temporary buffer is
 * allocated, never in the middle of a sort.
 * @note The temporary buffer is (length * elem_size) bytes, and that
 * multiplication is checked for overflow first.
 *
 * @see csort_sort
 */
bool ___csort_merge_sort(void *col, size_t length, size_t elem_size,
                         csort_item_getter_proc_t getter_proc,
                         ccol_comparison_proc_t comparison_proc,
                         ccol_memmgmt_procs_t *mprocs);

/* ========================================================================== */
/*                         PUBLIC SORT INTERFACE                              */
/* ========================================================================== */

/**
 * @brief Sort a collection with mergesort
 *
 * Sorts any kind of collection in place with an iterative mergesort. The
 * collection can be a C array, a vector or any custom container, as long as
 * the caller gives a suitable getter function and a suitable comparison
 * function.
 *
 * @param col Pointer to the collection to sort
 * @param length Number of elements in the collection
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function that gets the element at an index
 * @param comparison_proc Function that compares two elements
 * @param mprocs Memory management procedures. NULL selects the default
 * malloc and free; a non-NULL mprocs must hold all four of malloc, free,
 * calloc and realloc.
 *
 * @return true on success. A length of zero or one and an elem_size of 0
 * count as success too, since there's nothing to do. false when length is
 * more than ccol_max_elem_count, when mprocs is not NULL and is incomplete,
 * when the temporary merge buffer can't be allocated, and when
 * length * elem_size overflows size_t. In each of these cases the
 * collection is left completely unchanged.
 *
 * @note It's a wrapper around ___csort_merge_sort
 * @note getter_proc and comparison_proc must both be non-NULL for a
 * non-NULL col with a length of 2 or more, even when elem_size is 0 and
 * neither of them gets called. Only a col of NULL, or a length of 0 or 1,
 * lets a call pass no real procedures. Breaking this rule asserts and aborts
 * the process instead of giving false.
 * @note The sort is stable: equal elements keep their order.
 * @note Time complexity: O(n log n) in every case
 * @note Space complexity: O(n) for the temporary merge buffer
 *
 * Example usage:
 * @code
 * // Sort a plain C array of integers
 * int array[] = {5, 2, 8, 1, 9};
 * csort_sort(array, 5, sizeof(int),
 *            int_array_getter,
 *            csort_get_default_comparison_proc(array[0]),
 *            NULL);
 *
 * // Sort a cvec directly with csort_sort(). cvec_sort() and
 * // cvector_sort_with_comparison_proc() already do this for you, so use
 * // them unless you need this raw interface.
 * // getter_proc must have exactly the csort_item_getter_proc_t signature,
 * // void *(*)(void *, size_t). cvector_at() takes a cvec as its first
 * // parameter, not a void *, so you can't cast it to
 * // csort_item_getter_proc_t and pass it as is. Calling through such a
 * // cast pointer is undefined behavior (C11 6.3.2.3p8), even though cvec
 * // and void * have the same representation on every mainstream ABI. A
 * // small adapter with the exact signature avoids the problem.
 * void *my_vec_getter(void *collection, size_t index) {
 *   return cvector_at((cvec)collection, index);
 * }
 *
 * cvec my_vec;
 * // ... fill the vector ...
 * csort_sort(my_vec, cvector_elem_count(my_vec), sizeof(int),
 *            my_vec_getter,
 *            csort_get_default_comparison_proc(0),
 *            cvector_get_mprocs(my_vec));
 * @endcode
 *
 * @see csort_get_default_comparison_proc
 * @see cvec_sort (a convenience wrapper for a vector)
 */
#define csort_sort(col, length, elem_size, getter_proc, comparison_proc, \
                   mprocs)                                               \
  (___csort_merge_sort((col), (length), (elem_size), (getter_proc),      \
                       (comparison_proc), (mprocs)))

/* ========================================================================== */
/*                   COMPARISON PROCEDURE DECLARATIONS                        */
/* ========================================================================== */

/**
 * @brief Internal macro that builds the name of a comparison procedure
 *
 * Makes the full function name of the comparison procedure for one type.
 *
 * @param name Type suffix for the comparison function
 *
 * @return The function name: _csort_default_<name>_comparison_proc
 */
#define ___csort__get_default_integral_comparison_proc_name(name) \
  _csort_default_##name##_comparison_proc

/**
 * @brief Internal macro that declares a default comparison procedure
 *
 * Declares a function that compares two elements of the given type and
 * follows the standard comparator convention.
 *
 * @param type C type, for example int, float or char
 * @param name Type suffix for the function name, for example int, float or
 * char
 */
#define ___csort__declare_default_integral_comparison_proc(type, name) \
  int ___csort__get_default_integral_comparison_proc_name(name)(       \
      const void *first, const void *second)

/* Default comparison procedures for all the standard types */

/** @brief Compare two char values */
___csort__declare_default_integral_comparison_proc(char, char);

/** @brief Compare two signed char values */
___csort__declare_default_integral_comparison_proc(signed char, signed_char);

/** @brief Compare two short values */
___csort__declare_default_integral_comparison_proc(short, short);

/** @brief Compare two int values */
___csort__declare_default_integral_comparison_proc(int, int);

/** @brief Compare two long values */
___csort__declare_default_integral_comparison_proc(long, long);

/** @brief Compare two long long values */
___csort__declare_default_integral_comparison_proc(long long, long_long);

/** @brief Compare two unsigned char values */
___csort__declare_default_integral_comparison_proc(unsigned char,
                                                   unsigned_char);

/** @brief Compare two unsigned short values */
___csort__declare_default_integral_comparison_proc(unsigned short,
                                                   unsigned_short);

/** @brief Compare two unsigned int values */
___csort__declare_default_integral_comparison_proc(unsigned int, unsigned_int);

/** @brief Compare two unsigned long values */
___csort__declare_default_integral_comparison_proc(unsigned long,
                                                   unsigned_long);

/** @brief Compare two unsigned long long values */
___csort__declare_default_integral_comparison_proc(unsigned long long,
                                                   unsigned_long_long);

/** @brief Compare two bool values: false comes before true */
int _csort_default_bool_comparison_proc(const void *first, const void *second);

/**
 * @brief Compare two float values
 * @note NaN is greater than every non-NaN value and equal only to another
 * NaN, so the result is always a real total order, even when one or both
 * operands are NaN. The note on _csort_default_double_comparison_proc
 * explains why a merge-sort comparator needs that.
 */
___csort__declare_default_integral_comparison_proc(float, float);

/**
 * @brief Compare two double values
 * @note NaN is greater than every non-NaN value and equal only to another
 * NaN. IEEE 754's native `<` and `>` are both false when one operand is NaN,
 * so a naive comparator reports NaN as "equal" to everything, even to two
 * unrelated non-NaN values that just sit on either side of the NaN in the
 * collection. csort_merge decides between the left and the right element
 * from comparison_proc and needs a real strict weak ordering for that, so a
 * false "equal" corrupts the relative order of the elements around the NaN
 * and not only the NaN's own position. The cbstmap key comparator for
 * float, double and long double works the same way.
 */
___csort__declare_default_integral_comparison_proc(double, double);

/**
 * @brief Compare two long double values
 * @note NaN is greater than every non-NaN value and equal only to another
 * NaN, for the same reason as in _csort_default_double_comparison_proc.
 */
___csort__declare_default_integral_comparison_proc(long double, long_double);

#undef ___csort__declare_default_integral_comparison_proc

/* ========================================================================== */
/*                   TYPE-GENERIC COMPARISON SELECTION                        */
/* ========================================================================== */

/**
 * @brief Get the default comparison function for a type
 *
 * Uses C11 _Generic to pick the right comparison function from the type of
 * the variable you pass. All the standard integral types, all the
 * floating-point types and C strings are supported.
 *
 * The type of the expression is looked at during compilation and the
 * matching comparison function pointer comes back. The comparison functions
 * follow the standard C comparator convention and give a value less than 0,
 * equal to 0 or greater than 0.
 *
 * @param x Variable or expression whose type decides the comparison
 * function.
 *
 * @return Function pointer to the right comparison function, or NULL for a
 * type that isn't supported.
 *
 * @note Supported signed types: char, signed char, short, int, long, long
 *       long
 * @note Supported unsigned types: unsigned char, unsigned short, unsigned
 *       int, unsigned long, unsigned long long
 * @note Supported floating types: float, double, long double
 * @note bool (_Bool) is supported too: false comes before true
 * @note Supported string types: char* and const char*. They must be real
 * pointer variables or pointer expressions. The string comparator you get
 * puts a stored NULL before every non-NULL string, and that NULL is equal
 * only to another NULL.
 * @note An unsupported type, like a fixed-size char array (char[N]), gives
 * NULL. The comment below explains why that type needs an explicit check:
 * _ccol_is_char_ptr() alone doesn't exclude it.
 * @note Every const and non-const variant is supported
 * @note It relies on the _ccol_is_integral_type(), _ccol_is_char_ptr() and
 * _ccol_is_char_array() macros of common.h
 *
 * Example usage:
 * @code
 * // Get the comparator for an int
 * int dummy_int;
 * ccol_comparison_proc_t cmp = csort_get_default_comparison_proc(dummy_int);
 * // cmp now points to _csort_default_int_comparison_proc
 *
 * // Get the comparator for a string
 * char* dummy_str;
 * cmp = csort_get_default_comparison_proc(dummy_str);
 * // cmp now points to _csort_default_string_comparison_proc
 *
 * // Get the comparator for a double
 * double dummy_double;
 * cmp = csort_get_default_comparison_proc(dummy_double);
 * // cmp now points to _csort_default_double_comparison_proc
 * @endcode
 *
 * @see csort_sort
 * @see cvec_sort (it uses this macro)
 */
/* The local that holds the result below is called __csort_gdcp_result on
 * purpose. A name that a caller could also give its own argument variable,
 * like "comparison_proc", isn't safe here because of C's declarator scope
 * rules. If a caller writes csort_get_default_comparison_proc(comparison_proc)
 * with an argument variable of exactly that name, every (x) inside this
 * statement expression resolves to the macro's new local, which is always
 * NULL, and not to the caller's real variable: the local's scope starts
 * right after its own declarator, and that is before (x) is expanded. The
 * same hazard decides the names of cvec_push's internal locals. Giving the
 * internal result a name that only this macro uses is the standard defence
 * against this hazard of non-hygienic macros, and common.h does the same
 * thing for the same reason (ccol_scoped_ptr_release names its internal
 * temporary __ccol_released_ptr).
 *
 * The _ccol_is_char_ptr((x)) branch below also checks
 * _ccol_is_char_array((x)), because _ccol_is_char_ptr((x)) alone isn't
 * enough:
 * - C11 6.5.1.1p2 and 6.3.2.1p3 apply the ordinary array-to-pointer decay
 *   to the controlling expression of a _Generic selection.
 * - So a real fixed-size array x, such as a `char name[64]` field, also
 *   matches the `char *:` and `const char *:` associations of
 *   _ccol_is_char_ptr once it decays.
 * - Without a guard, the macro would take such an array for a string and
 *   give _csort_default_string_comparison_proc for it.
 * - That comparator's contract needs first and second to point to a stored
 *   char* VALUE. It dereferences one pointer level with
 *   *(const char **)first and doesn't expect the array's inline bytes.
 * - csort_merge would then call that comparator on the array's real bytes,
 *   and it would read the first sizeof(char*) bytes as a pointer and
 *   dereference it. That's undefined behavior, not just a wrong sort
 *   order.
 *
 * _ccol_is_char_array() only reads the type of x, so an rvalue x like a cast
 * works as well as an lvalue, and a char pointer with its own qualifier,
 * like `const char *const`, is recognised as a pointer. An array goes to the
 * NULL result for an unsupported type, which is what this macro documents
 * for every other type it doesn't know, so the exclusion adds no new
 * comparator semantics. This module's default comparators are meant for an
 * element that IS a scalar, a char pointer or a numeric value, not for one
 * whose content IS a byte buffer. A caller with fixed-size string fields
 * has a complete alternative in cvector_sort_with_comparison_proc() or
 * csort_sort() with an explicit comparator of its own; the char[N] field
 * examples in doc/csort.md are written that way. */
#if defined(_CCOL_CLANG_GENERIC_ASSOC_WARNING)
#define csort_get_default_comparison_proc(x)                                             \
  ({                                                                                     \
    ccol_comparison_proc_t __csort_gdcp_result = NULL;                                   \
    if (_ccol_is_integral_type((x))) {                                                   \
      _Pragma("GCC diagnostic push");                                                    \
      _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\"");            \
      __csort_gdcp_result = _Generic((x),                                                \
          char: ___csort__get_default_integral_comparison_proc_name(char),               \
          signed char: ___csort__get_default_integral_comparison_proc_name(              \
                                         signed_char),                                   \
          short: ___csort__get_default_integral_comparison_proc_name(short),             \
          int: ___csort__get_default_integral_comparison_proc_name(int),                 \
          long: ___csort__get_default_integral_comparison_proc_name(long),               \
          long long: ___csort__get_default_integral_comparison_proc_name(                \
                                         long_long),                                     \
          unsigned char: ___csort__get_default_integral_comparison_proc_name(            \
                                         unsigned_char),                                 \
          unsigned short: ___csort__get_default_integral_comparison_proc_name(           \
                                         unsigned_short),                                \
          unsigned int: ___csort__get_default_integral_comparison_proc_name(             \
                                         unsigned_int),                                  \
          unsigned long: ___csort__get_default_integral_comparison_proc_name(            \
                                         unsigned_long),                                 \
          unsigned long long: ___csort__get_default_integral_comparison_proc_name(       \
                                         unsigned_long_long),                            \
          float: ___csort__get_default_integral_comparison_proc_name(float),             \
          double: ___csort__get_default_integral_comparison_proc_name(double),           \
          long double: ___csort__get_default_integral_comparison_proc_name(              \
                                         long_double),                                   \
          const char: ___csort__get_default_integral_comparison_proc_name(               \
                                         char),                                          \
          const signed char: ___csort__get_default_integral_comparison_proc_name(        \
                                         signed_char),                                   \
          const short: ___csort__get_default_integral_comparison_proc_name(              \
                                         short),                                         \
          const int: ___csort__get_default_integral_comparison_proc_name(int),           \
          const long: ___csort__get_default_integral_comparison_proc_name(               \
                                         long),                                          \
          const long long: ___csort__get_default_integral_comparison_proc_name(          \
                                         long_long),                                     \
          const unsigned char: ___csort__get_default_integral_comparison_proc_name(      \
                                         unsigned_char),                                 \
          const unsigned short: ___csort__get_default_integral_comparison_proc_name(     \
                                         unsigned_short),                                \
          const unsigned int: ___csort__get_default_integral_comparison_proc_name(       \
                                         unsigned_int),                                  \
          const unsigned long: ___csort__get_default_integral_comparison_proc_name(      \
                                         unsigned_long),                                 \
          const unsigned long long: ___csort__get_default_integral_comparison_proc_name( \
                                         unsigned_long_long),                            \
          const float: ___csort__get_default_integral_comparison_proc_name(              \
                                         float),                                         \
          const double: ___csort__get_default_integral_comparison_proc_name(             \
                                         double),                                        \
          const long double: ___csort__get_default_integral_comparison_proc_name(        \
                                         long_double),                                   \
          default: NULL);                                                                \
      _Pragma("GCC diagnostic pop");                                                     \
    } else if (_ccol_is_char_ptr((x)) && !_ccol_is_char_array((x))) {                    \
      __csort_gdcp_result = _csort_default_string_comparison_proc;                       \
    } else {                                                                             \
      _Pragma("GCC diagnostic push");                                                    \
      _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\"");            \
      if (_Generic((x), _Bool: 1, const _Bool: 1, default: 0))                           \
        __csort_gdcp_result = _csort_default_bool_comparison_proc;                       \
      _Pragma("GCC diagnostic pop");                                                     \
    }                                                                                    \
    __csort_gdcp_result;                                                                 \
  })
#else
#define csort_get_default_comparison_proc(x)                                             \
  ({                                                                                     \
    ccol_comparison_proc_t __csort_gdcp_result = NULL;                                   \
    if (_ccol_is_integral_type((x))) {                                                   \
      __csort_gdcp_result = _Generic((x),                                                \
          char: ___csort__get_default_integral_comparison_proc_name(char),               \
          signed char: ___csort__get_default_integral_comparison_proc_name(              \
                                         signed_char),                                   \
          short: ___csort__get_default_integral_comparison_proc_name(short),             \
          int: ___csort__get_default_integral_comparison_proc_name(int),                 \
          long: ___csort__get_default_integral_comparison_proc_name(long),               \
          long long: ___csort__get_default_integral_comparison_proc_name(                \
                                         long_long),                                     \
          unsigned char: ___csort__get_default_integral_comparison_proc_name(            \
                                         unsigned_char),                                 \
          unsigned short: ___csort__get_default_integral_comparison_proc_name(           \
                                         unsigned_short),                                \
          unsigned int: ___csort__get_default_integral_comparison_proc_name(             \
                                         unsigned_int),                                  \
          unsigned long: ___csort__get_default_integral_comparison_proc_name(            \
                                         unsigned_long),                                 \
          unsigned long long: ___csort__get_default_integral_comparison_proc_name(       \
                                         unsigned_long_long),                            \
          float: ___csort__get_default_integral_comparison_proc_name(float),             \
          double: ___csort__get_default_integral_comparison_proc_name(double),           \
          long double: ___csort__get_default_integral_comparison_proc_name(              \
                                         long_double),                                   \
          const char: ___csort__get_default_integral_comparison_proc_name(               \
                                         char),                                          \
          const signed char: ___csort__get_default_integral_comparison_proc_name(        \
                                         signed_char),                                   \
          const short: ___csort__get_default_integral_comparison_proc_name(              \
                                         short),                                         \
          const int: ___csort__get_default_integral_comparison_proc_name(int),           \
          const long: ___csort__get_default_integral_comparison_proc_name(               \
                                         long),                                          \
          const long long: ___csort__get_default_integral_comparison_proc_name(          \
                                         long_long),                                     \
          const unsigned char: ___csort__get_default_integral_comparison_proc_name(      \
                                         unsigned_char),                                 \
          const unsigned short: ___csort__get_default_integral_comparison_proc_name(     \
                                         unsigned_short),                                \
          const unsigned int: ___csort__get_default_integral_comparison_proc_name(       \
                                         unsigned_int),                                  \
          const unsigned long: ___csort__get_default_integral_comparison_proc_name(      \
                                         unsigned_long),                                 \
          const unsigned long long: ___csort__get_default_integral_comparison_proc_name( \
                                         unsigned_long_long),                            \
          const float: ___csort__get_default_integral_comparison_proc_name(              \
                                         float),                                         \
          const double: ___csort__get_default_integral_comparison_proc_name(             \
                                         double),                                        \
          const long double: ___csort__get_default_integral_comparison_proc_name(        \
                                         long_double),                                   \
          default: NULL);                                                                \
    } else if (_ccol_is_char_ptr((x)) && !_ccol_is_char_array((x))) {                    \
      __csort_gdcp_result = _csort_default_string_comparison_proc;                       \
    } else if (_Generic((x), _Bool: 1, const _Bool: 1, default: 0)) {                    \
      __csort_gdcp_result = _csort_default_bool_comparison_proc;                         \
    }                                                                                    \
    __csort_gdcp_result;                                                                 \
  })
#endif

/**
 * @brief Compile-time predicate: does this type have a default comparator?
 *
 * Expands to an integer constant expression that is 1 when
 * csort_get_default_comparison_proc(x) gives a real comparison procedure
 * for the type of x, and 0 when that macro gives NULL. The whole expression
 * is evaluated by the compiler, but x itself never is. The result works
 * inside a _Static_assert, so a caller can reject an unsupported element
 * type at compile time instead of finding a NULL comparator at run time.
 *
 * @param x An expression of the type to test
 *
 * @return 1 when the type has a default comparison procedure, 0 otherwise
 *
 * @note Only the type of x is looked at; neither x nor its address is
 * evaluated, so an lvalue that comes from a null pointer is fine. The
 * container macros use exactly that, a companion pointer that carries the
 * type and is never dereferenced.
 * @note The association lists are an exact copy of those of
 * csort_get_default_comparison_proc, so the two always agree, and changing
 * one of them means changing both.
 * @note It's a bare _Generic chain and doesn't use the
 * _ccol_is_integral_type(), _ccol_is_char_ptr() and _ccol_is_char_array()
 * macros of common.h. Under Clang those expand to a statement expression,
 * which isn't a constant expression and so can't go inside a
 * _Static_assert.
 * @note A qualified type selects the association of its unqualified type,
 * because the controlling expression of a _Generic selection goes through
 * lvalue conversion, so no const associations are needed.
 * @note The last clause excludes a fixed-size array. As the controlling
 * expression of the second _Generic an array decays to a pointer, so
 * without the last clause `char name[64]` would count as a string. That
 * clause compares the undecayed type of x with its decayed pointer type and
 * ignores x's top-level qualifiers, so a char pointer that is itself const,
 * like `const char *const`, is a string, while a char array of any
 * qualification isn't.
 *
 * @see csort_get_default_comparison_proc
 * @see cvec_sort
 */
#define ___csort_has_default_comparison_proc(x) \
  (_Generic((x),                                \
       char: 1,                                 \
       signed char: 1,                          \
       short: 1,                                \
       int: 1,                                  \
       long: 1,                                 \
       long long: 1,                            \
       unsigned char: 1,                        \
       unsigned short: 1,                       \
       unsigned int: 1,                         \
       unsigned long: 1,                        \
       unsigned long long: 1,                   \
       float: 1,                                \
       double: 1,                               \
       long double: 1,                          \
       _Bool: 1,                                \
       default: 0) ||                           \
   (_Generic((x),                               \
        char *: 1,                              \
        const char *: 1,                        \
        signed char *: 1,                       \
        const signed char *: 1,                 \
        unsigned char *: 1,                     \
        const unsigned char *: 1,               \
        default: 0) &&                          \
    !_ccol_type_is_array_of_char(x)))

#pragma GCC visibility pop
