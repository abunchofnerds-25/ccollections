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

#include <csort.h>
#include <stdint.h>
#include <string.h>

/* Swap procedures region */
void csort_default_swap_proc(void* first, void* second, size_t elem_size) {
  unsigned char* ptr_btye_first = (unsigned char*)first;
  unsigned char* ptr_byte_second = (unsigned char*)second;
  unsigned char tmp;

  if (!first || !second || first == second) {
    return;
  }

  for (size_t byte_iter = 0; byte_iter < elem_size; ++byte_iter) {
    tmp = ptr_btye_first[byte_iter];
    ptr_btye_first[byte_iter] = ptr_byte_second[byte_iter];
    ptr_byte_second[byte_iter] = tmp;
  }
}

void csort_default_pointer_swap_proc(void* first, void* second,
                                     size_t elem_size __attribute__((unused))) {
  if (!first || !second || first == second) {
    return;
  }

  void* tmp = first;
  first = second;
  second = tmp;
}
/* End of Swap procedures region */

/* Comparison procedures region */
int csort_default_string_comparison_proc(const void* first,
                                         const void* second) {
  return strcmp(*(const char**)first, *(const char**)second);
}

#define ___csort__define_default_integral_comparison_proc(type, name) \
  int ___csort__get_default_integral_comparison_proc_name(name)(      \
      const void* first, const void* second) {                        \
    return (*(const type*)first > *(const type*)second) -             \
           (*(const type*)first < *(const type*)second);              \
  }

___csort__define_default_integral_comparison_proc(char, char);
___csort__define_default_integral_comparison_proc(short, short);
___csort__define_default_integral_comparison_proc(int, int);
___csort__define_default_integral_comparison_proc(long, long);
___csort__define_default_integral_comparison_proc(long long, long_long);
___csort__define_default_integral_comparison_proc(unsigned char, unsigned_char);
___csort__define_default_integral_comparison_proc(unsigned short,
                                                  unsigned_short);
___csort__define_default_integral_comparison_proc(unsigned int, unsigned_int);
___csort__define_default_integral_comparison_proc(unsigned long, unsigned_long);
___csort__define_default_integral_comparison_proc(unsigned long long,
                                                  unsigned_long_long);
___csort__define_default_integral_comparison_proc(float, float);
___csort__define_default_integral_comparison_proc(double, double);
___csort__define_default_integral_comparison_proc(long double, long_double);
#undef __define_default_integral_comparison_proc
/* End of Comparison procedures region */

static int csort_qsort_partition(void* col, int low, int high, size_t elem_size,
                                 csort_item_getter_proc_t getter_proc,
                                 ccol_comparison_proc_t comparison_proc,
                                 csort_item_swap_proc_t swap_proc) {
  int j;
  int i = low;
  void* pivot;
  void* pj;
  if (!getter_proc || !comparison_proc) {
    assert(false);
  }

  pivot = getter_proc(col, high);

  for (j = low; j < high; j++) {
    pj = getter_proc(col, j);

    if (comparison_proc(pj, pivot) < 0) {
      swap_proc(getter_proc(col, i), pj, elem_size);
      i++;
    }
  }
  swap_proc(getter_proc(col, i), getter_proc(col, high), elem_size);

  return i;
}

static void csort_qsort_recursion(void* col, int low, int high,
                                  size_t elem_size,
                                  csort_item_getter_proc_t getter_proc,
                                  ccol_comparison_proc_t comparison_proc,
                                  csort_item_swap_proc_t swap_proc) {
  if (low < high) {
    int pivot = csort_qsort_partition(col, low, high, elem_size, getter_proc,
                                      comparison_proc, swap_proc);
    csort_qsort_recursion(col, low, pivot - 1, elem_size, getter_proc,
                          comparison_proc, swap_proc);
    csort_qsort_recursion(col, pivot + 1, high, elem_size, getter_proc,
                          comparison_proc, swap_proc);
  }
}

void ___csort_qsort(void* col, size_t length, size_t elem_size,
                    csort_item_getter_proc_t getter_proc,
                    ccol_comparison_proc_t comparison_proc,
                    csort_item_swap_proc_t swap_proc) {
  if (!col) {
    return;
  }

  if (!getter_proc || !comparison_proc) {
    assert(false);
  }

  if (!swap_proc) {
    swap_proc = csort_default_swap_proc;
  }

  csort_qsort_recursion(col, 0, length - 1, elem_size, getter_proc,
                        comparison_proc, swap_proc);
}
