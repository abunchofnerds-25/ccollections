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
#include <stdlib.h>
#include <string.h>

const size_t minimum_capacity = 4;
const size_t scaling_factor = 2;

struct cvector {
  size_t elem_size;
  size_t elem_count;
  size_t capacity;
  ccol_memmgmt_procs_t *m_procs;
  void *data_ptr;
};

/* Releases all heap memory owned by the vector, including its data buffer and
 * the container struct itself. If a custom allocator was provided at creation,
 * the allocator struct is freed using its own free function before the vector
 * struct is freed – this ordering matters because the free function pointer
 * must still be accessible when freeing the container. */
void __cvector_destroy(cvec v) {
  if (v) {
    if (v->data_ptr) {
      _mem_free(v->m_procs, v->data_ptr);
    }

    if (v->m_procs) {
      ccol_free_t free_func = v->m_procs->free;
      free_func(v->m_procs);
      free_func(v);
    } else {
      mem_free(v);
    }
  }
}

/* Guards cvector_create_full against logically invalid arguments. An elem_size
 * of zero would corrupt every byte-offset calculation, so it is rejected
 * early rather than propagating a silent bad state. */
bool verify_cvector_create_inputs(size_t elem_size,
                                  ccol_memmgmt_procs_t *mmgt_procs,
                                  char **err) {
  if (elem_size == 0) {
    if (err) {
      *err = CCOL_ERR_STR("elem_size is zero");
    }
    return false;
  }

  if (!ccol_verify_memmgmt_procs(mmgt_procs, err)) {
    return false;
  }

  return true;
}

/* Allocates and initialises a new vector with the given element size and an
 * optional custom allocator. The initial backing buffer holds minimum_capacity
 * elements. On any allocation failure the partially constructed vector is torn
 * down and NULL is returned; *err receives a static error string when provided.
 */
cvec cvector_create_full(size_t elem_size, ccol_memmgmt_procs_t *mmgt_procs,
                         char **err) {
  if (!verify_cvector_create_inputs(elem_size, mmgt_procs, err)) {
    return NULL;
  }

  cvec v;
  v = _mem_calloc(mmgt_procs, 1, sizeof(cvector));
  if (!v) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate vector container");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(v, mmgt_procs, err)) {
    _mem_free(mmgt_procs, v);
    return NULL;
  }

  v->data_ptr = _mem_alloc(mmgt_procs, minimum_capacity * elem_size);
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

  v->capacity = minimum_capacity;
  v->elem_count = 0;
  v->elem_size = elem_size;

  return v;
}

/* Returns the custom allocator struct stored in the vector, or NULL if the
 * default malloc/free path is in use. */
ccol_memmgmt_procs_t *cvector_get_mprocs(cvec v) { return v->m_procs; }

/* Doubles the backing buffer capacity. The overflow guard prevents wrapping
 * when capacity is already near the architecture limit. On reallocation
 * failure the original pointer is restored so the vector remains usable. */
bool scale_the_cvector_size_up(cvec v) {
  if (!v) {
    ccol_assert(false);
  }

  if (v->capacity > (max_elem_count / scaling_factor)) {
    return false;  // Would overflow
  }

  size_t new_capacity = scaling_factor * v->capacity;
  if (new_capacity > SIZE_MAX / v->elem_size) {
    return false;  // byte size would overflow
  }

  void *orig = v->data_ptr;
  v->data_ptr =
      _mem_realloc(v->m_procs, v->data_ptr, new_capacity * v->elem_size);
  if (!v->data_ptr) {
    v->data_ptr = orig;
    return false;
  }

  v->capacity *= scaling_factor;
  return true;
}

/* Halves the backing buffer capacity, but never below minimum_capacity. On
 * reallocation failure the original pointer is silently restored – shrinking
 * is best-effort and a failure does not corrupt any existing data. */
void scale_the_cvector_size_down(cvec v) {
  if (!v) {
    ccol_assert(false);
  }

  if (v->capacity == minimum_capacity) {
    return;
  }

  void *orig = v->data_ptr;
  v->data_ptr = _mem_realloc(v->m_procs, v->data_ptr,
                             (v->capacity / scaling_factor) * v->elem_size);
  if (!v->data_ptr) {
    v->data_ptr = orig;
    return;
  }

  v->capacity /= scaling_factor;
}

