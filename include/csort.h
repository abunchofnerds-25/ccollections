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
 * @return true if the collection was fully sorted (including the trivial
 * col == NULL / length 0 or 1 / elem_size == 0 cases, which have nothing to
 * do); false if length exceeds max_elem_count, mprocs is non-NULL but does
 * not have all four function pointers populated, the temporary merge buffer
 * could not be allocated, or length * elem_size would overflow size_t, in
 * which case the collection is left completely untouched (no merge pass had
 * started yet)
 *
 * @note Use csort_sort() macro instead of calling this directly
 * @note Will assert if getter_proc or comparison_proc is NULL, unless col is
 * NULL or length is 0 or 1, in which case the trivial-success path returns
 * true without ever inspecting either of them
 * @note Returns true immediately if length is 0 or 1
 * @note Returns true immediately if elem_size is 0, without ever calling
 * getter_proc, comparison_proc, or allocating a temporary buffer: a
 * zero-sized element has no bytes for either of those to read or for a merge
 * pass to move
 * @note Rejects length > max_elem_count (2^63 on a 64-bit size_t); this cap
 * matches every other container in this library (cvector, chashmap, ...) and
 * exists so the internal bottom-up merge pass count can never overflow
 * @note A non-NULL mprocs must have malloc, free, calloc, and realloc all
 * populated, the same contract ccol_memmgmt_procs_t itself documents; an
 * incomplete mprocs is rejected (false) immediately before the temporary
 * buffer would otherwise be allocated with it, never partway through a sort
 * @note Allocates temporary buffer of size (length * elem_size) bytes; this
 * multiplication is itself overflow-checked before being attempted
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
 * @param mprocs Memory management procedures (NULL for default malloc/free;
 * if non-NULL, all four of malloc/free/calloc/realloc must be populated)
 *
 * @return true on success (including a zero- or one-element length, or an
 * elem_size of 0, none of which have anything to do), false if length
 * exceeds max_elem_count, mprocs is non-NULL but incomplete, the temporary
 * merge buffer could not be allocated, or length * elem_size would overflow
 * size_t (the collection is left completely untouched in that case)
 *
 * @note This is a macro wrapper around ___csort_merge_sort
 * @note getter_proc and comparison_proc must both be non-NULL for a non-NULL
 * col whose length is 2 or greater; this is required even when elem_size
 * is 0, in which case neither one would actually end up being called; only
 * a NULL col or a length of 0 or 1 exempts a call from needing genuine,
 * non-NULL procs. Violating this asserts (aborts the process), it does not
 * return false.
 * @note Sort is stable (preserves order of equal elements)
 * @note Time complexity: O(n log n) in all cases
 * @note Space complexity: O(n) for temporary merge buffer
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
 * // Sort a cvec directly with csort_sort() (cvec_sort()/
 * // cvector_sort_with_comparison_proc() below already do this for you;
 * // use those instead unless you specifically need the raw interface).
 * // getter_proc must have exactly csort_item_getter_proc_t's own signature
 * // (void *(*)(void *, size_t)); cvector_at() itself takes a cvec, not a
 * // void *, as its first parameter, so it cannot be cast directly to
 * // csort_item_getter_proc_t and passed as-is; calling it through such a
 * // cast pointer is undefined behavior (C11 6.3.2.3p8), even though cvec
 * // and void * share identical representation on every mainstream ABI. A
 * // small adapter with the exact signature closes this.
 * void *my_vec_getter(void *collection, size_t index) {
 *   return cvector_at((cvec)collection, index);
 * }
 *
 * cvec my_vec;
 * // ... populate vector ...
 * csort_sort(my_vec, cvector_elem_count(my_vec), sizeof(int),
 *            my_vec_getter,
 *            csort_get_default_comparison_proc(0),
 *            cvector_get_mprocs(my_vec));
 * @endcode
 *
 * @see csort_get_default_comparison_proc
 * @see cvec_sort (convenience wrapper for vectors)
 */
#define csort_sort(col, length, elem_size, getter_proc, comparison_proc, \
                   mprocs)                                               \
  (___csort_merge_sort((col), (length), (elem_size), (getter_proc),      \
                       (comparison_proc), (mprocs)))

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
 * @note NaN compares greater than every non-NaN value, and equal only to
 * another NaN, so the result is always a genuine total order even when one
 * or both operands are NaN (see csort_default_double_comparison_proc's own
 * note for why this matters for a merge-sort comparator specifically)
 */
___csort__declare_default_integral_comparison_proc(float, float);

/**
 * @brief Compare two double values
 * @note NaN compares greater than every non-NaN value, and equal only to
 * another NaN. IEEE 754's native `<`/`>` are both false whenever either
 * operand is NaN, which would otherwise make a naive comparator report NaN
 * as "equal" to everything, including two unrelated non-NaN values that
 * merely straddle it in the collection; since csort_merge's own
 * take-left/take-right decision depends on comparison_proc supplying a
 * genuine strict weak ordering, that false "equal" verdict would corrupt the
 * relative order of the surrounding non-NaN elements, not merely leave the
 * NaN's own position unspecified. This mirrors cbstmap's own float/double/
 * long double key comparator.
 */
___csort__declare_default_integral_comparison_proc(double, double);

/**
 * @brief Compare two long double values
 * @note NaN compares greater than every non-NaN value, and equal only to
 * another NaN, for the same reason as csort_default_double_comparison_proc
 */
___csort__declare_default_integral_comparison_proc(long double, long_double);

