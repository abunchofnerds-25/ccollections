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
#include <internal/csortimpl.h>
#include <stdlib.h>
#include <string.h>

const size_t _ccol_cvector_minimum_capacity = 4;
const size_t _ccol_cvector_scaling_factor = 2;

/* This is the load-factor divisor. cvector_pop_back uses it to decide when
 * to shrink. The capacity halves when elem_count falls below capacity /
 * shrink_threshold_divisor, that is below 1/4 full. This is deliberately a
 * separate constant. It does not reuse _ccol_cvector_minimum_capacity,
 * although both values are 4 today. _ccol_cvector_minimum_capacity is the
 * floor of the capacity. This constant is the "how empty is too empty"
 * ratio. The two describe unrelated invariants. One shared value for two
 * purposes changes the shrink ratio silently, at the moment when somebody
 * tunes _ccol_cvector_minimum_capacity for its own unrelated reason.
 */
static const size_t shrink_threshold_divisor = 4;

struct cvector {
  size_t elem_size;
  size_t elem_count;
  size_t capacity;
  ccol_memmgmt_procs_t *m_procs;
  void *data_ptr;
};

/* Frees all the heap memory of the vector. This includes the data buffer and
 * the container struct. If the caller gave a custom allocator at creation,
 * this function frees the allocator struct with the allocator's own free
 * function. It does this before it frees the vector struct. The order is
 * important, because the free function pointer must still be readable at
 * that time. */
void __cvector_destroy(cvec v) {
  if (v) {
    if (v->data_ptr) {
      _ccol_mem_free(v->m_procs, v->data_ptr);
    }

    if (v->m_procs) {
      ccol_free_t free_func = v->m_procs->free;
      free_func(v->m_procs);
      free_func(v);
    } else {
      ccol_mem_free(v);
    }
  }
}

/* Guards cvector_create_full against arguments that are not logically valid.
 * An elem_size of zero corrupts every calculation of a byte offset. This
 * function therefore rejects it early, and no silent bad state spreads. It
 * rejects a large elem_size for the same reason. Such an elem_size makes
 * _ccol_cvector_minimum_capacity * elem_size overflow size_t. That product
 * is the size of the very first allocation of the backing buffer. Every
 * later path that grows the capacity already guards its own multiplication
 * of byte sizes against an overflow. scale_the_cvector_size_up and
 * cvector_reserve are those paths. The first allocation must not be the one
 * place that wraps silently to a buffer that is too small while v->elem_size
 * still holds the real, very large value. */
static bool verify_cvector_create_inputs(size_t elem_size,
                                         ccol_memmgmt_procs_t *mmgt_procs,
                                         char **err) {
  if (elem_size == 0) {
    if (err) {
      *err = CCOL_ERR_STR("elem_size is zero");
    }
    return false;
  }

  if (elem_size > SIZE_MAX / _ccol_cvector_minimum_capacity) {
    if (err) {
      *err = CCOL_ERR_STR(
          "elem_size too large: initial capacity allocation would overflow");
    }
    return false;
  }

  if (!ccol_verify_memmgmt_procs(mmgt_procs, err)) {
    return false;
  }

  return true;
}

/* Allocates and initializes a new vector. The caller gives the element size
 * and an optional custom allocator. The first backing buffer holds
 * _ccol_cvector_minimum_capacity elements. On any allocation failure, this
 * function tears down the vector that it built in part and gives NULL. It
 * also writes a static error string into *err, when the caller gives an err.
 */
cvec cvector_create_full(size_t elem_size, ccol_memmgmt_procs_t *mmgt_procs,
                         char **err) {
  if (!verify_cvector_create_inputs(elem_size, mmgt_procs, err)) {
    return NULL;
  }

  cvec v;
  v = _ccol_mem_calloc(mmgt_procs, 1, sizeof(cvector));
  if (!v) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate vector container");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(v, mmgt_procs, err)) {
    _ccol_mem_free(mmgt_procs, v);
    return NULL;
  }

  v->data_ptr =
      _ccol_mem_alloc(mmgt_procs, _ccol_cvector_minimum_capacity * elem_size);
  if (!v->data_ptr) {
    __cvector_destroy(v);
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate data container");
    }
    return NULL;
  }

  if (err) {
    *err = NULL;
  }

  v->capacity = _ccol_cvector_minimum_capacity;
  v->elem_count = 0;
  v->elem_size = elem_size;

  return v;
}

