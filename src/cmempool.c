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

#include <assert.h>
#include <cmempool.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *_mempool_mark = "mempool";

typedef uintptr_t *addr_t;

#define ENTRY_TO_HEADER(entry)                   \
  (__internal_entry_header *)((uintptr_t)entry - \
                              offsetof(__internal_entry_header, next))

#define EXTENDED_SIZE_TO_USER_SIZE(extended_elem_size) \
  (extended_elem_size - offsetof(__internal_entry_header, next))

#define USER_SIZE_TO_EXTENDED_SIZE(elem_size) \
  (elem_size + offsetof(__internal_entry_header, next))

struct mempool {
  const char *mempool_mark;  // This field is used for sanity checks
  size_t extended_elem_size;
  size_t total_elem_count;
  bool fallback_to_dynamic_memory;
  size_t active_dynamic_memory_buffer_count;
  uintptr_t lower_addr_limit;
  uintptr_t upper_addr_limit;
  size_t free_elem_count;
  ccol_memmgmt_procs_t *m_procs;
  void *objects;
  bool is_preallocated;
  bool should_use_locks;
  void *free_inst;
  rw_lock_t lock;
};

#if UINTPTR_MAX == UINT32_MAX
const size_t elem_is_free = 0xdeadbeef;
const size_t elem_is_taken = 0xfeedcafe;
const size_t elem_is_not_a_pool_member = 0xfadeface;
#elif UINTPTR_MAX == UINT64_MAX
const size_t elem_is_free = 0xdeadbeefdeadbeef;
const size_t elem_is_taken = 0xfeedcafefeedcafe;
const size_t elem_is_not_a_pool_member = 0xfadefacefadeface;
#else
#error "Unexpected pointer size"
#endif

/* Destroys the memory pool. If dynamic fallback was enabled and some
 * dynamically allocated entries have not been freed yet, the call asserts to
 * make that leak visible – we'd rather crash loudly than silently lose memory.
 * The backing objects buffer is only freed if it was not supplied by the caller
 * as a preallocated buffer. */
void _mempool_destroy(mempool *mp) {
  if (mp) {
    if (mp->fallback_to_dynamic_memory) {
      if (mempool_dynamic_allocs_count(mp) > 0) {
        // This pool has dynamically allotated entries that have
        // not yet been freed. This is a leak, let's make it
        // noticed.
        ccol_assert(false);
      }
    }

    if (mp->should_use_locks) {
      rw_lock_destroy(mp->lock);
    }

    if (!mp->is_preallocated && mp->objects) {
      _mem_free(mp->m_procs, mp->objects);
    }

    if (mp->m_procs) {
      ccol_free_t free_func = mp->m_procs->free;
      free_func(mp->m_procs);
      free_func(mp);
    } else {
      mem_free(mp);
    }
  }
}

/* Initialises the free-list headers across the objects buffer and sets all
 * pool metadata fields. Each element's header stores a magic status sentinel
 * and a back-pointer to the pool for validation in mempool_free_entry. The
 * last element in the chain has next == NULL to terminate the list. */
void mempool_init_internal_scalars(mempool *mp, size_t elem_count,
                                   size_t extended_elem_size,
                                   bool fallback_to_dynamic_memory) {
  for (size_t i = 0; i < elem_count; ++i) {
    __internal_entry_header *header =
        (__internal_entry_header *)((uintptr_t)mp->objects +
                                    i * extended_elem_size);
    header->elem_status = elem_is_free;
    header->pool_ptr = mp;

    if (i == (elem_count - 1)) {
      header->next = NULL;
    } else {
      header->next =
          (addr_t)((uintptr_t)mp->objects + (i + 1) * extended_elem_size);
    }
  }

  mp->free_inst = mp->objects;
  mp->mempool_mark = _mempool_mark;
  mp->extended_elem_size = extended_elem_size;
  mp->total_elem_count = elem_count;
  mp->fallback_to_dynamic_memory = fallback_to_dynamic_memory;
  mp->active_dynamic_memory_buffer_count = 0;
  mp->lower_addr_limit = (uintptr_t)mp->objects;
  mp->upper_addr_limit =
      (uintptr_t)mp->objects + extended_elem_size * elem_count;
  mp->free_elem_count = elem_count;
}

