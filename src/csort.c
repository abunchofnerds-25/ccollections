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
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/*                      DEFAULT COMPARISON PROCEDURES                         */
/* ========================================================================== */

/* Compares two stored C-string pointers (const char **) with strcmp, in
 * lexicographic order. NULL comes before every string that is not NULL and
 * is equal only to another NULL. This is the default comparator when the
 * element type is ccol_char_ptr.
 *
 * NULL is an ordinary member of the char * element type: a list in the
 * style of argv holds NULL elements, and so does a sparse table of optional
 * strings, and a caller asks cvec_find() for the first such element with a
 * NULL needle. Because a NULL that goes straight to strcmp() dereferences a
 * null pointer, this comparator gives NULL an explicit position of its own,
 * and that position must make the result a real total order: every NULL
 * goes into one equivalence class, which sits strictly below every string.
 * The take-left-or-take-right decision of csort_merge() needs such an order
 * from comparison_proc, since a comparator that answers inconsistently for
 * one single member of the element type also misorders the elements around
 * that member. The float comparators below impose an explicit rule on NaN
 * for the same reason. */
int _csort_default_string_comparison_proc(const void *first,
                                          const void *second) {
  const char *_s1;
  const char *_s2;
  memcpy(&_s1, first, sizeof(_s1));
  memcpy(&_s2, second, sizeof(_s2));
  if (!_s1 || !_s2) {
    return (_s1 != NULL) - (_s2 != NULL);
  }
  return strcmp(_s1, _s2);
}

#define ___csort__define_default_integral_comparison_proc(type, name) \
  int ___csort__get_default_integral_comparison_proc_name(name)(      \
      const void *first, const void *second) {                        \
    type _v1;                                                         \
    type _v2;                                                         \
    memcpy(&_v1, first, sizeof(_v1));                                 \
    memcpy(&_v2, second, sizeof(_v2));                                \
    return (_v1 > _v2) - (_v1 < _v2);                                 \
  }

/* The macro above expands to a family of typed comparison functions, one
 * for each standard C integer type. Each function reads its two operands
 * with memcpy, because a caller can hand a default comparator an address
 * with neither the alignment nor the effective type of the element (a
 * custom getter over a packed buffer); the constant-size memcpy compiles to
 * the same single load as a typed read. It then gives -1, 0 or +1 with the
 * (a>b)-(a<b) idiom, which prevents the undefined behavior of an integer
 * subtraction at the edge values. */
___csort__define_default_integral_comparison_proc(char, char);
___csort__define_default_integral_comparison_proc(signed char, signed_char);
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
#undef ___csort__define_default_integral_comparison_proc

/* A three-way comparison for a floating-point type T, with an explicit rule
 * for NaN. The take-left-or-take-right decision of csort_merge() needs a
 * real strict weak ordering from comparison_proc over the whole collection,
 * and the native `<` and `>` of IEEE 754 cannot give one when a NaN is
 * present, because both are false for any comparison with a NaN operand.
 * The simple (a>b)-(a<b) idiom above, which serves every other numeric
 * type, silently reports a NaN as "equal" to every value, including two
 * completely unrelated values that are not NaN and only sit on the two
 * sides of the NaN in the collection. Without an explicit rule for NaN, the
 * position of the NaN is not the only unspecified thing: the relative order
 * of the elements around it is also corrupt. A merge step that compares a
 * real value against that NaN hears "equal" and picks a side from that
 * false premise, instead of from the real values that wait on the other
 * side. For example, csort_sort() on the doubles {9, NaN, 1, 4, NaN, 2, 7}
 * gives {1, 4, 9, NaN, NaN, 2, 7}, whose subsequence without the NaN values
 * (1, 4, 9, 2, 7) is not sorted. Every comparison that csort_merge() made
 * gave a value that fits *some* total order, because 0 means "equal", but
 * that order is not well defined. A NaN that is greater than every value
 * which is not NaN, and equal only to another NaN, restores a real total
 * order; cmp_float_val in cbstmap imposes the same rule for the BST map
 * keys of this library. Because isnan() is a type-generic macro from C99
 * <math.h>, this one macro serves float, double and long double with no
 * variant for each type. */
