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

/* Everything declared from here to the end of this header is part of the
 * public Application Binary Interface (ABI) of libccollections. The shared
 * library exports all of it. The library is built with
 * -fvisibility=hidden. A function or object that is not inside one of these
 * blocks stays internal to the library. It is absent from the dynamic
 * symbol table of the library. The application that links against the
 * library cannot interpose it. A symbol with the same name in that
 * application cannot collide with it. */
#pragma GCC visibility push(default)

/**
 * @file csort.h
 * @brief Generic sort library with an iterative mergesort
 *
 * This header gives a type-generic sort facility with:
 * - An iterative mergesort algorithm (a stable sort, with no recursion)
 * - O(n log n) worst-case time complexity
 * - O(n) space complexity for the temporary buffer
 * - Default comparison functions for all the standard C types
 * - Support for a custom comparison function
 * - Support for custom memory management
 * - Type-inferred selection of the comparison function with _Generic
 *
 * The library uses function pointers as an abstraction. It can therefore
 * sort any type of collection: an array, a vector or a custom container.
 * The caller must give a getter function and a comparison function.
 *
 * The mergesort is iterative (bottom-up) and not recursive. This makes it
 * safe for a large set of data, because the stack cannot overflow.
 */

/**
 * @brief Function pointer type that gets an element from a collection
 *
 * A getter function hides the details of how a collection stores its
 * elements. The sort algorithm can therefore work with any type of
 * collection.
 *
 * @param collection Pointer to the collection that the library sorts
 * @param index Zero-based index of the element to get
 *
 * @return Pointer to the element at the given index
 *
 * @note The pointer must stay valid for the whole sort operation
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
 * // For a vector (cvector.h has this one already)
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
 * The leading underscore marks this function as internal. Reach it through
 * csort_get_default_comparison_proc(), which selects it from the type of
 * its argument. The name and the signature can change in any release.
 *
 * This function compares two stored string pointers with strcmp(). Both
 * arguments must be pointers to char pointers (char**).
 *
 * A stored pointer can be NULL. NULL comes before every string that is not
 * NULL, and the empty string is one of those. NULL is equal only to another
 * NULL. This keeps the result a real total order over the whole char* type.
 * A collection that holds NULL elements therefore sorts and searches, and
 * the caller needs no special handling. The function also answers a NULL
 * needle, and it does not dereference it. Without that rule, a NULL element
 * reaches strcmp() as a null pointer.
 *
 * @param first Pointer to the first char*, that is a char**. It must not be
 * NULL itself, because it is the address of the stored pointer.
 * @param second Pointer to the second char*, that is a char**. It has the
 * same requirement.
 *
 * @return A negative value when *first < *second. Zero when the two are
 * equal, and zero when both are NULL. A positive value when
 * *first > *second.
 *
 * @note The function follows strcmp() for a pair of strings that are not
 * NULL
 * @note A string that is not NULL must be null-terminated
 * @note NULL is the least value. The function never dereferences it.
 *
 * @see strcmp
 */
int _csort_default_string_comparison_proc(const void *first,
                                          const void *second);

