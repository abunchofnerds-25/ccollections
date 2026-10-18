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

const size_t _ccol_cvector_minimum_capacity = 4;
const size_t _ccol_cvector_scaling_factor = 2;

/* The load-factor divisor that cvector_pop_back uses to decide when to
 * shrink: the capacity halves once elem_count drops below capacity /
 * shrink_threshold_divisor, i.e. below 1/4 full. We keep it as its own
 * constant on purpose instead of reusing _ccol_cvector_minimum_capacity,
 * even though both are 4. _ccol_cvector_minimum_capacity is the floor of the
 * capacity and this one is the "how empty is too empty" ratio, and those two
 * have nothing to do with each other. If they shared one value, tuning
 * _ccol_cvector_minimum_capacity for its own reasons would quietly change
 * the shrink ratio too.
 */
static const size_t shrink_threshold_divisor = 4;

struct cvector {
  size_t elem_size;
  size_t elem_count;
  size_t capacity;
  ccol_memmgmt_procs_t *m_procs;
  void *data_ptr;
};

/* A copy of the caller's procs sits right after the struct in the same
 * block, so it doesn't need an allocation of its own. */
_Static_assert(sizeof(struct cvector) % _Alignof(ccol_memmgmt_procs_t) == 0,
               "the procs copy after struct cvector must be aligned");

/* Frees all the heap memory of the vector, which is the data buffer and the
 * block of the container struct. With a custom allocator m_procs points
 * either at an interned procs struct or at the copy in the tail of that
 * block, so the free function pointer is read before the block is freed. */
void __cvector_destroy(cvec v) {
  if (v) {
    if (v->data_ptr) {
      _ccol_mem_free(v->m_procs, v->data_ptr);
    }

    if (v->m_procs) {
      ccol_free_t free_func = v->m_procs->free;
      free_func(v);
    } else {
      _ccol_default_mem_free(v);
    }
  }
}

/* Guards cvector_create_full against arguments that make no sense. An
 * elem_size of zero breaks every byte offset calculation, so we reject it
 * immediately instead of letting a bad state spread quietly. A very large
 * elem_size is rejected for the same reason: it makes
 * _ccol_cvector_minimum_capacity * elem_size overflow size_t, and that
 * product is the size of the very first allocation of the backing buffer.
 * Every later path that grows the capacity (scale_the_cvector_size_up and
 * cvector_reserve) already guards its own byte size multiplication against
 * an overflow, so the first allocation can't be the one place that quietly
 * wraps to a too small buffer while v->elem_size holds the real, huge
 * value. */
static bool verify_cvector_create_inputs(size_t elem_size,
                                         ccol_memmgmt_procs_t *mmgt_procs,
                                         char **err) {
  if (elem_size == 0) {
    if (err) {
      *err = _CCOL_ERR_STR("elem_size is zero");
    }
    return false;
  }

  if (elem_size > SIZE_MAX / _ccol_cvector_minimum_capacity) {
    if (err) {
      *err = _CCOL_ERR_STR(
          "elem_size too large: initial capacity allocation would overflow");
    }
    return false;
  }

  if (!_ccol_verify_memmgmt_procs(mmgt_procs, err)) {
    return false;
  }

  return true;
}

/* Allocates and initializes a new vector with the element size and the
 * optional custom allocator that the caller gives. The first backing buffer
 * holds _ccol_cvector_minimum_capacity elements. An interned procs pointer
 * is kept as it is, since the interned copy lives for the whole process, and
 * any other procs struct is copied into the tail of the vector's own block.
 * When any allocation fails the partly built vector is torn down, NULL comes
 * back, and a static error string goes into *err if the caller gave an err.
 */
