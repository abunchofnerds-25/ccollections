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
#include <stdlib.h>

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

// Stack entry to track ranges that need to be sorted
typedef struct {
  int low;
  int high;
} csort_stack_entry;

// Simple stack implementation for iterative quicksort
typedef struct {
  csort_stack_entry* entries;
  size_t capacity;
  size_t size;
} csort_stack;

static bool csort_stack_init(csort_stack* stack, size_t initial_capacity,
                             ccol_memmgmt_procs_t* mprocs) {
  stack->entries = (csort_stack_entry*)_mem_alloc(
      mprocs, initial_capacity * sizeof(csort_stack_entry));
  if (!stack->entries) {
    return false;
  }
  stack->capacity = initial_capacity;
  stack->size = 0;
  return true;
}

static void csort_stack_destroy(csort_stack* stack,
                                ccol_memmgmt_procs_t* mprocs) {
  if (stack->entries) {
    _mem_free(mprocs, stack->entries);
    stack->entries = NULL;
  }
  stack->capacity = 0;
  stack->size = 0;
}

static bool csort_stack_push(csort_stack* stack, int low, int high,
                             ccol_memmgmt_procs_t* mprocs) {
  if (stack->size >= stack->capacity) {
    // Need to grow the stack
    size_t new_capacity = stack->capacity * 2;
    csort_stack_entry* new_entries = (csort_stack_entry*)_mem_realloc(
        mprocs, stack->entries, new_capacity * sizeof(csort_stack_entry));
    if (!new_entries) {
      return false;
    }
    stack->entries = new_entries;
    stack->capacity = new_capacity;
  }

  stack->entries[stack->size].low = low;
  stack->entries[stack->size].high = high;
  stack->size++;
  return true;
}

static bool csort_stack_pop(csort_stack* stack, int* low, int* high) {
  if (stack->size == 0) {
    return false;
  }

  stack->size--;
  *low = stack->entries[stack->size].low;
  *high = stack->entries[stack->size].high;
  return true;
}

static bool csort_stack_is_empty(csort_stack* stack) {
  return stack->size == 0;
}

// Iterative quicksort implementation
static void csort_qsort_iterative(void* col, int low, int high,
                                  size_t elem_size,
                                  csort_item_getter_proc_t getter_proc,
                                  ccol_comparison_proc_t comparison_proc,
                                  csort_item_swap_proc_t swap_proc,
                                  ccol_memmgmt_procs_t* mprocs) {
  if (low >= high) {
    return;
  }

  // Initialize stack with initial capacity
  // For an array of length n, worst case depth is O(n)
  // But average case is O(log n), so start with a reasonable size
  size_t initial_capacity = 64;
  if (high - low + 1 > 1000) {
    // For larger arrays, start with bigger stack
    initial_capacity = 128;
  }

  csort_stack stack;
  if (!csort_stack_init(&stack, initial_capacity, mprocs)) {
    // Failed to allocate stack, cannot sort
    return;
  }

  // Push initial range
  if (!csort_stack_push(&stack, low, high, mprocs)) {
    csort_stack_destroy(&stack, mprocs);
    return;
  }

  // Process ranges until stack is empty
  while (!csort_stack_is_empty(&stack)) {
    int curr_low, curr_high;
    if (!csort_stack_pop(&stack, &curr_low, &curr_high)) {
      break;
    }

    // Partition the current range
    int pivot = csort_qsort_partition(col, curr_low, curr_high, elem_size,
                                      getter_proc, comparison_proc, swap_proc);

    // Push left subarray (if it has more than 1 element)
    if (pivot - 1 > curr_low) {
      if (!csort_stack_push(&stack, curr_low, pivot - 1, mprocs)) {
        // Stack push failed, cleanup and return
        csort_stack_destroy(&stack, mprocs);
        return;
      }
    }

    // Push right subarray (if it has more than 1 element)
    if (pivot + 1 < curr_high) {
      if (!csort_stack_push(&stack, pivot + 1, curr_high, mprocs)) {
        // Stack push failed, cleanup and return
        csort_stack_destroy(&stack, mprocs);
        return;
      }
    }
  }

  csort_stack_destroy(&stack, mprocs);
}

void ___csort_qsort(void* col, size_t length, size_t elem_size,
                    csort_item_getter_proc_t getter_proc,
                    ccol_comparison_proc_t comparison_proc,
                    csort_item_swap_proc_t swap_proc,
                    ccol_memmgmt_procs_t* mprocs) {
  if (!col) {
    return;
  }

  if (length == 0 || length == 1) {
    return;
  }

  if (!getter_proc || !comparison_proc) {
    assert(false);
  }

  if (!swap_proc) {
    swap_proc = csort_default_swap_proc;
  }

  csort_qsort_iterative(col, 0, length - 1, elem_size, getter_proc,
                        comparison_proc, swap_proc, mprocs);
}
