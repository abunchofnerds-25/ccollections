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

#include <csort.h>
#include <internal/csortimpl.h>
#include <internal/test_helpers/csortimpl.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Compares two stored string pointers with strcmp(). It's the default
// comparator that csort_get_default_comparison_proc() gives for char *
// elements. first and second are the addresses of the stored pointers. NULL is
// part of the char * element type (argv-style lists, a cvec_find() with a NULL
// needle), so every NULL equals every other NULL and sorts below every string;
// the merge needs a real total order, as the NaN rule below does.
int csort__default_string_comparison_proc(const void *first,
                                          const void *second) {
  const char *s1;
  const char *s2;
  memcpy(&s1, first, sizeof(s1));
  memcpy(&s2, second, sizeof(s2));
  if (!s1 || !s2) {
    return (s1 != NULL) - (s2 != NULL);
  }
  return strcmp(s1, s2);
}

// Defines the default comparator of an integer type. The operands are read with
// memcpy because a getter can hand over an address with neither the alignment
// nor the effective type of the element; a constant-size memcpy is one load. (a
// > b) - (a < b) gives -1, 0 or 1 with no overflow at the edge values.
#define csort__define_default_integral_comparison_proc(type, name)      \
  int csort__default_cmp(name)(const void *first, const void *second) { \
    type v1;                                                            \
    type v2;                                                            \
    memcpy(&v1, first, sizeof(v1));                                     \
    memcpy(&v2, second, sizeof(v2));                                    \
    return (v1 > v2) - (v1 < v2);                                       \
  }

csort__define_default_integral_comparison_proc(char, char);
csort__define_default_integral_comparison_proc(signed char, signed_char);
csort__define_default_integral_comparison_proc(short, short);
csort__define_default_integral_comparison_proc(int, int);
csort__define_default_integral_comparison_proc(long, long);
csort__define_default_integral_comparison_proc(long long, long_long);
csort__define_default_integral_comparison_proc(unsigned char, unsigned_char);
csort__define_default_integral_comparison_proc(unsigned short, unsigned_short);
csort__define_default_integral_comparison_proc(unsigned int, unsigned_int);
csort__define_default_integral_comparison_proc(unsigned long, unsigned_long);
csort__define_default_integral_comparison_proc(unsigned long long,
                                               unsigned_long_long);
#undef csort__define_default_integral_comparison_proc

// Compares two bool values, false before true; the default comparator for bool.
// It's written out because before C23 bool is a macro and would expand inside
// the name the macro above builds.
int csort__default_bool_comparison_proc(const void *first, const void *second) {
  _Bool v1;
  _Bool v2;
  memcpy(&v1, first, sizeof(v1));
  memcpy(&v2, second, sizeof(v2));
  return (v1 > v2) - (v1 < v2);
}

// Defines the default comparator of a floating type. NaN is greater than every
// other value and equal only to NaN. A plain (a > b) - (a < b) calls NaN equal
// to everything, so a merge step picks a side from that answer and the real
// values around the NaN come out unsorted ({9, NaN, 1, 4, NaN, 2, 7} gives {1,
// 4, 9, NaN, NaN, 2, 7}). isnan() is type-generic, so one macro serves all
// three types.
#define csort__define_default_float_comparison_proc(type, name)         \
  int csort__default_cmp(name)(const void *first, const void *second) { \
    type v1;                                                            \
    type v2;                                                            \
    memcpy(&v1, first, sizeof(v1));                                     \
    memcpy(&v2, second, sizeof(v2));                                    \
    bool nan1 = isnan(v1);                                              \
    bool nan2 = isnan(v2);                                              \
    if (nan1 || nan2) {                                                 \
      return nan1 == nan2 ? 0 : (nan1 ? 1 : -1);                        \
    }                                                                   \
    return (v1 > v2) - (v1 < v2);                                       \
  }

csort__define_default_float_comparison_proc(float, float);
csort__define_default_float_comparison_proc(double, double);
csort__define_default_float_comparison_proc(long double, long_double);
#undef csort__define_default_float_comparison_proc

// Merges the sorted runs [left, mid] and [mid + 1, right] of col, for the
// getter-based mergesort. Both runs are copied into temp_buffer, then merged
// back through getter_proc. Taking the left element on a tie keeps the sort
// stable. The procs are never NULL here; csort__merge_sort() checks them.
static void csort_merge(void *col, size_t left, size_t mid, size_t right,
                        size_t elem_size, csort_item_getter_proc getter_proc,
                        ccol_comparison_proc comparison_proc,
                        void *temp_buffer) {
  size_t i, j, k;
  size_t n1 = mid - left + 1;
  size_t n2 = right - mid;

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

  i = 0;
  j = n1;
  k = left;

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

  while (i < n1) {
    void *src = (unsigned char *)temp_buffer + i * elem_size;
    void *dst = getter_proc(col, k);
    memcpy(dst, src, elem_size);
    i++;
    k++;
  }

  while (j < n1 + n2) {
    void *src = (unsigned char *)temp_buffer + j * elem_size;
    void *dst = getter_proc(col, k);
    memcpy(dst, src, elem_size);
    j++;
    k++;
  }
}

