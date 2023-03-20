/*
MIT License

Copyright (c) 2018 Danis Ozdemir

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

#define mem_alloc(size) malloc(size)
#define mem_calloc(elem_count, elem_size) calloc(elem_count, elem_size)
#define mem_realloc(ptr, new_size) realloc(ptr, new_size)
#define mem_free(ptr) free(ptr)

#define _mem_alloc(m_procs, size) \
  (m_procs) ? m_procs->malloc(size) : mem_alloc(size)
#define _mem_calloc(m_procs, e_count, e_size) \
  (m_procs) ? m_procs->calloc(e_count, e_size) : mem_calloc(e_count, e_size)
#define _mem_realloc(m_procs, ptr, new_size) \
  (m_procs) ? m_procs->realloc(ptr, new_size) : mem_realloc(ptr, new_size)
#define _mem_free(m_procs, ptr) (m_procs) ? m_procs->free(ptr) : mem_free(ptr)

#define stringify(s) #s
#define x_stringify(s) stringify(s)
#define CERR_STR(x) (__FILE__ ":" x_stringify(__LINE__) " - " x)

const uint32_t minimum_capacity = 4;
const uint32_t scaling_factor = 2;

struct cvector {
  uint32_t elem_size;
  uint32_t elem_count;
  uint32_t capacity;
  ccol_memmgmt_procs_t* m_procs;
  void* data_ptr;
};

void __cvector_destroy(cvec v) {
  if (v) {
    if (v->m_procs) {
      void (*free_proc)(void*) = v->m_procs->free;
      free_proc(v->data_ptr);
      free_proc(v->m_procs);
      free_proc(v);
    } else {
      mem_free(v->data_ptr);
      mem_free(v);
    }
  }
}

bool verify_cvector_create_inputs(uint32_t elem_size,
                                  ccol_memmgmt_procs_t* mmgt_procs,
                                  char** err) {
  if (elem_size == 0) {
    if (err) {
      *err = CERR_STR("elem_size is zero");
    }
    return false;
  }

  if (!ccol_verify_memmgmt_procs(mmgt_procs, err)) {
    return false;
  }

  return true;
}

cvec cvector_create_with_mprocs(uint32_t elem_size,
                                ccol_memmgmt_procs_t* mmgt_procs, char** err) {
  if (!verify_cvector_create_inputs(elem_size, mmgt_procs, err)) {
    return NULL;
  }

  cvec v;
  v = _mem_calloc(mmgt_procs, 1, sizeof(cvector));
  if (!v) {
    if (err) {
      *err = CERR_STR("failed to allocate vector container");
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
      *err = CERR_STR("failed to allocate data container");
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

bool scale_the_cvector_size_up(cvec v) {
  if (!v) {
    return false;
  }

  void* orig = v->data_ptr;
  v->data_ptr = _mem_realloc(v->m_procs, v->data_ptr,
                             scaling_factor * v->capacity * v->elem_size);
  if (!v->data_ptr) {
    v->data_ptr = orig;
    return false;
  }

  v->capacity *= scaling_factor;
  return true;
}

void scale_the_cvector_size_down(cvec v) {
  if (!v) {
    return;
  }

  if (v->capacity == minimum_capacity) {
    return;
  }

  void* orig = v->data_ptr;
  v->data_ptr = _mem_realloc(v->m_procs, v->data_ptr,
                             (v->capacity / scaling_factor) * v->elem_size);
  if (!v->data_ptr) {
    v->data_ptr = orig;
    return;
  }

  v->capacity /= scaling_factor;
}

static inline void assign(void* dest, const void* src, uint32_t size) {
  if (size == sizeof(unsigned int)) {
    *(unsigned int*)dest = *(unsigned int*)src;
  } else if (size == sizeof(unsigned char)) {
    *(unsigned char*)dest = *(unsigned char*)src;
  } else if (size == sizeof(uint64_t)) {
    *(uint64_t*)dest = *(uint64_t*)src;
  } else if (size == sizeof(uint16_t)) {
    *(uint16_t*)dest = *(uint16_t*)src;
  } else {
    memcpy(dest, src, size);
  }
}

ccol_retval_t cvector_push_back(cvec v, const void* new_elem) {
  if (!v || !new_elem) {
    return ccol_invalid_args;
  }

  ccol_retval_t result = ccol_success;

  if (v->elem_count < v->capacity) {
    assign((void*)((uint64_t)v->data_ptr + v->elem_count * v->elem_size),
           new_elem, v->elem_size);
    if (++v->elem_count == v->capacity) {
      // Ignoring the return value of scale_the_cvector_size_up
      // as we managed to insert the new_elem.
      scale_the_cvector_size_up(v);
    }
  } else {
    if (scale_the_cvector_size_up(v)) {
      assign((void*)((uint64_t)v->data_ptr + v->elem_count * v->elem_size),
             new_elem, v->elem_size);
      ++v->elem_count;
    } else {
      result = ccol_not_enough_memory;
    }
  }

  return result;
}

ccol_retval_t cvector_pop_back(cvec v, void* target_elem) {
  if (!v || !target_elem) {
    return ccol_invalid_args;
  }

  ccol_retval_t result = ccol_container_empty;

  if (v->elem_count > 0) {
    result = ccol_success;
    --v->elem_count;

    assign(target_elem,
           (void*)((uint64_t)v->data_ptr + v->elem_count * v->elem_size),
           v->elem_size);

    if (v->elem_count < (v->capacity / minimum_capacity)) {
      scale_the_cvector_size_down(v);
    }
  }

  return result;
}

void* cvector_at(cvec v, uint32_t index) {
  if (!v) {
    return NULL;
  }

  if (v->elem_count > 0 && index < v->elem_count) {
    return (void*)((uint64_t)v->data_ptr + index * v->elem_size);
  }

  return NULL;
}

uint32_t cvector_elem_count(cvec v) {
  if (!v) {
    return 0;
  }

  return v->elem_count;
}

void cvector_reset(cvec v) {
  if (!v) {
    return;
  }

  void* orig = v->data_ptr;
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

#ifdef RUNNING_UNIT_TESTS
uint32_t cvector_get_capacity(cvec v) {
  if (!v) {
    return 0;
  }

  return v->capacity;
}
#endif
