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


void csort_qsort_recursion(void *col, int low, int high, uint32_t elem_size, csort_item_getter_t getter, csort_item_comparer_t comparer);

#define csort_sort(collection, length, elem_size, getter, comparer) (csort_qsort_recursion(collection, 0, length - 1, elem_size, getter, comparer))

/* Decleration of default comparers */
int csort_default_string_comparer(const void *first, const void *second);

#define __declare_default_integral_comparer(type, name)                              \
int csort_default_##name##_comparer(const void *first, const void *second);

__declare_default_integral_comparer(char, char)
__declare_default_integral_comparer(short, short)
__declare_default_integral_comparer(int, int)
__declare_default_integral_comparer(long, long)
__declare_default_integral_comparer(long long, long_long)
__declare_default_integral_comparer(unsigned char, unsigned_char)
__declare_default_integral_comparer(unsigned short, unsigned_short)
__declare_default_integral_comparer(unsigned int, unsigned_int)
__declare_default_integral_comparer(unsigned long, unsigned_long)
__declare_default_integral_comparer(unsigned long long, unsigned_long_long)
__declare_default_integral_comparer(float, float)
__declare_default_integral_comparer(double, double)
__declare_default_integral_comparer(long double, long_double)

#undef __declare_default_integral_comparer
/* End of Decleration of default comparers */

#define __get_collection_getter(col)    \
  _Generic((col),                       \
    cvec: (csort_item_getter_t)cvec_at, \
    default: NULL                       \
  )


#define __get_collection_comparer(col)                            \
  ({                                                              \
    csort_item_comparer_t comparer;                               \
    if (is_integral_type(typeof(*(col##__cvec_type_var)))) {      \
    } else if (is_char_ptr(data)) {                               \
    }                                                             \
    comparer;                                                     \
  })


#define csort_qsort(collection, length) \
  ({                                    \
    size_t size = sizeof(typeof(*collection##__cvec_type_var));   \
    csort_qsort_recursion(                                        \
      collection,                                                 \
      0,                                                          \
      length - 1,                                                 \
      size,                                                       \
      __get_collection_getter(collection),                        \
      comparer)                                                   \
  })


/* Default comparer getter Macros */
#define __get_default_integral_comparer_name(name) csort_default_##name##_comparer

#define __csort_default_get_comparer(x)                                                     \
({                                                                                          \
  csort_item_comparer_t comparer = NULL;                                                    \
  if(is_integral_type(x)) {                                                                 \
    comparer =   _Generic((x),                                                              \
      char: __get_default_integral_comparer_name(char),                                     \
      short: __get_default_integral_comparer_name(short),                                   \
      int: __get_default_integral_comparer_name(int),                                       \
      long: __get_default_integral_comparer_name(long),                                     \
      long long: __get_default_integral_comparer_name(long_long),                           \
      unsigned char: __get_default_integral_comparer_name(unsigned_char),                   \
      unsigned short: __get_default_integral_comparer_name(unsigned_short),                 \
      unsigned int: __get_default_integral_comparer_name(unsigned_int),                     \
      unsigned long: __get_default_integral_comparer_name(unsigned_long),                   \
      unsigned long long: __get_default_integral_comparer_name(unsigned_long_long),         \
      float: __get_default_integral_comparer_name(float),                                   \
      double: __get_default_integral_comparer_name(double),                                 \
      long double: __get_default_integral_comparer_name(long_double),                       \
      const char: __get_default_integral_comparer_name(char),                               \
      const short: __get_default_integral_comparer_name(short),                             \
      const int: __get_default_integral_comparer_name(int),                                 \
      const long: __get_default_integral_comparer_name(long),                               \
      const long long: __get_default_integral_comparer_name(long_long),                     \
      const unsigned char: __get_default_integral_comparer_name(unsigned_char),             \
      const unsigned short: __get_default_integral_comparer_name(unsigned_short),           \
      const unsigned int: __get_default_integral_comparer_name(unsigned_int),               \
      const unsigned long: __get_default_integral_comparer_name(unsigned_long),             \
      const unsigned long long: __get_default_integral_comparer_name(unsigned_long_long),   \
      const float: __get_default_integral_comparer_name(float),                             \
      const double: __get_default_integral_comparer_name(double),                           \
      const long double: __get_default_integral_comparer_name(long_double),                 \
      default: NULL);                                                                       \
  } else if(is_char_ptr(x)) {                                                               \
    comparer = csort_default_string_comparer;                                               \
  }                                                                                         \
  comparer;                                                                                 \
})

/* End of Default comparer getter Macros */


// #define csort_qsort(collection, length)
