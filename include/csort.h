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

// A stable, iterative (bottom-up) mergesort for any collection: O(n log n) in
// every case, O(n) extra space, no recursion. It reaches the elements through a
// getter and compares them with a comparator, and comes with default
// comparators for the standard C types.
//
// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal.
#pragma GCC visibility push(default)

// Gives a pointer to the element at index of collection, for example &((int
// *)collection)[index] for a C array. The pointer must stay valid for the whole
// sort.
typedef void *(*csort_item_getter_proc)(void *collection, size_t index);

// Internal: The default comparator for char * elements. Reach it through
// csort_get_default_comparison_proc(). first and second are the addresses of
// the stored pointers (char **). Two strings compare with strcmp(); a stored
// NULL sorts before every string and equals only another NULL, so it's never
// dereferenced.
int csort__default_string_comparison_proc(const void *first,
                                          const void *second);

// Internal: Sorts col; the body of csort_sort(). Use the macro instead.
bool csort__merge_sort(void *col, size_t length, size_t elem_size,
                       csort_item_getter_proc getter_proc,
                       ccol_comparison_proc comparison_proc,
                       ccol_memmgmt_procs *mprocs);

// Sorts col in place, stable. getter_proc reaches element i and comparison_proc
// compares two elements; mprocs allocates the length * elem_size temporary
// buffer (NULL for malloc and free, otherwise all four functions must be set).
// A NULL col, a length of 0 or 1 and an elem_size of 0 give true and do
// nothing. It gives false, with the collection unchanged, when length is above
// ccol_max_elem_count, mprocs is incomplete, length * elem_size overflows or
// the buffer can't be allocated. For a non-NULL col with 2 or more elements
// both procs must be non-NULL, or it asserts. A getter must have exactly the
// csort_item_getter_proc signature; calling cvector_at() through a cast pointer
// is undefined behaviour, so write a small adapter.
#define csort_sort(col, length, elem_size, getter_proc, comparison_proc, \
                   mprocs)                                               \
  (csort__merge_sort((col), (length), (elem_size), (getter_proc),        \
                     (comparison_proc), (mprocs)))

// Internal: Expands to the name csort__default_<name>_comparison_proc, so the
// declarations, the definitions and the selector all spell one name.
#define csort__default_cmp(name) csort__default_##name##_comparison_proc

// Internal: Expands to the prototype of the default comparator for name, which
// the list below uses to declare one per type.
#define csort__declare_default_integral_comparison_proc(type, name) \
  int csort__default_cmp(name)(const void *first, const void *second)

// Internal: The default comparators of the standard types. bool puts false
// before true. For float, double and long double, NaN is greater than every
// other value and equal only to NaN; the merge needs a real total order, and a
// plain (a > b) - (a < b) calls NaN equal to everything, which scrambles the
// values around it.
csort__declare_default_integral_comparison_proc(char, char);
csort__declare_default_integral_comparison_proc(signed char, signed_char);
csort__declare_default_integral_comparison_proc(short, short);
csort__declare_default_integral_comparison_proc(int, int);
csort__declare_default_integral_comparison_proc(long, long);
csort__declare_default_integral_comparison_proc(long long, long_long);
csort__declare_default_integral_comparison_proc(unsigned char, unsigned_char);
csort__declare_default_integral_comparison_proc(unsigned short, unsigned_short);
csort__declare_default_integral_comparison_proc(unsigned int, unsigned_int);
csort__declare_default_integral_comparison_proc(unsigned long, unsigned_long);
csort__declare_default_integral_comparison_proc(unsigned long long,
                                                unsigned_long_long);
int csort__default_bool_comparison_proc(const void *first, const void *second);
csort__declare_default_integral_comparison_proc(float, float);
csort__declare_default_integral_comparison_proc(double, double);
csort__declare_default_integral_comparison_proc(long double, long_double);

#undef csort__declare_default_integral_comparison_proc

#if defined(CCOL__CLANG_GENERIC_ASSOC_WARNING)
// Evaluates to the default comparator (a ccol_comparison_proc) for the type
// of x, or NULL for a type without one, so you can sort or search without
// writing a comparator. x is used only for its type and is never evaluated, so
// any expression of the wanted type works. It covers every integral and
// floating type (const too), enums, bool (false before true), and char * and
// const char * with plain, signed or unsigned char (strcmp order, a NULL
// pointer before every string). NaN sorts after every other value and equals
// only NaN. A char array such as char name[64] gives NULL, since its bytes
// aren't a pointer; pass your own comparator for fixed-size string fields.
#define csort_get_default_comparison_proc(x)                                  \
  ({                                                                          \
    ccol_comparison_proc csort__gdcp_result = NULL;                           \
    if (ccol__is_integral_type((x))) {                                        \
      _Pragma("GCC diagnostic push");                                         \
      _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
      csort__gdcp_result = _Generic((x),                                      \
          char: csort__default_cmp(char),                                     \
          signed char: csort__default_cmp(signed_char),                       \
          short: csort__default_cmp(short),                                   \
          int: csort__default_cmp(int),                                       \
          long: csort__default_cmp(long),                                     \
          long long: csort__default_cmp(long_long),                           \
          unsigned char: csort__default_cmp(unsigned_char),                   \
          unsigned short: csort__default_cmp(unsigned_short),                 \
          unsigned int: csort__default_cmp(unsigned_int),                     \
          unsigned long: csort__default_cmp(unsigned_long),                   \
          unsigned long long: csort__default_cmp(unsigned_long_long),         \
          float: csort__default_cmp(float),                                   \
          double: csort__default_cmp(double),                                 \
          long double: csort__default_cmp(long_double),                       \
          const char: csort__default_cmp(char),                               \
          const signed char: csort__default_cmp(signed_char),                 \
          const short: csort__default_cmp(short),                             \
          const int: csort__default_cmp(int),                                 \
          const long: csort__default_cmp(long),                               \
          const long long: csort__default_cmp(long_long),                     \
          const unsigned char: csort__default_cmp(unsigned_char),             \
          const unsigned short: csort__default_cmp(unsigned_short),           \
          const unsigned int: csort__default_cmp(unsigned_int),               \
          const unsigned long: csort__default_cmp(unsigned_long),             \
          const unsigned long long: csort__default_cmp(unsigned_long_long),   \
          const float: csort__default_cmp(float),                             \
          const double: csort__default_cmp(double),                           \
          const long double: csort__default_cmp(long_double),                 \
          default: NULL);                                                     \
      _Pragma("GCC diagnostic pop");                                          \
    } else if (ccol__is_char_ptr((x)) && !ccol__is_char_array((x))) {         \
      csort__gdcp_result = csort__default_string_comparison_proc;             \
    } else {                                                                  \
      _Pragma("GCC diagnostic push");                                         \
      _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
      if (_Generic((x), _Bool: 1, const _Bool: 1, default: 0))                \
        csort__gdcp_result = csort__default_bool_comparison_proc;             \
      _Pragma("GCC diagnostic pop");                                          \
    }                                                                         \
    csort__gdcp_result;                                                       \
  })