#define ___csort__define_default_float_comparison_proc(type, name) \
  int ___csort__get_default_integral_comparison_proc_name(name)(   \
      const void *first, const void *second) {                     \
    type _v1;                                                      \
    type _v2;                                                      \
    memcpy(&_v1, first, sizeof(_v1));                              \
    memcpy(&_v2, second, sizeof(_v2));                             \
    bool _nan1 = isnan(_v1);                                       \
    bool _nan2 = isnan(_v2);                                       \
    if (_nan1 || _nan2) {                                          \
      return _nan1 == _nan2 ? 0 : (_nan1 ? 1 : -1);                \
    }                                                              \
    return (_v1 > _v2) - (_v1 < _v2);                              \
  }

___csort__define_default_float_comparison_proc(float, float);
___csort__define_default_float_comparison_proc(double, double);
___csort__define_default_float_comparison_proc(long double, long_double);
#undef ___csort__define_default_float_comparison_proc

/* ========================================================================== */
/*                         MERGESORT IMPLEMENTATION                           */
/* ========================================================================== */

/**
 * @brief Merge two sorted subarrays into one
 *
 * This function merges two contiguous sorted subarrays, [left...mid] and
 * [mid+1...right], into one sorted array, with a temporary buffer for the
 * intermediate results of the merge.
 *
 * @param col Pointer to the collection that the library sorts
 * @param left First index of the left subarray
 * @param mid Last index of the left subarray; the right subarray starts at
 * mid+1.
 * @param right Last index of the right subarray
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function that gets the element at an index. It must not
 * be NULL: csort_mergesort_iterative(), the only caller, gives that
 * guarantee, because ___csort_merge_sort(), the only way to reach that
 * caller, makes its own NULL check first.
 * @param comparison_proc Function that compares two elements, with the same
 * guarantee as getter_proc.
 * @param temp_buffer Temporary buffer for the merge, which the caller
 * allocates in advance.
 */
static void csort_merge(void *col, size_t left, size_t mid, size_t right,
                        size_t elem_size, csort_item_getter_proc_t getter_proc,
                        ccol_comparison_proc_t comparison_proc,
                        void *temp_buffer) {
  size_t i, j, k;
  size_t n1 = mid - left + 1;  // Size of the left subarray
  size_t n2 = right - mid;     // Size of the right subarray

  // Copy both subarrays into the temporary buffer:
  // the left subarray goes to temp_buffer[0...n1-1],
  // and the right subarray goes to temp_buffer[n1...n1+n2-1].
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
  i = 0;     // Index for the left subarray
  j = n1;    // Index for the right subarray, which starts after the left
             // subarray in the buffer.
  k = left;  // Index for the merged array

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

  // Copy the elements that stay in the left subarray
  while (i < n1) {
    void *src = (unsigned char *)temp_buffer + i * elem_size;
    void *dst = getter_proc(col, k);
    memcpy(dst, src, elem_size);
    i++;
    k++;
  }

  // Copy the elements that stay in the right subarray
  while (j < n1 + n2) {
    void *src = (unsigned char *)temp_buffer + j * elem_size;
    void *dst = getter_proc(col, k);
    memcpy(dst, src, elem_size);
    j++;
    k++;
  }
}

typedef enum {
  csort_buffer_ready,
  csort_buffer_not_needed,
  csort_buffer_refused
} csort_buffer_outcome;

/* Checks the shape of a sort of length elements of elem_size bytes before
 * the caller allocates the temporary buffer that every merge pass reuses.
 * Both sort drivers below share it, so they apply one set of limits. It
 * gives csort_buffer_ready when the caller must allocate
 * length * elem_size bytes and sort, csort_buffer_not_needed for an input
 * with nothing to move, and csort_buffer_refused when the sort must report
 * failure. */