/* Creates a fixed-size memory pool for elem_count elements of elem_size bytes
 * each. The actual stored element size is extended by the size of the
 * __internal_entry_header prepended to each slot. If elem_size < sizeof(addr_t)
 * it is silently bumped to that minimum so the free-list next pointer fits. */
mempool *mempool_create(size_t elem_count, size_t elem_size,
                        bool fallback_to_dynamic_memory, bool single_threaded,
                        ccol_memmgmt_procs_t *mmgmt_procs, char **err) {
  if (err) {
    *err = NULL;
  }

  if (elem_count == 0 || elem_size == 0) {
    if (err) {
      *err = CCOL_ERR_STR("elem_count or elem_size is zero");
    }
    return NULL;
  } else if (elem_size < sizeof(addr_t)) {
    elem_size = sizeof(addr_t);
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  mempool *mp = (mempool *)_mem_calloc(mmgmt_procs, 1, sizeof(mempool));
  if (!mp) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate memory pool struct");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(mp, mmgmt_procs, err)) {
    _mem_free(mmgmt_procs, mp);
    return NULL;
  }

  size_t extended_elem_size = USER_SIZE_TO_EXTENDED_SIZE(elem_size);
  mp->objects = _mem_calloc(mmgmt_procs, elem_count, extended_elem_size);
  if (!mp->objects) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate memory pool data area");
    }
    mempool_destroy(mp);
    return NULL;
  }

  mp->should_use_locks = !single_threaded;

  if (mp->should_use_locks) {
    if (rw_lock_init(mp->lock) != 0) {
      if (err) {
        *err = CCOL_ERR_STR("failed to initialize the rw lock");
      }
      mempool_destroy(mp);
      return NULL;
    }
  }

  mempool_init_internal_scalars(mp, elem_count, extended_elem_size,
                                fallback_to_dynamic_memory);

  return mp;
}

/* Like mempool_create but uses an externally supplied buffer (e.g. a static
 * array) as the element store. elem_count is derived from buf_size / extended
 * element size. The buffer is never freed by the pool; the caller remains
 * responsible for its lifetime. Useful for embedded or stack-allocated pools.
 */
mempool *mempool_create_from_preallocated_buffer(
    void *buffer, size_t buf_size, size_t elem_size,
    bool fallback_to_dynamic_memory, bool single_threaded,
    ccol_memmgmt_procs_t *mmgmt_procs, char **err) {
  if (err) {
    *err = NULL;
  }

  if (!buffer || elem_size < sizeof(addr_t) ||
      buf_size < (sizeof(__internal_entry_header))) {
    if (err) {
      *err = CCOL_ERR_STR("buffer is not acceptable");
    }
    return NULL;
  }

  size_t extended_elem_size = USER_SIZE_TO_EXTENDED_SIZE(elem_size);
  size_t elem_count = buf_size / extended_elem_size;
  if (elem_count == 0) {
    if (err) {
      *err = CCOL_ERR_STR("calculated elem_count is zero");
    }
    return NULL;
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  mempool *mp = (mempool *)_mem_calloc(mmgmt_procs, 1, sizeof(mempool));
  if (!mp) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate mempool struct");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(mp, mmgmt_procs, err)) {
    _mem_free(mmgmt_procs, mp);
    return NULL;
  }

  mp->is_preallocated = true;
  mp->objects = buffer;

  mp->should_use_locks = !single_threaded;

  if (mp->should_use_locks) {
    if (rw_lock_init(mp->lock) != 0) {
      if (err) {
        *err = CCOL_ERR_STR("failed to init the rw lock");
      }
      mempool_destroy(mp);
      return NULL;
    }
  }

  mempool_init_internal_scalars(mp, elem_count, extended_elem_size,
                                fallback_to_dynamic_memory);

  return mp;
}

/* Allocates one element from the pool in O(1) time by popping the head of the
 * internal free list. If the pool is exhausted and fallback_to_dynamic_memory
 * is enabled, a fresh heap allocation is made instead and tagged as
 * elem_is_not_a_pool_member so it is routed through free on return. */
