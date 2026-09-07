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

#include <cmempool.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *_mempool_mark = "mempool";

typedef uintptr_t *addr_t;

#define ENTRY_TO_HEADER(entry)                     \
  (__internal_entry_header *)((uintptr_t)(entry) - \
                              offsetof(__internal_entry_header, next))

#define EXTENDED_SIZE_TO_USER_SIZE(extended_elem_size) \
  ((extended_elem_size) - offsetof(__internal_entry_header, next))

#define USER_SIZE_TO_EXTENDED_SIZE(elem_size) \
  ((elem_size) + offsetof(__internal_entry_header, next))

/* Every entry tagged elem_is_not_a_pool_member (a dynamic/fallback entry,
 * whether from a single mempool's own fallback_to_dynamic_memory path or
 * from an r_mempool's pseudo_pool) is allocated with one extra size_t
 * prepended immediately before its __internal_entry_header, recording that
 * entry's genuine user-visible byte count. A pool-owned entry never needs
 * this: its size is always recoverable from its owning pool's fixed
 * extended_elem_size. A dynamic entry's size varies per allocation and has
 * nowhere else to live; without this prefix, nothing can later recover
 * how many bytes of an existing dynamic entry are safe to copy elsewhere
 * (see DYNAMIC_ENTRY_USER_SIZE and r_mempool_realloc_entry). */
#define DYNAMIC_ENTRY_PREFIX_SIZE (sizeof(size_t))

#define DYNAMIC_ENTRY_RAW_BLOCK(header) \
  ((void *)((uint8_t *)(header) - DYNAMIC_ENTRY_PREFIX_SIZE))

#define DYNAMIC_ENTRY_USER_SIZE(header) \
  (*(size_t *)DYNAMIC_ENTRY_RAW_BLOCK(header))

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
  // The r_mempool that owns this sub-pool (or its embedded pseudo_pool), or
  // NULL for a standalone mempool created directly via mempool_create()/
  // mempool_create_from_preallocated_buffer(). Set once, at construction
  // time, by init_r_mempool_internal_pools/init_preallocated_r_mempool_
  // internal_pools/init_r_mempool_pseudo_pool. Lets entry_belongs_to_rmp()
  // answer "does this entry belong to THIS r_mempool" in O(1) via a direct
  // pointer comparison instead of scanning every one of the r_mempool's own
  // sub-pools; declared void* (rather than r_mempool*) purely to avoid a
  // forward declaration, since struct r_mempool is defined later in this
  // file.
  void *owner_rmp;
};

#if UINTPTR_MAX == UINT32_MAX
const size_t elem_is_free = 0xdeadbeef;
const size_t elem_is_taken = 0xfeedcafe;
const size_t elem_is_not_a_pool_member = 0xfadeface;
const size_t elem_is_freed_dynamic_member = 0xdeadc0de;
#elif UINTPTR_MAX == UINT64_MAX
const size_t elem_is_free = 0xdeadbeefdeadbeef;
const size_t elem_is_taken = 0xfeedcafefeedcafe;
const size_t elem_is_not_a_pool_member = 0xfadefacefadeface;
const size_t elem_is_freed_dynamic_member = 0xdeadc0dedeadc0de;
#else
#error "Unexpected pointer size"
#endif