static inline __attribute__((always_inline)) csort_buffer_outcome
csort_check_sort_shape(size_t length, size_t elem_size,
                       ccol_memmgmt_procs_t *mprocs) {
  // Reject a length that is more than ccol_max_elem_count before anything
  // else; that limit is 2^63 on a 64-bit size_t. The bottom-up loop below
  // doubles curr_size (1, 2, 4, and so on) up to the largest power of two
  // below length, and stops when curr_size reaches length or goes past
  // it. For a length above ccol_max_elem_count, that sequence reaches
  // curr_size == ccol_max_elem_count while curr_size < length is true,
  // so the loop body runs one more time and curr_size *= 2 then
  // overflows all the way round to 0. From then on curr_size < length is
  // true for ever, because 0 doubled is 0, and the loop can never stop.
  //
  // The multiplication guard directly below this one,
  // length > SIZE_MAX / elem_size, does NOT make this check
  // unnecessary, and elem_size == 1 is the reason: for elem_size == 1,
  // SIZE_MAX / elem_size is SIZE_MAX itself, so that guard rejects
  // nothing on the basis of size, and every length up to SIZE_MAX passes.
  // Without this check, such a length reaches the loop above with no bound
  // at all. The multiplication guard alone covers every elem_size of 2 or
  // more, because SIZE_MAX / elem_size is then below ccol_max_elem_count
  // already, so the range that this check rejects for those elem_size
  // values is empty. An elem_size of 0 is a completely different case: it
  // never reaches this loop for any length, because it has its own
  // dedicated short-circuit directly below, so it is not the reason for
  // this check either. An elem_size of 1 is the one case that this check
  // covers on its own, and a regression test must exercise that case (see
  // csort_sort_rejects_length_exceeding_max_elem_count_for_nonzero_elem_size
  // in tests/csort/tests.c).
  //
  // Every other container in this library refuses to grow past
  // ccol_max_elem_count for the same reason (see cvector_push_back,
  // cvector_reserve and cvector_append_array); this check is the equivalent
  // cap of csort on its own length parameter. It never rejects
  // ccol_max_elem_count itself: the last run of the loop body is at
  // curr_size == ccol_max_elem_count / 2, and curr_size then doubles to
  // exactly ccol_max_elem_count and stops there, with no overflow. Only a
  // length strictly greater than that limit is rejected.
  if (length > ccol_max_elem_count) {
    return csort_buffer_refused;
  }

  // An element of size zero carries no content that getter_proc or
  // comparison_proc can tell apart, and no bytes for a merge
  // pass to move, so the temp buffer would need 0 bytes for any length.
  // Instead of making that allocation of zero bytes, this code treats the
  // case like the trivial length of 0 or 1: it gives success
  // immediately, and calls neither getter_proc nor comparison_proc nor
  // the allocator. The C standard says that malloc(0) is
  // implementation-defined, and the same holds for a custom malloc from the
  // caller with a request of size 0: such a call can give NULL, and that
  // NULL does not mean "out of memory". An allocation here therefore risks a
  // false report of "out of memory" from csort_sort() for an input with
  // nothing to sort. This early return also makes the SIZE_MAX / elem_size
  // check below always safe, with no separate guard for zero,
  // because elem_size is known to be nonzero when it runs.
  if (elem_size == 0) {
    return csort_buffer_not_needed;
  }

  // Guard the size computation of the temp buffer against an overflow of
  // size_t, before the multiplication runs. A wrap-around of
  // length * elem_size gives _ccol_mem_alloc a tiny, wrapped size, while
  // every merge pass below reads and writes full elements of elem_size
  // bytes at indices that come from the real length, which did not wrap; such
  // a pass corrupts the heap memory past the buffer that is too small. Every
  // other count * elem_size computation in this library guards the same
  // multiplication in the same way (cvector_create_full,
  // scale_the_cvector_size_up and cvector_reserve, for example), and this
  // is the instance of that guard for the merge sort.
  if (length > SIZE_MAX / elem_size) {
    return csort_buffer_refused;
  }

  // An mprocs that is not NULL must hold all four function pointers: this is
  // the all-or-nothing contract that ccol_memmgmt_procs_t itself documents.
  // Each other entry point in this library that accepts an allocator applies
  // that contract before it touches such a struct (for example
  // cvector_create_full, chmap_create_full, cbmap_create_full and
  // ctpool_create_full); those functions check mprocs once, when they make a
  // container, and then use it again. csort_sort() is different, because it
  // takes a new mprocs on each call and a caller can make that struct by
  // hand: csort_sort(3) documents a direct mprocs as a usual use case, so the
  // mprocs does not always come from cvector_get_mprocs() of a container,
  // which is checked. Without this check, a struct from a caller with .malloc
  // set and .free left as NULL lets _ccol_mem_alloc() below succeed, and the
  // program crashes later, through a call to a NULL function pointer, when
  // _ccol_mem_free() frees the temp buffer.
  //
  // The check sits here, directly before the only allocation of this
  // function, and not earlier, because no path that returns before the
  // allocation touches mprocs. These paths are:
  // - a col of NULL (in the callers)
  // - a length of 0 or 1 (in the callers)
  // - an elem_size of 0 (a trivial success)
  // - a length above ccol_max_elem_count and the overflow guard on
  //   length * elem_size (refusals).
  // A malformed mprocs that this call does not use must not change the
  // result of any of these paths.
  if (!ccol_verify_memmgmt_procs(mprocs, (char **)NULL)) {
    return csort_buffer_refused;
  }

  return csort_buffer_ready;
}