/* Gives the custom allocator struct that the vector stores. Gives NULL when
 * the vector uses the default malloc and free path. */
ccol_memmgmt_procs_t *cvector_get_mprocs(cvec v) {
  if (!v) {
    ccol_assert(false);
  }

  return v->m_procs;
}

/* Doubles the capacity of the backing buffer. Gives ccol_container_full when
 * the vector is already at the architectural ceiling for growth. That
 * ceiling is ccol_max_elem_count, or the largest byte size that this
 * elem_size can address. It is a structural limit and not a temporary
 * allocation failure. This function gives ccol_not_enough_memory only when
 * the realloc itself fails. A caller must keep the two apart. cvector_push_back
 * is the one caller of this function. It shows this difference to its own
 * caller, and it does not report ccol_not_enough_memory for both cases. On a
 * failure of the reallocation, this function restores the original pointer,
 * so the vector stays usable. */
static ccol_retval_t scale_the_cvector_size_up(cvec v) {
  if (!v) {
    ccol_assert(false);
  }

  if (v->capacity > (ccol_max_elem_count / _ccol_cvector_scaling_factor)) {
    return ccol_container_full;  // elem_count would overflow
  }

  size_t new_capacity = _ccol_cvector_scaling_factor * v->capacity;
  if (new_capacity > SIZE_MAX / v->elem_size) {
    return ccol_container_full;  // byte size would overflow
  }

  void *orig = v->data_ptr;
  v->data_ptr =
      _ccol_mem_realloc(v->m_procs, v->data_ptr, new_capacity * v->elem_size);
  if (!v->data_ptr) {
    v->data_ptr = orig;
    return ccol_not_enough_memory;
  }

  v->capacity *= _ccol_cvector_scaling_factor;
  return ccol_success;
}

/* Halves the capacity of the backing buffer, but never below
 * _ccol_cvector_minimum_capacity. On a failure of the reallocation, this
 * function restores the original pointer silently. A shrink is a best
 * effort, and a failure corrupts none of the data that is there. */
static void scale_the_cvector_size_down(cvec v) {
  if (!v) {
    ccol_assert(false);
  }

  if (v->capacity == _ccol_cvector_minimum_capacity) {
    return;
  }

  void *orig = v->data_ptr;
  v->data_ptr = _ccol_mem_realloc(
      v->m_procs, v->data_ptr,
      (v->capacity / _ccol_cvector_scaling_factor) * v->elem_size);
  if (!v->data_ptr) {
    v->data_ptr = orig;
    return;
  }

  v->capacity /= _ccol_cvector_scaling_factor;
}

/* Appends a copy of *new_elem at the end of the vector. It grows the backing
 * buffer by _ccol_cvector_scaling_factor when the current capacity is full.
 * It gives ccol_not_enough_memory when the reallocation for that growth
 * fails. It gives ccol_container_full when the vector is already at the
 * architectural ceiling for growth. See scale_the_cvector_size_up.
 *
 * new_elem can alias into the backing buffer of v. An example is a pointer
 * from cvector_at(v, i) or from cvector_data_ptr(v). cvector_append_array
 * gives the same guarantee. When the vector must grow,
 * scale_the_cvector_size_up can realloc the buffer to a new address. Without
 * care, that move leaves new_elem dangling before this function reads it.
 * This function therefore converts an aliasing new_elem to a byte offset
 * before the growth. It computes the address again from the new value of
 * v->data_ptr afterwards. It never dereferences new_elem across the possible
 * move. */