/* Destroys the memory pool. If dynamic fallback was enabled and some
 * dynamically allocated entries have not been freed yet, the call asserts to
 * make that leak visible; we'd rather crash loudly than silently lose memory.
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
static void mempool_init_internal_scalars(mempool *mp, size_t elem_count,
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
  } else if (elem_size > SIZE_MAX - offsetof(__internal_entry_header, next) -
                             (_Alignof(__internal_entry_header) - 1)) {
    // Adding the header overhead to elem_size, and then rounding the result
    // up to __internal_entry_header's own alignment requirement (so every
    // entry past the first one in the pool's contiguous buffer lands on a
    // properly aligned address, not just the first; see
    // _ccol_mempool_align_up()'s own doc comment for why that rounding is
    // necessary at all), would overflow size_t and wrap extended_elem_size
    // to a value smaller than the header itself, causing every per-element
    // header write to land outside the allocated buffer.
    if (err) {
      *err = CCOL_ERR_STR(
          "elem_size is too large: header overhead would "
          "overflow size_t");
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

  size_t extended_elem_size =
      _ccol_mempool_align_up(USER_SIZE_TO_EXTENDED_SIZE(elem_size));
  mp->objects = _mem_calloc(mp->m_procs, elem_count, extended_elem_size);
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
      mp->should_use_locks = false;
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

  if (!buffer || buf_size < (sizeof(__internal_entry_header))) {
    if (err) {
      *err = CCOL_ERR_STR("buffer is not acceptable");
    }
    return NULL;
  }

  // __internal_entry_header's fields (a size_t and a pointer) require
  // alignment the caller's buffer is not guaranteed to already have unless it
  // was declared via DECLARE_PREALLOCATED_MEMPOOL_BUFFER (which is now
  // itself _Alignas(__internal_entry_header)); every per-element header write
  // in mempool_init_internal_scalars would otherwise be a misaligned access,
  // which is undefined behavior and a real fault risk on strict-alignment
  // architectures.
  if ((uintptr_t)buffer % _Alignof(__internal_entry_header) != 0) {
    if (err) {
      *err = CCOL_ERR_STR(
          "buffer is not sufficiently aligned for __internal_entry_header");
    }
    return NULL;
  }

  // Mirrors mempool_create's own three-way elem_size handling exactly,
  // including check ORDER: a genuine zero is a caller mistake worth its own
  // distinct error; an elem_size too large for the header overhead plus
  // alignment rounding to be added without overflowing size_t is rejected
  // next; only once both of those are ruled out is a small-but-nonzero size
  // silently rounded up to fit the free-list pointer. The zero-size and
  // overflow ranges can never overlap with the round-up range in practice
  // (sizeof(addr_t) is tiny), but keeping this function's check order
  // identical to mempool_create's own, rather than merely equivalent, means
  // the two can never silently drift apart if either threshold changes.
  if (elem_size == 0) {
    if (err) {
      *err = CCOL_ERR_STR("elem_size is zero");
    }
    return NULL;
  } else if (elem_size > SIZE_MAX - offsetof(__internal_entry_header, next) -
                             (_Alignof(__internal_entry_header) - 1)) {
    // Adding the header overhead to elem_size, and then rounding the result
    // up to __internal_entry_header's own alignment requirement (see
    // mempool_create's identical check, and _ccol_mempool_align_up()'s own
    // doc comment, for the full explanation), would overflow size_t and
    // wrap extended_elem_size to a value smaller than the header itself,
    // causing every per-element header write to land outside the caller's
    // buffer.
    if (err) {
      *err = CCOL_ERR_STR(
          "elem_size is too large: header overhead would "
          "overflow size_t");
    }
    return NULL;
  } else if (elem_size < sizeof(addr_t)) {
    elem_size = sizeof(addr_t);
  }

  size_t extended_elem_size =
      _ccol_mempool_align_up(USER_SIZE_TO_EXTENDED_SIZE(elem_size));
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
      mp->should_use_locks = false;
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
    // memory allocation mechanisms. The allocation is prefixed with the
    // entry's own user-visible size (see DYNAMIC_ENTRY_USER_SIZE) so a
    // later r_mempool_realloc_entry call can recover it faithfully.
    if (mp->extended_elem_size <= SIZE_MAX - DYNAMIC_ENTRY_PREFIX_SIZE) {
      void *raw_block = _mem_alloc(
          mp->m_procs, DYNAMIC_ENTRY_PREFIX_SIZE + mp->extended_elem_size);
      if (raw_block) {
        __internal_entry_header *header =
            (__internal_entry_header *)((uint8_t *)raw_block +
                                        DYNAMIC_ENTRY_PREFIX_SIZE);
        *(size_t *)raw_block =
            EXTENDED_SIZE_TO_USER_SIZE(mp->extended_elem_size);
        header->elem_status = elem_is_not_a_pool_member;
        header->pool_ptr = mp;
        result = (void *)&header->next;
        ++mp->active_dynamic_memory_buffer_count;
      }
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
 * any other state (double-free or corruption -> assert). */