/**
 * @brief An iterative, bottom-up mergesort
 *
 * This function sorts iteratively, with no recursion. It starts with
 * subarrays of size 1 and merges pairs of subarrays step by step, so the
 * sorted subarrays grow to size 2, then 4, then 8, and so on, until the
 * whole array is sorted.
 *
 * @param col Pointer to the collection to sort
 * @param low First index, which the range includes
 * @param high Last index, which the range includes
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function that gets the element at an index
 * @param comparison_proc Function that compares two elements
 * @param mprocs Memory management procedures for the allocation of the
 * buffer
 */
static bool csort_mergesort_iterative(void *col, size_t low, size_t high,
                                      size_t elem_size,
                                      csort_item_getter_proc_t getter_proc,
                                      ccol_comparison_proc_t comparison_proc,
                                      ccol_memmgmt_procs_t *mprocs) {
  if (low >= high) {
    return true;
  }

  size_t length = high - low + 1;

  csort_buffer_outcome outcome =
      csort_check_sort_shape(length, elem_size, mprocs);
  if (outcome != csort_buffer_ready) {
    return outcome == csort_buffer_not_needed;
  }

  // Allocate the temporary buffer for the merge, which every merge
  // operation reuses.
  void *temp_buffer = _ccol_mem_alloc(mprocs, length * elem_size);
  if (!temp_buffer) {
    // The allocation of the temporary buffer failed, so no sort is
    // possible. Because no merge has run yet, the collection is in its
    // original order at this point, and the caller can see exactly what
    // happened from the return value alone.
    return false;
  }

  // Bottom-up merge sort: start with subarrays of size 1, then merge pairs
  // to get size 2, then 4, then 8, and so on.
  for (size_t curr_size = 1; curr_size < length; curr_size *= 2) {
    // Pick the start of the left subarray for the merge
    for (size_t left_start = low; left_start <= high;
         left_start += 2 * curr_size) {
      // Calculate the end of the left subarray
      size_t mid = left_start + curr_size - 1;

      // There is no right subarray when mid >= high, so there is nothing to
      // merge
      if (mid >= high) {
        break;
      }

      // Calculate the end of the right subarray:
      // (left_start + 2*curr_size - 1) or high, whichever value is
      // smaller.
      size_t right_end = left_start + 2 * curr_size - 1;
      if (right_end > high) {
        right_end = high;
      }

      // Merge the two subarrays [left_start...mid] and [mid+1...right_end]
      csort_merge(col, left_start, mid, right_end, elem_size, getter_proc,
                  comparison_proc, temp_buffer);
    }
  }

  // Free the temporary buffer
  _ccol_mem_free(mprocs, temp_buffer);
  return true;
}