// What csort_check_sort_shape() tells a driver: allocate and sort, return true
// with nothing to do, or return false.
typedef enum {
  csort_buffer_ready,
  csort_buffer_not_needed,
  csort_buffer_refused
} csort_buffer_outcome;

// Decides whether a sort of length elements of elem_size bytes may allocate its
// buffer and run. Both drivers call it, so they apply the same limits. A length
// above ccol_max_elem_count is refused, because the doubling run size would
// overflow to 0 and the passes would never end (the overflow guard can't catch
// that when elem_size is 1). An elem_size of 0 needs no buffer and is a
// success, since malloc(0) may give NULL and read as "out of memory". length *
// elem_size must not overflow, or the passes write past a short buffer. An
// incomplete mprocs is refused here, right before the only allocation, so it
// can't change the result of a call that never allocates.
static inline __attribute__((always_inline)) csort_buffer_outcome
csort_check_sort_shape(size_t length, size_t elem_size,
                       ccol_memmgmt_procs *mprocs) {
  if (length > ccol_max_elem_count) {
    return csort_buffer_refused;
  }

  if (elem_size == 0) {
    return csort_buffer_not_needed;
  }

  if (length > SIZE_MAX / elem_size) {
    return csort_buffer_refused;
  }

  if (!ccol__verify_memmgmt_procs(mprocs, (char **)NULL)) {
    return csort_buffer_refused;
  }

  return csort_buffer_ready;
}

// Sorts the range [low, high] of col with the bottom-up mergesort; it's the
// body of csort__merge_sort(). Runs of size 1, 2, 4 and so on are merged in
// pairs through one buffer of length * elem_size. Gives false when the shape is
// refused or the allocation fails, before any merge, so the collection is
// unchanged.
static bool csort_mergesort_iterative(void *col, size_t low, size_t high,
                                      size_t elem_size,
                                      csort_item_getter_proc getter_proc,
                                      ccol_comparison_proc comparison_proc,
                                      ccol_memmgmt_procs *mprocs) {
  if (low >= high) {
    return true;
  }

  size_t length = high - low + 1;

  csort_buffer_outcome outcome =
      csort_check_sort_shape(length, elem_size, mprocs);
  if (outcome != csort_buffer_ready) {
    return outcome == csort_buffer_not_needed;
  }

  void *temp_buffer = ccol__mem_alloc(mprocs, length * elem_size);
  if (!temp_buffer) {
    return false;
  }

  for (size_t curr_size = 1; curr_size < length; curr_size *= 2) {
    for (size_t left_start = low; left_start <= high;
         left_start += 2 * curr_size) {
      size_t mid = left_start + curr_size - 1;

      if (mid >= high) {
        break;
      }

      size_t right_end = left_start + 2 * curr_size - 1;
      if (right_end > high) {
        right_end = high;
      }

      csort_merge(col, left_start, mid, right_end, elem_size, getter_proc,
                  comparison_proc, temp_buffer);
    }
  }

  ccol__mem_free(mprocs, temp_buffer);
  return true;
}

#ifdef RUNNING_UNIT_TESTS
// Counts the contiguous sorts that allocated a buffer, for the tests.
static _Atomic unsigned long long g_csort_contiguous_sorts_for_tests = 0;
// Gives the counter above, so the vector tests can prove that cvec_sort takes
// the contiguous path.
unsigned long long csort__contiguous_sorts_for_tests(void) {
  return g_csort_contiguous_sorts_for_tests;
}
#endif