/**
 * @brief The internal mergesort. Do not call it directly.
 *
 * This function is an iterative (bottom-up) mergesort algorithm. It has no
 * recursion, so the stack cannot overflow. The algorithm uses a temporary
 * buffer. It allocates that buffer with the memory management procedures
 * that the caller gives.
 *
 * Properties of the algorithm:
 * - A stable sort. It keeps the relative order of equal elements.
 * - O(n log n) time complexity in every case: worst, average and best
 * - O(n) space complexity for the temporary merge buffer
 * - An iterative algorithm, with no recursion and no concern about the
 *   depth of the stack
 *
 * @param col Pointer to the collection to sort
 * @param length Number of elements in the collection
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function that gets the element at an index. It is
 * necessary.
 * @param comparison_proc Function that compares two elements. It is
 * necessary.
 * @param mprocs Memory management procedures for the allocation of the
 * buffer. NULL selects the default procedures.
 *
 * @return true when the function sorts the whole collection. It also gives
 * true for the trivial cases, which have nothing to do: a col of NULL, a
 * length of 0 or 1, and an elem_size of 0. The function gives false when
 * length is more than ccol_max_elem_count. It gives false when mprocs is
 * not NULL and does not hold all four function pointers. It gives false
 * when it cannot allocate the temporary merge buffer. It also gives false
 * when length * elem_size overflows size_t. In each of these cases the
 * collection stays completely unchanged, because no merge pass started yet.
 *
 * @note Use the csort_sort() macro. Do not call this function directly.
 * @note The function asserts when getter_proc or comparison_proc is NULL.
 * It does not assert when col is NULL, or when length is 0 or 1. The
 * trivial-success path then gives true and looks at neither procedure.
 * @note The function gives true immediately when length is 0 or 1
 * @note The function gives true immediately when elem_size is 0. It calls
 * neither getter_proc nor comparison_proc, and it allocates no temporary
 * buffer. An element of size zero has no bytes for those procedures to
 * read, and no bytes for a merge pass to move.
 * @note The function rejects a length that is more than ccol_max_elem_count
 * (2^63 on a 64-bit size_t). Every other container in this library has the
 * same cap, for example cvector and chashmap. The cap exists so that the
 * internal count of bottom-up merge passes can never overflow.
 * @note An mprocs that is not NULL must hold malloc, free, calloc and
 * realloc. This is the same contract that ccol_memmgmt_procs_t itself
 * documents. The function rejects an incomplete mprocs with false. It does
 * this directly before it allocates the temporary buffer, and never in the
 * middle of a sort.
 * @note The function allocates a temporary buffer of (length * elem_size)
 * bytes. It checks this multiplication for an overflow first.
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
 * This macro sorts any type of collection in place. It uses an iterative
 * mergesort algorithm. The collection can be a C array, a vector or any
 * custom container. The caller must give a suitable getter function and a
 * suitable comparison function.
 *
 * @param col Pointer to the collection to sort
 * @param length Number of elements in the collection
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function that gets the element at an index
 * @param comparison_proc Function that compares two elements
 * @param mprocs Memory management procedures. NULL selects the default
 * malloc and free. An mprocs that is not NULL must hold all four of malloc,
 * free, calloc and realloc.
 *
 * @return true on success. A length of zero or one, and an elem_size of 0,
 * are also a success, because they have nothing to do. The macro gives
 * false when length is more than ccol_max_elem_count. It gives false when
 * mprocs is not NULL and is incomplete. It gives false when it cannot
 * allocate the temporary merge buffer. It also gives false when
 * length * elem_size overflows size_t. The collection stays completely
 * unchanged in each of these cases.
 *
 * @note This macro is a wrapper around ___csort_merge_sort
 * @note getter_proc and comparison_proc must both not be NULL for a col
 * that is not NULL and has a length of 2 or more. This is necessary even
 * when elem_size is 0, although the macro then calls neither of them. Only
 * a col of NULL, or a length of 0 or 1, lets a call give no real
 * procedures. A call that breaks this rule asserts and aborts the process.
 * It does not give false.
 * @note The sort is stable. It keeps the order of equal elements.
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
 * // cvector_sort_with_comparison_proc() do this for you already. Use
 * // those macros, unless you need this raw interface.
 * // getter_proc must have exactly the signature of
 * // csort_item_getter_proc_t, which is void *(*)(void *, size_t). The
 * // first parameter of cvector_at() is a cvec and not a void *. You
 * // therefore cannot cast cvector_at() to csort_item_getter_proc_t and
 * // pass it as it is. A call through such a cast pointer is undefined
 * // behavior (C11 6.3.2.3p8). This is true although cvec and void * have
 * // the same representation on every mainstream ABI. A small adapter with
 * // the exact signature closes this hole.
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
 * This macro makes the full function name for a comparison procedure of one
 * type.
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
 * This macro makes a function declaration. That function compares two
 * elements of the given type. It obeys the standard comparator convention.
 *
 * @param type C type, for example int, float or char
 * @param name Type suffix for the function name, for example int, float or
 * char
 */
#define ___csort__declare_default_integral_comparison_proc(type, name) \
  int ___csort__get_default_integral_comparison_proc_name(name)(       \
      const void *first, const void *second)

/* Declare the default comparison procedures for all the standard types */

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

/**
 * @brief Compare two float values
 * @note NaN is greater than every value that is not NaN. It is equal only
 * to another NaN. The result is therefore always a real total order, also
 * when one operand or both operands are NaN. The note on
 * _csort_default_double_comparison_proc explains why this is important for a
 * merge-sort comparator.
 */