void *mempool_alloc_entry(mempool *mp) {
  if (!mp) {
    ccol_assert(false);
  }

  void *result = NULL;

  if (mp->should_use_locks) {
    rw_lock_wrlock(mp->lock);
  }

  if (mp->free_inst) {
    __internal_entry_header *header = (__internal_entry_header *)mp->free_inst;

    if (header->elem_status != elem_is_free || header->pool_ptr != mp) {
      // We have a corruption!
      if (mp->should_use_locks) {
        rw_lock_unlock(mp->lock);
      }
      ccol_assert(false);
    }

    mp->free_inst = header->next;
    header->elem_status = elem_is_taken;
    result = (void *)&header->next;
    --mp->free_elem_count;
  } else if (mp->fallback_to_dynamic_memory) {
    // Seems like we exhausted our buffers and
    // we are asked to fallback to the dynamic
    // memory allocation mechanisms.
    void *new_buffer = _mem_alloc(mp->m_procs, mp->extended_elem_size);
    if (new_buffer) {
      __internal_entry_header *header = (__internal_entry_header *)new_buffer;
      header->elem_status = elem_is_not_a_pool_member;
      header->pool_ptr = mp;
      result = (void *)&header->next;
      ++mp->active_dynamic_memory_buffer_count;
    }
  }

  if (mp->should_use_locks) {
    rw_lock_unlock(mp->lock);
  }

  return result;
}

/* Allocates one element and zeroes the user-visible bytes before returning.
 * The header portion preceding the user area is intentionally left intact. */
void *mempool_calloc_entry(mempool *mp) {
  void *result = mempool_alloc_entry(mp);

  if (result) {
    mem_zero(result, EXTENDED_SIZE_TO_USER_SIZE(mp->extended_elem_size));
  }

  return result;
}

/* Checks whether c_entry falls within the pool's object buffer and lands on a
 * valid element boundary (i.e. is a multiple of extended_elem_size from the
 * base). Used to distinguish pool-owned entries from dynamic fallback entries
 * during free. */
static inline bool valid_mempool_addr(mempool *mp, uintptr_t c_entry) {
  return (c_entry >= mp->lower_addr_limit) &&
         (c_entry < mp->upper_addr_limit) &&
         (c_entry - mp->lower_addr_limit) % mp->extended_elem_size == 0;
}

/* Low-level free that takes a pointer to the entry header rather than the user
 * pointer. Handles three cases: dynamic fallback entries (freed via the pool's
 * allocator), valid pool-owned taken entries (pushed onto the free list), and
 * any other state (double-free or corruption → assert). */
void __mempool_free_entry(mempool *mp, __internal_entry_header *header) {
  if (!mp) {
    ccol_assert(false);
  }

  uintptr_t c_header = (uintptr_t)header;

  if (mp->should_use_locks) {
    rw_lock_wrlock(mp->lock);
  }

  if (header->elem_status == elem_is_not_a_pool_member) {
    // We allocated this buffer when we had exhausted
    // our own buffers.
    if (mp->active_dynamic_memory_buffer_count == 0) {
      // Something is not right, most probably a double free
      if (mp->should_use_locks) {
        rw_lock_unlock(mp->lock);
      }
      ccol_assert(false);
    }
    --mp->active_dynamic_memory_buffer_count;
    _mem_free(mp->m_procs, header);
    if (mp->should_use_locks) {
      rw_lock_unlock(mp->lock);
    }
    return;
  }

  if (valid_mempool_addr(mp, c_header)) {
    if (header->elem_status != elem_is_taken) {
      // This block seems to be tampered with
      addr_t addr = header->next;
      if (valid_mempool_addr(mp, (uintptr_t)(*addr))) {
        // Was this address returned to the pool before?
        if (header->elem_status == elem_is_free) {
          // Double free!
          if (mp->should_use_locks) {
            rw_lock_unlock(mp->lock);
          }
          ccol_assert(false);
        }
      }

      // Somehow the entry got overwritten.
      if (mp->should_use_locks) {
        rw_lock_unlock(mp->lock);
      }
      ccol_assert(false);
    }

    header->elem_status = elem_is_free;
    header->next = (addr_t)mp->free_inst;
    mp->free_inst = header;
    ++mp->free_elem_count;
  } else {
    if (mp->should_use_locks) {
      rw_lock_unlock(mp->lock);
    }
    ccol_assert(false);
  }

  if (mp->should_use_locks) {
    rw_lock_unlock(mp->lock);
  }
}

/* Public free entry point. Accepts a NULL pointer without asserting (matching
 * the behaviour of standard free). Walks back from the user pointer to the
 * internal header using ENTRY_TO_HEADER, verifies the mempool_mark sentinel,
 * then calls __mempool_free_entry to perform the actual release. */
