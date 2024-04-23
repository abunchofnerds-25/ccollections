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

/**
 * @file csort.h
 * @brief Generic sorting library with iterative mergesort implementation
 *
 * Provides a type-generic sorting facility with:
 * - Iterative mergesort algorithm (stable sort, no recursion)
 * - O(n log n) worst-case time complexity
 * - O(n) space complexity for temporary buffer
 * - Default comparison functions for all standard C types
 * - Custom comparison function support
 * - Integration with custom memory management
 * - Type-safe comparison function selection via _Generic
 *
 * The library uses function pointers for abstraction, allowing it to sort
 * any collection type (arrays, vectors, custom containers) as long as getter
 * and comparison functions are provided.
 *
 * The mergesort implementation is iterative (bottom-up) rather than recursive,
 * making it safe for large datasets without risk of stack overflow.
 */

/**
 * @brief Function pointer type for retrieving elements from a collection
 *
 * Getter functions abstract away the details of how elements are stored,
 * allowing the sort algorithm to work with any collection type.
 *
 * @param collection Pointer to the collection being sorted
 * @param index Zero-based index of element to retrieve
 *
 * @return Pointer to element at the given index
 *
 * @note The returned pointer must remain valid during the sort operation
 * @note For C arrays: return &array[index]
 * @note For vectors: return cvector_at(vec, index)
 *
 * Example implementations:
 * @code
 * // For a plain C array of ints
 * void *int_array_getter(void *collection, size_t index) {
 *     return &((int*)collection)[index];
 * }
 *
 * // For a vector (already implemented in cvector.h)
 * void *vector_getter(void *collection, size_t index) {
 *     return cvector_at((cvec)collection, index);
 * }
 * @endcode
 */
typedef void *(*csort_item_getter_proc_t)(void *collection, size_t index);

/* ========================================================================== */
/*                      DEFAULT COMPARISON PROCEDURES                         */
/* ========================================================================== */

/**
 * @brief Default comparison function for C strings
 *
 * Compares two strings using strcmp(). Both arguments must be pointers to
 * char pointers (char**).
 *
 * @param first Pointer to first char* (i.e., char**)
 * @param second Pointer to second char* (i.e., char**)
 *
 * @return Negative if *first < *second, zero if equal, positive if *first >
 * *second
 *
 * @note Uses strcmp() semantics
 * @note Both strings must be null-terminated
 *
 * @see strcmp
 */
int csort_default_string_comparison_proc(const void *first, const void *second);

/* ========================================================================== */
/*                         INTERNAL DECLARATIONS                              */
/* ========================================================================== */

/**
 * @brief Internal mergesort implementation (do not call directly)
 *
 * Implements an iterative (bottom-up) mergesort algorithm that avoids recursion
 * and potential stack overflow. The algorithm uses a temporary buffer allocated
 * via the provided memory management procedures.
 *
 * Algorithm characteristics:
 * - Stable sort (preserves relative order of equal elements)
 * - O(n log n) time complexity in all cases (worst, average, best)
 * - O(n) space complexity for temporary merge buffer
 * - Iterative implementation (no recursion, no stack depth concerns)
 *
 * @param col Pointer to collection to sort
 * @param length Number of elements in collection
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function to get element at index (required)
 * @param comparison_proc Function to compare two elements (required)
 * @param mprocs Memory management procedures for buffer allocation (NULL =
 * default)
 *
 * @note Use csort_sort() macro instead of calling this directly
 * @note Will assert if getter_proc or comparison_proc is NULL
 * @note Returns immediately if length is 0 or 1
 * @note Allocates temporary buffer of size (length * elem_size) bytes
 * @note If buffer allocation fails, sort is aborted (no error indication)
 *
 * @see csort_sort
 */
void ___csort_qsort(void *col, size_t length, size_t elem_size,
                    csort_item_getter_proc_t getter_proc,
                    ccol_comparison_proc_t comparison_proc,
                    ccol_memmgmt_procs_t *mprocs);

/* ========================================================================== */
/*                         PUBLIC SORT INTERFACE                              */
/* ========================================================================== */

/**
 * @brief Sort a collection using mergesort
 *
 * Sorts any collection type in-place using an iterative mergesort algorithm.
 * The collection can be a C array, vector, or any custom container as long
 * as appropriate getter and comparison functions are provided.
 *
 * @param col Pointer to collection to sort
 * @param length Number of elements in the collection
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function to retrieve element at index
 * @param comparison_proc Function to compare two elements
 * @param mprocs Memory management procedures (NULL for default malloc/free)
 *
 * @note This is a macro wrapper around ___csort_qsort
 * @note Sort is stable (preserves order of equal elements)
 * @note Time complexity: O(n log n) in all cases
 * @note Space complexity: O(n) for temporary merge buffer
 * @note Returns without error on allocation failure
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
 * // Sort a vector
 * cvec my_vec;
 * // ... populate vector ...
 * csort_sort(my_vec, cvector_elem_count(my_vec), sizeof(int),
 *            (csort_item_getter_proc_t)cvector_at,
 *            csort_get_default_comparison_proc(0),
 *            cvector_get_mprocs(my_vec));
 * @endcode
 *
 * @see csort_get_default_comparison_proc
 * @see cvec_sort (convenience wrapper for vectors)
 */