/* Appends a copy of *new_elem at the end of the vector, growing the backing
 * buffer by scaling_factor if the current capacity is exhausted. Returns
 * ccol_not_enough_memory if the growth reallocation fails. */
ccol_retval_t cvector_push_back(cvec v, const void *new_elem) {
  if (!v) {
    ccol_assert(false);
  }

  if (!new_elem) {
    return ccol_invalid_args;
  }

  if (v->elem_count == max_elem_count) {
    return ccol_container_full;
  }

  ccol_retval_t result = ccol_success;

  if (v->elem_count < v->capacity) {
    mem_cpy((void *)((char *)v->data_ptr + v->elem_count * v->elem_size),
            new_elem, v->elem_size);
    ++v->elem_count;
  } else {
    if (scale_the_cvector_size_up(v)) {
      mem_cpy((void *)((char *)v->data_ptr + v->elem_count * v->elem_size),
              new_elem, v->elem_size);
      ++v->elem_count;
    } else {
      result = ccol_not_enough_memory;
    }
  }

  return result;
}

/* Removes the last element and copies it into *target_elem. When the element
 * count drops below capacity / minimum_capacity the buffer is shrunk to avoid
 * holding on to excessive memory. Returns ccol_container_empty on an empty
 * vector without touching *target_elem. */
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

    mem_cpy(target_elem,
            (void *)((char *)v->data_ptr + v->elem_count * v->elem_size),
            v->elem_size);

    if (v->elem_count < (v->capacity / minimum_capacity)) {
      scale_the_cvector_size_down(v);
    }
  }

  return result;
}

/* Pre-allocates enough backing storage for at least new_capacity_count
 * elements. The requested count is rounded up to the nearest power of two
 * (minimum minimum_capacity) so that subsequent push_back calls hit a
 * predictable capacity boundary. Does nothing if current capacity already
 * satisfies the request. */
bool cvector_reserve(cvec v, size_t new_capacity_count) {
  if (!v) {
    ccol_assert(false);
  }

  size_t refined_new_capacity_count =
      find_nearest_gte_power_of_two(new_capacity_count);
  if (refined_new_capacity_count < minimum_capacity) {
    refined_new_capacity_count = minimum_capacity;
  }

  if (refined_new_capacity_count > max_elem_count) {
    // That's too much, reject it.
    return false;
  }

  if (refined_new_capacity_count > SIZE_MAX / v->elem_size) {
    return false;  // byte size would overflow
  }

  if (refined_new_capacity_count <= v->capacity) {
    // We already have the requested capacity
    return true;
  }

  // We need to extend our capacity
  void *orig = v->data_ptr;
  v->data_ptr = _mem_realloc(v->m_procs, v->data_ptr,
                             refined_new_capacity_count * v->elem_size);
  if (!v->data_ptr) {
    // Reallocation attempt failed, restore the original pointer
    v->data_ptr = orig;
    return false;
  }

  // Reallocation attempt succeeded, all went well
  v->capacity = refined_new_capacity_count;
  return true;
}

/* Bulk-appends elem_count elements from arr_ptr to the vector, reserving
 * additional capacity when needed. The total element count overflow check
 * catches both a size_t wrap-around and exceeding max_elem_count. */
bool cvector_append_array(cvec v, void *arr_ptr, size_t elem_count) {
  if (!v) {
    ccol_assert(false);
  }

  size_t new_elem_count = v->elem_count + elem_count;
  if (new_elem_count < v->elem_count || new_elem_count > max_elem_count) {
    // Either new_elem_count wrapped or it's greater than the max_elem_count
    return false;
  }

  if (new_elem_count <= v->capacity) {
    // We have enough space already
    mem_cpy((void *)((char *)v->data_ptr + v->elem_count * v->elem_size),
            arr_ptr, elem_count * v->elem_size);
    v->elem_count += elem_count;
    return true;
  }

  // Our capacity is not enough, let's try to see whether we can reserve
  if (!cvector_reserve(v, new_elem_count)) {
    return false;
  }

  // We now should have enough space
  mem_cpy((void *)((char *)v->data_ptr + v->elem_count * v->elem_size), arr_ptr,
          elem_count * v->elem_size);
  v->elem_count += elem_count;
  return true;
}