#undef ___csort__declare_default_integral_comparison_proc

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
 * @note Supported signed types: char, signed char, short, int, long, long
 *       long
 * @note Supported unsigned types: unsigned char, unsigned short, unsigned int,
 *       unsigned long, unsigned long long
 * @note Supported floating types: float, double, long double
 * @note Supported string types: char*, const char* (genuine pointer
 * variables/expressions only)
 * @note Returns NULL for unsupported types, including a fixed-size char
 * array (char[N]); see the implementation note below for why this needs an
 * explicit check rather than falling out of is_char_ptr() on its own
 * @note All const and non-const variants are supported
 * @note Uses is_integral_type(), is_char_ptr(), and is_char_array() macros
 * from common.h
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
/* The result-holding local below is deliberately named
 * __csort_gdcp_result rather than something a caller might plausibly also
 * name their own argument variable (e.g. "comparison_proc", the name this
 * temporary used to carry): per C's declarator-scope rules, a caller
 * invoking csort_get_default_comparison_proc(comparison_proc) with an
 * argument variable of that exact name would otherwise have every use of
 * (x) inside this statement expression silently resolve to this macro's own
 * freshly-declared, always-NULL local instead of the caller's real
 * variable, since this local's scope begins immediately after its own
 * declarator, before (x) is ever expanded; the same class of bug already
 * found and fixed for cvec_push/cvec_push_rvalue's own internal locals (see
 * the cvector module's own history). A name this specific to this one
 * macro's own internal result is the standard mitigation for this
 * non-hygienic-macro footgun, matching how common.h's own
 * ccol_scoped_ptr_release names its internal temporary
 * __ccol_released_ptr for the identical reason.
 *
 * The is_char_ptr((x)) branch below additionally checks
 * is_char_array(__csort_gdcp_arr_probe), not is_char_ptr((x)) alone. Per
 * C11 6.5.1.1p2/6.3.2.1p3, the controlling expression of a _Generic
 * selection undergoes the ordinary array-to-pointer decay applied to any
 * expression used as an rvalue, so a genuine fixed-size array x (e.g. a
 * `char name[64]` field) also matches is_char_ptr's own
 * `char *:`/`const char *:` associations after decaying; is_char_ptr(x)
 * alone cannot tell "x is really a char* variable" apart from "x is a char
 * array that merely decayed to look like one for this one comparison."
 * Left unguarded, such an array was silently classified as a string and
 * handed csort_default_string_comparison_proc, a comparator whose contract
 * requires first/second to point at a stored char* VALUE (it dereferences
 * one pointer indirection via *(const char **)first), not at the array's
 * own inline byte content. Reproduced directly: this macro used to return a
 * non-NULL comparator for a `char name[64]` array that, when later invoked
 * by csort_merge on the array's real 64 bytes of string content, read the
 * first sizeof(char*) of those bytes as if they were a pointer value and
 * dereferenced it; undefined behavior, not merely a wrong sort order.
 * common.h's own is_char_array() macro already exists to draw this exact
 * distinction (see determine_ccol_data_type(), which checks it before
 * is_char_ptr() for the identical reason), but it needs to evaluate
 * &(data), so it requires an addressable lvalue; x itself is documented to
 * be any expression (including a bare rvalue like a cast, matching e.g.
 * this file's own signed_char_default_comparator_is_not_null_and_sorts_
 * signed test), so is_char_array((x)) cannot be applied to x directly
 * without breaking every rvalue caller. __csort_gdcp_arr_probe sidesteps
 * this: typeof(x), like sizeof and _Generic's own controlling expression,
 * only inspects x's type at compile time (never its value, and critically
 * never its address either), so declaring a fresh local of that same type
 * needs no addressability from x at all; the probe itself is then a
 * genuine, always-addressable local variable that is_char_array() can
 * safely operate on in x's place, faithfully preserving whether x's own
 * true (undecayed) type was an array. Excluding an array here correctly
 * falls through to the NULL, unsupported-type result this macro already
 * documents for every other type it does not recognize, rather than
 * inventing new comparator semantics: this module's default comparators
 * are for elements that ARE a scalar, char pointer, or numeric value, not
 * for elements whose content IS a byte buffer; a caller with fixed-size
 * string fields is still fully served by cvector_sort_with_comparison_proc()
 * / csort_sort() with an explicit, hand-written comparator (exactly how
 * README.md's own char[N]-field examples are written already). */
#if defined __clang__
#define csort_get_default_comparison_proc(x)                                             \
  ({                                                                                     \
    ccol_comparison_proc_t __csort_gdcp_result = NULL;                                   \
    if (is_integral_type((x))) {                                                         \
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
    } else if (is_char_ptr((x))) {                                                       \
      typeof(x) __csort_gdcp_arr_probe = {0};                                            \
      if (!is_char_array(__csort_gdcp_arr_probe)) {                                      \
        __csort_gdcp_result = csort_default_string_comparison_proc;                      \
      }                                                                                  \
    }                                                                                    \
    __csort_gdcp_result;                                                                 \
  })
#else
#define csort_get_default_comparison_proc(x)                                             \
  ({                                                                                     \
    ccol_comparison_proc_t __csort_gdcp_result = NULL;                                   \
    if (is_integral_type((x))) {                                                         \
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
    } else if (is_char_ptr((x))) {                                                       \
      typeof(x) __csort_gdcp_arr_probe = {0};                                            \
      if (!is_char_array(__csort_gdcp_arr_probe)) {                                      \
        __csort_gdcp_result = csort_default_string_comparison_proc;                      \
      }                                                                                  \
    }                                                                                    \
    __csort_gdcp_result;                                                                 \
  })
#endif
