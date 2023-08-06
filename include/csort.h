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

typedef struct 
{
  void *collection;
  int length;
  uint32_t elem_size;
  csort_item_getter_t getter;
  csort_item_comparer_t comparer;
} csort_qsort_opt_t;


#define csort_swap(i, j, elem_size)                                               \
({                                                                                \
    unsigned char *ci = (unsigned char *)i;                                       \
    unsigned char *cj = (unsigned char *)j;                                       \
    unsigned char tmp;                                                            \
                                                                                  \
    for (uint32_t byte_iter = 0; byte_iter < (uint32_t)elem_size; ++byte_iter) {  \
      tmp = ci[byte_iter];                                                        \
      ci[byte_iter] = cj[byte_iter];                                              \
      cj[byte_iter] = tmp;                                                        \
    }                                                                             \
})

#define csort_qsort_partition(col, low, high, elem_size, getter, comparer) \
({                                                                         \
  int j;                                                                   \
  int i = low;                                                             \
  void *pivot;                                                             \
  void *pj;                                                                \
  if (!getter || !comparer) {                                              \
    assert(false);                                                         \
  }                                                                        \
                                                                           \
  pivot = getter(col, high);                                               \
                                                                           \
   for (j = low; j < high; j++){                                           \
    pj = getter(col, j);                                                   \
                                                                           \
    if (comparer(pj, pivot) < 0) {                                         \
        csort_swap(getter(col, i), pj, elem_size);                         \
        i++;                                                               \
      }                                                                    \
    }                                                                      \
  csort_swap(getter(col, i), getter(col, high), elem_size);                \
                                                                           \
  i;                                                                       \
})


void csort_qsort_recursion(void *col, int low, int high, uint32_t elem_size, csort_item_getter_t getter, csort_item_comparer_t comparer);

#define csort_qsort(collection, length, elem_size, getter, comparer) (csort_qsort_recursion(collection, 0, length - 1, elem_size, getter, comparer))
