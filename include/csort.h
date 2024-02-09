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
 * @brief Generic sorting library with iterative quicksort implementation
 *
 * Provides a type-generic sorting facility with:
 * - Iterative quicksort (no recursion, stack-safe)
 * - Default comparison functions for all standard C types
 * - Custom comparison and swap function support
 * - Integration with custom memory management
 * - Type-safe comparison function selection via _Generic
 *
 * The library uses function pointers for abstraction, allowing it to sort
 * any collection type (arrays, vectors, custom containers) as long as getter
 * and comparison functions are provided.
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
 * @note The returned pointer must remain valid during the sort
 * @note For arrays: return &array[index]
 * @note For vectors: return cvector_at(vec, index)
 */
typedef void *(*csort_item_getter_proc_t)(void *collection, size_t index);

/**
 * @brief Function pointer type for swapping two elements
 *
 * Swap functions exchange the contents of two elements. The default
 * implementation performs byte-by-byte swap. Custom swap functions can
 * be provided for optimization or special handling.
 *
 * @param first Pointer to first element
 * @param second Pointer to second element
 * @param elem_size Size of each element in bytes
 *
 * @note Both pointers must be valid and non-NULL
 * @note Should handle first == second gracefully (no-op)
 * @note elem_size may be unused by pointer-based swap implementations
 */
typedef void (*csort_item_swap_proc_t)(void *first, void *second,
                                       size_t elem_size);

/* ========================================================================== */
/*                         DEFAULT SWAP PROCEDURES                            */
/* ========================================================================== */

/**
 * @brief Default swap implementation (byte-by-byte)
 *
 * Swaps two elements by exchanging their bytes one at a time. Works for any
 * data type but is relatively slow for large elements.
 *
 * @param first Pointer to first element
 * @param second Pointer to second element
 * @param elem_size Size of each element in bytes
 *
 * @note Safe for any data type
 * @note No-op if first or second is NULL, or if first == second
 * @note O(elem_size) complexity
 *
 * @see csort_default_pointer_swap_proc
 */
void csort_default_swap_proc(void *first, void *second, size_t elem_size);

/**
 * @brief Pointer swap implementation (for pointer arrays)
 *
 * Swaps two pointer values. Intended for use with arrays of pointers where
 * only the pointers need to be exchanged, not the data they point to.
 *
 * @param first Pointer to first pointer variable
 * @param second Pointer to second pointer variable
 * @param elem_size Unused (attribute unused to suppress warnings)
 *
 * @note Only swaps the pointer values, not pointed-to data
 * @note No-op if first or second is NULL, or if first == second
 * @note O(1) complexity
 *
 * @warning Current implementation has a bug - it swaps local copies, not the
 * actual pointers
 *
 * @see csort_default_swap_proc
 */
void csort_default_pointer_swap_proc(void *first, void *second,
                                     size_t elem_size __attribute__((unused)));

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
 * @brief Internal quicksort implementation (do not call directly)
 *
 * Implements an iterative quicksort algorithm using an explicit stack to avoid
 * recursion and potential stack overflow. Uses dynamic memory for the stack
 * with automatic growth.
 *
 * @param col Pointer to collection to sort
 * @param length Number of elements in collection
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function to get element at index
 * @param comparison_proc Function to compare two elements
 * @param swap_proc Function to swap two elements (NULL for default)
 * @param mprocs Memory management procedures for stack allocation
 *
 * @note Use csort_sort() macro instead of calling this directly
 * @note Will assert if getter_proc or comparison_proc is NULL
 * @note Returns immediately if length is 0 or 1
 * @note Stack starts at 64 entries, grows to 128+ for large arrays
 * @note If stack allocation fails, sort is aborted (partial results)
 *
 * @see csort_sort
 */
void ___csort_qsort(void *col, size_t length, size_t elem_size,
                    csort_item_getter_proc_t getter_proc,
                    ccol_comparison_proc_t comparison_proc,
                    csort_item_swap_proc_t swap_proc,
                    ccol_memmgmt_procs_t *mprocs);

/**
 * @brief Sort a collection using iterative quicksort
 *
 * Main sorting macro that provides a convenient interface to the quicksort
 * implementation. Supports any collection type with appropriate getter and
 * comparison functions.
 *
 * @param collection Pointer to collection to sort
 * @param length Number of elements to sort
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function to retrieve element at index
 * @param comparison_proc Function to compare two elements
 * @param swap_proc Function to swap elements (NULL for default byte-swap)
 * @param mprocs Memory management procedures (NULL for default malloc/free)
 *
 * @note Average complexity: O(n log n)
 * @note Worst case: O(n²) (rare with random pivot selection)
 * @note Space complexity: O(log n) average for stack, O(n) worst case
 * @note Iterative implementation - no recursion, no stack overflow risk
 * @note Not stable - equal elements may be reordered
 *
 * Example:
 * @code
 * int arr[] = {3, 1, 4, 1, 5, 9, 2, 6};
 * csort_sort(arr, 8, sizeof(int),
 *            (csort_item_getter_proc_t)array_getter,
 *            csort_default_int_comparison_proc,
 *            NULL, NULL);
 * @endcode
 */
#define csort_sort(collection, length, elem_size, getter_proc,                 \
                   comparison_proc, swap_proc, mprocs)                         \
  (___csort_qsort(collection, length, elem_size, getter_proc, comparison_proc, \
                  swap_proc, mprocs))

/* ========================================================================== */
/*                   COMPARISON PROCEDURE DECLARATIONS                        */
/* ========================================================================== */

/**
 * @brief Internal macro to generate comparison procedure name
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
 * @param x Variable or expression whose type determines the comparison function
 *
 * @return Function pointer to appropriate comparison function, or NULL if
 * unsupported
 *
 * @note Supports: char, short, int, long, long long (signed and unsigned)
 * @note Supports: float, double, long double
 * @note Supports: char* and const char* (C strings)
 * @note Returns NULL for unsupported types
 * @note All variants (const and non-const) are supported
 * @note Uses is_integral_type() and is_char_ptr() macros from common.h
 *
 * Example:
 * @code
 * int dummy_int;
 * ccol_comparison_proc_t cmp = csort_get_default_comparison_proc(dummy_int);
 * // cmp now points to csort_default_int_comparison_proc
 *
 * char* dummy_str;
 * cmp = csort_get_default_comparison_proc(dummy_str);
 * // cmp now points to csort_default_string_comparison_proc
 * @endcode
 *
 * @see csort_sort
 * @see cvec_sort (uses this macro)
 */
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