void _mempool_free_entry(void *entry) {
  if (!entry) {
    // Let's resemble the dynamic memory allocation approach here.
    // Releasing a NULL pointer is acceptable.
    return;
  }

  __internal_entry_header *header = ENTRY_TO_HEADER(entry);

  // Let's check the invariant parts.
  if (!header) {
    ccol_assert(false);
  }

  if (!header->pool_ptr) {
    ccol_assert(false);
  }

  if (header->pool_ptr->mempool_mark != _mempool_mark) {
    ccol_assert(false);
  }

  // Passed the initial checks, no corruption so far.
  __mempool_free_entry(header->pool_ptr, header);
}

/* Returns the total number of elements the pool was sized for (free + in-use).
 * Acquires the read lock when the pool is in multi-threaded mode. */
size_t mempool_total_capacity(mempool *mp) {
  if (!mp) {
    ccol_assert(false);
  }

  size_t result = 0;

  if (mp->should_use_locks) {
    rw_lock_rdlock(mp->lock);
  }

  result = mp->total_elem_count;

  if (mp->should_use_locks) {
    rw_lock_unlock(mp->lock);
  }

  return result;
}

/* Returns the number of pool-owned elements currently in use (total - free).
 * Does not include dynamic fallback allocations. */
size_t mempool_used_count(mempool *mp) {
  if (!mp) {
    ccol_assert(false);
  }

  size_t result = 0;

  if (mp->should_use_locks) {
    rw_lock_rdlock(mp->lock);
  }

  result = mp->total_elem_count - mp->free_elem_count;

  if (mp->should_use_locks) {
    rw_lock_unlock(mp->lock);
  }

  return result;
}

/* Returns the number of dynamic fallback allocations currently outstanding.
 * Non-zero means pool entries were exhausted at some point. */
size_t mempool_dynamic_allocs_count(mempool *mp) {
  if (!mp) {
    ccol_assert(false);
  }

  size_t result = 0;

  if (mp->should_use_locks) {
    rw_lock_rdlock(mp->lock);
  }

  result = mp->active_dynamic_memory_buffer_count;

  if (mp->should_use_locks) {
    rw_lock_unlock(mp->lock);
  }

  return result;
}

// Ranged memory pool implementation starts
const size_t min_allowed_smallest_size = 16;
const size_t max_allowed_largest_size = 9223372036854775808UL;

struct r_mempool {
  mempool **mem_pools;  // The real memory pools
  mempool pseudo_pool;
  r_memory_fallback_policy_t fb_policy;
  bool should_use_locks;
  ccol_memmgmt_procs_t *m_procs;
  size_t number_of_mempools;
  size_t *reverse_size_lookup_array;
  size_t reverse_size_lookup_array_length;
  size_t smallest_size;
  size_t largest_size;
  size_t smallest_elem_count;
};

/* Destroys the ranged memory pool. If the fallback policy is
 * fallback_at_last_exhaustion and outstanding dynamic entries exist, asserts
 * to expose the leak (same philosophy as _mempool_destroy). Each internal
 * sub-pool is destroyed individually before the sub-pool array is freed. */
void _r_mempool_destroy(r_mempool *rmp) {
  if (rmp) {
    if (rmp->fb_policy == fallback_at_last_exhaustion) {
      if (mempool_dynamic_allocs_count(&rmp->pseudo_pool) > 0) {
        // We have dynamic pointers that have not been freed yet
        // That's a potential leak, let's make it noticed.
        ccol_assert(false);
      }
    }

    if (rmp->mem_pools) {
      for (size_t i = 0; i < rmp->number_of_mempools; ++i) {
        if (rmp->mem_pools[i]) {
          mempool_destroy(rmp->mem_pools[i]);
        }
      }
      _mem_free(rmp->m_procs, rmp->mem_pools);
    }
    if (rmp->reverse_size_lookup_array) {
      _mem_free(rmp->m_procs, rmp->reverse_size_lookup_array);
    }

    if (rmp->fb_policy == fallback_at_last_exhaustion) {
      if (rmp->should_use_locks) {
        rw_lock_destroy(rmp->pseudo_pool.lock);
      }
    }

    if (rmp->m_procs) {
      ccol_free_t free_func = rmp->m_procs->free;
      free_func(rmp->m_procs);
      free_func(rmp);
    } else {
      mem_free(rmp);
    }
  }
}

