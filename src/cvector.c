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

#include <cvector.h>
#include <internal/cpow2.h>
#include <internal/cprocsintern.h>
#include <internal/csortimpl.h>
#include <internal/test_helpers/cvector.h>
#include <stdlib.h>
#include <string.h>

// The smallest capacity a vector ever has, and the factor it grows and shrinks
// by. They're global so the white-box tests can read them.
const size_t ccol__cvector_minimum_capacity = 4;
const size_t ccol__cvector_scaling_factor = 2;

// A pop shrinks the buffer once less than 1/4 of the capacity is in use. It's a
// constant of its own, separate from the minimum capacity, so tuning one never
// changes the other.
static const size_t shrink_threshold_divisor = 4;

// The vector itself. m_procs is NULL for the default allocator, an interned
// procs pointer, or a pointer to the copy that sits right after this struct in
// the same block.
struct cvector {
  size_t elem_size;
  size_t elem_count;
  size_t capacity;
  ccol_memmgmt_procs *m_procs;
  void *data_ptr;
};

// The procs copy after the struct must be aligned for its type.
_Static_assert(sizeof(struct cvector) % _Alignof(ccol_memmgmt_procs) == 0,
               "the procs copy after struct cvector must be aligned");

// Frees the data buffer and the vector's own block, so cvector_destroy() and
// the scope cleanup have one place that releases a vector. A NULL v does
// nothing. The free function is read before the block goes away, because
// m_procs can point into that block.
void cvector__destroy(cvec v) {
  if (v) {
    if (v->data_ptr) {
      ccol__mem_free(v->m_procs, v->data_ptr);
    }

    if (v->m_procs) {
      ccol_free free_func = v->m_procs->free;
      free_func(v);
    } else {
      ccol__default_mem_free(v);
    }
  }
}

// Checks the arguments of cvector_create_full before anything is allocated, so
// a bad argument never leaves a half-built vector. It gives false, with *err
// set, for an elem_size of zero, an elem_size whose first 4-element buffer
// would overflow size_t, or a procs struct with a NULL function.
static bool verify_cvector_create_inputs(size_t elem_size,
                                         ccol_memmgmt_procs *mmgt_procs,
                                         char **err) {
  if (elem_size == 0) {
    if (err) {
      *err = CCOL__ERR_STR("elem_size is zero");
    }
    return false;
  }

  if (elem_size > SIZE_MAX / ccol__cvector_minimum_capacity) {
    if (err) {
      *err = CCOL__ERR_STR(
          "elem_size too large: initial capacity allocation would overflow");
    }
    return false;
  }

  if (!ccol__verify_memmgmt_procs(mmgt_procs, err)) {
    return false;
  }

  return true;
}

// Creates an empty vector with room for 4 elements of elem_size bytes; it's
// the one constructor that every create path goes through. It gives NULL,
// with *err set, when an argument is bad or an allocation fails. An interned
// procs pointer is kept as it is; any other procs struct is copied into the
// tail of the vector's block, so a vector costs two allocations.
cvec cvector_create_full(size_t elem_size, ccol_memmgmt_procs *mmgt_procs,
                         char **err) {
  if (!verify_cvector_create_inputs(elem_size, mmgt_procs, err)) {
    return NULL;
  }

  bool copy_procs = mmgt_procs && !ccol_procs_is_interned(mmgt_procs);
  cvec v;
  v = ccol__mem_calloc(
      mmgt_procs, 1,
      sizeof(cvector) + (copy_procs ? sizeof(ccol_memmgmt_procs) : 0));
  if (!v) {
    if (err) {
      *err = CCOL__ERR_STR("failed to allocate vector container");
    }
    return NULL;
  }

  if (copy_procs) {
    v->m_procs = (ccol_memmgmt_procs *)(v + 1);
    memcpy(v->m_procs, mmgt_procs, sizeof(ccol_memmgmt_procs));
  } else {
    v->m_procs = mmgt_procs;
  }

  v->data_ptr =
      ccol__mem_alloc(mmgt_procs, ccol__cvector_minimum_capacity * elem_size);
  if (!v->data_ptr) {
    cvector__destroy(v);
    if (err) {
      *err = CCOL__ERR_STR("failed to allocate data container");
    }
    return NULL;
  }

  if (err) {
    *err = NULL;
  }

  v->capacity = ccol__cvector_minimum_capacity;
  v->elem_count = 0;
  v->elem_size = elem_size;

  return v;
}