static void __mempool_free_entry(mempool *mp, __internal_entry_header *header) {
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
    if (mp->active_dynamic_memory_buffer_count == 0 ||
        valid_mempool_addr(mp, c_header)) {
      // Either a double free, or elem_status was corrupted into claiming
      // this is a dynamic entry while its address actually lands inside
      // this pool's own object buffer. Freeing it via _mem_free below would
      // call free() on a pointer that was never independently allocated.
      if (mp->should_use_locks) {
        rw_lock_unlock(mp->lock);
      }
      ccol_assert(false);
    }
    --mp->active_dynamic_memory_buffer_count;
    // Mark the header as a freed dynamic entry BEFORE the block is handed
    // back to the allocator, mirroring the pool-owned path's own
    // elem_status = elem_is_free write a few lines below. Without this, a
    // double free of a dynamic/fallback entry while at least one OTHER
    // dynamic entry is still outstanding was undetectable: the
    // active_dynamic_memory_buffer_count == 0 check above only ever caught
    // this entry being the SOLE outstanding one, and every other check in
    // this branch reads elem_status/pool_ptr from memory that, on a real
    // double free, has already been handed back to (and is now free to be
    // reused by) the underlying allocator; undefined behavior this
    // module has no business relying on. Writing this sentinel here costs
    // nothing extra (the block is still ours to write to right up until
    // the _mem_free call below) and turns a second, illegitimate free of
    // the SAME still-unreused block into a guaranteed, controlled assert
    // instead of a silent double free plus corrupted dynamic-alloc
    // bookkeeping: elem_status will no longer read elem_is_not_a_pool_member
    // (so the branch above is skipped), and the address is never inside
    // this pool's own object buffer (so valid_mempool_addr's else-branch
    // assert below fires). This gives dynamic entries the exact same
    // "detect an immediate double free before the block is reused"
    // guarantee pool-owned entries already have; a double free that races
    // an intervening, unrelated reallocation of the same freed block is
    // inherently undetectable by any scheme that doesn't keep a live
    // registry of outstanding pointers, and is out of scope here.
    header->elem_status = elem_is_freed_dynamic_member;
    // The real allocation starts DYNAMIC_ENTRY_PREFIX_SIZE bytes before
    // header (see mempool_alloc_entry's fallback branch and
    // mempool_pseudo_alloc_entry); free the whole block, not just the
    // header-and-onward portion of it.
    _mem_free(mp->m_procs, DYNAMIC_ENTRY_RAW_BLOCK(header));
    if (mp->should_use_locks) {
      rw_lock_unlock(mp->lock);
    }
    return;
  }

  if (valid_mempool_addr(mp, c_header)) {
    if (header->elem_status != elem_is_taken) {
      // Either a double-free (elem_is_free) or corruption (any other status).
      // Both are fatal, so assert unconditionally without further branching.
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
    // c_header lands outside this pool's own object buffer and elem_status
    // is not elem_is_not_a_pool_member either: either genuine corruption, or
    // a double free of an already-freed dynamic/fallback entry (whose
    // elem_status was rewritten to elem_is_freed_dynamic_member by its first,
    // legitimate free above, specifically so this case is caught here).
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
// The highest power of two a size_t can hold (common.h's own
// max_power_of_two_size_t; 2^63 on a 64-bit size_t, 2^31 on a 32-bit one),
// not SIZE_MAX / 2: the latter is one less than that power of two (e.g.
// 2^63 - 1, not 2^63, on a 64-bit platform, since SIZE_MAX itself is odd),
// which would silently reject the single largest power-of-two size this
// module's own documented "must be <= 2^63 bytes" contract promises.
const size_t max_allowed_largest_size = max_power_of_two_size_t;

struct r_mempool {
  mempool **mem_pools;  // The real memory pools
  mempool pseudo_pool;
  r_memory_fallback_policy_t fb_policy;
  bool should_use_locks;
  ccol_memmgmt_procs_t *m_procs;
  size_t number_of_mempools;
  size_t smallest_size;
  size_t largest_size;
  size_t smallest_elem_count;
  // Kept alongside smallest_size (its own linear value, 1 << this) so
  // r_mempool_pool_index_for_size can turn the "/smallest_size" division
  // every size-to-tier lookup needs into an exact right-shift instead.
  uint8_t smallest_size_power_of_two;
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

    if (rmp->fb_policy == fallback_at_last_exhaustion) {
      if (rmp->pseudo_pool.should_use_locks) {
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
static bool assess_r_mempool_create_inputs(
    r_mempool *rmp, uint8_t smallest_size_power_of_two,
    uint8_t largest_size_power_of_two, uint8_t smallest_elem_count_power_of_two,
    r_memory_fallback_policy_t fb_policy, bool single_threaded, char **err) {
  if (smallest_size_power_of_two == 0 || largest_size_power_of_two == 0 ||
      smallest_elem_count_power_of_two == 0) {
    if (err) {
      *err = CCOL_ERR_STR("zero sizes are not acceptable");
    }
    return false;
  }

  if (largest_size_power_of_two >= sizeof(size_t) * CHAR_BIT ||
      smallest_size_power_of_two >= sizeof(size_t) * CHAR_BIT ||
      smallest_elem_count_power_of_two >= sizeof(size_t) * CHAR_BIT) {
    if (err) {
      *err = CCOL_ERR_STR("power of two exceeds size_t width");
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
  rmp->smallest_size_power_of_two = smallest_size_power_of_two;
  rmp->number_of_mempools =
      largest_size_power_of_two - smallest_size_power_of_two + 1;

  return true;
}

/* Initialises the pseudo_pool embedded in rmp, which acts as a sentinel/tracker
 * for global dynamic fallback allocations under the fallback_at_last_exhaustion
 * policy. In that mode the pseudo_pool's active_dynamic_memory_buffer_count
 * tracks all dynamic entries across all sub-pools. */
static bool init_r_mempool_pseudo_pool(r_mempool *rmp) {
  mem_zero(&rmp->pseudo_pool, sizeof(mempool));
  rmp->pseudo_pool.m_procs = rmp->m_procs;
  rmp->pseudo_pool.owner_rmp = rmp;
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
static bool init_r_mempool_internal_pools(r_mempool *rmp, char **err) {
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
  size_t first_count = rmp->smallest_elem_count;

  for (size_t esize = first_size, ecount = first_count, index = 0;
       index < rmp->number_of_mempools; esize *= 2, ecount /= 2, ++index) {
    rmp->mem_pools[index] = mempool_create(
        ecount, esize, rmp->fb_policy == fallback_at_first_exhaustion,
        !rmp->should_use_locks, rmp->m_procs, err);
    if (!rmp->mem_pools[index]) {
      // The cleanup will be performed by the caller.
      return false;
    }
    rmp->mem_pools[index]->owner_rmp = rmp;
  }

  return true;
}

/* Maps an allocation size to the index of the sub-pool tier that should
 * service it, in true O(1): no table, no memory access at all, just a shift
 * and a hardware bit-scan. Every tier's size is smallest_size * 2^p, so the
 * question "which tier does size belong to" reduces to "how many bits does
 * (size-1), expressed in units of smallest_size, need"; exactly a
 * bit-length computation.
 *
 * This used to be answered by a precomputed reverse_size_lookup_array sized
 * largest_size/smallest_size entries (exponential in the number of tiers,
 * 2^(largest_size_power_of_two - smallest_size_power_of_two), not linear);
 * a caller picking a modest-looking size range (e.g. 16 bytes to 1 GiB, 26
 * tiers) needed a ~512 MiB lookup table just to answer this question. This
 * formula needs no table and no allocation, so it fails only in exactly the
 * cases r_mempool_alloc_entry/etc. already reject up front (size == 0 or
 * size > largest_size). */
static inline size_t r_mempool_pool_index_for_size(r_mempool *rmp,
                                                   size_t size) {
  size_t t = (size - 1) >> rmp->smallest_size_power_of_two;
  if (t == 0) {
    // size <= smallest_size: always the smallest (index 0) tier.
    return 0;
  }
  return (size_t)(sizeof(unsigned long long) * CHAR_BIT) -
         (size_t)__builtin_clzll((unsigned long long)t);
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

  return rmp;
}

/* Carves the preallocated_buffer into contiguous sub-buffer segments, one per
 * sub-pool, using the same size/count progression as
 * init_r_mempool_internal_pools. Asserts that cumulative_size equals
 * preallocated_buffer_size to ensure the caller has provided a buffer of
 * exactly the right size. */
static bool init_preallocated_r_mempool_internal_pools(
    r_mempool *rmp, void *preallocated_buffer, size_t preallocated_buffer_size,
    char **err) {
  if (!init_r_mempool_pseudo_pool(rmp)) {
    if (err) {
      *err = CCOL_ERR_STR("failed to initialize the pseudo_pool");
    }
    return false;
  }

  size_t first_size = rmp->smallest_size;
  size_t first_count = rmp->smallest_elem_count;

  /* Validate the total buffer size before touching any sub-buffer so that
   * an incorrectly sized buffer never causes out-of-bounds writes during
   * sub-pool initialisation. Every per-tier term and the running sum are
   * individually checked for overflow before being formed: for
   * astronomically large (physically unrealizable) parameter combinations,
   * the true required buffer size can itself exceed SIZE_MAX, and a plain
   * multiply-then-accumulate would silently wrap in that case, which could
   * let an incorrectly-small preallocated_buffer_size slip past the
   * expected_size comparison below instead of being rejected. Since the
   * second loop further down recomputes esize/ecount via the exact same,
   * deterministic progression, confirming here that no term or partial sum
   * overflows is sufficient to guarantee the second loop's own arithmetic
   * cannot overflow either. */
  size_t expected_size = 0;
  for (size_t esize = first_size, ecount = first_count, index = 0;
       index < rmp->number_of_mempools; esize *= 2, ecount /= 2, ++index) {
    if (esize > SIZE_MAX - offsetof(__internal_entry_header, next)) {
      if (err) {
        *err = CCOL_ERR_STR(
            "requested configuration's buffer size overflows size_t");
      }
      return false;
    }
    size_t elem_extended_size = esize + offsetof(__internal_entry_header, next);
    if (ecount != 0 && elem_extended_size > SIZE_MAX / ecount) {
      if (err) {
        *err = CCOL_ERR_STR(
            "requested configuration's buffer size overflows size_t");
      }
      return false;
    }
    size_t term = ecount * elem_extended_size;
    if (expected_size > SIZE_MAX - term) {
      if (err) {
        *err = CCOL_ERR_STR(
            "requested configuration's buffer size overflows size_t");
      }
      return false;
    }
    expected_size += term;
  }
  if (expected_size != preallocated_buffer_size) {
    if (err) {
      *err = CCOL_ERR_STR("buffer sizes differ");
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

  size_t cumulative_size = 0;

  for (size_t esize = first_size, ecount = first_count, index = 0;
       index < rmp->number_of_mempools; esize *= 2, ecount /= 2, ++index) {
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
    rmp->mem_pools[index]->owner_rmp = rmp;
    cumulative_size += sub_buffer_size;
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

  if (!buffer) {
    if (err) {
      *err = CCOL_ERR_STR("buffer is NULL");
    }
    return NULL;
  }

  // Checked once, up front, for the same reason
  // mempool_create_from_preallocated_buffer checks it: every sub-pool's own
  // per-element header write assumes properly aligned storage for
  // __internal_entry_header's size_t/pointer fields. Each sub-pool's own
  // creation call would eventually re-derive this (its own segment's offset
  // from the buffer start is always a multiple of the header's alignment
  // requirement, so misalignment can only ever originate from the buffer's
  // own starting address), but checking it here gives a single, immediate,
  // clearly-attributed error instead of a failure surfacing from deep inside
  // sub-pool construction.
  if ((uintptr_t)buffer % _Alignof(__internal_entry_header) != 0) {
    if (err) {
      *err = CCOL_ERR_STR(
          "buffer is not sufficiently aligned for __internal_entry_header");
    }
    return NULL;
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

  return rmp;
}

/* Allocates a dynamic entry tagged as elem_is_not_a_pool_member through the
 * pseudo_pool, which acts as a tracker for global fallback allocations under
 * the fallback_at_last_exhaustion policy. The pseudo_pool itself has no object
 * buffer; it only maintains the active_dynamic_memory_buffer_count counter.
 * Since the pseudo_pool has no fixed element size of its own (unlike a real
 * sub-pool, whose entries can always recover their size from
 * pool_ptr->extended_elem_size), every entry is prefixed with its own
 * user-visible size via DYNAMIC_ENTRY_PREFIX_SIZE/DYNAMIC_ENTRY_USER_SIZE, so
 * a later r_mempool_realloc_entry call can recover exactly how many bytes are
 * safe to copy out of it, instead of guessing (or discarding the data
 * entirely, as an earlier version of this pool did).
 *
 * Unlike mempool_create/mempool_create_from_preallocated_buffer, elem_size is
 * deliberately NOT rounded up to sizeof(addr_t) here. That rounding exists
 * only so a POOL-OWNED entry's user area is always large enough to double as
 * a free-list node (header->next is written into it while the entry sits
 * free); a pseudo_pool entry is a one-off heap allocation that is never
 * linked into any free list; it is simply handed to _mem_free once released,
 * so it has no such minimum-size requirement. Rounding here would also
 * corrupt the entry's own size prefix relative to what the caller actually
 * asked for: r_mempool_realloc_entry's "same size, no move needed" fast path
 * for a pseudo_pool entry compares the caller's newly requested size directly
 * against DYNAMIC_ENTRY_USER_SIZE (this prefix), so recording a size other
 * than exactly what was requested would make that comparison fail for a
 * request under sizeof(addr_t) bytes even when nothing about the request
 * actually changed, defeating the fast path silently on every such call. */
static void *mempool_pseudo_alloc_entry(mempool *mp, size_t elem_size) {
  void *result = NULL;

  // Checked BEFORE computing USER_SIZE_TO_EXTENDED_SIZE(elem_size), mirroring
  // mempool_create/mempool_create_from_preallocated_buffer's own guard.
  // Computing the extended size first and only checking its result
  // afterward (as this used to do) would let an elem_size near SIZE_MAX
  // silently wrap the addition to a small extended_elem_size, passing the
  // post-hoc check while still recording the original, huge elem_size in
  // the entry's own size prefix a few lines below; a dangerously undersized
  // allocation whose size prefix lies about its real capacity to any later
  // r_mempool_realloc_entry call.
  if (elem_size > SIZE_MAX - offsetof(__internal_entry_header, next) -
                      DYNAMIC_ENTRY_PREFIX_SIZE) {
    return NULL;
  }

  size_t extended_elem_size = USER_SIZE_TO_EXTENDED_SIZE(elem_size);

  if (mp->should_use_locks) {
    rw_lock_wrlock(mp->lock);
  }

  void *raw_block =
      _mem_alloc(mp->m_procs, DYNAMIC_ENTRY_PREFIX_SIZE + extended_elem_size);
  if (raw_block) {
    __internal_entry_header *header =
        (__internal_entry_header *)((uint8_t *)raw_block +
                                    DYNAMIC_ENTRY_PREFIX_SIZE);
    *(size_t *)raw_block = elem_size;
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

/* Allocates a buffer of at least size bytes. r_mempool_pool_index_for_size is
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

  size_t pool_index = r_mempool_pool_index_for_size(rmp, size);

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

/* Returns the true user-visible byte count of a single, already-validated
 * entry: for a pool-owned entry (elem_is_taken), that is always recoverable
 * from its owning pool's fixed extended_elem_size; for a dynamic entry
 * (elem_is_not_a_pool_member, whether a single mempool's own
 * fallback_to_dynamic_memory entry or an r_mempool pseudo_pool entry), it is
 * read back from the size prefix mempool_alloc_entry/mempool_pseudo_alloc_
 * entry stored ahead of the header at allocation time. Callers must only
 * pass a header whose elem_status is already known to be one of these two
 * values. */
static inline size_t entry_user_size(__internal_entry_header *header) {
  if (header->elem_status == elem_is_not_a_pool_member) {
    return DYNAMIC_ENTRY_USER_SIZE(header);
  }
  return EXTENDED_SIZE_TO_USER_SIZE(header->pool_ptr->extended_elem_size);
}

/* Returns true iff header->pool_ptr is genuinely one of rmp's own sub-pools
 * or rmp's own pseudo_pool, i.e. header names a real member of THIS specific
 * r_mempool rather than merely any mempool anywhere in the process that
 * happens to share the same global mempool_mark sentinel (every mempool
 * does, by construction). Used by r_mempool_realloc_entry to reject a
 * pointer obtained from a different r_mempool (or from a bare
 * mempool_create() pool) instead of silently accepting it.
 *
 * O(1): every sub-pool (and the pseudo_pool) has its owner_rmp field set
 * once, at construction time, by init_r_mempool_internal_pools/init_
 * preallocated_r_mempool_internal_pools/init_r_mempool_pseudo_pool, so this
 * reduces to a single pointer comparison instead of a linear scan over
 * rmp->mem_pools (which used to run on every single realloc call). A bare
 * mempool_create() pool (never wrapped by any r_mempool) has owner_rmp ==
 * NULL, which can never equal a non-NULL rmp, so it is correctly rejected
 * too. */
static bool entry_belongs_to_rmp(r_mempool *rmp,
                                 __internal_entry_header *header) {
  return header->pool_ptr->owner_rmp == (void *)rmp;
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

  __internal_entry_header *header = NULL;

  if (addr) {
    header = ENTRY_TO_HEADER(addr);
    uintptr_t c_header = (uintptr_t)header;

    // Validate header before trusting anything else about it. Every one of
    // these must hold for a genuinely live entry that really belongs to
    // THIS r_mempool: the mark check rules out garbage/non-mempool memory;
    // entry_belongs_to_rmp rules out a pointer obtained from a different
    // r_mempool (or a standalone mempool_create() pool) that merely shares
    // the same global mempool_mark; the status check rules out an
    // already-freed or corrupted entry; and the two valid_mempool_addr
    // checks mirror __mempool_free_entry's own bounds/stride validation
    // exactly (a taken entry's address must genuinely land inside its
    // claimed pool's own object buffer, and a not-a-pool-member entry's
    // address must NOT, since it was heap-allocated independently of any
    // pool buffer) rather than trusting elem_status/pool_ptr alone. Without
    // these last two checks, a stale/corrupted header that merely LOOKS
    // like a live entry of this r_mempool (mark and pool_ptr intact, but
    // its actual address outside that pool's buffer, or vice versa) would
    // be silently treated as genuine instead of being caught the same way
    // mempool_free_entry() already catches it. All five are checked up
    // front, before the fast "same tier, return addr unchanged" path below,
    // since that path used to skip every one of these checks and could
    // silently hand back an already-freed free-list node to the caller.
    if (!header->pool_ptr || header->pool_ptr->mempool_mark != _mempool_mark ||
        !entry_belongs_to_rmp(rmp, header) ||
        (header->elem_status != elem_is_taken &&
         header->elem_status != elem_is_not_a_pool_member) ||
        (header->elem_status == elem_is_taken &&
         !valid_mempool_addr(header->pool_ptr, c_header)) ||
        (header->elem_status == elem_is_not_a_pool_member &&
         valid_mempool_addr(header->pool_ptr, c_header))) {
      ccol_assert(false);
    }

    if (header->pool_ptr == &rmp->pseudo_pool) {
      // The pseudo_pool (fallback_at_last_exhaustion's shared dynamic-
      // fallback tracker) has no fixed per-tier extended_elem_size the way
      // a real sub-pool does: every pseudo_pool entry is individually
      // heap-allocated to exactly whatever size it was originally
      // requested with (see mempool_pseudo_alloc_entry), and
      // header->pool_ptr->extended_elem_size is therefore always 0 for
      // such an entry, which the "does size route to the same real
      // sub-pool" comparison below could never match. The right analogue
      // of "same tier, no move needed" for a pseudo_pool entry is "the
      // request is for exactly the number of bytes already held"; the
      // one-size-wide "tier" a pseudo_pool entry actually occupies. A
      // genuinely different size still moves (right-sizing), mirroring how
      // a real sub-pool entry shrunk far enough to cross into a smaller
      // tier also moves rather than keeping its oversized block.
      if (size == entry_user_size(header)) {
        return addr;
      }
    } else {
      size_t new_ext_size =
          rmp->mem_pools[r_mempool_pool_index_for_size(rmp, size)]
              ->extended_elem_size;

      if (new_ext_size == header->pool_ptr->extended_elem_size) {
        // The requested size matches the current
        // size, return the original pointer.
        return addr;
      }
    }
  }

  void *new_entry = r_mempool_alloc_entry(rmp, size);
  if (new_entry && addr) {
    // Both sizes are read back from the entries actually involved (never
    // inferred from the *ideal* target tier for `size`), so this is correct
    // regardless of whether either entry ended up pool-owned, escalated to a
    // larger tier, or (under fallback_at_last_exhaustion) routed through
    // the pseudo_pool, which can legitimately hold far fewer bytes than the
    // ideal tier its own size would otherwise suggest.
    size_t old_user_size = entry_user_size(header);
    size_t new_user_size = entry_user_size(ENTRY_TO_HEADER(new_entry));
    size_t copy_size =
        old_user_size < new_user_size ? old_user_size : new_user_size;

    mem_cpy(new_entry, addr, copy_size);
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

  return mempool_used_count(
      rmp->mem_pools[r_mempool_pool_index_for_size(rmp, size)]);
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

  return mempool_total_capacity(
      rmp->mem_pools[r_mempool_pool_index_for_size(rmp, size)]);
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
    return mempool_dynamic_allocs_count(
        rmp->mem_pools[r_mempool_pool_index_for_size(rmp, size)]);
  }

  return mempool_dynamic_allocs_count(&rmp->pseudo_pool);
}