/* Appends all elements of v_from to v_to by delegating to cvector_append_array.
 * Both vectors must have the same elem_size; a mismatch is a fatal assertion
 * because it indicates a programming error at the call site. */
bool cvector_append_cvector(cvec v_to, cvec v_from) {
  if (!v_to || !v_from || v_to->elem_size != v_from->elem_size) {
    ccol_assert(false);
  }

  if (v_to == v_from) {
    /* Pre-reserve so cvector_append_array never reallocates mid-copy.
     * Without this, realloc may free the source buffer while arr_ptr
     * still points into it. */
    size_t count = v_from->elem_count;
    if (!cvector_reserve(v_to, v_to->elem_count + count)) {
      return false;
    }
  }

  return cvector_append_array(v_to, v_from->data_ptr, v_from->elem_count);
}

/* Returns a raw pointer to the first element of the contiguous backing buffer.
 * Callers must not hold onto this pointer across any push_back or append
 * call, as those may realloc the buffer to a different address. */
void *cvector_data_ptr(cvec v) {
  if (!v) {
    ccol_assert(false);
  }

  return v->data_ptr;
}

/* Returns a pointer to the element at the given index, or NULL if the index is
 * out of range. The empty-vector guard (elem_count > 0) ensures index is never
 * compared against an uninitialised zero elem_count. */
void *cvector_at(cvec v, size_t index) {
  if (!v) {
    ccol_assert(false);
  }

  if (v->elem_count > 0 && index < v->elem_count) {
    return (void *)((char *)v->data_ptr + index * v->elem_size);
  }

  return NULL;
}

/* Returns the number of live elements currently stored in the vector. */
size_t cvector_elem_count(cvec v) {
  if (!v) {
    ccol_assert(false);
  }

  return v->elem_count;
}

/* Clears all elements and attempts to shrink the backing buffer to
 * minimum_capacity to reclaim memory. The element count is set to zero
 * regardless of whether the reallocation succeeds. */
void cvector_reset(cvec v) {
  if (!v) {
    ccol_assert(false);
  }

  void *orig = v->data_ptr;
  v->data_ptr =
      _mem_realloc(v->m_procs, v->data_ptr, minimum_capacity * v->elem_size);
  if (!v->data_ptr) {
    // Capacity should remain unchanged if reallocation fails.
    v->data_ptr = orig;
  } else {
    v->capacity = minimum_capacity;
  }
  v->elem_count = 0;
}

/* Internal iterator implementation — NOT exposed in the public header.
 * The cmap_iterator base field MUST be first so that a cmap_iterator *
 * pointing at it is also a valid cvec_iter_impl_t *. */
typedef struct {
  cmap_iterator base;         /* public face — key_pair / val_pair / fn ptrs */
  cmap_pair key_pair_storage; /* index storage (base.key_pair points here) */
  cmap_pair val_pair_storage; /* element ptr storage (base.val_pair → here) */
  size_t index;               /* key_pair_storage.ptr points to this field */
  cvec vec;                   /* back-reference for advancing and freeing */
} cvec_iter_impl_t;

static void cvec_iter_impl_free(cmap_iterator *it) {
  cvec_iter_impl_t *impl = (cvec_iter_impl_t *)it;
  _mem_free(impl->vec->m_procs, impl);
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

cmap_iterator *cvector_cmap_begin_iter(cvec v, char **err) {
  if (err) {
    *err = NULL;
  }
  if (!v) {
    ccol_assert(false);
  }
  if (v->elem_count == 0) {
    return NULL;
  }
  cvec_iter_impl_t *impl = _mem_calloc(v->m_procs, 1, sizeof(cvec_iter_impl_t));
  if (!impl) {
    if (err) {
      *err = CCOL_ERR_STR("Failed to allocate iterator");
    }
    return NULL;
  }
  impl->index = 0;
  impl->vec = v;
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
/* Exposes the internal backing-buffer capacity for white-box unit tests that
 * verify the grow/shrink thresholds. Not part of the public API. */
size_t cvector_get_capacity(cvec v) {
  if (!v) {
    ccol_assert(false);
  }

  return v->capacity;
}
#endif