// Gives the vector's allocator (NULL for the default one), so a caller can
// allocate related memory the same way. Asserts on a NULL v.
ccol_memmgmt_procs *cvector_get_mprocs(cvec v) {
  if (!v) {
    ccol__assert(false);
  }

  return v->m_procs;
}

// Doubles the capacity of the buffer, which keeps push_back amortized O(1).
// It gives ccol_container_full when the element count or the byte size would
// overflow, and ccol_not_enough_memory when realloc fails, so push_back can
// report the two apart. On failure the old buffer stays.
static ccol_retval scale_the_cvector_size_up(cvec v) {
  if (!v) {
    ccol__assert(false);
  }

  if (v->capacity > (ccol_max_elem_count / ccol__cvector_scaling_factor)) {
    return ccol_container_full;
  }

  size_t new_capacity = ccol__cvector_scaling_factor * v->capacity;
  if (new_capacity > SIZE_MAX / v->elem_size) {
    return ccol_container_full;
  }

  void *orig = v->data_ptr;
  v->data_ptr =
      ccol__mem_realloc(v->m_procs, v->data_ptr, new_capacity * v->elem_size);
  if (!v->data_ptr) {
    v->data_ptr = orig;
    return ccol_not_enough_memory;
  }

  v->capacity *= ccol__cvector_scaling_factor;
  return ccol_success;
}

// Halves the capacity of the buffer, never below the minimum, so a vector that
// shrank gives memory back. A failed realloc is ignored, since the old buffer
// holds the data just as well.
static void scale_the_cvector_size_down(cvec v) {
  if (!v) {
    ccol__assert(false);
  }

  if (v->capacity == ccol__cvector_minimum_capacity) {
    return;
  }

  void *orig = v->data_ptr;
  v->data_ptr = ccol__mem_realloc(
      v->m_procs, v->data_ptr,
      (v->capacity / ccol__cvector_scaling_factor) * v->elem_size);
  if (!v->data_ptr) {
    v->data_ptr = orig;
    return;
  }

  v->capacity /= ccol__cvector_scaling_factor;
}

// Copies *new_elem to the end of the vector, growing the buffer when it's
// full; it's the raw append that cvec_push uses. It gives ccol_success or the
// error of the growth. new_elem may point into v's own buffer, so before a
// growth that can move the buffer we turn it into an offset and rebuild the
// pointer afterwards.
ccol_retval cvector_push_back(cvec v, const void *new_elem) {
  if (!v) {
    ccol__assert(false);
  }

  if (!new_elem) {
    return ccol_invalid_args;
  }

  if (v->elem_count == ccol_max_elem_count) {
    return ccol_container_full;
  }

  ccol_retval result = ccol_success;

  if (v->elem_count < v->capacity) {
    memcpy((void *)((char *)v->data_ptr + v->elem_count * v->elem_size),
           new_elem, v->elem_size);
    ++v->elem_count;
  } else {
    const char *buf_start = (const char *)v->data_ptr;
    const char *buf_end = buf_start + v->capacity * v->elem_size;
    bool new_elem_aliases_self =
        (const char *)new_elem >= buf_start && (const char *)new_elem < buf_end;
    size_t new_elem_offset = new_elem_aliases_self
                                 ? (size_t)((const char *)new_elem - buf_start)
                                 : 0;

    ccol_retval grow_result = scale_the_cvector_size_up(v);
    if (grow_result == ccol_success) {
      if (new_elem_aliases_self) {
        new_elem = (const char *)v->data_ptr + new_elem_offset;
      }
      memcpy((void *)((char *)v->data_ptr + v->elem_count * v->elem_size),
             new_elem, v->elem_size);
      ++v->elem_count;
    } else {
      result = grow_result;
    }
  }

  return result;
}