#define csort_sort(col, length, elem_size, getter_proc, comparison_proc, \
                   mprocs)                                               \
  (___csort_qsort(col, length, elem_size, getter_proc, comparison_proc, mprocs))

/* ========================================================================== */
/*                   COMPARISON PROCEDURE DECLARATIONS                        */
/* ========================================================================== */

/**
 * @brief Internal macro to generate comparison procedure name
 *
 * Creates the full function name for a type-specific comparison procedure.
 *
 * @param name Type suffix for the comparison function
 *
 * @return Function name: csort_default_<name>_comparison_proc
 */
#define ___csort__get_default_integral_comparison_proc_name(name) \
  csort_default_##name##_comparison_proc

/**
 * @brief Internal macro to declare a default comparison procedure
 *
 * Generates a function declaration for comparing elements of the given type.
 * The generated function follows the standard comparator convention.
 *
 * @param type C type (e.g., int, float, char)
 * @param name Type suffix for function name (e.g., int, float, char)
 */
#define ___csort__declare_default_integral_comparison_proc(type, name) \
  int ___csort__get_default_integral_comparison_proc_name(name)(       \
      const void *first, const void *second)

/* Declare default comparison procedures for all standard types */

/** @brief Compare two char values */
___csort__declare_default_integral_comparison_proc(char, char);

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

/** @brief Compare two float values */
___csort__declare_default_integral_comparison_proc(float, float);

/** @brief Compare two double values */
___csort__declare_default_integral_comparison_proc(double, double);

/** @brief Compare two long double values */
___csort__declare_default_integral_comparison_proc(long double, long_double);

#undef __declare_default_integral_comparison_proc

/* ========================================================================== */
/*                   TYPE-GENERIC COMPARISON SELECTION                        */
/* ========================================================================== */

/**
 * @brief Get the default comparison function for a type
 *
 * Uses C11 _Generic to automatically select the appropriate comparison function
 * based on the type of the provided variable. Supports all standard integral
 * types, floating-point types, and C strings.
 *
 * This macro examines the type of the provided expression at compile-time and
 * returns the appropriate comparison function pointer. The comparison functions
 * follow the standard C comparator convention (return <0, 0, or >0).
 *
 * @param x Variable or expression whose type determines the comparison function
 *
 * @return Function pointer to appropriate comparison function, or NULL if
 * unsupported
 *
 * @note Supported signed types: char, short, int, long, long long
 * @note Supported unsigned types: unsigned char, unsigned short, unsigned int,
 *       unsigned long, unsigned long long
 * @note Supported floating types: float, double, long double
 * @note Supported string types: char*, const char*
 * @note Returns NULL for unsupported types
 * @note All const and non-const variants are supported
 * @note Uses is_integral_type() and is_char_ptr() macros from common.h
 *
 * Example usage:
 * @code
 * // Get comparator for integers
 * int dummy_int;
 * ccol_comparison_proc_t cmp = csort_get_default_comparison_proc(dummy_int);
 * // cmp now points to csort_default_int_comparison_proc
 *
 * // Get comparator for strings
 * char* dummy_str;
 * cmp = csort_get_default_comparison_proc(dummy_str);
 * // cmp now points to csort_default_string_comparison_proc
 *
 * // Get comparator for doubles
 * double dummy_double;
 * cmp = csort_get_default_comparison_proc(dummy_double);
 * // cmp now points to csort_default_double_comparison_proc
 * @endcode
 *
 * @see csort_sort
 * @see cvec_sort (uses this macro)
 */
