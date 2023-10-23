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

typedef void *(*csort_item_getter_t)(void *collection, uint32_t index);
typedef int (*csort_item_comparer_t)(const void *first, const void *second);
typedef void (*csort_item_swapper_t)(void *first, void *second, uint32_t elem_size);

/* Declaration of default sort functions */
void csort_default_swapper(void *first, void *second, uint32_t elem_size);
int csort_default_string_comparer(const void *first, const void *second);
/* End of Declaration of default sort functions */

/*Declarations for internal use*/
void ___csort_qsort_recursion(void *col, int low, int high, uint32_t elem_size, csort_item_getter_t getter, csort_item_comparer_t comparer, csort_item_swapper_t swapper);
/*End of Declarations for internal use*/

#define csort_sort(collection, length, elem_size, getter, comparer, swapper) (___csort_qsort_recursion(collection, 0, length - 1, elem_size, getter, comparer, swapper))

/* Comparer related macros */
/* Comparer declaration denerator macros */
#define ___csort__get_default_integral_comparer_name(name) csort_default_##name##_comparer

#define ___csort__declare_default_integral_comparer(type, name)                               \
int ___csort__get_default_integral_comparer_name(name)(const void *first, const void *second);

___csort__declare_default_integral_comparer(char, char)
___csort__declare_default_integral_comparer(short, short)
___csort__declare_default_integral_comparer(int, int)
___csort__declare_default_integral_comparer(long, long)
___csort__declare_default_integral_comparer(long long, long_long)
___csort__declare_default_integral_comparer(unsigned char, unsigned_char)
___csort__declare_default_integral_comparer(unsigned short, unsigned_short)
___csort__declare_default_integral_comparer(unsigned int, unsigned_int)
___csort__declare_default_integral_comparer(unsigned long, unsigned_long)
___csort__declare_default_integral_comparer(unsigned long long, unsigned_long_long)
___csort__declare_default_integral_comparer(float, float)
___csort__declare_default_integral_comparer(double, double)
___csort__declare_default_integral_comparer(long double, long_double)

#undef __declare_default_integral_comparer
/* End of Comparer declaration denerator macros */

#define csort_get_default_comparer(x)                                                               \
({                                                                                                  \
  csort_item_comparer_t comparer = NULL;                                                            \
  if(is_integral_type(x)) {                                                                         \
    comparer =   _Generic((x),                                                                      \
      char: ___csort__get_default_integral_comparer_name(char),                                     \
      short: ___csort__get_default_integral_comparer_name(short),                                   \
      int: ___csort__get_default_integral_comparer_name(int),                                       \
      long: ___csort__get_default_integral_comparer_name(long),                                     \
      long long: ___csort__get_default_integral_comparer_name(long_long),                           \
      unsigned char: ___csort__get_default_integral_comparer_name(unsigned_char),                   \
      unsigned short: ___csort__get_default_integral_comparer_name(unsigned_short),                 \
      unsigned int: ___csort__get_default_integral_comparer_name(unsigned_int),                     \
      unsigned long: ___csort__get_default_integral_comparer_name(unsigned_long),                   \
      unsigned long long: ___csort__get_default_integral_comparer_name(unsigned_long_long),         \
      float: ___csort__get_default_integral_comparer_name(float),                                   \
      double: ___csort__get_default_integral_comparer_name(double),                                 \
      long double: ___csort__get_default_integral_comparer_name(long_double),                       \
      const char: ___csort__get_default_integral_comparer_name(char),                               \
      const short: ___csort__get_default_integral_comparer_name(short),                             \
      const int: ___csort__get_default_integral_comparer_name(int),                                 \
      const long: ___csort__get_default_integral_comparer_name(long),                               \
      const long long: ___csort__get_default_integral_comparer_name(long_long),                     \
      const unsigned char: ___csort__get_default_integral_comparer_name(unsigned_char),             \
      const unsigned short: ___csort__get_default_integral_comparer_name(unsigned_short),           \
      const unsigned int: ___csort__get_default_integral_comparer_name(unsigned_int),               \
      const unsigned long: ___csort__get_default_integral_comparer_name(unsigned_long),             \
      const unsigned long long: ___csort__get_default_integral_comparer_name(unsigned_long_long),   \
      const float: ___csort__get_default_integral_comparer_name(float),                             \
      const double: ___csort__get_default_integral_comparer_name(double),                           \
      const long double: ___csort__get_default_integral_comparer_name(long_double),                 \
      default: NULL);                                                                               \
  } else if(is_char_ptr(x)) {                                                                       \
    comparer = csort_default_string_comparer;                                                       \
  }                                                                                                 \
  comparer;                                                                                         \
})
/* End of Comparer related macros */