/* Validates the power-of-two size parameters for an r_mempool and derives the
 * number of internal sub-pools. The constraint
 * smallest_elem_count_power_of_two >= (largest - smallest) ensures that each
 * successively larger sub-pool can have at least one element (dividing the
 * count by 2 for each doubling of size). Populates rmp fields on success. */
bool assess_r_mempool_create_inputs(r_mempool *rmp,
                                    uint8_t smallest_size_power_of_two,
                                    uint8_t largest_size_power_of_two,
                                    uint8_t smallest_elem_count_power_of_two,
                                    r_memory_fallback_policy_t fb_policy,
                                    bool single_threaded, char **err) {
  if (smallest_size_power_of_two == 0 || largest_size_power_of_two == 0 ||
      smallest_elem_count_power_of_two == 0) {
    if (err) {
      *err = CCOL_ERR_STR("zero sizes are not acceptable");
    }
    return false;
  }

  if (largest_size_power_of_two <= smallest_size_power_of_two ||
      (smallest_elem_count_power_of_two <
       (largest_size_power_of_two - smallest_size_power_of_two))) {
    if (err) {
      *err = CCOL_ERR_STR("inconsistent sizes are not acceptable");
    }
    return false;
  }

  if (fb_policy < 0 || fb_policy >= __fallback_end_place_holder) {
    if (err) {
      *err = CCOL_ERR_STR("unknown fallback policy");
    }
    return false;
  }

  size_t smallest_size = (size_t)1 << smallest_size_power_of_two;
  size_t largest_size = (size_t)1 << largest_size_power_of_two;
  size_t smallest_elem_count = (size_t)1 << smallest_elem_count_power_of_two;

  if (largest_size > max_allowed_largest_size ||
      smallest_size < min_allowed_smallest_size) {
    if (err) {
      *err = CCOL_ERR_STR("sizes beyond limits are not acceptable");
    }
    return false;
  }

  rmp->should_use_locks = !single_threaded;
  rmp->smallest_size = smallest_size;
  rmp->largest_size = largest_size;
  rmp->smallest_elem_count = smallest_elem_count;
  rmp->number_of_mempools =
      largest_size_power_of_two - smallest_size_power_of_two + 1;
  rmp->reverse_size_lookup_array_length = (largest_size) / (smallest_size);

  return true;
}

/* Initialises the pseudo_pool embedded in rmp, which acts as a sentinel/tracker
 * for global dynamic fallback allocations under the fallback_at_last_exhaustion
 * policy. In that mode the pseudo_pool's active_dynamic_memory_buffer_count
 * tracks all dynamic entries across all sub-pools. */
bool init_r_mempool_pseudo_pool(r_mempool *rmp) {
  mem_zero(&rmp->pseudo_pool, sizeof(mempool));
  if (rmp->fb_policy == fallback_at_last_exhaustion) {
    if (rmp->should_use_locks) {
      if (rw_lock_init(rmp->pseudo_pool.lock) != 0) {
        return false;
      }
      rmp->pseudo_pool.should_use_locks = true;
    }
    rmp->pseudo_pool.fallback_to_dynamic_memory = true;
    rmp->pseudo_pool.mempool_mark = _mempool_mark;
  }

  return true;
}

/* Creates all sub-pools ranging from smallest_size to largest_size, each with
 * half as many elements as the previous (compensating for twice the element
 * size). The fallback policy per sub-pool is fallback_at_first_exhaustion iff
 * the r_mempool's overall policy is also first-exhaustion. */
bool init_r_mempool_internal_pools(r_mempool *rmp, char **err) {
  if (!init_r_mempool_pseudo_pool(rmp)) {
    if (err) {
      *err = CCOL_ERR_STR("failed to initialize the pseudo_pool");
    }
    return false;
  }

  rmp->mem_pools = (mempool **)_mem_alloc(
      rmp->m_procs, rmp->number_of_mempools * sizeof(mempool *));
  if (!rmp->mem_pools) {
    // The cleanup will be performed by the caller.
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate mem_pools array");
    }
    return false;
  }
  mem_zero(rmp->mem_pools, rmp->number_of_mempools * sizeof(mempool *));

  size_t first_size = rmp->smallest_size;
  size_t last_size = rmp->largest_size;
  size_t first_count = rmp->smallest_elem_count;

  for (size_t esize = first_size, ecount = first_count, index = 0;
       esize <= last_size; esize *= 2, ecount /= 2, ++index) {
    rmp->mem_pools[index] = mempool_create(
        ecount, esize, rmp->fb_policy == fallback_at_first_exhaustion,
        !rmp->should_use_locks, rmp->m_procs, err);
    if (!rmp->mem_pools[index]) {
      // The cleanup will be performed by the caller.
      return false;
    }
  }

  return true;
}