/* ========================================================================== */
/*                  CONTIGUOUS MERGESORT (INTERNAL ENTRY POINT)               */
/* ========================================================================== */

#ifdef RUNNING_UNIT_TESTS
static _Atomic unsigned long long g_csort_contiguous_sorts_for_tests = 0;
unsigned long long _csort_contiguous_sorts_for_tests(void) {
  return g_csort_contiguous_sorts_for_tests;
}
#endif

/* Merges the sorted run [left, mid] with the sorted run [mid + 1, right] of
 * an array whose element i sits at base + i * elem_size. Only the left run
 * is copied out, and the merge then writes back from the start of the left
 * run. The write position never passes the read position of the right run
 * while a left element is waiting, because it trails that read position by
 * exactly the number of left elements not merged yet, so the elements of
 * the right run that remain once the left run is exhausted are already in
 * their final place. A tie takes the left element, which keeps the sort
 * stable.
 *
 * elem_size is a compile-time constant at every call site that
 * csort_contiguous_passes() specializes, so each memcpy below becomes one
 * load and one store there. */
static inline __attribute__((always_inline)) void csort_merge_contiguous(
    unsigned char *base, size_t left, size_t mid, size_t right,
    size_t elem_size, ccol_comparison_proc_t comparison_proc,
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

/* Runs every bottom-up merge pass over [0, length). The pass structure is
 * the same as csort_mergesort_iterative(), so both drivers make the same
 * comparisons in the same order and give the same stable result. */
static inline __attribute__((always_inline)) void csort_contiguous_passes(
    unsigned char *base, size_t length, size_t elem_size,
    ccol_comparison_proc_t comparison_proc, unsigned char *temp_buffer) {
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

/* The sort of cvec_sort() and cvector_sort_with_comparison_proc(), which
 * addresses each element directly from the base pointer instead of making a
 * getter call for each access. Its limits and results are those of
 * ___csort_merge_sort(), because csort_check_sort_shape() applies the same
 * checks and the pass structure is identical. */
bool _csort_merge_sort_contiguous(void *base, size_t length, size_t elem_size,
                                  ccol_comparison_proc_t comparison_proc,
                                  ccol_memmgmt_procs_t *mprocs) {
  if (!base || length < 2) {
    return true;
  }
  if (!comparison_proc) {
    ccol_assert(false);
    return false;
  }

  csort_buffer_outcome outcome =
      csort_check_sort_shape(length, elem_size, mprocs);
  if (outcome != csort_buffer_ready) {
    return outcome == csort_buffer_not_needed;
  }

  unsigned char *temp_buffer = _ccol_mem_alloc(mprocs, length * elem_size);
  if (!temp_buffer) {
    return false;
  }
#ifdef RUNNING_UNIT_TESTS
  g_csort_contiguous_sorts_for_tests++;
#endif

  /* The common element widths get a copy of the passes with a constant
   * elem_size, so that every element move is a single load and store and
   * not a call to memcpy with a length known only at run time. */
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

  _ccol_mem_free(mprocs, temp_buffer);
  return true;
}

/* ========================================================================== */
/*                         PUBLIC SORT INTERFACE                              */
/* ========================================================================== */

/* The public entry point of csort. It checks the inputs and then calls the
 * iterative bottom-up mergesort, which is a stable sort and not a
 * quicksort. */
bool ___csort_merge_sort(void *col, size_t length, size_t elem_size,
                         csort_item_getter_proc_t getter_proc,
                         ccol_comparison_proc_t comparison_proc,
                         ccol_memmgmt_procs_t *mprocs) {
  if (!col) {
    return true;
  }

  if (length == 0 || length == 1) {
    return true;
  }

  if (!getter_proc || !comparison_proc) {
    ccol_assert(false);
    return false;
  }

  return csort_mergesort_iterative(col, 0, length - 1, elem_size, getter_proc,
                                   comparison_proc, mprocs);
}