// Removes the last element and copies it to *target_elem; it's the raw pop
// that cvec_pop uses. The buffer shrinks once the vector is less than 1/4
// full. An empty vector gives ccol_container_empty and leaves *target_elem
// alone.
ccol_retval cvector_pop_back(cvec v, void *target_elem) {
  if (!v) {
    ccol__assert(false);
  }

  if (!target_elem) {
    return ccol_invalid_args;
  }

  ccol_retval result = ccol_container_empty;

  if (v->elem_count > 0) {
    result = ccol_success;
    --v->elem_count;

    memcpy(target_elem,
           (void *)((char *)v->data_ptr + v->elem_count * v->elem_size),
           v->elem_size);

    if (v->elem_count < (v->capacity / shrink_threshold_divisor)) {
      scale_the_cvector_size_down(v);
    }
  }

  return result;
}

// Grows the buffer to hold at least new_capacity_count elements, rounded up to
// a power of two (at least the minimum), so a caller who knows the final size
// avoids repeated reallocations. It never shrinks, and gives false on an
// overflow or a failed realloc, with the vector unchanged.
bool cvector_reserve(cvec v, size_t new_capacity_count) {
  if (!v) {
    ccol__assert(false);
  }

  size_t refined_new_capacity_count =
      ccol__find_nearest_gte_power_of_two(new_capacity_count);
  if (refined_new_capacity_count < ccol__cvector_minimum_capacity) {
    refined_new_capacity_count = ccol__cvector_minimum_capacity;
  }

  if (refined_new_capacity_count > ccol_max_elem_count) {
    return false;
  }

  if (refined_new_capacity_count > SIZE_MAX / v->elem_size) {
    return false;
  }

  if (refined_new_capacity_count <= v->capacity) {
    return true;
  }

  void *orig = v->data_ptr;
  v->data_ptr = ccol__mem_realloc(v->m_procs, v->data_ptr,
                                  refined_new_capacity_count * v->elem_size);
  if (!v->data_ptr) {
    v->data_ptr = orig;
    return false;
  }

  v->capacity = refined_new_capacity_count;
  return true;
}

#ifdef RUNNING_UNIT_TESTS
// Counts the memmove branch of cvector_copy_into_tail, so a white-box test can
// see that an overlapping copy really took the safe path. The copied data alone
// can't tell, since memcpy may happen to work on an overlap.
size_t cvector_overlap_copy_count_for_tests = 0;
#endif

// Copies n bytes from src to the slot after the last element; elem_count isn't
// changed. It exists because an aliasing source can overlap the destination:
// we use memmove when the ranges overlap and memcpy otherwise. In test builds
// the memmove branch bumps cvector_overlap_copy_count_for_tests.
static void cvector_copy_into_tail(cvec v, const void *src, size_t n) {
  char *dst = (char *)v->data_ptr + v->elem_count * v->elem_size;
  const char *s = (const char *)src;
  bool may_overlap = dst < s + n && s < dst + n;
  if (may_overlap) {
#ifdef RUNNING_UNIT_TESTS
    cvector_overlap_copy_count_for_tests++;
#endif
    memmove(dst, src, n);
  } else {
    memcpy(dst, src, n);
  }
}

