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
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/*                      DEFAULT COMPARISON PROCEDURES                         */
/* ========================================================================== */

/* Compares two C-string pointers (const char **) lexicographically via strcmp.
 * Used as the default comparator when the element type is ccol_char_ptr. */
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

/* The macro above expands to a family of typed comparison functions for all
 * standard C integer types. Each function dereferences its void* arguments to
 * the concrete type and returns -1/0/+1 using the (a>b)-(a<b) idiom which
 * avoids undefined behaviour from integer subtraction on edge values. */
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

/* Three-way comparison for a floating-point type T, with an explicit NaN
 * rule: csort_merge()'s take-left/take-right merge decision requires
 * comparison_proc to supply a genuine strict weak ordering over the whole
 * collection, but IEEE 754's native `<`/`>` cannot provide that whenever a
 * NaN is present, since both are false for any comparison involving a NaN
 * operand; the naive (a>b)-(a<b) idiom used for every other numeric type
 * above therefore silently reports a NaN as "equal" to every other value it
 * is compared against, including two completely unrelated non-NaN values
 * that merely happen to straddle it in the collection being sorted. Left
 * unfixed, this does not merely leave the NaN's own position unspecified: it
 * corrupts the relative order of the surrounding non-NaN elements too, since
 * a merge step comparing a real value against that NaN is told they are
 * "equal" and picks a side based on that false premise rather than on the
 * real values still waiting on the other side. Reproduced directly against
 * the built library before this fix: csort_sort() on
 * {9, NaN, 1, 4, NaN, 2, 7} (double) produced {1, 4, 9, NaN, NaN, 2, 7};
 * the non-NaN subsequence 1, 4, 9, 2, 7 is not sorted, even though every
 * comparison csort_merge() performed returned a value consistent with *some*
 * total order (0 for "equal"), just not a well-defined one. Ordering NaN as
 * greater than every non-NaN value, and equal only to another NaN, restores
 * a real total order; this mirrors cbstmap's own cmp_float_val, which
 * documents and fixes the identical defect for this library's BST map keys.
 * isnan() is a type-generic (C99 <math.h>) macro, so this one macro serves
 * float/double/long double without a per-type variant. */
#define ___csort__define_default_float_comparison_proc(type, name) \
  int ___csort__get_default_integral_comparison_proc_name(name)(   \
      const void *first, const void *second) {                     \
    type _v1 = *(const type *)first;                               \
    type _v2 = *(const type *)second;                              \
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
 * Merges two contiguous sorted subarrays [left...mid] and [mid+1...right]
 * into a single sorted array. Uses a temporary buffer to hold intermediate
 * results during the merge operation.
 *
 * @param col Pointer to collection being sorted
 * @param left Starting index of left subarray
 * @param mid Ending index of left subarray (mid+1 is start of right subarray)
 * @param right Ending index of right subarray
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function to get element at index; must be non-NULL,
 * guaranteed by the only caller (csort_mergesort_iterative(), itself only
 * ever reached via ___csort_merge_sort() after that function's own NULL
 * check)
 * @param comparison_proc Function to compare two elements; same non-NULL
 * guarantee as getter_proc
 * @param temp_buffer Pre-allocated temporary buffer for merging
 */
static void csort_merge(void *col, size_t left, size_t mid, size_t right,
                        size_t elem_size, csort_item_getter_proc_t getter_proc,
                        ccol_comparison_proc_t comparison_proc,
                        void *temp_buffer) {
  size_t i, j, k;
  size_t n1 = mid - left + 1;  // Size of left subarray
  size_t n2 = right - mid;     // Size of right subarray

  // Copy both subarrays to temporary buffer
  // Left subarray goes to temp_buffer[0...n1-1]
  // Right subarray goes to temp_buffer[n1...n1+n2-1]
  for (i = 0; i < n1; i++) {
    void *src = getter_proc(col, left + i);
    void *dst = (unsigned char *)temp_buffer + i * elem_size;
    mem_cpy(dst, src, elem_size);
  }

  for (j = 0; j < n2; j++) {
    void *src = getter_proc(col, mid + 1 + j);
    void *dst = (unsigned char *)temp_buffer + (n1 + j) * elem_size;
    mem_cpy(dst, src, elem_size);
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
      mem_cpy(dst, elem_i, elem_size);
      i++;
    } else {
      mem_cpy(dst, elem_j, elem_size);
      j++;
    }
    k++;
  }

  // Copy any remaining elements from left subarray
  while (i < n1) {
    void *src = (unsigned char *)temp_buffer + i * elem_size;
    void *dst = getter_proc(col, k);
    mem_cpy(dst, src, elem_size);
    i++;
    k++;
  }

  // Copy any remaining elements from right subarray
  while (j < n1 + n2) {
    void *src = (unsigned char *)temp_buffer + j * elem_size;
    void *dst = getter_proc(col, k);
    mem_cpy(dst, src, elem_size);
    j++;
    k++;
  }
}

