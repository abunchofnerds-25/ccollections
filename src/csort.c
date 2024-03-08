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
#include <stdlib.h>
#include <string.h>

/* Swap procedures region */
void csort_default_swap_proc(void *first, void *second, size_t elem_size) {
  unsigned char *ptr_btye_first = (unsigned char *)first;
  unsigned char *ptr_byte_second = (unsigned char *)second;
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

void csort_default_pointer_swap_proc(void *first, void *second,
                                     size_t elem_size __attribute__((unused))) {
  if (!first || !second || first == second) {
    return;
  }

  void *tmp = first;
  first = second;
  second = tmp;
}
/* End of Swap procedures region */

/* Comparison procedures region */
int csort_default_string_comparison_proc(const void *first,
                                         const void *second) {
  return strcmp(*(const char **)first, *(const char **)second);
}

#define ___csort__define_default_integral_comparison_proc(type, name) \
  int ___csort__get_default_integral_comparison_proc_name(name)(      \
      const void *first, const void *second) {                        \
    return (*(const type *)first > *(const type *)second) -           \
           (*(const type *)first < *(const type *)second);            \
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

// Merge two sorted subarrays [left...mid] and [mid+1...right]
static void csort_merge(void *col, int left, int mid, int right,
                        size_t elem_size, csort_item_getter_proc_t getter_proc,
                        ccol_comparison_proc_t comparison_proc,
                        void *temp_buffer) {
  int i, j, k;
  int n1 = mid - left + 1;  // Size of left subarray
  int n2 = right - mid;     // Size of right subarray

  if (!getter_proc || !comparison_proc) {
    assert(false);
  }

  // Copy both subarrays to temporary buffer
  // Left subarray goes to temp_buffer[0...n1-1]
  // Right subarray goes to temp_buffer[n1...n1+n2-1]
  for (i = 0; i < n1; i++) {
    void *src = getter_proc(col, left + i);
    void *dst = (unsigned char *)temp_buffer + i * elem_size;
    memcpy(dst, src, elem_size);
  }

  for (j = 0; j < n2; j++) {
    void *src = getter_proc(col, mid + 1 + j);
    void *dst = (unsigned char *)temp_buffer + (n1 + j) * elem_size;
    memcpy(dst, src, elem_size);
  }

  // Merge the two subarrays back into col
  i = 0;     // Index for left subarray
  j = n1;    // Index for right subarray (starts after left subarray in buffer)
  k = left;  // Index for merged array

  while (i < n1 && j < n1 + n2) {
    void *elem_i = (unsigned char *)temp_buffer + i * elem_size;
    void *elem_j = (unsigned char *)temp_buffer + j * elem_size;
    void *dst = getter_proc(col, k);

    if (comparison_proc(elem_i, elem_j) <= 0) {
      memcpy(dst, elem_i, elem_size);
      i++;
    } else {
      memcpy(dst, elem_j, elem_size);
      j++;
    }
    k++;
  }

  // Copy any remaining elements from left subarray
  while (i < n1) {
    void *src = (unsigned char *)temp_buffer + i * elem_size;
    void *dst = getter_proc(col, k);
    memcpy(dst, src, elem_size);
    i++;
    k++;
  }

  // Copy any remaining elements from right subarray
  while (j < n1 + n2) {
    void *src = (unsigned char *)temp_buffer + j * elem_size;
    void *dst = getter_proc(col, k);
    memcpy(dst, src, elem_size);
    j++;
    k++;
  }
}

// Iterative merge sort implementation using bottom-up approach
static void csort_mergesort_iterative(void *col, int low, int high,
                                      size_t elem_size,
                                      csort_item_getter_proc_t getter_proc,
                                      ccol_comparison_proc_t comparison_proc,
                                      csort_item_swap_proc_t swap_proc
                                      __attribute__((unused)),
                                      ccol_memmgmt_procs_t *mprocs) {
  if (low >= high) {
    return;
  }

  int length = high - low + 1;

  // Allocate temporary buffer for merging
  // This buffer will be reused for all merge operations
  void *temp_buffer = _mem_alloc(mprocs, length * elem_size);
  if (!temp_buffer) {
    // Failed to allocate temporary buffer, cannot sort
    return;
  }

  // Bottom-up merge sort: start with subarrays of size 1,
  // then merge pairs to get size 2, then 4, 8, etc.
  for (int curr_size = 1; curr_size < length; curr_size *= 2) {
    // Pick starting point of left subarray to be merged
    for (int left_start = low; left_start <= high;
         left_start += 2 * curr_size) {
      // Calculate the end of left subarray
      int mid = left_start + curr_size - 1;

      // If there's no right subarray (mid >= high), nothing to merge
      if (mid >= high) {
        break;
      }

      // Calculate the end of right subarray
      // It should be (left_start + 2*curr_size - 1) or high, whichever is
      // smaller
      int right_end = left_start + 2 * curr_size - 1;
      if (right_end > high) {
        right_end = high;
      }

      // Merge the two subarrays [left_start...mid] and [mid+1...right_end]
      csort_merge(col, left_start, mid, right_end, elem_size, getter_proc,
                  comparison_proc, temp_buffer);
    }
  }

  // Free the temporary buffer
  _mem_free(mprocs, temp_buffer);
}

void ___csort_qsort(void *col, size_t length, size_t elem_size,
                    csort_item_getter_proc_t getter_proc,
                    ccol_comparison_proc_t comparison_proc,
                    csort_item_swap_proc_t swap_proc,
                    ccol_memmgmt_procs_t *mprocs) {
  if (!col) {
    return;
  }

  if (length == 0 || length == 1) {
    return;
  }

  if (!getter_proc || !comparison_proc) {
    assert(false);
  }

  // Note: swap_proc is not used in merge sort but kept for API compatibility
  csort_mergesort_iterative(col, 0, length - 1, elem_size, getter_proc,
                            comparison_proc, swap_proc, mprocs);
}