// Appends elem_count elements from arr_ptr with at most one reallocation, which
// is cheaper than a push per element. It gives false on an overflow or a failed
// reservation. arr_ptr may point into v's own buffer: Such a source must stay
// inside the reserved capacity (past it is a heap over-read), and it's turned
// into an offset before cvector_reserve so the realloc can't leave it
// dangling.
bool cvector_append_array(cvec v, const void *arr_ptr, size_t elem_count) {
  if (!v) {
    ccol__assert(false);
  }

  if (elem_count == 0) {
    return true;
  }

  if (!arr_ptr) {
    return false;
  }

  size_t new_elem_count = v->elem_count + elem_count;
  if (new_elem_count < v->elem_count || new_elem_count > ccol_max_elem_count) {
    return false;
  }

  const char *buf_start = (const char *)v->data_ptr;
  const char *buf_end = buf_start + v->capacity * v->elem_size;
  bool arr_ptr_aliases_self =
      (const char *)arr_ptr >= buf_start && (const char *)arr_ptr < buf_end;
  size_t arr_ptr_offset =
      arr_ptr_aliases_self ? (size_t)((const char *)arr_ptr - buf_start) : 0;

  if (arr_ptr_aliases_self &&
      elem_count > (size_t)(buf_end - (const char *)arr_ptr) / v->elem_size) {
    return false;
  }

  if (new_elem_count <= v->capacity) {
    cvector_copy_into_tail(v, arr_ptr, elem_count * v->elem_size);
    v->elem_count += elem_count;
    return true;
  }

  if (!cvector_reserve(v, new_elem_count)) {
    return false;
  }

  if (arr_ptr_aliases_self) {
    arr_ptr = (const char *)v->data_ptr + arr_ptr_offset;
  }

  cvector_copy_into_tail(v, arr_ptr, elem_count * v->elem_size);
  v->elem_count += elem_count;
  return true;
}

// Appends every element of v_from to v_to, so joining two vectors needs no
// loop. Asserts when either is NULL or the element sizes differ. Appending a
// vector to itself reserves the room first, so the copy never reallocates
// while it reads from the same buffer.
bool cvector_append_cvector(cvec v_to, cvec v_from) {
  if (!v_to || !v_from || v_to->elem_size != v_from->elem_size) {
    ccol__assert(false);
  }

  if (v_to == v_from) {
    size_t count = v_from->elem_count;
    size_t new_count = v_to->elem_count + count;
    if (new_count < v_to->elem_count || new_count > ccol_max_elem_count) {
      return false;
    }
    if (!cvector_reserve(v_to, new_count)) {
      return false;
    }
  }

  return cvector_append_array(v_to, v_from->data_ptr, v_from->elem_count);
}

// Gives the start of the element buffer, for code that wants a plain C array.
// Asserts on a NULL v.
void *cvector_data_ptr(cvec v) {
  if (!v) {
    ccol__assert(false);
  }

  return v->data_ptr;
}

// Gives the address of element index, or NULL when it's out of range, so a
// caller can read or change an element in place. Asserts on a NULL v.
void *cvector_at(cvec v, size_t index) {
  if (!v) {
    ccol__assert(false);
  }

  if (v->elem_count > 0 && index < v->elem_count) {
    return (void *)((char *)v->data_ptr + index * v->elem_size);
  }

  return NULL;
}

// Gives the number of elements; the struct is opaque, so this is the only way
// to read it. Asserts on a NULL v.
size_t cvector_elem_count(cvec v) {
  if (!v) {
    ccol__assert(false);
  }

  return v->elem_count;
}

// Empties the vector and shrinks the buffer to the minimum capacity, so a
// vector can be reused without a new create. When that realloc fails the
// capacity stays as it is, and the count is zero anyway.
void cvector_reset(cvec v) {
  if (!v) {
    ccol__assert(false);
  }

  if (v->capacity != ccol__cvector_minimum_capacity) {
    void *orig = v->data_ptr;
    v->data_ptr = ccol__mem_realloc(
        v->m_procs, v->data_ptr, ccol__cvector_minimum_capacity * v->elem_size);
    if (!v->data_ptr) {
      v->data_ptr = orig;
    } else {
      v->capacity = ccol__cvector_minimum_capacity;
    }
  }
  v->elem_count = 0;
}

// Sorts the elements in place with csort's stable mergesort; the two sort
// macros need a function that sees the buffer, which the opaque struct hides
// from them. The temporary buffer comes from v's allocator. It gives false,
// with v unchanged, when that allocation fails.
bool cvector__sort(cvec v, ccol_comparison_proc comparison_proc) {
  if (!v) {
    ccol__assert(false);
  }
  return csort__merge_sort_contiguous(v->data_ptr, v->elem_count, v->elem_size,
                                      comparison_proc, v->m_procs);
}