/**
 * @brief Iterative mergesort implementation using bottom-up approach
 *
 * Implements mergesort iteratively without recursion by starting with
 * subarrays of size 1 and progressively merging pairs to create larger
 * sorted subarrays (size 2, 4, 8, etc.) until the entire array is sorted.
 *
 * @param col Pointer to collection to sort
 * @param low Starting index (inclusive)
 * @param high Ending index (inclusive)
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function to get element at index
 * @param comparison_proc Function to compare two elements
 * @param mprocs Memory management procedures for buffer allocation
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

  // Reject any length exceeding max_elem_count (2^63 on a 64-bit size_t)
  // before doing anything else. The bottom-up loop below doubles curr_size
  // (1, 2, 4, ..., up to the largest power of two less than length) and
  // relies on curr_size eventually meeting or exceeding length to stop. For
  // length > max_elem_count, that doubling sequence reaches
  // curr_size == max_elem_count while curr_size < length still holds, so the
  // loop body runs once more and curr_size *= 2 then overflows all the way
  // around to 0; after which curr_size < length holds forever (0 doubled
  // is still 0) and the loop can never terminate.
  //
  // The multiplication guard just below this one (length > SIZE_MAX /
  // elem_size) does NOT make this check redundant, and elem_size == 1 is
  // specifically why: for elem_size == 1, SIZE_MAX / elem_size == SIZE_MAX
  // itself, so that guard rejects nothing on the basis of size at all;
  // every length up to SIZE_MAX sails through it and would reach the
  // doubling loop above with no bound whatsoever were this check not here.
  // (elem_size >= 2 is fully covered by the multiplication guard alone,
  // since SIZE_MAX / elem_size is then already below max_elem_count, so this
  // check's rejection range for those elem_size values is empty; elem_size
  // == 0 is a different, unrelated case entirely; it never reaches this
  // loop at all regardless of length, via its own dedicated short-circuit
  // immediately below, so it is not what makes this check load-bearing
  // either.) elem_size == 1 is therefore the one case this check exists to
  // cover on its own, and the one a regression test for it must actually
  // exercise (see csort_sort_rejects_length_exceeding_max_elem_count_for_
  // nonzero_elem_size in tests/csort/tests.c).
  //
  // Every other container in this library already refuses to grow past
  // max_elem_count for the same underlying reason (see cvector_push_back,
  // cvector_reserve, cvector_append_array); csort had no equivalent cap on
  // its own length parameter. max_elem_count itself is never reachable by
  // this check (the doubling sequence's last body execution is at
  // curr_size == max_elem_count / 2, which then doubles to exactly
  // max_elem_count and stops there without overflowing), so only lengths
  // strictly greater than it are rejected.
  if (length > max_elem_count) {
    return false;
  }

  // A zero-sized element carries no distinguishable content for
  // getter_proc/comparison_proc to read or for a merge pass to move: the
  // temp buffer's own required size is 0 bytes regardless of length. Rather
  // than actually issuing that zero-byte allocation, treat this the same way
  // as the trivial length 0/1 case and return success immediately, without
  // ever calling getter_proc, comparison_proc, or the allocator. malloc(0)
  // (or a caller-supplied custom malloc's own handling of a size-0 request)
  // is explicitly implementation-defined by the C standard (it may return
  // NULL without that meaning "out of memory"), so allocating here would
  // risk csort_sort() spuriously reporting OOM for an input that has nothing
  // to sort in the first place. This also makes the SIZE_MAX / elem_size
  // check just below always safe to evaluate without a separate zero guard,
  // since elem_size is now known to be nonzero by the time it runs.
  if (elem_size == 0) {
    return true;
  }

  // Guard the temp-buffer size computation against size_t overflow before
  // ever multiplying: length * elem_size wrapping around would hand
  // _mem_alloc a tiny, wrapped size while every merge pass below still
  // writes/reads full elem_size-sized elements at indices derived from the
  // real, un-wrapped length, corrupting heap memory past the undersized
  // buffer. Every other count * elem_size computation in this library
  // (cvector_create_full, scale_the_cvector_size_up, cvector_reserve, ...)
  // already guards this same multiplication the same way; this was the one
  // remaining unguarded one in the merge sort's own call chain.
  if (length > SIZE_MAX / elem_size) {
    return false;
  }

  // A non-NULL mprocs must have all four function pointers populated (the
  // same "all-or-nothing" contract ccol_memmgmt_procs_t itself documents,
  // and the one every other allocator-accepting entry point in this library
  // (cvector_create_full, chmap_create_full, cbmap_create_full,
  // ctpool_create_full, ...) already enforces before ever touching such a
  // struct). Unlike those, mprocs here is not validated once at a
  // container's own construction time and then reused: csort_sort() takes a
  // fresh, potentially caller-hand-built mprocs on every single call, and
  // README.md documents passing one directly (not merely via a container's
  // already-validated cvector_get_mprocs()) as a first-class use case. Left
  // unchecked, a caller-supplied struct with e.g. .malloc set but .free left
  // NULL let _mem_alloc() below succeed and only crashed later, calling
  // through a NULL function pointer, when _mem_free() tried to release the
  // temp buffer; reproduced directly against the built library before this
  // check was added. Checked here, immediately before the only allocation
  // this function ever performs, rather than earlier: every trivial-success
  // path above (col == NULL, length 0/1, elem_size == 0, length exceeding
  // max_elem_count, the length * elem_size overflow guard just above) never
  // touches mprocs at all, so none of those documented contracts should
  // start failing merely because an mprocs this particular call was never
  // going to use happens to be malformed.
  if (!ccol_verify_memmgmt_procs(mprocs, (char **)NULL)) {
    return false;
  }

  // Allocate temporary buffer for merging
  // This buffer will be reused for all merge operations
  void *temp_buffer = _mem_alloc(mprocs, length * elem_size);
  if (!temp_buffer) {
    // Failed to allocate temporary buffer, cannot sort. The collection is
    // still in its original, unmodified order at this point (no merge has
    // run yet), so the caller can tell exactly what happened from the
    // return value alone.
    return false;
  }

  // Bottom-up merge sort: start with subarrays of size 1,
  // then merge pairs to get size 2, then 4, 8, etc.
  for (size_t curr_size = 1; curr_size < length; curr_size *= 2) {
    // Pick starting point of left subarray to be merged
    for (size_t left_start = low; left_start <= high;
         left_start += 2 * curr_size) {
      // Calculate the end of left subarray
      size_t mid = left_start + curr_size - 1;

      // If there's no right subarray (mid >= high), nothing to merge
      if (mid >= high) {
        break;
      }

      // Calculate the end of right subarray
      // It should be (left_start + 2*curr_size - 1) or high, whichever is
      // smaller
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
  _mem_free(mprocs, temp_buffer);
  return true;
}

/* ========================================================================== */
/*                         PUBLIC SORT INTERFACE                              */
/* ========================================================================== */

/* Public entry point for csort. Validates inputs, then delegates to the
 * iterative bottom-up mergesort. Despite the name inherited from early
 * development, this is a stable mergesort, not quicksort. */
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