/* Builds a lookup table that maps any allocation size (expressed as
 * (size-1)/smallest_size) to the index of the appropriate sub-pool. This
 * pre-computation allows r_mempool_alloc_entry to find the right sub-pool in
 * O(1) instead of scanning. Entries double at each power-of-two boundary
 * matching the doubling of sub-pool element sizes. */
bool init_r_mempool_reverse_size_lookup_array(r_mempool *rmp, char **err) {
  rmp->reverse_size_lookup_array = (size_t *)_mem_alloc(
      rmp->m_procs, rmp->largest_size / rmp->smallest_size * sizeof(size_t));
  if (!rmp->reverse_size_lookup_array) {
    // The cleanup will be performed by the caller.
    if (err) {
      *err = CCOL_ERR_STR(
          "failed to allocate r_mempool reverse size lookup array");
    }
    return false;
  }

  size_t threshold_plus_one = 1;
  size_t target_index = 0;
  for (size_t i = 0; i < rmp->reverse_size_lookup_array_length; ++i) {
    rmp->reverse_size_lookup_array[i] = target_index;
    if (i == (threshold_plus_one - 1)) {
      ++target_index;
      threshold_plus_one *= 2;
    }
  }

  return true;
}

/* Creates a ranged memory pool spanning power-of-two sizes from
 * 2^smallest_size_power_of_two to 2^largest_size_power_of_two bytes. The pool
 * with the smallest element count holds 2^smallest_elem_count_power_of_two
 * elements; larger sub-pools hold proportionally fewer elements. */
r_mempool *r_mempool_create(uint8_t smallest_size_power_of_two,
                            uint8_t largest_size_power_of_two,
                            uint8_t smallest_elem_count_power_of_two,
                            r_memory_fallback_policy_t fb_policy,
                            bool single_threaded,
                            ccol_memmgmt_procs_t *mmgmt_procs, char **err) {
  if (err) {
    *err = NULL;
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  r_mempool *rmp = (r_mempool *)_mem_calloc(mmgmt_procs, 1, sizeof(r_mempool));
  if (!rmp) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate r_mempool struct");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(rmp, mmgmt_procs, err)) {
    _mem_free(mmgmt_procs, rmp);
    return NULL;
  }

  if (!assess_r_mempool_create_inputs(
          rmp, smallest_size_power_of_two, largest_size_power_of_two,
          smallest_elem_count_power_of_two, fb_policy, single_threaded, err)) {
    r_mempool_destroy(rmp);
    return NULL;
  }
  rmp->fb_policy = fb_policy;

  if (!init_r_mempool_internal_pools(rmp, err)) {
    r_mempool_destroy(rmp);
    return NULL;
  }

  if (!init_r_mempool_reverse_size_lookup_array(rmp, err)) {
    r_mempool_destroy(rmp);
    return NULL;
  }

  return rmp;
}

/* Carves the preallocated_buffer into contiguous sub-buffer segments, one per
 * sub-pool, using the same size/count progression as
 * init_r_mempool_internal_pools. Asserts that cumulative_size equals
 * preallocated_buffer_size to ensure the caller has provided a buffer of
 * exactly the right size. */
