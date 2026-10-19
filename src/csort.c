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

/* ========================================================================== */
/*                      DEFAULT COMPARISON PROCEDURES                         */
/* ========================================================================== */

/* Compares two stored C-string pointers (const char **) with strcmp, in
 * lexicographic order. NULL sorts before every string that isn't NULL and is
 * equal only to another NULL. This is the default comparator when the
 * element type is ccol_char_ptr.
 *
 * NULL is an ordinary member of the char * element type. An argv-style list
 * holds NULL elements, so does a sparse table of optional strings, and a
 * caller asks cvec_find() for the first such element with a NULL needle.
 * Passing a NULL straight to strcmp() dereferences a null pointer, so this
 * comparator gives NULL its own explicit position, and that position must
 * keep the result a real total order: all NULLs form one equivalence class
 * that sits strictly below every string. csort_merge() decides between
 * taking the left or the right element from comparison_proc, and that
 * decision needs such an order; a comparator that answers inconsistently for
 * even one member of the element type misorders the elements around that
 * member as well. The float comparators below have an explicit NaN rule for
 * the same reason. */
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

#define ___csort__define_default_integral_comparison_proc(type, name)   \
  int _csort_default_cmp(name)(const void *first, const void *second) { \
    type _v1;                                                           \
    type _v2;                                                           \
    memcpy(&_v1, first, sizeof(_v1));                                   \
    memcpy(&_v2, second, sizeof(_v2));                                  \
    return (_v1 > _v2) - (_v1 < _v2);                                   \
  }

/* The macro above expands to a family of typed comparison functions, one per
 * standard C integer type. Each one reads its two operands with memcpy,
 * because a caller can hand a default comparator an address that has neither
 * the alignment nor the effective type of the element (a custom getter over
 * a packed buffer, for example). A constant-size memcpy compiles to the same
 * single load as a typed read. The result is -1, 0 or +1 from the
 * (a>b)-(a<b) idiom, which avoids the undefined behavior that an integer
 * subtraction has at the edge values. */
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

/* bool has two values, and false comes before true. The operands are read
 * with memcpy for the same reason as above. The function is written out
 * instead of going through the macro because before C23 <stdbool.h> defines
 * bool as a macro, and as a macro argument it would expand into the
 * function name. */
int _csort_default_bool_comparison_proc(const void *first, const void *second) {
  _Bool _v1;
  _Bool _v2;
  memcpy(&_v1, first, sizeof(_v1));
  memcpy(&_v2, second, sizeof(_v2));
  return (_v1 > _v2) - (_v1 < _v2);
}

/* A three-way comparison for a floating-point type T with an explicit rule
 * for NaN. csort_merge() decides between the left and the right element from
 * comparison_proc, and that needs a real strict weak ordering over the whole
 * collection. IEEE 754's native `<` and `>` can't give one once a NaN is
 * present, since both are false for any comparison with a NaN operand. The
 * plain (a>b)-(a<b) idiom above, which serves every other numeric type,
 * silently reports a NaN as "equal" to every value, even to two completely
 * unrelated non-NaN values that just happen to sit on either side of the NaN
 * in the collection. So without a NaN rule it isn't only the NaN's position
 * that is unspecified, the relative order of the elements around it gets
 * corrupted too: a merge step comparing a real value with that NaN hears
 * "equal" and picks a side from that, instead of from the real values
 * waiting on the other side. For example, csort_sort() on the doubles
 * {9, NaN, 1, 4, NaN, 2, 7} gives {1, 4, 9, NaN, NaN, 2, 7}, and the non-NaN
 * subsequence (1, 4, 9, 2, 7) isn't sorted. Every single comparison
 * csort_merge() made fits *some* total order, because 0 means "equal", but
 * that order isn't well defined. Making a NaN greater than every non-NaN
 * value and equal only to another NaN gives a real total order back;
 * cmp_float_val in cbstmap uses the same rule for this library's BST map
 * keys. It's tempting to fold these comparators into the integer macro
 * above, but for a float type that brings exactly this corruption back.
 * isnan() is a type-generic macro from C99 <math.h>, so this one macro
 * covers float, double and long double without a variant per type. */
