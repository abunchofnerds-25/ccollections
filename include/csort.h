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
typedef int (*csort_item_comparer_t)(void *first, void *second);

int csort_qsort_default_integral_type_comparer(void *first, void *second);
int csort_qsort_default_string_comparer(void *first, void *second);
void csort_qsort_recursion(void *col, int low, int high, uint32_t elem_size, csort_item_getter_t getter, csort_item_comparer_t comparer);

#define csort_qsort_ex(collection, length, elem_size, getter, comparer) (csort_qsort_recursion(collection, 0, length - 1, elem_size, getter, comparer))


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

// #define csort_qsort(collection, length)