___csort__declare_default_integral_comparison_proc(float, float);

/**
 * @brief Compare two double values
 * @note NaN is greater than every value that is not NaN. It is equal only
 * to another NaN. The native `<` and `>` of IEEE 754 are both false when
 * one operand is NaN. A simple comparator therefore reports NaN as "equal"
 * to everything. This includes two unrelated values that are not NaN and
 * only sit on the two sides of the NaN in the collection. The
 * take-left-or-take-right decision of csort_merge needs a real strict weak
 * ordering from comparison_proc. A false "equal" verdict therefore corrupts
 * the relative order of the elements around the NaN. It does not only leave
 * the position of the NaN unspecified. The key comparator of cbstmap for
 * float, double and long double works in the same way.
 */
___csort__declare_default_integral_comparison_proc(double, double);

/**
 * @brief Compare two long double values
 * @note NaN is greater than every value that is not NaN. It is equal only
 * to another NaN. The reason is the same as for
 * _csort_default_double_comparison_proc.
 */
___csort__declare_default_integral_comparison_proc(long double, long_double);

#undef ___csort__declare_default_integral_comparison_proc

/* ========================================================================== */
/*                   TYPE-GENERIC COMPARISON SELECTION                        */
/* ========================================================================== */

/**
 * @brief Get the default comparison function for a type
 *
 * This macro uses C11 _Generic. It selects the correct comparison function
 * automatically from the type of the variable that the caller gives. The
 * macro supports all the standard integral types, all the floating-point
 * types and C strings.
 *
 * The macro looks at the type of the given expression at compile time. It
 * then gives the correct comparison function pointer. The comparison
 * functions obey the standard C comparator convention. Each one gives a
 * value less than 0, equal to 0, or more than 0.
 *
 * @param x Variable or expression. Its type decides the comparison
 * function.
 *
 * @return Function pointer to the correct comparison function. The macro
 * gives NULL for a type that it does not support.
 *
 * @note Supported signed types: char, signed char, short, int, long, long
 *       long
 * @note Supported unsigned types: unsigned char, unsigned short, unsigned
 *       int, unsigned long, unsigned long long
 * @note Supported floating types: float, double, long double
 * @note Supported string types: char* and const char*. These must be real
 * pointer variables or pointer expressions. The string comparator that the
 * macro gives puts a stored NULL before every string that is not NULL. That
 * NULL is equal only to another NULL.
 * @note The macro gives NULL for a type that it does not support. A
 * fixed-size char array (char[N]) is one such type. The comment below
 * explains why this type needs an explicit check. ccol_is_char_ptr() alone
 * does not exclude it.
 * @note The macro supports every const variant and every variant that is
 * not const
 * @note The macro uses the ccol_is_integral_type(), ccol_is_char_ptr() and
 * ccol_is_char_array() macros from common.h
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
/* The local below that holds the result has the name __csort_gdcp_result
 * on purpose. A name that a caller can also give to its own argument
 * variable is not safe here. "comparison_proc" is such a name. The
 * declarator-scope rules of C give the reason. Consider a caller that
 * writes csort_get_default_comparison_proc(comparison_proc) with an
 * argument variable of exactly that name. Then each use of (x) inside this
 * statement expression resolves to the new local of this macro, which is
 * always NULL. It does not resolve to the real variable of the caller. The
 * scope of the local starts directly after its own declarator, and that is
 * before the expansion of (x). The same hazard sets the names of the
 * internal locals in cvec_push. A name that belongs only to the internal
 * result of this macro is the standard defence against this hazard of a
 * non-hygienic macro. common.h uses the same defence for the same reason:
 * ccol_scoped_ptr_release gives its internal temporary the name
 * __ccol_released_ptr.
 *
 * The ccol_is_char_ptr((x)) branch below also checks
 * ccol_is_char_array((x)). ccol_is_char_ptr((x)) alone is not enough
 * for these reasons:
 * - C11 6.5.1.1p2 and 6.3.2.1p3 apply the ordinary array-to-pointer decay
 *   to the controlling expression of a _Generic selection.
 * - Therefore, a real fixed-size array x, such as a `char name[64]` field, also
 *   matches the `char *:` and `const char *:` associations of
 *   ccol_is_char_ptr after the decay.
 * - Without a guard, the macro identifies such an array as a string, and
 *   gives _csort_default_string_comparison_proc for it.
 * - The contract of that comparator requires that first and second point
 *   to a stored char* VALUE. It dereferences one pointer level with
 *   *(const char **)first. It does not expect the inline bytes of an array.
 * - csort_merge then calls that comparator on the real bytes of the array.
 *   The comparator reads the first sizeof(char*) bytes as a pointer value
 *   and dereferences it. That is undefined behavior, and not only an
 *   incorrect sort order.
 *
 * ccol_is_char_array() reads only the type of x. Therefore, an rvalue x, such
 * as a cast, is as correct as an lvalue. It also identifies a char pointer that
 * has its own qualifier, such as `const char *const`, as a pointer. An array
 * goes to the NULL result for an unsupported type. This macro documents that
 * result for each other type that it does not know. Therefore, the exclusion
 * adds no new comparator semantics. The default comparators of this module are
 * for an element that IS a scalar, a char pointer or a numeric value. They are
 * not for an element whose content IS a byte buffer. A caller with fixed-size
 * string fields has a full alternative: cvector_sort_with_comparison_proc() or
 * csort_sort() with an explicit comparator that the caller writes. The char[N]
 * field examples in doc/csort.md use that shape. */