#if defined __clang__
#define csort_get_default_comparison_proc(x)                                             \
  ({                                                                                     \
    ccol_comparison_proc_t comparison_proc = NULL;                                       \
    if (is_integral_type(x)) {                                                           \
      _Pragma("GCC diagnostic push");                                                    \
      _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\"");            \
      comparison_proc = _Generic((x),                                                    \
          char: ___csort__get_default_integral_comparison_proc_name(char),               \
          short: ___csort__get_default_integral_comparison_proc_name(short),             \
          int: ___csort__get_default_integral_comparison_proc_name(int),                 \
          long: ___csort__get_default_integral_comparison_proc_name(long),               \
          long long: ___csort__get_default_integral_comparison_proc_name(                \
                                     long_long),                                         \
          unsigned char: ___csort__get_default_integral_comparison_proc_name(            \
                                     unsigned_char),                                     \
          unsigned short: ___csort__get_default_integral_comparison_proc_name(           \
                                     unsigned_short),                                    \
          unsigned int: ___csort__get_default_integral_comparison_proc_name(             \
                                     unsigned_int),                                      \
          unsigned long: ___csort__get_default_integral_comparison_proc_name(            \
                                     unsigned_long),                                     \
          unsigned long long: ___csort__get_default_integral_comparison_proc_name(       \
                                     unsigned_long_long),                                \
          float: ___csort__get_default_integral_comparison_proc_name(float),             \
          double: ___csort__get_default_integral_comparison_proc_name(double),           \
          long double: ___csort__get_default_integral_comparison_proc_name(              \
                                     long_double),                                       \
          const char: ___csort__get_default_integral_comparison_proc_name(               \
                                     char),                                              \
          const short: ___csort__get_default_integral_comparison_proc_name(              \
                                     short),                                             \
          const int: ___csort__get_default_integral_comparison_proc_name(int),           \
          const long: ___csort__get_default_integral_comparison_proc_name(               \
                                     long),                                              \
          const long long: ___csort__get_default_integral_comparison_proc_name(          \
                                     long_long),                                         \
          const unsigned char: ___csort__get_default_integral_comparison_proc_name(      \
                                     unsigned_char),                                     \
          const unsigned short: ___csort__get_default_integral_comparison_proc_name(     \
                                     unsigned_short),                                    \
          const unsigned int: ___csort__get_default_integral_comparison_proc_name(       \
                                     unsigned_int),                                      \
          const unsigned long: ___csort__get_default_integral_comparison_proc_name(      \
                                     unsigned_long),                                     \
          const unsigned long long: ___csort__get_default_integral_comparison_proc_name( \
                                     unsigned_long_long),                                \
          const float: ___csort__get_default_integral_comparison_proc_name(              \
                                     float),                                             \
          const double: ___csort__get_default_integral_comparison_proc_name(             \
                                     double),                                            \
          const long double: ___csort__get_default_integral_comparison_proc_name(        \
                                     long_double),                                       \
          default: NULL);                                                                \
      _Pragma("GCC diagnostic pop");                                                     \
    } else if (is_char_ptr(x)) {                                                         \
      comparison_proc = csort_default_string_comparison_proc;                            \
    }                                                                                    \
    comparison_proc;                                                                     \
  })
#else
#define csort_get_default_comparison_proc(x)                                             \
  ({                                                                                     \
    ccol_comparison_proc_t comparison_proc = NULL;                                       \
    if (is_integral_type(x)) {                                                           \
      comparison_proc = _Generic((x),                                                    \
          char: ___csort__get_default_integral_comparison_proc_name(char),               \
          short: ___csort__get_default_integral_comparison_proc_name(short),             \
          int: ___csort__get_default_integral_comparison_proc_name(int),                 \
          long: ___csort__get_default_integral_comparison_proc_name(long),               \
          long long: ___csort__get_default_integral_comparison_proc_name(                \
                                     long_long),                                         \
          unsigned char: ___csort__get_default_integral_comparison_proc_name(            \
                                     unsigned_char),                                     \
          unsigned short: ___csort__get_default_integral_comparison_proc_name(           \
                                     unsigned_short),                                    \
          unsigned int: ___csort__get_default_integral_comparison_proc_name(             \
                                     unsigned_int),                                      \
          unsigned long: ___csort__get_default_integral_comparison_proc_name(            \
                                     unsigned_long),                                     \
          unsigned long long: ___csort__get_default_integral_comparison_proc_name(       \
                                     unsigned_long_long),                                \
          float: ___csort__get_default_integral_comparison_proc_name(float),             \
          double: ___csort__get_default_integral_comparison_proc_name(double),           \
          long double: ___csort__get_default_integral_comparison_proc_name(              \
                                     long_double),                                       \
          const char: ___csort__get_default_integral_comparison_proc_name(               \
                                     char),                                              \
          const short: ___csort__get_default_integral_comparison_proc_name(              \
                                     short),                                             \
          const int: ___csort__get_default_integral_comparison_proc_name(int),           \
          const long: ___csort__get_default_integral_comparison_proc_name(               \
                                     long),                                              \
          const long long: ___csort__get_default_integral_comparison_proc_name(          \
                                     long_long),                                         \
          const unsigned char: ___csort__get_default_integral_comparison_proc_name(      \
                                     unsigned_char),                                     \
          const unsigned short: ___csort__get_default_integral_comparison_proc_name(     \
                                     unsigned_short),                                    \
          const unsigned int: ___csort__get_default_integral_comparison_proc_name(       \
                                     unsigned_int),                                      \
          const unsigned long: ___csort__get_default_integral_comparison_proc_name(      \
                                     unsigned_long),                                     \
          const unsigned long long: ___csort__get_default_integral_comparison_proc_name( \
                                     unsigned_long_long),                                \
          const float: ___csort__get_default_integral_comparison_proc_name(              \
                                     float),                                             \
          const double: ___csort__get_default_integral_comparison_proc_name(             \
                                     double),                                            \
          const long double: ___csort__get_default_integral_comparison_proc_name(        \
                                     long_double),                                       \
          default: NULL);                                                                \
    } else if (is_char_ptr(x)) {                                                         \
      comparison_proc = csort_default_string_comparison_proc;                            \
    }                                                                                    \
    comparison_proc;                                                                     \
  })
#endif