bool init_preallocated_r_mempool_internal_pools(r_mempool *rmp,
                                                void *preallocated_buffer,
                                                size_t preallocated_buffer_size,
                                                char **err) {
  if (!init_r_mempool_pseudo_pool(rmp)) {
    if (err) {
      *err = CCOL_ERR_STR("failed to initialize the pseudo_pool");
    }
    return false;
  }

  rmp->mem_pools = (mempool **)_mem_calloc(
      rmp->m_procs, rmp->number_of_mempools, sizeof(mempool *));
  if (!rmp->mem_pools) {
    // The cleanup will be performed by the caller.
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate mem_pools array");
    }
    return false;
  }

  size_t first_size = rmp->smallest_size;
  size_t last_size = rmp->largest_size;
  size_t first_count = rmp->smallest_elem_count;

  size_t cumulative_size = 0;

  for (size_t esize = first_size, ecount = first_count, index = 0;
       esize <= last_size; esize *= 2, ecount /= 2, ++index) {
    // Using different adjacent segments of the preallocated buffer
    // with different sizes to accommodate different pools of memory.
    uint8_t *sub_buffer = (uint8_t *)preallocated_buffer + cumulative_size;
    size_t sub_buffer_size =
        ecount * (esize + offsetof(__internal_entry_header, next));
    rmp->mem_pools[index] = mempool_create_from_preallocated_buffer(
        sub_buffer, sub_buffer_size, esize,
        rmp->fb_policy == fallback_at_first_exhaustion, !rmp->should_use_locks,
        rmp->m_procs, err);
    if (!rmp->mem_pools[index]) {
      // The cleanup will be performed by the caller.
      return false;
    }
    cumulative_size += sub_buffer_size;
  }

  if (cumulative_size != preallocated_buffer_size) {
    if (err) {
      *err = CCOL_ERR_STR("buffer sizes differ");
    }
    return false;
  }

  return true;
}

/* Like r_mempool_create but uses an externally supplied buffer for all sub-pool
 * element storage. The buffer is not freed by the r_mempool; the caller is
 * responsible for its lifetime. */
r_mempool *r_mempool_create_from_preallocated_buffer(
    void *buffer, size_t buf_size, uint8_t smallest_size_power_of_two,
    uint8_t largest_size_power_of_two, uint8_t smallest_elem_count_power_of_two,
    r_memory_fallback_policy_t fb_policy, bool single_threaded,
    ccol_memmgmt_procs_t *mmgmt_procs, char **err) {
  if (err) {
    *err = NULL;
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  r_mempool *rmp = (r_mempool *)_mem_calloc(mmgmt_procs, 1, sizeof(r_mempool));
  if (!rmp) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate r_mempool struct");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(rmp, mmgmt_procs, err)) {
    _mem_free(mmgmt_procs, rmp);
    return NULL;
  }

  if (!assess_r_mempool_create_inputs(
          rmp, smallest_size_power_of_two, largest_size_power_of_two,
          smallest_elem_count_power_of_two, fb_policy, single_threaded, err)) {
    r_mempool_destroy(rmp);
    return NULL;
  }
  rmp->fb_policy = fb_policy;

  if (!init_preallocated_r_mempool_internal_pools(rmp, buffer, buf_size, err)) {
    r_mempool_destroy(rmp);
    return NULL;
  }

  if (!init_r_mempool_reverse_size_lookup_array(rmp, err)) {
    r_mempool_destroy(rmp);
    return NULL;
  }

  return rmp;
}

/* Allocates a dynamic entry tagged as elem_is_not_a_pool_member through the
 * pseudo_pool, which acts as a tracker for global fallback allocations under
 * the fallback_at_last_exhaustion policy. The pseudo_pool itself has no object
 * buffer; it only maintains the active_dynamic_memory_buffer_count counter. */
void *mempool_pseudo_alloc_entry(mempool *mp, size_t elem_size) {
  void *result = NULL;

  if (elem_size < sizeof(addr_t)) {
    elem_size = sizeof(addr_t);
  }

  size_t extended_elem_size = USER_SIZE_TO_EXTENDED_SIZE(elem_size);

  if (mp->should_use_locks) {
    rw_lock_wrlock(mp->lock);
  }

  void *new_buffer = _mem_alloc(mp->m_procs, extended_elem_size);
  if (new_buffer) {
    __internal_entry_header *header = (__internal_entry_header *)new_buffer;
    header->elem_status = elem_is_not_a_pool_member;
    header->pool_ptr = mp;
    result = (void *)&header->next;
    ++mp->active_dynamic_memory_buffer_count;
  }

  if (mp->should_use_locks) {
    rw_lock_unlock(mp->lock);
  }

  return result;
}

/* Allocates a buffer of at least size bytes. The reverse_size_lookup_array is
 * used to find the smallest sub-pool whose element size fits size; if that
 * sub-pool is exhausted the next larger one is tried (escalation). Only if all
 * sub-pools are exhausted does the fallback_at_last_exhaustion path kick in. */