#if defined __clang__
#define csort_get_default_comparison_proc(x)                                             \
  ({                                                                                     \
    ccol_comparison_proc_t __csort_gdcp_result = NULL;                                   \
    if (ccol_is_integral_type((x))) {                                                    \
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
    } else if (ccol_is_char_ptr((x)) && !ccol_is_char_array((x))) {                      \
      __csort_gdcp_result = _csort_default_string_comparison_proc;                       \
    }                                                                                    \
    __csort_gdcp_result;                                                                 \
  })
#else
#define csort_get_default_comparison_proc(x)                                             \
  ({                                                                                     \
    ccol_comparison_proc_t __csort_gdcp_result = NULL;                                   \
    if (ccol_is_integral_type((x))) {                                                    \
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
    } else if (ccol_is_char_ptr((x)) && !ccol_is_char_array((x))) {                      \
      __csort_gdcp_result = _csort_default_string_comparison_proc;                       \
    }                                                                                    \
    __csort_gdcp_result;                                                                 \
  })
#endif

/**
 * @brief Compile-time predicate: does this type have a default comparator?
 *
 * This macro expands to an integer constant expression. The value is 1 when
 * csort_get_default_comparison_proc(x) gives a real comparison procedure
 * for the type of x. The value is 0 when that macro gives NULL. The
 * compiler evaluates the whole expression, and it never evaluates x itself.
 * The result works inside a _Static_assert. A caller can therefore reject
 * an unsupported element type at compile time. Without this macro, the
 * caller finds a NULL comparator at run time instead.
 *
 * @param x An expression of the type to test
 *
 * @return 1 when the type has a default comparison procedure, and 0 in
 * every other case
 *
 * @note The macro looks only at the type of x. It evaluates neither x nor
 * the address of x. An lvalue that comes from a null pointer is therefore
 * fine. The container macros use that shape, where a companion pointer
 * carries the type and nothing dereferences that pointer.
 * @note The association lists are an exact copy of the lists of
 * csort_get_default_comparison_proc. The two therefore always agree. A
 * change to one of them is a change to both.
 * @note This macro is a bare _Generic chain. It does not use the
 * ccol_is_integral_type(), ccol_is_char_ptr() and ccol_is_char_array()
 * macros of common.h. Those macros expand to a statement expression under
 * Clang. A statement expression is not a constant expression, so it cannot
 * appear inside a _Static_assert.
 * @note A qualified type selects the association of its unqualified type.
 * The controlling expression of a _Generic selection gets an lvalue
 * conversion, so this macro needs no const associations.
 * @note The last clause excludes a fixed-size array. An array decays to a
 * pointer as the controlling expression of the second _Generic. Without the
 * last clause, `char name[64]` becomes a string. That clause compares the
 * undecayed type of x against its decayed pointer type, and it ignores the
 * top-level qualifiers of x. A char pointer that is itself const, such as
 * `const char *const`, is therefore a string, and a char array of any
 * qualification is not.
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