#else
// Evaluates to the default comparator (a ccol_comparison_proc) for the type
// of x, or NULL for a type without one, so you can sort or search without
// writing a comparator. x is used only for its type and is never evaluated, so
// any expression of the wanted type works. It covers every integral and
// floating type (const too), enums, bool (false before true), and char * and
// const char * with plain, signed or unsigned char (strcmp order, a NULL
// pointer before every string). NaN sorts after every other value and equals
// only NaN. A char array such as char name[64] gives NULL, since its bytes
// aren't a pointer; pass your own comparator for fixed-size string fields.
#define csort_get_default_comparison_proc(x)                                \
  ({                                                                        \
    ccol_comparison_proc csort__gdcp_result = NULL;                         \
    if (ccol__is_integral_type((x))) {                                      \
      csort__gdcp_result = _Generic((x),                                    \
          char: csort__default_cmp(char),                                   \
          signed char: csort__default_cmp(signed_char),                     \
          short: csort__default_cmp(short),                                 \
          int: csort__default_cmp(int),                                     \
          long: csort__default_cmp(long),                                   \
          long long: csort__default_cmp(long_long),                         \
          unsigned char: csort__default_cmp(unsigned_char),                 \
          unsigned short: csort__default_cmp(unsigned_short),               \
          unsigned int: csort__default_cmp(unsigned_int),                   \
          unsigned long: csort__default_cmp(unsigned_long),                 \
          unsigned long long: csort__default_cmp(unsigned_long_long),       \
          float: csort__default_cmp(float),                                 \
          double: csort__default_cmp(double),                               \
          long double: csort__default_cmp(long_double),                     \
          const char: csort__default_cmp(char),                             \
          const signed char: csort__default_cmp(signed_char),               \
          const short: csort__default_cmp(short),                           \
          const int: csort__default_cmp(int),                               \
          const long: csort__default_cmp(long),                             \
          const long long: csort__default_cmp(long_long),                   \
          const unsigned char: csort__default_cmp(unsigned_char),           \
          const unsigned short: csort__default_cmp(unsigned_short),         \
          const unsigned int: csort__default_cmp(unsigned_int),             \
          const unsigned long: csort__default_cmp(unsigned_long),           \
          const unsigned long long: csort__default_cmp(unsigned_long_long), \
          const float: csort__default_cmp(float),                           \
          const double: csort__default_cmp(double),                         \
          const long double: csort__default_cmp(long_double),               \
          default: NULL);                                                   \
    } else if (ccol__is_char_ptr((x)) && !ccol__is_char_array((x))) {       \
      csort__gdcp_result = csort__default_string_comparison_proc;           \
    } else if (_Generic((x), _Bool: 1, const _Bool: 1, default: 0)) {       \
      csort__gdcp_result = csort__default_bool_comparison_proc;             \
    }                                                                       \
    csort__gdcp_result;                                                     \
  })
#endif

// Internal: Evaluates to 1 when csort_get_default_comparison_proc(x) gives a
// comparator and to 0 when it gives NULL; cvec_sort uses it to refuse an
// element type with no default comparator at compile time. It's an integer
// constant expression, so it works in a _Static_assert, and x is never
// evaluated. The association lists must stay in sync with that macro. It's a
// bare _Generic chain because the common.h helpers are statement expressions
// under Clang, and the last clause keeps a char array from counting as a
// string.
#define csort__has_default_comparison_proc(x) \
  (_Generic((x),                              \
       char: 1,                               \
       signed char: 1,                        \
       short: 1,                              \
       int: 1,                                \
       long: 1,                               \
       long long: 1,                          \
       unsigned char: 1,                      \
       unsigned short: 1,                     \
       unsigned int: 1,                       \
       unsigned long: 1,                      \
       unsigned long long: 1,                 \
       float: 1,                              \
       double: 1,                             \
       long double: 1,                        \
       _Bool: 1,                              \
       default: 0) ||                         \
   (_Generic((x),                             \
        char *: 1,                            \
        const char *: 1,                      \
        signed char *: 1,                     \
        const signed char *: 1,               \
        unsigned char *: 1,                   \
        const unsigned char *: 1,             \
        default: 0) &&                        \
    !ccol__type_is_array_of_char(x)))

#pragma GCC visibility pop
