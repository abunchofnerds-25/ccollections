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

#pragma once

#include <common.h>

// Ordinary mempool declarations
typedef struct mempool mempool;

// The struct '__internal_entry_header' is internal and the user should not need
// to make us of it.
// This is only here to calculate 'offsetof(__internal_entry_header, next)' to
// be able to provide precalculated sizes for preallocated buffers. Any other
// approach would be a maintenance burden. So, as a user of this library, you
// are expected to ignore this internal struct.
typedef struct __internal_entry_header {
  size_t elem_status;
  mempool* pool_ptr;
  // The following field should always be the last field.
  uintptr_t* next;
} __internal_entry_header;

mempool* mempool_create(size_t elem_count, size_t elem_size,
                        bool fallback_to_dynamic_memory,
                        bool will_be_accessed_by_only_one_thread,
                        ccol_memmgmt_procs_t* mmgmt_procs, char** err);

#define DECLARE_PREALLOCATED_MEMPOOL_BUFFER(name, elem_count, elem_size) \
  uint8_t                                                                \
      name[elem_count * (elem_size + offsetof(__internal_entry_header, next))]

mempool* mempool_create_from_preallocated_buffer(
    void* buffer, size_t buf_size, size_t elem_size,
    bool fallback_to_dynamic_memory, bool will_be_accessed_by_only_one_thread,
    ccol_memmgmt_procs_t* mmgmt_procs, char** err);

void _mempool_destroy(mempool* mp);

#define mempool_destroy(mp) \
  do {                      \
    _mempool_destroy(mp);   \
    mp = NULL;              \
  } while (0)

void* mempool_alloc_entry(mempool* mp);

void* mempool_calloc_entry(mempool* mp);

void _mempool_free_entry(void* entry);

#define mempool_free_entry(entry) \
  do {                            \
    _mempool_free_entry(entry);   \
    entry = NULL;                 \
  } while (0)

size_t mempool_total_capacity(mempool* mp);

size_t mempool_used_count(mempool* mp);

size_t mempool_dynamic_allocs_count(mempool* mp);

// Ranged mempool declarations
// The ranged mempools are a quick alternative to dynamic memory
// allocation in which the memory is preallocated and served in
// a similar fashion to the heap.
// Here, unlike the 'normal / single-sized' memory pools, the element
// sizes and the number of elements should all be powers of two. Just to
// clarify any of these input values would be used as "2^value".
typedef struct r_mempool r_mempool;

typedef enum r_memory_fallback_policy_t {
  fallback_disabled = 0,
  fallback_at_first_exhaustion,
  fallback_at_last_exhaustion,
  // This one should always remain at the end
  __fallback_end_place_holder
} r_memory_fallback_policy_t;

// The following function will create an 'array' of memory pools
// that covers the buffer sizes starting from the smallest_size
// up to the largest_size. The number of elements in each memory
// pool will be the half of its predecessor, and double of its
// successor. The number of the 'smallest_sized' elements will
// be number_of_smallest_size_elems (Again, 'ceiled' to the
// closest power of two).
r_mempool* r_mempool_create(uint8_t smallest_size_power_of_two,
                            uint8_t largest_size_power_of_two,
                            uint8_t number_of_smallest_size_elems_power_of_two,
                            r_memory_fallback_policy_t fb_policy,
                            bool will_be_accessed_by_only_one_thread,
                            ccol_memmgmt_procs_t* mmgmt_procs, char** err);

// The macro CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE calculates
// the required size for a rmempool. It is not meant to be used
// directly, it is mostly there as a helper to the following macro
// DECLARE_PREALLOCATED_RMEMPOOL_BUFFER.
#define CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE(SS, LS, SC) \
  ((LS - SS + 1) * (1 << SC) * (1 << SS) +                      \
   (2 * (1 << SC) * offsetof(__internal_entry_header, next) *   \
    ((1 << (LS - SS + 1)) - 1)) /                               \
       (1 << (LS - SS + 1)))

// The macro DECLARE_PREALLOCATED_RMEMPOOL_BUFFER declares a preallocated
// buffer using the given name and size parameters. This buffer
// then needs to be passed into the function
// r_mempool_create_from_preallocated_buffer to be 'organized'
// as a memory pool.
#define DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(                    \
    name, smallest_size_power_of_two, largest_size_power_of_two, \
    number_of_smallest_size_elems_power_of_two)                  \
  uint8_t name[CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE(      \
      smallest_size_power_of_two, largest_size_power_of_two,     \
      number_of_smallest_size_elems_power_of_two)]

r_mempool* r_mempool_create_from_preallocated_buffer(
    void* buffer, size_t buf_size, uint8_t smallest_size_power_of_two,
    uint8_t largest_size_power_of_two,
    uint8_t number_of_smallest_size_elems_power_of_two,
    r_memory_fallback_policy_t fb_policy,
    bool will_be_accessed_by_only_one_thread, ccol_memmgmt_procs_t* mmgmt_procs,
    char** err);

void _r_mempool_destroy(r_mempool* rmp);

#define r_mempool_destroy(rmp) \
  do {                         \
    _r_mempool_destroy(rmp);   \
    rmp = NULL;                \
  } while (0)

size_t r_mempool_used_count(r_mempool* rmp, size_t size);

size_t r_mempool_total_capacity(r_mempool* rmp, size_t size);

size_t r_mempool_dynamic_allocs_count(r_mempool* rmp, size_t size);

void* r_mempool_alloc_entry(r_mempool* rmp, size_t size);

void* r_mempool_calloc_entry(r_mempool* rmp, size_t size);

void* r_mempool_realloc_entry(r_mempool* rmp, void* addr, size_t size);

#define r_mempool_free_entry(entry) mempool_free_entry(entry)
