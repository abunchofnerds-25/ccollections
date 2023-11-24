/*
MIT License

Copyright (c) 2018 Danis Ozdemir

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

typedef void *(*csort_item_getter_proc_t)(void *collection, uint32_t index);
typedef void (*csort_item_swap_proc_t)(void *first, void *second, size_t elem_size);

/* Declaration of default sort functions */
void csort_default_swap_proc(void *first, void *second, size_t elem_size);
void csort_default_pointer_swap_proc(void *first, void *second, size_t elem_size __attribute__((unused)));
int csort_default_string_comparison_proc(const void *first, const void *second);
/* End of Declaration of default sort functions */

/*Declarations for internal use*/
void ___csort_qsort(void *col, size_t length, size_t elem_size, csort_item_getter_proc_t getter_proc, ccol_comparison_proc_t comparison_proc, csort_item_swap_proc_t swap_proc);
/*End of Declarations for internal use*/

#define csort_sort(collection, length, elem_size, getter_proc, comparison_proc, swap_proc) (___csort_qsort(collection, length, elem_size, getter_proc, comparison_proc, swap_proc))

/* Comparison procedure related macros */
/* Comparison procedure declaration denerator macros */
#define ___csort__get_default_integral_comparison_proc_name(name) csort_default_##name##_comparison_proc

#define ___csort__declare_default_integral_comparison_proc(type, name)                                \
int ___csort__get_default_integral_comparison_proc_name(name)(const void *first, const void *second);

___csort__declare_default_integral_comparison_proc(char, char)
___csort__declare_default_integral_comparison_proc(short, short)
___csort__declare_default_integral_comparison_proc(int, int)
___csort__declare_default_integral_comparison_proc(long, long)
___csort__declare_default_integral_comparison_proc(long long, long_long)
___csort__declare_default_integral_comparison_proc(unsigned char, unsigned_char)
___csort__declare_default_integral_comparison_proc(unsigned short, unsigned_short)
___csort__declare_default_integral_comparison_proc(unsigned int, unsigned_int)
___csort__declare_default_integral_comparison_proc(unsigned long, unsigned_long)
___csort__declare_default_integral_comparison_proc(unsigned long long, unsigned_long_long)
___csort__declare_default_integral_comparison_proc(float, float)
___csort__declare_default_integral_comparison_proc(double, double)
___csort__declare_default_integral_comparison_proc(long double, long_double)

#undef __declare_default_integral_comparison_proc
/* End of Comparison procedure declaration denerator macros */

#define csort_get_default_comparison_proc(x)                                                               \
({                                                                                                         \
  ccol_comparison_proc_t comparison_proc = NULL;                                                           \
  if(is_integral_type(x)) {                                                                                \
    comparison_proc =   _Generic((x),                                                                      \
      char: ___csort__get_default_integral_comparison_proc_name(char),                                     \
      short: ___csort__get_default_integral_comparison_proc_name(short),                                   \
      int: ___csort__get_default_integral_comparison_proc_name(int),                                       \
      long: ___csort__get_default_integral_comparison_proc_name(long),                                     \
      long long: ___csort__get_default_integral_comparison_proc_name(long_long),                           \
      unsigned char: ___csort__get_default_integral_comparison_proc_name(unsigned_char),                   \
      unsigned short: ___csort__get_default_integral_comparison_proc_name(unsigned_short),                 \
      unsigned int: ___csort__get_default_integral_comparison_proc_name(unsigned_int),                     \
      unsigned long: ___csort__get_default_integral_comparison_proc_name(unsigned_long),                   \
      unsigned long long: ___csort__get_default_integral_comparison_proc_name(unsigned_long_long),         \
      float: ___csort__get_default_integral_comparison_proc_name(float),                                   \
      double: ___csort__get_default_integral_comparison_proc_name(double),                                 \
      long double: ___csort__get_default_integral_comparison_proc_name(long_double),                       \
      const char: ___csort__get_default_integral_comparison_proc_name(char),                               \
      const short: ___csort__get_default_integral_comparison_proc_name(short),                             \
      const int: ___csort__get_default_integral_comparison_proc_name(int),                                 \
      const long: ___csort__get_default_integral_comparison_proc_name(long),                               \
      const long long: ___csort__get_default_integral_comparison_proc_name(long_long),                     \
      const unsigned char: ___csort__get_default_integral_comparison_proc_name(unsigned_char),             \
      const unsigned short: ___csort__get_default_integral_comparison_proc_name(unsigned_short),           \
      const unsigned int: ___csort__get_default_integral_comparison_proc_name(unsigned_int),               \
      const unsigned long: ___csort__get_default_integral_comparison_proc_name(unsigned_long),             \
      const unsigned long long: ___csort__get_default_integral_comparison_proc_name(unsigned_long_long),   \
      const float: ___csort__get_default_integral_comparison_proc_name(float),                             \
      const double: ___csort__get_default_integral_comparison_proc_name(double),                           \
      const long double: ___csort__get_default_integral_comparison_proc_name(long_double),                 \
      default: NULL);                                                                                      \
  } else if(is_char_ptr(x)) {                                                                              \
    comparison_proc = csort_default_string_comparison_proc;                                                \
  }                                                                                                        \
  comparison_proc;                                                                                         \
})
/* End of Comparison Procedure related macros */