// Merges the runs [left, mid] and [mid + 1, right] of a contiguous array, for
// the contiguous sort. Only the left run is copied out. The write position
// trails the read position of the right run by the number of left elements not
// merged yet, so it never overtakes it, and when the left run is used up the
// rest of the right run is already in place. Ties take the left element, so the
// sort is stable.
static inline __attribute__((always_inline)) void csort_merge_contiguous(
    unsigned char *base, size_t left, size_t mid, size_t right,
    size_t elem_size, ccol_comparison_proc comparison_proc,
    unsigned char *temp_buffer) {
  size_t left_bytes = (mid - left + 1) * elem_size;
  unsigned char *out = base + left * elem_size;
  memcpy(temp_buffer, out, left_bytes);
  unsigned char *l = temp_buffer;
  unsigned char *l_end = temp_buffer + left_bytes;
  unsigned char *r = base + (mid + 1) * elem_size;
  unsigned char *r_end = base + (right + 1) * elem_size;
  while (l < l_end && r < r_end) {
    if (comparison_proc(l, r) <= 0) {
      memcpy(out, l, elem_size);
      l += elem_size;
    } else {
      memcpy(out, r, elem_size);
      r += elem_size;
    }
    out += elem_size;
  }
  if (l < l_end) {
    memcpy(out, l, (size_t)(l_end - l));
  }
}

// Runs every bottom-up merge pass over [0, length) of a contiguous array, in
// the same order as csort_mergesort_iterative(), so both drivers give the same
// result. It's inlined once per element width.
static inline __attribute__((always_inline)) void csort_contiguous_passes(
    unsigned char *base, size_t length, size_t elem_size,
    ccol_comparison_proc comparison_proc, unsigned char *temp_buffer) {
  size_t high = length - 1;
  for (size_t curr_size = 1; curr_size < length; curr_size *= 2) {
    for (size_t left_start = 0; left_start <= high;
         left_start += 2 * curr_size) {
      size_t mid = left_start + curr_size - 1;
      if (mid >= high) {
        break;
      }
      size_t right_end = left_start + 2 * curr_size - 1;
      if (right_end > high) {
        right_end = high;
      }
      csort_merge_contiguous(base, left_start, mid, right_end, elem_size,
                             comparison_proc, temp_buffer);
    }
  }
}

// Sorts the length elements at base in place, stably; it's the sort behind
// cvec_sort() and cvector_sort_with_comparison_proc(), which avoids a getter
// call per element access. The buffer comes from mprocs. Gives true on success
// or when there's nothing to do, false when csort_check_sort_shape() refuses or
// the allocation fails. The widths 1, 2, 4, 8 and 16 get their own copy of the
// passes with a constant elem_size, so every element move is one load and one
// store. Bumps the test counter when it allocates.
bool csort__merge_sort_contiguous(void *base, size_t length, size_t elem_size,
                                  ccol_comparison_proc comparison_proc,
                                  ccol_memmgmt_procs *mprocs) {
  if (!base || length < 2) {
    return true;
  }
  if (!comparison_proc) {
    ccol__assert(false);
    return false;
  }

  csort_buffer_outcome outcome =
      csort_check_sort_shape(length, elem_size, mprocs);
  if (outcome != csort_buffer_ready) {
    return outcome == csort_buffer_not_needed;
  }

  unsigned char *temp_buffer = ccol__mem_alloc(mprocs, length * elem_size);
  if (!temp_buffer) {
    return false;
  }
#ifdef RUNNING_UNIT_TESTS
  g_csort_contiguous_sorts_for_tests++;
#endif

  unsigned char *b = base;
  switch (elem_size) {
    case 1:
      csort_contiguous_passes(b, length, 1, comparison_proc, temp_buffer);
      break;
    case 2:
      csort_contiguous_passes(b, length, 2, comparison_proc, temp_buffer);
      break;
    case 4:
      csort_contiguous_passes(b, length, 4, comparison_proc, temp_buffer);
      break;
    case 8:
      csort_contiguous_passes(b, length, 8, comparison_proc, temp_buffer);
      break;
    case 16:
      csort_contiguous_passes(b, length, 16, comparison_proc, temp_buffer);
      break;
    default:
      csort_contiguous_passes(b, length, elem_size, comparison_proc,
                              temp_buffer);
      break;
  }

  ccol__mem_free(mprocs, temp_buffer);
  return true;
}

// Sorts col in place through getter_proc and comparison_proc; it's the body of
// csort_sort(), for collections that aren't one contiguous array. A NULL col or
// a length of 0 or 1 gives true without looking at the procs; otherwise a NULL
// proc asserts. Gives the result of csort_mergesort_iterative().
bool csort__merge_sort(void *col, size_t length, size_t elem_size,
                       csort_item_getter_proc getter_proc,
                       ccol_comparison_proc comparison_proc,
                       ccol_memmgmt_procs *mprocs) {
  if (!col) {
    return true;
  }

  if (length == 0 || length == 1) {
    return true;
  }

  if (!getter_proc || !comparison_proc) {
    ccol__assert(false);
    return false;
  }

  return csort_mergesort_iterative(col, 0, length - 1, elem_size, getter_proc,
                                   comparison_proc, mprocs);
}