#define ___csort__define_default_float_comparison_proc(type, name)      \
  int _csort_default_cmp(name)(const void *first, const void *second) { \
    type _v1;                                                           \
    type _v2;                                                           \
    memcpy(&_v1, first, sizeof(_v1));                                   \
    memcpy(&_v2, second, sizeof(_v2));                                  \
    bool _nan1 = isnan(_v1);                                            \
    bool _nan2 = isnan(_v2);                                            \
    if (_nan1 || _nan2) {                                               \
      return _nan1 == _nan2 ? 0 : (_nan1 ? 1 : -1);                     \
    }                                                                   \
    return (_v1 > _v2) - (_v1 < _v2);                                   \
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
 * Merges the two contiguous sorted subarrays [left...mid] and
 * [mid+1...right] into one sorted array, using a temporary buffer for the
 * intermediate results.
 *
 * @param col Pointer to the collection being sorted
 * @param left First index of the left subarray
 * @param mid Last index of the left subarray; the right subarray starts at
 * mid+1.
 * @param right Last index of the right subarray
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function that gets the element at an index. It must not
 * be NULL. csort_mergesort_iterative(), the only caller, guarantees that,
 * because ___csort_merge_sort(), the only way to reach that caller, does its
 * own NULL check first.
 * @param comparison_proc Function that compares two elements, with the same
 * guarantee as getter_proc.
 * @param temp_buffer Temporary buffer for the merge, allocated beforehand by
 * the caller.
 */
static void csort_merge(void *col, size_t left, size_t mid, size_t right,
                        size_t elem_size, csort_item_getter_proc_t getter_proc,
                        ccol_comparison_proc_t comparison_proc,
                        void *temp_buffer) {
  size_t i, j, k;
  size_t n1 = mid - left + 1;  // Size of the left subarray
  size_t n2 = right - mid;     // Size of the right subarray

  // Copy both subarrays into the temporary buffer. The left one goes to
  // temp_buffer[0...n1-1] and the right one to
  // temp_buffer[n1...n1+n2-1].
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
  j = n1;    // Index for the right subarray, which starts right after the
             // left one in the buffer.
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

  // Copy whatever is left in the left subarray
  while (i < n1) {
    void *src = (unsigned char *)temp_buffer + i * elem_size;
    void *dst = getter_proc(col, k);
    memcpy(dst, src, elem_size);
    i++;
    k++;
  }

  // Copy whatever is left in the right subarray
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
 * the caller allocates the temporary buffer that all merge passes reuse.
 * Both sort drivers below share it, so they apply the same limits. It gives
 * csort_buffer_ready when the caller has to allocate length * elem_size
 * bytes and sort, csort_buffer_not_needed when the input has nothing to
 * move, and csort_buffer_refused when the sort reports failure. */
static inline __attribute__((always_inline)) csort_buffer_outcome
csort_check_sort_shape(size_t length, size_t elem_size,
                       ccol_memmgmt_procs_t *mprocs) {
  // Reject a length above ccol_max_elem_count before anything else; on a
  // 64-bit size_t that limit is 2^63. The bottom-up loop below doubles
  // curr_size (1, 2, 4 and so on) up to the largest power of two below
  // length, and stops once curr_size reaches or passes length. For a
  // length above ccol_max_elem_count that sequence gets to
  // curr_size == ccol_max_elem_count while curr_size < length is still
  // true, so the body runs one more time and curr_size *= 2 overflows all
  // the way round to 0. From there curr_size < length is true forever,
  // because 0 doubled is 0, and the loop never stops.
  //
  // The multiplication guard right below this one,
  // length > SIZE_MAX / elem_size, does NOT make this check unnecessary,
  // and elem_size == 1 is why. For elem_size == 1, SIZE_MAX / elem_size is
  // SIZE_MAX itself, so that guard rejects nothing on size and every
  // length up to SIZE_MAX gets through. Without this check such a length
  // reaches the loop above with no bound at all. For every elem_size of 2
  // or more the multiplication guard alone is enough, because
  // SIZE_MAX / elem_size is already below ccol_max_elem_count, so the
  // range this check rejects is empty for them. An elem_size of 0 is a
  // different case altogether: it never reaches the loop for any length
  // because it has its own short-circuit right below, so it isn't the
  // reason for this check either. elem_size == 1 is the one case only this
  // check covers, and a regression test needs to exercise it (see
  // csort_sort_rejects_length_exceeding_max_elem_count_for_nonzero_elem_size
  // in tests/csort/tests.c).
  //
  // Every other container in this library refuses to grow past
  // ccol_max_elem_count for the same reason (see cvector_push_back,
  // cvector_reserve and cvector_append_array), and this is csort's
  // equivalent cap on its own length parameter. ccol_max_elem_count itself
  // is never rejected: the last run of the loop body is at
  // curr_size == ccol_max_elem_count / 2, after which curr_size doubles to
  // exactly ccol_max_elem_count and the loop stops without an overflow.
  // Only a length strictly greater than the limit is rejected.
  if (length > ccol_max_elem_count) {
    return csort_buffer_refused;
  }

  // A zero-size element has no content that getter_proc or
  // comparison_proc could tell apart, and no bytes for a merge pass to
  // move, so the temp buffer would need 0 bytes whatever the length.
  // Instead of making that zero-byte allocation we handle it like the
  // trivial lengths 0 and 1: success at once, without calling
  // getter_proc, comparison_proc or the allocator. malloc(0) is
  // implementation-defined in the C standard, and a caller's custom malloc
  // asked for 0 bytes behaves the same way; it can give NULL, and that NULL
  // doesn't mean "out of memory". Allocating here would therefore risk a
  // false "out of memory" from csort_sort() for an input with nothing to
  // sort. The early return also keeps the SIZE_MAX / elem_size check below
  // safe without a separate zero guard, since elem_size is known to be
  // nonzero by the time it runs.
  if (elem_size == 0) {
    return csort_buffer_not_needed;
  }

  // Guard the temp buffer's size computation against a size_t overflow
  // before the multiplication runs. If length * elem_size wrapped around,
  // _ccol_mem_alloc would get a tiny wrapped size, while every merge pass
  // below reads and writes full elem_size elements at indices that come
  // from the real, unwrapped length, and such a pass would corrupt the heap
  // past the undersized buffer. Every other count * elem_size computation
  // in this library guards the same multiplication the same way
  // (cvector_create_full, scale_the_cvector_size_up and cvector_reserve,
  // for example), and this is the merge sort's copy of that guard.
  if (length > SIZE_MAX / elem_size) {
    return csort_buffer_refused;
  }

  // A non-NULL mprocs must hold all four function pointers, which is the
  // all-or-nothing contract that ccol_memmgmt_procs_t itself documents.
  // Every other entry point in this library that takes an allocator applies
  // that contract before touching the struct (for example
  // cvector_create_full, chmap_create_full, cbmap_create_full and
  // ctpool_create_mp). Those functions check mprocs once when they create a
  // container and reuse it afterwards. csort_sort() is different since it
  // takes a fresh mprocs on every call, and a caller can build that struct
  // by hand: csort_sort(3) documents passing mprocs directly as a usual use
  // case, so it doesn't always come from cvector_get_mprocs() of an already
  // checked container. Without this check, a caller's struct with .malloc
  // set and .free left NULL lets _ccol_mem_alloc() below succeed, and the
  // program crashes later through a NULL function pointer when
  // _ccol_mem_free() frees the temp buffer.
  //
  // The check sits right before the function's only allocation and not
  // earlier, because none of the paths that return before the allocation
  // touch mprocs. Those paths are:
  // - a col of NULL (in the callers)
  // - a length of 0 or 1 (in the callers)
  // - an elem_size of 0 (a trivial success)
  // - a length above ccol_max_elem_count and the overflow guard on
  //   length * elem_size (refusals).
  // A malformed mprocs that this call doesn't use must not change the
  // result of any of them.
  if (!_ccol_verify_memmgmt_procs(mprocs, (char **)NULL)) {
    return csort_buffer_refused;
  }

  return csort_buffer_ready;
}

/**
 * @brief An iterative, bottom-up mergesort
 *
 * Sorts with loops and no recursion. It starts from subarrays of size 1 and
 * merges them in pairs, so the sorted subarrays grow to size 2, then 4, then
 * 8 and so on until the whole array is sorted.
 *
 * @param col Pointer to the collection to sort
 * @param low First index, included in the range
 * @param high Last index, included in the range
 * @param elem_size Size of each element in bytes
 * @param getter_proc Function that gets the element at an index
 * @param comparison_proc Function that compares two elements
 * @param mprocs Memory management procedures used to allocate the buffer
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

  // Allocate the temporary merge buffer; every merge reuses it.
  void *temp_buffer = _ccol_mem_alloc(mprocs, length * elem_size);
  if (!temp_buffer) {
    // We couldn't get the temporary buffer, so there's no way to sort. No
    // merge has run yet, so the collection is still in its original order
    // and the return value alone tells the caller exactly what happened.
    return false;
  }

  // Bottom-up merge sort: start from subarrays of size 1, then merge pairs
  // into size 2, then 4, then 8 and so on.
  for (size_t curr_size = 1; curr_size < length; curr_size *= 2) {
    // Pick where the left subarray of this merge starts
    for (size_t left_start = low; left_start <= high;
         left_start += 2 * curr_size) {
      // Work out where the left subarray ends
      size_t mid = left_start + curr_size - 1;

      // When mid >= high there is no right subarray, so nothing to merge
      if (mid >= high) {
        break;
      }

      // Work out where the right subarray ends, which is
      // (left_start + 2*curr_size - 1) or high, whichever is smaller.
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
 * an array whose element i sits at base + i * elem_size. We copy out only
 * the left run and then write back from the start of the left run. While a
 * left element is waiting, the write position can't pass the read position
 * of the right run, because it trails that read position by exactly the
 * number of left elements not merged yet. So once the left run is used up,
 * whatever is left of the right run is already in its final place. On a tie
 * we take the left element, which is what keeps the sort stable.
 *
 * At every call site that csort_contiguous_passes() specializes, elem_size
 * is a compile-time constant, so each memcpy below turns into one load and
 * one store there. */
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

/* Runs all the bottom-up merge passes over [0, length). The passes have the
 * same structure as in csort_mergesort_iterative(), so both drivers make the
 * same comparisons in the same order and give the same stable result. */
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

/* The sort behind cvec_sort() and cvector_sort_with_comparison_proc(). It
 * addresses each element straight from the base pointer instead of making a
 * getter call per access. Its limits and results match those of
 * ___csort_merge_sort(), since csort_check_sort_shape() applies the same
 * checks and the passes are structured the same way. */
bool _csort_merge_sort_contiguous(void *base, size_t length, size_t elem_size,
                                  ccol_comparison_proc_t comparison_proc,
                                  ccol_memmgmt_procs_t *mprocs) {
  if (!base || length < 2) {
    return true;
  }
  if (!comparison_proc) {
    _ccol_assert(false);
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

  /* The common element widths get their own copy of the passes with a
   * constant elem_size, so every element move is a single load and store
   * instead of a memcpy call whose length is only known at run time. */
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

/* csort's public entry point. It checks the inputs and then runs the
 * iterative bottom-up mergesort, which is stable (it's not a quicksort). */
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
    _ccol_assert(false);
    return false;
  }

  return csort_mergesort_iterative(col, 0, length - 1, elem_size, getter_proc,
                                   comparison_proc, mprocs);
}