ccol_retval_t cvector_push_back(cvec v, const void *new_elem) {
  if (!v) {
    ccol_assert(false);
  }

  if (!new_elem) {
    return ccol_invalid_args;
  }

  if (v->elem_count == ccol_max_elem_count) {
    return ccol_container_full;
  }

  ccol_retval_t result = ccol_success;

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

    ccol_retval_t grow_result = scale_the_cvector_size_up(v);
    if (grow_result == ccol_success) {
      if (new_elem_aliases_self) {
        // v->data_ptr can move. Compute new_elem again from the new
        // buffer.
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

/* Removes the last element and copies it into *target_elem. The buffer
 * shrinks when the element count falls below capacity /
 * _ccol_cvector_minimum_capacity. The vector then does not hold too much
 * memory. This function gives ccol_container_empty for an empty vector, and
 * it does not touch *target_elem in that case. */
ccol_retval_t cvector_pop_back(cvec v, void *target_elem) {
  if (!v) {
    ccol_assert(false);
  }

  if (!target_elem) {
    return ccol_invalid_args;
  }

  ccol_retval_t result = ccol_container_empty;

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

/* Allocates enough backing storage in advance for new_capacity_count
 * elements or more. This function rounds the count that the caller asks for
 * up to the nearest power of two. The minimum is
 * _ccol_cvector_minimum_capacity. A later push_back call therefore meets a
 * capacity boundary that the caller can predict. This function does nothing
 * when the current capacity is enough for the request. */
bool cvector_reserve(cvec v, size_t new_capacity_count) {
  if (!v) {
    ccol_assert(false);
  }

  size_t refined_new_capacity_count =
      _ccol_find_nearest_gte_power_of_two(new_capacity_count);
  if (refined_new_capacity_count < _ccol_cvector_minimum_capacity) {
    refined_new_capacity_count = _ccol_cvector_minimum_capacity;
  }

  if (refined_new_capacity_count > ccol_max_elem_count) {
    // That is too much. Reject it.
    return false;
  }

  if (refined_new_capacity_count > SIZE_MAX / v->elem_size) {
    return false;  // The byte size would overflow
  }

  if (refined_new_capacity_count <= v->capacity) {
    // The vector has the capacity that the caller asks for already
    return true;
  }

  // The vector needs more capacity
  void *orig = v->data_ptr;
  v->data_ptr = _ccol_mem_realloc(v->m_procs, v->data_ptr,
                                  refined_new_capacity_count * v->elem_size);
  if (!v->data_ptr) {
    // The reallocation failed. Restore the original pointer.
    v->data_ptr = orig;
    return false;
  }

  // The reallocation succeeded. All went well.
  v->capacity = refined_new_capacity_count;
  return true;
}

/* Copies n bytes from src into the tail slot of the vector. That slot is at
 * v->data_ptr + v->elem_count * v->elem_size. This function does not touch
 * v->elem_count itself. arr_ptr can alias anywhere inside the backing buffer
 * of v. It can also alias reserved capacity past v->elem_count that holds no
 * live element yet. The source range can overlap the destination range when
 * the elem_count from the caller is large enough. memcpy is not safe for an
 * overlap. This function therefore uses memmove when the two ranges really
 * do overlap. It keeps memcpy for the very common case with no alias. */
#ifdef RUNNING_UNIT_TESTS
/* Counts the cvector_copy_into_tail calls that take the memmove branch, the
 * branch that is safe for an overlap. A white-box test can then assert that
 * the code really runs that branch. Without the counter, the test can look
 * only at the output of the copy. That is not enough: a real memcpy is not
 * portable, and it can leave a given overlapping range without visible
 * corruption even when the code skips the safe branch. "The data came out
 * right" is therefore no proof on its own that the code took the safe path.
 * This counter is not part of the public API. */
size_t cvector_overlap_copy_count_for_tests = 0;
#endif

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

/* Appends elem_count elements from arr_ptr to the vector in one operation.
 * It reserves more capacity when this is necessary. The overflow check on
 * the total element count catches two problems. It catches a wrap-around of
 * a size_t, and it catches a count above ccol_max_elem_count.
 *
 * arr_ptr can alias into the backing buffer of v. An example is a pointer
 * from cvector_data_ptr(v) or from cvector_at(v, i). The growth of the
 * vector can need a realloc inside cvector_reserve. That call can move the
 * backing buffer to a new address. Without care, the move leaves arr_ptr
 * dangling before this function reads it. This function therefore converts
 * an aliasing arr_ptr to a byte offset before the reservation. It computes
 * the address again from the new value of v->data_ptr afterwards. It never
 * dereferences arr_ptr across the possible move. The comment on
 * cvector_copy_into_tail describes the separate hazard of two ranges that
 * overlap, which this function also guards against.
 *
 * A source range that aliases also has the reserved extent of the buffer as
 * its bound. This function gives false when such a range runs past that
 * bound. Without the bound, an offset near the top of the reserved region
 * plus a large elem_count reads off the end of the allocation. */
bool cvector_append_array(cvec v, const void *arr_ptr, size_t elem_count) {
  if (!v) {
    ccol_assert(false);
  }

  if (elem_count == 0) {
    return true;
  }

  if (!arr_ptr) {
    return false;
  }

  size_t new_elem_count = v->elem_count + elem_count;
  if (new_elem_count < v->elem_count || new_elem_count > ccol_max_elem_count) {
    // Either new_elem_count wrapped, or it is greater than
    // ccol_max_elem_count
    return false;
  }

  // Does arr_ptr point into the backing buffer of this vector? Two separate
  // things depend on the answer. A source range that aliases the buffer gets
  // the reserved extent of that buffer as its bound, directly below. An
  // aliasing arr_ptr must also survive a realloc further down.
  const char *buf_start = (const char *)v->data_ptr;
  const char *buf_end = buf_start + v->capacity * v->elem_size;
  bool arr_ptr_aliases_self =
      (const char *)arr_ptr >= buf_start && (const char *)arr_ptr < buf_end;
  size_t arr_ptr_offset =
      arr_ptr_aliases_self ? (size_t)((const char *)arr_ptr - buf_start) : 0;

  // A source range that aliases can run past v->elem_count into reserved
  // capacity that holds no live element yet. It must not run past the
  // reservation itself. The bytes after buf_end belong to no allocation of
  // this library, so a read of them is a heap over-read. Only the case with
  // an alias can have a bound. A source array of the caller has no extent
  // that this function can see. This is true for every C array that a caller
  // passes as a bare pointer.
  if (arr_ptr_aliases_self &&
      elem_count * v->elem_size > (size_t)(buf_end - (const char *)arr_ptr)) {
    return false;
  }

  if (new_elem_count <= v->capacity) {
    // The vector has enough space already
    cvector_copy_into_tail(v, arr_ptr, elem_count * v->elem_size);
    v->elem_count += elem_count;
    return true;
  }

  // The capacity is not enough, so try to reserve more. The realloc inside
  // cvector_reserve can move the buffer. This is why the code below computes
  // an aliasing arr_ptr again from the byte offset above. It does not
  // dereference arr_ptr across the move.
  if (!cvector_reserve(v, new_elem_count)) {
    return false;
  }

  if (arr_ptr_aliases_self) {
    // v->data_ptr can move. Compute arr_ptr again from the new buffer.
    arr_ptr = (const char *)v->data_ptr + arr_ptr_offset;
  }

  // The vector now has enough space
  cvector_copy_into_tail(v, arr_ptr, elem_count * v->elem_size);
  v->elem_count += elem_count;
  return true;
}

/* Appends all the elements of v_from to v_to. It calls
 * cvector_append_array to do this. Both vectors must have the same
 * elem_size. Two different values are a fatal assertion, because they show a
 * programming error at the call site. */
bool cvector_append_cvector(cvec v_to, cvec v_from) {
  if (!v_to || !v_from || v_to->elem_size != v_from->elem_size) {
    ccol_assert(false);
  }

  if (v_to == v_from) {
    /* Reserve in advance, so that cvector_append_array never reallocates
     * in the middle of the copy. Without this, realloc can free the source
     * buffer while arr_ptr still points into it. */
    size_t count = v_from->elem_count;
    size_t new_count = v_to->elem_count + count;
    /* This is the same overflow check that cvector_append_array makes.
     * Without it, a wrap-around of a size_t here lets this addition report
     * less capacity than the call really needs. No real program reaches
     * this, because elem_count must already be at ccol_max_elem_count, and
     * such a vector is enormous. The later check inside
     * cvector_append_array must still not be the only guard between this
     * call and an incorrect reservation. */
    if (new_count < v_to->elem_count || new_count > ccol_max_elem_count) {
      return false;
    }
    if (!cvector_reserve(v_to, new_count)) {
      return false;
    }
  }

  return cvector_append_array(v_to, v_from->data_ptr, v_from->elem_count);
}

/* Gives a raw pointer to the first element of the contiguous backing buffer.
 * A caller must not keep this pointer across a push_back call or an append
 * call. Those calls can realloc the buffer to a different address. */
void *cvector_data_ptr(cvec v) {
  if (!v) {
    ccol_assert(false);
  }

  return v->data_ptr;
}

/* Gives a pointer to the element at the given index. Gives NULL when the
 * index is out of range. The guard for an empty vector is elem_count > 0. It
 * makes sure that the code never compares the index against an
 * uninitialized elem_count of zero. */
void *cvector_at(cvec v, size_t index) {
  if (!v) {
    ccol_assert(false);
  }

  if (v->elem_count > 0 && index < v->elem_count) {
    return (void *)((char *)v->data_ptr + index * v->elem_size);
  }

  return NULL;
}

/* Gives the number of live elements that the vector holds now. */
size_t cvector_elem_count(cvec v) {
  if (!v) {
    ccol_assert(false);
  }

  return v->elem_count;
}

/* Removes all the elements. It then tries to shrink the backing buffer to
 * _ccol_cvector_minimum_capacity and free memory. It sets the element count
 * to zero, whether the reallocation succeeds or fails. It skips the
 * reallocation completely when the capacity is already
 * _ccol_cvector_minimum_capacity, because such a shrink does nothing.
 * scale_the_cvector_size_down has the same early-return guard. */
void cvector_reset(cvec v) {
  if (!v) {
    ccol_assert(false);
  }

  if (v->capacity != _ccol_cvector_minimum_capacity) {
    void *orig = v->data_ptr;
    v->data_ptr = _ccol_mem_realloc(
        v->m_procs, v->data_ptr, _ccol_cvector_minimum_capacity * v->elem_size);
    if (!v->data_ptr) {
      // The capacity must stay unchanged when the reallocation fails.
      v->data_ptr = orig;
    } else {
      v->capacity = _ccol_cvector_minimum_capacity;
    }
  }
  v->elem_count = 0;
}

/* The sort of cvec_sort() and cvector_sort_with_comparison_proc(). The
 * elements are contiguous, so the sort addresses them from data_ptr
 * directly and makes no call for each element access. */
bool _cvector_sort(cvec v, ccol_comparison_proc_t comparison_proc) {
  if (!v) {
    ccol_assert(false);
  }
  return _csort_merge_sort_contiguous(v->data_ptr, v->elem_count, v->elem_size,
                                      comparison_proc, v->m_procs);
}

/* Does a linear scan for the first element that is equal to *elem. When cmp
 * is NULL, the comparison falls back to memcmp over elem_size bytes. Gives
 * the zero-based index of the first match. Gives ccol_invalid_size when
 * there is no match. */
size_t cvector_find(cvec v, const void *elem, ccol_comparison_proc_t cmp) {
  if (!v) {
    ccol_assert(false);
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

/* The internal iterator. The public header does NOT expose it. The
 * cmap_iterator base field MUST come first. A cmap_iterator * that points at
 * that field is then also a valid cvec_iter_impl_t *. */
typedef struct {
  cmap_iterator base;         /* public face: key_pair, val_pair, fn ptrs */
  cmap_pair key_pair_storage; /* index storage; base.key_pair points here */
  cmap_pair
      val_pair_storage; /* element ptr storage; base.val_pair points here */
  size_t index;         /* key_pair_storage.ptr points to this field */
  cvec vec;             /* back-reference, to move on */
  /* The free function of the allocator that made this iterator, or NULL for
   * the default one. The iterator keeps its own copy because the scope-exit
   * cleanup of ccol_iter_declare can free an iterator that a loop left early
   * after the caller has already destroyed the vector, and with it the procs
   * struct that the vector owns. Freeing the iterator must therefore never
   * read the vector. */
  ccol_free_t free_fn;
} cvec_iter_impl_t;

static void cvec_iter_impl_free(cmap_iterator *it) {
  cvec_iter_impl_t *impl = (cvec_iter_impl_t *)it;
  ccol_free_t free_fn = impl->free_fn;
  if (free_fn) {
    free_fn(impl);
  } else {
    ccol_mem_free(impl);
  }
}

static cmap_iterator *cvec_cmap_iter_next(cmap_iterator *it) {
  if (!it) {
    ccol_assert(false);
  }
  cvec_iter_impl_t *impl = (cvec_iter_impl_t *)it;
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
  cvec_iter_impl_t *impl =
      _ccol_mem_calloc(v->m_procs, 1, sizeof(cvec_iter_impl_t));
  if (!impl) {
    if (err) {
      *err = CCOL_ERR_STR("Failed to allocate iterator");
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
  impl->base._next_fn = cvec_cmap_iter_next;
  impl->base._free_fn = cvec_iter_impl_free;
  impl->base._direct_ptr = true;
  return &impl->base;
}

#ifdef RUNNING_UNIT_TESTS
/* Gives the internal capacity of the backing buffer to a white-box unit
 * test. Such a test checks the thresholds for growth and for a shrink. This
 * function is not part of the public API. */
size_t cvector_get_capacity(cvec v) {
  if (!v) {
    ccol_assert(false);
  }

  return v->capacity;
}
#endif