// Gives the index of the first element equal to *elem, or ccol_invalid_size,
// so a caller can search without writing the loop. A NULL cmp means memcmp
// over elem_size bytes.
size_t cvector_find(cvec v, const void *elem, ccol_comparison_proc cmp) {
  if (!v) {
    ccol__assert(false);
  }

  if (!elem) {
    return ccol_invalid_size;
  }

  for (size_t i = 0; i < v->elem_count; i++) {
    const void *current = (const char *)v->data_ptr + i * v->elem_size;
    bool match = cmp ? (cmp(current, elem) == 0)
                     : (memcmp(current, elem, v->elem_size) == 0);
    if (match) {
      return i;
    }
  }

  return ccol_invalid_size;
}

// The vector's iterator. base must be the first member, so a cmap_iterator *
// is also a cvec_iter_impl *. key_pair points at index and val_pair at the
// current element.
typedef struct {
  cmap_iterator base;
  cmap_pair key_pair_storage;
  cmap_pair val_pair_storage;
  size_t index;
  cvec vec;
  // The allocator's free function (NULL for the default). The iterator keeps
  // its own copy because a scope-exit cleanup can free it after the vector, and
  // the vector's procs, are gone.
  ccol_free free_fn;
} cvec_iter_impl;

// Frees an iterator with the free function it saved. It never goes through
// the vector, because the scope cleanup can run after the vector is gone.
static void cvec_iter_impl_free(cmap_iterator *it) {
  cvec_iter_impl *impl = (cvec_iter_impl *)it;
  ccol_free free_fn = impl->free_fn;
  if (free_fn) {
    free_fn(impl);
  } else {
    ccol__default_mem_free(impl);
  }
}

// Moves the iterator to the next element, or frees it and gives NULL after the
// last one; it's the next function that ccol_iter_next() calls.
static cmap_iterator *cvec_cmap_iter_next(cmap_iterator *it) {
  if (!it) {
    ccol__assert(false);
  }
  cvec_iter_impl *impl = (cvec_iter_impl *)it;
  size_t next_index = impl->index + 1;
  if (next_index >= impl->vec->elem_count) {
    cvec_iter_impl_free(it);
    return NULL;
  }
  impl->index = next_index;
  impl->val_pair_storage.ptr =
      (char *)impl->vec->data_ptr + next_index * impl->vec->elem_size;
  return it;
}

// Creates an iterator at the first element, so a vector works with the common
// ccol_begin() API. It's allocated with v's allocator. It gives NULL for a
// NULL or empty vector (with *err NULL) and for a failed allocation (with
// *err set).
cmap_iterator *cvector_begin_iter(cvec v, char **err) {
  if (err) {
    *err = NULL;
  }
  if (!v) {
    return NULL;
  }
  if (v->elem_count == 0) {
    return NULL;
  }
  cvec_iter_impl *impl =
      ccol__mem_calloc(v->m_procs, 1, sizeof(cvec_iter_impl));
  if (!impl) {
    if (err) {
      *err = CCOL__ERR_STR("Failed to allocate iterator");
    }
    return NULL;
  }
  impl->index = 0;
  impl->vec = v;
  impl->free_fn = v->m_procs ? v->m_procs->free : NULL;
  impl->key_pair_storage.ptr = &impl->index;
  impl->key_pair_storage.size = sizeof(size_t);
  impl->val_pair_storage.ptr = v->data_ptr;
  impl->val_pair_storage.size = v->elem_size;
  impl->base.key_pair = &impl->key_pair_storage;
  impl->base.val_pair = &impl->val_pair_storage;
  impl->base.ccol__next_fn = cvec_cmap_iter_next;
  impl->base.ccol__free_fn = cvec_iter_impl_free;
  impl->base.ccol__direct_ptr = true;
  return &impl->base;
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: Gives the capacity of the buffer, so the tests can check the
// growth and shrink thresholds that the public API doesn't show.
size_t cvector_get_capacity(cvec v) {
  if (!v) {
    ccol__assert(false);
  }

  return v->capacity;
}
#endif