void *r_mempool_alloc_entry(r_mempool *rmp, size_t size) {
  if (!rmp) {
    ccol_assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return NULL;
  }

  size_t pool_index =
      rmp->reverse_size_lookup_array[(size - 1) / rmp->smallest_size];

  void *result = NULL;

  for (; pool_index < rmp->number_of_mempools; ++pool_index) {
    result = mempool_alloc_entry(rmp->mem_pools[pool_index]);
    if (result) {
      break;
    }
  }

  if (!result && rmp->fb_policy == fallback_at_last_exhaustion) {
    result = mempool_pseudo_alloc_entry(&rmp->pseudo_pool, size);
  }

  return result;
}

/* Allocates and zeroes a buffer of at least size bytes from the r_mempool. */
void *r_mempool_calloc_entry(r_mempool *rmp, size_t size) {
  void *result = r_mempool_alloc_entry(rmp, size);

  if (result) {
    mem_zero(result, size);
  }

  return result;
}

/* Reallocates addr to a buffer of at least size bytes. If the requested size
 * maps to the same extended element size as the current allocation, the
 * original pointer is returned unchanged (no copy). Otherwise a new entry is
 * allocated, the smaller of old/new user sizes is copied, and the old entry is
 * freed. */
void *r_mempool_realloc_entry(r_mempool *rmp, void *addr, size_t size) {
  if (!rmp) {
    ccol_assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return NULL;
  }

  size_t min_user_size = 0;

  if (addr) {
    __internal_entry_header *header = ENTRY_TO_HEADER(addr);

    size_t index = (size - 1) / rmp->smallest_size;
    size_t new_ext_size = rmp->mem_pools[rmp->reverse_size_lookup_array[index]]
                              ->extended_elem_size;

    if (new_ext_size == header->pool_ptr->extended_elem_size) {
      // The requested size matches the current
      // size, return the original pointer.
      return addr;
    }

    if (header->elem_status == elem_is_not_a_pool_member) {
      // pseudo_pool entries have extended_elem_size = 0; computing
      // EXTENDED_SIZE_TO_USER_SIZE(0) underflows. Use the new size as
      // the copy bound — the original per-entry size is not recorded.
      min_user_size = EXTENDED_SIZE_TO_USER_SIZE(new_ext_size);
    } else {
      min_user_size =
          EXTENDED_SIZE_TO_USER_SIZE(header->pool_ptr->extended_elem_size);
      if (EXTENDED_SIZE_TO_USER_SIZE(new_ext_size) < min_user_size) {
        min_user_size = EXTENDED_SIZE_TO_USER_SIZE(new_ext_size);
      }
    }
  }

  void *new_entry = r_mempool_alloc_entry(rmp, size);
  if (new_entry && addr) {
    mem_cpy(new_entry, addr, min_user_size);
    mempool_free_entry(addr);
  }

  return new_entry;
}

/* Returns the used element count of the sub-pool that handles allocations of
 * the given size. Returns 0 for sizes outside the pool's range. */
size_t r_mempool_used_count(r_mempool *rmp, size_t size) {
  if (!rmp) {
    ccol_assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return 0;
  }

  size_t index = (size - 1) / rmp->smallest_size;

  return mempool_used_count(
      rmp->mem_pools[rmp->reverse_size_lookup_array[index]]);
}

/* Returns the total element capacity of the sub-pool that handles allocations
 * of the given size. Returns 0 for sizes outside the pool's range. */
size_t r_mempool_total_capacity(r_mempool *rmp, size_t size) {
  if (!rmp) {
    ccol_assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return 0;
  }

  size_t index = (size - 1) / rmp->smallest_size;

  return mempool_total_capacity(
      rmp->mem_pools[rmp->reverse_size_lookup_array[index]]);
}

/* Returns the number of outstanding dynamic fallback allocations for the
 * given size. For fallback_at_last_exhaustion, the count is held in the single
 * pseudo_pool regardless of size. For fallback_at_first_exhaustion, the count
 * is per-sub-pool. Returns 0 for fallback_disabled or out-of-range sizes. */
size_t r_mempool_dynamic_allocs_count(r_mempool *rmp, size_t size) {
  if (!rmp) {
    ccol_assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return 0;
  }

  if (rmp->fb_policy == fallback_disabled) {
    return 0;
  }

  if (rmp->fb_policy == fallback_at_first_exhaustion) {
    size_t index = (size - 1) / rmp->smallest_size;

    return mempool_dynamic_allocs_count(
        rmp->mem_pools[rmp->reverse_size_lookup_array[index]]);
  }

  return mempool_dynamic_allocs_count(&rmp->pseudo_pool);
}