cvec cvector_create_full(size_t elem_size, ccol_memmgmt_procs_t *mmgt_procs,
                         char **err) {
  if (!verify_cvector_create_inputs(elem_size, mmgt_procs, err)) {
    return NULL;
  }

  bool copy_procs = mmgt_procs && !ccol_procs_is_interned(mmgt_procs);
  cvec v;
  v = _ccol_mem_calloc(
      mmgt_procs, 1,
      sizeof(cvector) + (copy_procs ? sizeof(ccol_memmgmt_procs_t) : 0));
  if (!v) {
    if (err) {
      *err = _CCOL_ERR_STR("failed to allocate vector container");
    }
    return NULL;
  }

  if (copy_procs) {
    v->m_procs = (ccol_memmgmt_procs_t *)(v + 1);
    memcpy(v->m_procs, mmgt_procs, sizeof(ccol_memmgmt_procs_t));
  } else {
    v->m_procs = mmgt_procs;
  }

  v->data_ptr =
      _ccol_mem_alloc(mmgt_procs, _ccol_cvector_minimum_capacity * elem_size);
  if (!v->data_ptr) {
    __cvector_destroy(v);
    if (err) {
      *err = _CCOL_ERR_STR("failed to allocate data container");
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

/* Gives the custom allocator struct that the vector stores, or NULL when it
 * uses the default malloc and free path. */
ccol_memmgmt_procs_t *cvector_get_mprocs(cvec v) {
  if (!v) {
    _ccol_assert(false);
  }

  return v->m_procs;
}

/* Doubles the capacity of the backing buffer. ccol_container_full comes back
 * when the vector is already at the architectural ceiling for growth, which
 * is ccol_max_elem_count or the largest byte size this elem_size can
 * address; that's a structural limit and not a passing allocation failure.
 * ccol_not_enough_memory comes back only when the realloc itself fails, and
 * the caller needs to keep the two apart: cvector_push_back, our only caller,
 * passes the difference on to its own caller instead of reporting
 * ccol_not_enough_memory for both. When the reallocation fails the original
 * pointer is put back, so the vector stays usable. */
static ccol_retval_t scale_the_cvector_size_up(cvec v) {
  if (!v) {
    _ccol_assert(false);
  }

  if (v->capacity > (ccol_max_elem_count / _ccol_cvector_scaling_factor)) {
    return ccol_container_full;  // elem_count would overflow
  }

  size_t new_capacity = _ccol_cvector_scaling_factor * v->capacity;
  if (new_capacity > SIZE_MAX / v->elem_size) {
    return ccol_container_full;  // the byte size would overflow
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
 * _ccol_cvector_minimum_capacity. If the reallocation fails we quietly put
 * the original pointer back, because a shrink is only a best effort and a
 * failed one doesn't damage any of the existing data. */
static void scale_the_cvector_size_down(cvec v) {
  if (!v) {
    _ccol_assert(false);
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

/* Appends a copy of *new_elem at the end of the vector, and grows the backing
 * buffer by _ccol_cvector_scaling_factor when the capacity is full. It gives
 * ccol_not_enough_memory when the reallocation for that growth fails, and
 * ccol_container_full when the vector is already at the architectural
 * ceiling for growth (see scale_the_cvector_size_up).
 *
 * new_elem may point into v's own backing buffer, for example a pointer from
 * cvector_at(v, i) or from cvector_data_ptr(v), and cvector_append_array
 * gives the same guarantee. When the vector has to grow,
 * scale_the_cvector_size_up can realloc the buffer to a new address, and
 * without care new_elem would be dangling by the time we read it. So an
 * aliasing new_elem is turned into a byte offset before the growth and its
 * address is computed again from the new v->data_ptr afterwards, which means
 * new_elem is never dereferenced across the possible move. */
ccol_retval_t cvector_push_back(cvec v, const void *new_elem) {
  if (!v) {
    _ccol_assert(false);
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
        // v->data_ptr can move, so work out new_elem again from the new
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
 * shrinks once the element count drops below capacity /
 * shrink_threshold_divisor, so the vector doesn't sit on too much memory.
 * On an empty vector it gives ccol_container_empty and leaves *target_elem
 * alone. */
ccol_retval_t cvector_pop_back(cvec v, void *target_elem) {
  if (!v) {
    _ccol_assert(false);
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

/* Allocates backing storage in advance for at least new_capacity_count
 * elements. The requested count is rounded up to the nearest power of two,
 * with a minimum of _ccol_cvector_minimum_capacity, so a later push_back
 * hits a capacity boundary the caller can predict. When the capacity is
 * already enough for the request nothing happens. */
bool cvector_reserve(cvec v, size_t new_capacity_count) {
  if (!v) {
    _ccol_assert(false);
  }

  size_t refined_new_capacity_count =
      _ccol_find_nearest_gte_power_of_two(new_capacity_count);
  if (refined_new_capacity_count < _ccol_cvector_minimum_capacity) {
    refined_new_capacity_count = _ccol_cvector_minimum_capacity;
  }

  if (refined_new_capacity_count > ccol_max_elem_count) {
    // That's too much, so reject it.
    return false;
  }

  if (refined_new_capacity_count > SIZE_MAX / v->elem_size) {
    return false;  // The byte size would overflow
  }

  if (refined_new_capacity_count <= v->capacity) {
    // The vector already has the capacity the caller asks for
    return true;
  }

  // The vector needs more capacity
  void *orig = v->data_ptr;
  v->data_ptr = _ccol_mem_realloc(v->m_procs, v->data_ptr,
                                  refined_new_capacity_count * v->elem_size);
  if (!v->data_ptr) {
    // The reallocation failed, put the original pointer back.
    v->data_ptr = orig;
    return false;
  }

  // The reallocation worked.
  v->capacity = refined_new_capacity_count;
  return true;
}

/* Copies n bytes from src into the tail slot of the vector, at
 * v->data_ptr + v->elem_count * v->elem_size, without touching
 * v->elem_count itself. arr_ptr may point anywhere inside v's backing
 * buffer, including reserved capacity past v->elem_count that holds no live
 * element yet, so with a big enough elem_count from the caller the source
 * and the destination ranges can overlap. memcpy isn't safe for an overlap,
 * so we use memmove when the two ranges really overlap and keep memcpy for
 * the far more common case with no alias. Paying for memmove on every append
 * just to cover a rare alias isn't worth it. */
#ifdef RUNNING_UNIT_TESTS
/* Counts the cvector_copy_into_tail calls that take the memmove branch (the
 * one that is safe for an overlap), so a white-box test can assert that the
 * code really goes through that branch. Looking at the output of the copy
 * alone isn't enough for the test: memcpy's behaviour isn't portable, and it
 * can leave a given overlapping range without any visible corruption even
 * when the safe branch is skipped, so seeing "the data come out right"
 * proves nothing about which path ran. The counter isn't part of the public
 * API. */
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

/* Appends elem_count elements from arr_ptr to the vector in one go, and
 * reserves more capacity when it has to. The overflow check on the total
 * element count catches both a size_t wrap-around and a count above
 * ccol_max_elem_count.
 *
 * arr_ptr may point into v's own backing buffer, for example a pointer from
 * cvector_data_ptr(v) or from cvector_at(v, i). Growing the vector can need
 * a realloc inside cvector_reserve, which can move the backing buffer to a
 * new address, and without care arr_ptr would be dangling by the time we
 * read it. So an aliasing arr_ptr is turned into a byte offset before the
 * reservation and its address is computed again from the new v->data_ptr
 * afterwards, and arr_ptr is never dereferenced across the possible move.
 * The comment on cvector_copy_into_tail describes the other hazard, two
 * overlapping ranges, which this function guards against as well.
 *
 * An aliasing source range is also bounded by the reserved extent of the
 * buffer, and we give false when such a range runs past it. Without that
 * bound an offset near the top of the reserved region plus a large
 * elem_count would read off the end of the allocation. */
bool cvector_append_array(cvec v, const void *arr_ptr, size_t elem_count) {
  if (!v) {
    _ccol_assert(false);
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

  // Does arr_ptr point into this vector's backing buffer? Two different
  // things depend on that: a source range that aliases the buffer gets the
  // reserved extent of that buffer as its bound (right below), and an
  // aliasing arr_ptr must also survive a realloc further down.
  const char *buf_start = (const char *)v->data_ptr;
  const char *buf_end = buf_start + v->capacity * v->elem_size;
  bool arr_ptr_aliases_self =
      (const char *)arr_ptr >= buf_start && (const char *)arr_ptr < buf_end;
  size_t arr_ptr_offset =
      arr_ptr_aliases_self ? (size_t)((const char *)arr_ptr - buf_start) : 0;

  // An aliasing source range may run past v->elem_count into reserved
  // capacity that holds no live element yet, but it must not run past the
  // reservation itself: the bytes after buf_end belong to no allocation of
  // ours, so reading them is a heap over-read. Only the aliasing case can be
  // bounded at all, because a source array of the caller has no extent we
  // can see, as with every C array a caller passes as a bare pointer.
  if (arr_ptr_aliases_self &&
      elem_count > (size_t)(buf_end - (const char *)arr_ptr) / v->elem_size) {
    return false;
  }

  if (new_elem_count <= v->capacity) {
    // The vector has enough space already
    cvector_copy_into_tail(v, arr_ptr, elem_count * v->elem_size);
    v->elem_count += elem_count;
    return true;
  }

  // The capacity isn't enough, so try to reserve more. The realloc inside
  // cvector_reserve can move the buffer, which is why the code below works
  // out an aliasing arr_ptr again from the byte offset above instead of
  // dereferencing arr_ptr across the move.
  if (!cvector_reserve(v, new_elem_count)) {
    return false;
  }

  if (arr_ptr_aliases_self) {
    // v->data_ptr can move, so work out arr_ptr again from the new buffer.
    arr_ptr = (const char *)v->data_ptr + arr_ptr_offset;
  }

  // By this point the vector has enough space
  cvector_copy_into_tail(v, arr_ptr, elem_count * v->elem_size);
  v->elem_count += elem_count;
  return true;
}

/* Appends all the elements of v_from to v_to through cvector_append_array.
 * Both vectors must have the same elem_size. Two different values are a
 * fatal assertion, since they point to a programming error at the call
 * site. */
bool cvector_append_cvector(cvec v_to, cvec v_from) {
  if (!v_to || !v_from || v_to->elem_size != v_from->elem_size) {
    _ccol_assert(false);
  }

  if (v_to == v_from) {
    /* Reserve up front so cvector_append_array never reallocates in the
     * middle of the copy, otherwise realloc could free the source buffer
     * while arr_ptr still points into it. */
    size_t count = v_from->elem_count;
    size_t new_count = v_to->elem_count + count;
    /* The same overflow check that cvector_append_array makes. Without it a
     * size_t wrap-around here would let this addition report less capacity
     * than the call really needs. No real program gets here, since
     * elem_count would already have to be at ccol_max_elem_count (a huge
     * vector), but the later check inside cvector_append_array shouldn't be
     * the only thing between this call and a wrong reservation. */
    if (new_count < v_to->elem_count || new_count > ccol_max_elem_count) {
      return false;
    }
    if (!cvector_reserve(v_to, new_count)) {
      return false;
    }
  }

  return cvector_append_array(v_to, v_from->data_ptr, v_from->elem_count);
}

/* Gives a raw pointer to the first element of the contiguous backing
 * buffer. Don't keep this pointer across a push_back or an append call,
 * because those can realloc the buffer to another address. */
void *cvector_data_ptr(cvec v) {
  if (!v) {
    _ccol_assert(false);
  }

  return v->data_ptr;
}

/* Gives a pointer to the element at the given index, or NULL when the index
 * is out of range. The empty vector guard (elem_count > 0) makes sure the
 * index is never compared against an uninitialized elem_count of zero. */
void *cvector_at(cvec v, size_t index) {
  if (!v) {
    _ccol_assert(false);
  }

  if (v->elem_count > 0 && index < v->elem_count) {
    return (void *)((char *)v->data_ptr + index * v->elem_size);
  }

  return NULL;
}

/* Gives the number of live elements in the vector. */
size_t cvector_elem_count(cvec v) {
  if (!v) {
    _ccol_assert(false);
  }

  return v->elem_count;
}

/* Removes all the elements and then tries to shrink the backing buffer to
 * _ccol_cvector_minimum_capacity to give memory back. The element count
 * goes to zero whether the reallocation works or not, and the reallocation
 * is skipped altogether when the capacity is already
 * _ccol_cvector_minimum_capacity, since such a shrink would do nothing;
 * scale_the_cvector_size_down has the same early return. */
void cvector_reset(cvec v) {
  if (!v) {
    _ccol_assert(false);
  }

  if (v->capacity != _ccol_cvector_minimum_capacity) {
    void *orig = v->data_ptr;
    v->data_ptr = _ccol_mem_realloc(
        v->m_procs, v->data_ptr, _ccol_cvector_minimum_capacity * v->elem_size);
    if (!v->data_ptr) {
      // The capacity stays as it is when the reallocation fails.
      v->data_ptr = orig;
    } else {
      v->capacity = _ccol_cvector_minimum_capacity;
    }
  }
  v->elem_count = 0;
}

/* The sort behind cvec_sort() and cvector_sort_with_comparison_proc(). The
 * elements are contiguous, so the sort addresses them straight from
 * data_ptr and makes no call per element access. */
bool _cvector_sort(cvec v, ccol_comparison_proc_t comparison_proc) {
  if (!v) {
    _ccol_assert(false);
  }
  return _csort_merge_sort_contiguous(v->data_ptr, v->elem_count, v->elem_size,
                                      comparison_proc, v->m_procs);
}

/* A linear scan for the first element equal to *elem. With a NULL cmp the
 * comparison falls back to memcmp over elem_size bytes. Gives the zero-based
 * index of the first match, or ccol_invalid_size when nothing matches. */
size_t cvector_find(cvec v, const void *elem, ccol_comparison_proc_t cmp) {
  if (!v) {
    _ccol_assert(false);
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

/* The internal iterator, which the public header does NOT expose. The
 * cmap_iterator base field MUST be first, so a cmap_iterator * pointing at
 * that field is also a valid cvec_iter_impl_t *. */
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
   * after the caller already destroyed the vector, and the procs struct the
   * vector owns along with it. So freeing the iterator must never read the
   * vector. */
  ccol_free_t free_fn;
} cvec_iter_impl_t;

static void cvec_iter_impl_free(cmap_iterator *it) {
  cvec_iter_impl_t *impl = (cvec_iter_impl_t *)it;
  ccol_free_t free_fn = impl->free_fn;
  if (free_fn) {
    free_fn(impl);
  } else {
    _ccol_default_mem_free(impl);
  }
}

static cmap_iterator *cvec_cmap_iter_next(cmap_iterator *it) {
  if (!it) {
    _ccol_assert(false);
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
      *err = _CCOL_ERR_STR("Failed to allocate iterator");
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
 * test that checks the growth and shrink thresholds. It isn't part of the
 * public API. */
size_t cvector_get_capacity(cvec v) {
  if (!v) {
    _ccol_assert(false);
  }

  return v->capacity;
}
#endif
