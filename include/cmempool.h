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

#pragma once

#include <common.h>

/**
 * @file cmempool.h
 * @brief Fixed-size and ranged memory pool allocators for efficient memory
 * management
 *
 * Provides two types of memory pools:
 * - mempool: Fixed-size element pool with optional dynamic fallback
 * - r_mempool: Ranged pool supporting power-of-2 sizes with intelligent
 * allocation
 *
 * Key features:
 * - O(1) allocation and deallocation
 * - Thread-safe or single-threaded operation
 * - Preallocated buffer support for embedded systems
 * - Corruption detection via assertions
 * - Configurable fallback to dynamic allocation
 * - Zero external fragmentation (fixed-size pools)
 */

/* ========================================================================== */
/*                         BASIC MEMORY POOL                                  */
/* ========================================================================== */

/** @brief Opaque handle to a fixed-size memory pool */
typedef struct mempool mempool;

/**
 * @brief Internal entry header structure (for size calculations only)
 *
 * This structure is internal to the memory pool implementation and SHOULD NOT
 * be accessed directly by users. It is exposed only to enable compile-time
 * calculation of buffer sizes for preallocated memory pools using offsetof().
 *
 * @warning Do not access or modify this structure directly
 * @note Each allocated entry includes this header overhead
 */
typedef struct __internal_entry_header {
  size_t elem_status; /**< Entry state (free/taken/not-a-pool-member) */
  mempool *pool_ptr;  /**< Back-pointer to owning pool */
  uintptr_t *next;    /**< Next free entry (must be last field) */
} __internal_entry_header;

/**
 * @brief True iff _ccol_mempool_align_up(x) is well-defined for x, i.e. the
 * rounding-up addition it performs does not overflow size_t.
 *
 * Not meant to be used directly; mempool_create(), mempool_create_from_
 * preallocated_buffer(), and DECLARE_PREALLOCATED_MEMPOOL_BUFFER all check
 * this (directly or via _ccol_mempool_buffer_params_fit) before relying on
 * _ccol_mempool_align_up()'s result.
 */
#define _ccol_mempool_align_up_fits(x) \
  ((x) <= SIZE_MAX - (_Alignof(__internal_entry_header) - 1))

/**
 * @brief Rounds x up to the nearest multiple of __internal_entry_header's
 * own alignment requirement.
 *
 * Every entry in a mempool's contiguous backing buffer (whether heap
 * allocated or supplied via mempool_create_from_preallocated_buffer /
 * DECLARE_PREALLOCATED_MEMPOOL_BUFFER) sits extended_elem_size bytes after
 * the previous one. The buffer's own starting address being aligned for
 * __internal_entry_header (already validated separately) is only enough to
 * keep entry 0 correctly aligned; every later entry's alignment depends on
 * extended_elem_size ITSELF being a multiple of that same alignment, or the
 * per-entry offset drifts out of alignment one stride at a time. This is
 * what makes every per-element header write/read
 * (mempool_init_internal_scalars, mempool_alloc_entry, __mempool_free_
 * entry, ...) for an entry other than the first a misaligned access
 * (undefined behavior, and a real fault risk on strict-alignment
 * architectures) whenever a caller's own elem_size, once the header
 * overhead is added, is not already a multiple of this alignment (e.g. any
 * plain struct built only from < 8-byte-aligned members, like a 12-byte
 * "float x, y, z" struct on a 64-bit platform). Callers must first confirm
 * x fits via _ccol_mempool_align_up_fits(x), since the addition below can
 * otherwise overflow.
 */
#define _ccol_mempool_align_up(x)                    \
  (((x) + (_Alignof(__internal_entry_header) - 1)) & \
   ~(size_t)(_Alignof(__internal_entry_header) - 1))

/**
 * @brief Create a fixed-size memory pool
 *
 * Creates a memory pool that manages a fixed number of fixed-size elements.
 * All allocations return elements of the same size. Provides O(1) allocation
 * and deallocation using a free list.
 *
 * @param elem_count Number of elements in the pool (must be > 0)
 * @param elem_size Size of each element in bytes (must be > 0, minimum
 * sizeof(uintptr_t), maximum SIZE_MAX minus the internal header overhead)
 * @param fallback_to_dynamic_memory If true, allocate from heap when pool
 * exhausted
 * @param single_threaded If true, omit locking (faster but not thread-safe)
 * @param mmgmt_procs Custom memory management procedures, or NULL for default
 * malloc/free
 * @param err Optional pointer to receive error string on failure (pass NULL to
 * ignore)
 *
 * @return Pointer to newly created memory pool, or NULL on failure
 *
 * @note Actual element size includes header overhead
 * (offsetof(__internal_entry_header, next)), further rounded up to
 * __internal_entry_header's own alignment requirement so every entry in the
 * pool's contiguous buffer, not just the first, lands on a properly aligned
 * address
 * @note If elem_size < sizeof(uintptr_t), it is rounded up
 * @note Returns NULL with error if elem_size would overflow size_t once the
 * header overhead and alignment rounding are added
 * @note Thread-safe if single_threaded is false (uses read-write locks)
 * @note With fallback enabled, pool never fails allocation (until system OOM)
 * @note The pool must be destroyed with mempool_destroy() when done
 *
 * @see mempool_create_from_preallocated_buffer
 * @see mempool_destroy
 * @see mempool_alloc_entry
 */
mempool *mempool_create(size_t elem_count, size_t elem_size,
                        bool fallback_to_dynamic_memory, bool single_threaded,
                        ccol_memmgmt_procs_t *mmgmt_procs, char **err);

/**
 * @brief Declare a preallocated buffer for a memory pool
 *
 * Macro that declares a uint8_t array sized correctly for a memory pool with
 * the specified parameters. The buffer can then be passed to
 * mempool_create_from_preallocated_buffer().
 *
 * @param name Variable name for the buffer
 * @param elem_count Number of elements the pool will hold
 * @param elem_size Size of each element in bytes
 *
 * @note Automatically includes header overhead in size calculation
 * @note Useful for embedded systems or avoiding heap allocation
 * @note The declared buffer is aligned suitably for __internal_entry_header
 * (whose fields include a size_t and a pointer), so it can be handed directly
 * to mempool_create_from_preallocated_buffer() without any extra alignment
 * considerations on the caller's part
 * @note elem_size smaller than sizeof(uintptr_t) is rounded up to fit the
 * free-list pointer before the buffer's size is computed, the same way
 * mempool_create_from_preallocated_buffer() itself rounds it; this keeps the
 * declared buffer's element count in agreement with what that constructor
 * will actually carve it into, rather than the buffer being sized for the
 * caller's smaller, unrounded elem_size while the constructor divides it up
 * using the larger, rounded one
 * @note The per-element stride (rounded elem_size + header overhead) is
 * further rounded up to __internal_entry_header's own alignment requirement,
 * again matching mempool_create_from_preallocated_buffer()'s own stride
 * exactly; this keeps every entry past the first one in the resulting pool
 * correctly aligned, not just entry 0
 * @note elem_count must be nonzero; rejected at compile time (via a
 * _Static_assert), rather than silently producing an undersized (or, for
 * elem_count == 0, zero-length) array, if elem_count is zero or if
 * elem_count * (rounded and aligned elem_size + header overhead) would
 * overflow size_t
 *
 * @see mempool_create_from_preallocated_buffer
 *
 * Example:
 * @code
 * DECLARE_PREALLOCATED_MEMPOOL_BUFFER(my_buffer, 100, 64);
 * mempool *mp = mempool_create_from_preallocated_buffer(
 *     my_buffer, sizeof(my_buffer), 64, false, false, NULL, NULL);
 * @endcode
 */
/* The leading ccol_max(elem_size, sizeof(uintptr_t)) <= SIZE_MAX -
 * offsetof(...) term below guards the FIRST addition this macro performs
 * (bumped elem_size + header overhead), before that sum is ever handed to
 * _ccol_mempool_align_up_fits(), which only proves the SECOND addition
 * (align_up's own rounding step) is safe for whatever value it is given.
 * Without this leading term, an elem_size within offsetof(__internal_entry_
 * header, next) bytes of SIZE_MAX made that first addition itself silently
 * wrap to a small value before _ccol_mempool_align_up_fits ever saw it,
 * so every later term in this macro (and DECLARE_PREALLOCATED_MEMPOOL_
 * BUFFER's own array-size expression, which repeats the identical
 * unprotected addition) was evaluated against that wrapped, wrong value
 * instead of failing the way mempool_create()'s equivalent, subtraction-
 * only check already does for the exact same elem_size range. */
#define _ccol_mempool_buffer_params_fit(elem_count, elem_size)             \
  ((elem_count) > 0 &&                                                     \
   ccol_max((elem_size), sizeof(uintptr_t)) <=                             \
       SIZE_MAX - offsetof(__internal_entry_header, next) &&               \
   _ccol_mempool_align_up_fits(ccol_max((elem_size), sizeof(uintptr_t)) +  \
                               offsetof(__internal_entry_header, next)) && \
   _ccol_mempool_align_up(ccol_max((elem_size), sizeof(uintptr_t)) +       \
                          offsetof(__internal_entry_header, next)) <=      \
       SIZE_MAX / (elem_count))

#define DECLARE_PREALLOCATED_MEMPOOL_BUFFER(name, elem_count, elem_size)     \
  _Static_assert(                                                            \
      _ccol_mempool_buffer_params_fit((elem_count), (elem_size)),            \
      "DECLARE_PREALLOCATED_MEMPOOL_BUFFER: elem_count must be nonzero, "    \
      "and elem_count * elem_size (rounded up for the minimum element "      \
      "size and for __internal_entry_header's own alignment requirement) "   \
      "must not overflow size_t");                                           \
  _Alignas(__internal_entry_header) uint8_t                                  \
      name[(elem_count) *                                                    \
           _ccol_mempool_align_up(ccol_max((elem_size), sizeof(uintptr_t)) + \
                                  offsetof(__internal_entry_header, next))]

/**
 * @brief Create a memory pool from a preallocated buffer
 *
 * Creates a memory pool using a user-provided buffer instead of allocating
 * from the heap. Useful for embedded systems or when heap allocation is
 * undesirable. The buffer must be properly sized using
 * DECLARE_PREALLOCATED_MEMPOOL_BUFFER or manual calculation.
 *
 * @param buffer Pointer to preallocated buffer
 * @param buf_size Size of buffer in bytes
 * @param elem_size Size of each element in bytes (must be > 0; if smaller
 * than sizeof(uintptr_t) it is rounded up, mirroring mempool_create();
 * maximum SIZE_MAX minus the internal header overhead)
 * @param fallback_to_dynamic_memory If true, allocate from heap when pool
 * exhausted
 * @param single_threaded If true, omit locking (faster but not thread-safe)
 * @param mmgmt_procs Custom memory management procedures, or NULL for default
 * malloc/free
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created memory pool, or NULL on failure
 *
 * @note Element count is calculated as buf_size / stride, where stride is
 * elem_size + header_overhead further rounded up to __internal_entry_
 * header's own alignment requirement (so every entry, not just the first,
 * lands on a properly aligned address); the same stride
 * DECLARE_PREALLOCATED_MEMPOOL_BUFFER sizes its own buffer with
 * @note Buffer is not freed by mempool_destroy() (user manages buffer lifetime)
 * @note Pool struct itself is still allocated via mmgmt_procs
 * @note Returns NULL with error if elem_size is zero
 * @note If elem_size is nonzero but < sizeof(uintptr_t), it is rounded up
 * (same as mempool_create())
 * @note Returns NULL with error if elem_size would overflow size_t once the
 * header overhead and alignment rounding are added
 * @note Returns NULL with error if buffer is not sufficiently aligned for
 * __internal_entry_header (a buffer declared via
 * DECLARE_PREALLOCATED_MEMPOOL_BUFFER is always properly aligned; a
 * hand-rolled buffer must be aligned to at least _Alignof(max_align_t) or
 * explicitly to __internal_entry_header's own alignment requirement)
 *
 * @see DECLARE_PREALLOCATED_MEMPOOL_BUFFER
 * @see mempool_create
 * @see mempool_destroy
 */
mempool *mempool_create_from_preallocated_buffer(
    void *buffer, size_t buf_size, size_t elem_size,
    bool fallback_to_dynamic_memory, bool single_threaded,
    ccol_memmgmt_procs_t *mmgmt_procs, char **err);

/**
 * @brief Destroy a memory pool (internal function)
 *
 * @param mp Memory pool to destroy
 *
 * @warning Do not call directly - use mempool_destroy() macro instead
 */
void _mempool_destroy(mempool *mp);

/**
 * @brief Destroy a memory pool and set pointer to NULL
 *
 * Frees all resources associated with the memory pool. For pools created from
 * preallocated buffers, the buffer itself is not freed (only the pool struct).
 *
 * @param mp Memory pool to destroy (will be set to NULL after destruction)
 *
 * @warning Unfreed, dynamically allocated pointers via the fallback memory
 * management mechanism will make this function assert to make sure a potential
 * leak does not go unnoticed. If no fallback is requested during the creation
 * of this pool, such an assert does not happen. As a rule of thumb, the
 * allocated buffers should always be freed.
 * @note Safe to call with NULL pointer
 * @note For preallocated pools, user must manage buffer lifetime
 */
#define mempool_destroy(mp) \
  do {                      \
    _mempool_destroy((mp)); \
    mp = NULL;              \
  } while (0)

/**
 * @brief Allocate an entry from the pool
 *
 * Returns a pointer to an available entry from the pool. If the pool is
 * exhausted and fallback is enabled, allocates from the heap. Contents are
 * uninitialized.
 *
 * @param mp Memory pool to allocate from
 *
 * @return Pointer to allocated entry, or NULL if pool exhausted and no fallback
 *
 * @note O(1) complexity
 * @note Returned memory is uninitialized (use mempool_calloc_entry() for
 * zeroed)
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Will assert if mp is NULL
 * @note Entry must be freed with mempool_free_entry()
 * @note With fallback enabled, only returns NULL on system OOM
 *
 * @see mempool_calloc_entry
 * @see mempool_free_entry
 */
void *mempool_alloc_entry(mempool *mp);

/**
 * @brief Allocate a zero-initialized entry from the pool
 *
 * Like mempool_alloc_entry(), but zeros the memory before returning.
 *
 * @param mp Memory pool to allocate from
 *
 * @return Pointer to zero-initialized entry, or NULL if pool exhausted and no
 * fallback
 *
 * @note O(1) allocation + mem_set cost
 * @note Only zeros user-visible portion (not internal header)
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Will assert if mp is NULL
 *
 * @see mempool_alloc_entry
 * @see mempool_free_entry
 */
void *mempool_calloc_entry(mempool *mp);

/**
 * @brief Free an entry back to the pool (internal function)
 *
 * @param entry Entry to free (obtained from mempool_alloc_entry or
 * mempool_calloc_entry)
 *
 * @warning Do not call directly - use mempool_free_entry() macro instead
 */
void _mempool_free_entry(void *entry);

/**
 * @brief Free an entry back to the pool and set pointer to NULL
 *
 * Returns an allocated entry to the pool's free list for reuse. Performs
 * extensive corruption detection via assertions. Safe to call with NULL.
 *
 * @param entry Entry to free (will be set to NULL after freeing)
 *
 * @note O(1) complexity
 * @note Safe to call with NULL (no-op, like free())
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Detects double-free via assertions
 * @note Detects corruption via magic values and pointer validation
 * @note For dynamically allocated entries (fallback), frees to heap
 * @note Will assert on: double free, invalid entry, corrupted header, wrong
 * pool
 *
 * @see mempool_alloc_entry
 * @see mempool_calloc_entry
 */
#define mempool_free_entry(entry) \
  do {                            \
    _mempool_free_entry((entry)); \
    entry = NULL;                 \
  } while (0)

/**
 * @brief Get total capacity of the pool
 *
 * Returns the total number of fixed-size entries the pool was created with.
 * Does not include dynamically allocated entries.
 *
 * @param mp Memory pool to query
 *
 * @return Total number of pool entries (not counting dynamic allocations)
 *
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Will assert if mp is NULL
 * @note Dynamic allocations are tracked separately
 *
 * @see mempool_used_count
 * @see mempool_dynamic_allocs_count
 */
size_t mempool_total_capacity(mempool *mp);

/**
 * @brief Get number of currently allocated pool entries
 *
 * Returns the number of entries from the pool that are currently allocated
 * (total capacity minus free entries). Does not include dynamic allocations.
 *
 * @param mp Memory pool to query
 *
 * @return Number of pool entries currently in use
 *
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Will assert if mp is NULL
 * @note Dynamic allocations are tracked separately
 * @note used_count = total_capacity - free_count
 *
 * @see mempool_total_capacity
 * @see mempool_dynamic_allocs_count
 */
size_t mempool_used_count(mempool *mp);

/**
 * @brief Get number of dynamically allocated entries
 *
 * Returns the number of entries allocated from the heap because the pool
 * was exhausted. Only non-zero if fallback_to_dynamic_memory was enabled.
 *
 * @param mp Memory pool to query
 *
 * @return Number of heap-allocated entries currently active
 *
 * @note Returns 0 if fallback was not enabled at creation
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Will assert if mp is NULL
 * @note These entries are freed normally with mempool_free_entry()
 *
 * @see mempool_total_capacity
 * @see mempool_used_count
 */
size_t mempool_dynamic_allocs_count(mempool *mp);

/* ========================================================================== */
/*                         RANGED MEMORY POOL                                 */
/* ========================================================================== */

/** @brief Opaque handle to a ranged memory pool */
typedef struct r_mempool r_mempool;

/**
 * @brief Fallback policy for ranged memory pools
 *
 * Determines when and how the ranged memory pool falls back to dynamic
 * allocation when individual pools are exhausted.
 */
typedef enum r_memory_fallback_policy_t {
  fallback_disabled = 0,        /**< Never use dynamic allocation */
  fallback_at_first_exhaustion, /**< Each size pool has its own fallback */
  fallback_at_last_exhaustion,  /**< Only fallback after all pools exhausted */
  __fallback_end_place_holder   /**< Sentinel value (internal use) */
} r_memory_fallback_policy_t;

/**
 * @brief Create a ranged memory pool
 *
 * Creates a collection of fixed-size memory pools covering a range of
 * power-of-2 sizes. Allocations are satisfied from the smallest pool that
 * can accommodate the requested size. Pool sizes double while element counts
 * halve as sizes increase.
 *
 * For example: smallest_size=4, largest_size=6, elem_count=8 creates:
 * - Pool 0: 2^4=16 bytes, 2^8=256 elements
 * - Pool 1: 2^5=32 bytes, 2^7=128 elements
 * - Pool 2: 2^6=64 bytes, 2^6=64 elements
 *
 * @param smallest_size_power_of_two log2 of smallest element size (e.g., 4 for
 * 16 bytes)
 * @param largest_size_power_of_two log2 of largest element size (e.g., 10 for
 * 1024 bytes)
 * @param smallest_elem_count_power_of_two log2 of element count in smallest
 * pool
 * @param fb_policy Fallback policy when pools are exhausted
 * @param single_threaded If true, omit locking (faster but not thread-safe)
 * @param mmgmt_procs Custom memory management procedures, or NULL for default
 * malloc/free
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created ranged memory pool, or NULL on failure
 *
 * @note All parameters must be > 0
 * @note largest_size_power_of_two must be > smallest_size_power_of_two
 * @note smallest_elem_count_power_of_two must be >= (largest - smallest)
 * @note Smallest size must be >= 16 bytes (min_allowed_smallest_size)
 * @note Largest size must be <= 2^63 bytes (max_allowed_largest_size)
 * @note Number of pools = (largest - smallest + 1)
 * @note Thread-safe if single_threaded is false
 * @note The pool must be destroyed with r_mempool_destroy() when done
 *
 * @see r_mempool_create_from_preallocated_buffer
 * @see r_mempool_destroy
 * @see r_mempool_alloc_entry
 */
r_mempool *r_mempool_create(uint8_t smallest_size_power_of_two,
                            uint8_t largest_size_power_of_two,
                            uint8_t number_of_smallest_size_elems_power_of_two,
                            r_memory_fallback_policy_t fb_policy,
                            bool single_threaded,
                            ccol_memmgmt_procs_t *mmgmt_procs, char **err);

/**
 * @brief Calculate buffer size for preallocated ranged memory pool
 *
 * Internal macro for calculating the exact buffer size needed for a ranged
 * memory pool with given parameters. Used by
 * DECLARE_PREALLOCATED_RMEMPOOL_BUFFER.
 *
 * @param SS smallest_size_power_of_two
 * @param LS largest_size_power_of_two
 * @param SC smallest_elem_count_power_of_two
 *
 * @return Size in bytes required for the buffer
 *
 * @note For internal use - use DECLARE_PREALLOCATED_RMEMPOOL_BUFFER instead
 *
 * The second term below is mathematically
 * 2 * 2^SC * header_overhead * (2^N - 1) / 2^N, where N = LS - SS + 1; the
 * r_mempool_create() validation this macro's own parameters must already
 * satisfy (SC >= LS - SS, i.e. SC + 1 >= N) guarantees that division is
 * exact. Rather than forming the full, un-reduced product 2 * 2^SC *
 * header_overhead * (2^N - 1) and dividing it down afterward (which can
 * overflow size_t well before the final division would have brought the
 * value back into range, silently wrapping to a wrong, too-small buffer
 * size), the 2 * 2^SC / 2^N factor is reduced first via a single right
 * shift (exact under the same precondition, and always well-defined
 * since (LS - SS) is itself bounded below size_t's width by
 * r_mempool_create()'s own validation) before multiplying by the much
 * smaller remaining factors. This does not (and cannot) avoid overflow for
 * parameters large enough that the requested buffer itself is not
 * representable in a size_t; it only removes the overflow the original
 * multiply-then-divide ordering introduced on top of that inherent limit.
 */
#define CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE(SS, LS, SC)    \
  (((LS) - (SS) + 1) * ((size_t)1 << (SC)) * ((size_t)1 << (SS)) + \
   ((((size_t)1 << (SC)) >> ((LS) - (SS))) *                       \
    offsetof(__internal_entry_header, next) *                      \
    (((size_t)1 << ((LS) - (SS) + 1)) - 1)))

/**
 * @brief Compile-time guard for DECLARE_PREALLOCATED_RMEMPOOL_BUFFER
 *
 * True iff the three power-of-two parameters produce a well-defined
 * CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE result that fits in a size_t.
 * Not meant to be used directly; DECLARE_PREALLOCATED_RMEMPOOL_BUFFER is the
 * public entry point.
 *
 * Every sub-condition below is ordered so that a term is only ever
 * evaluated once every condition it depends on for well-definedness has
 * already been confirmed true by an earlier, short-circuited && operand
 * (neither GCC nor Clang requires the short-circuited side of && / || in a
 * _Static_assert condition to itself be free of undefined behavior, e.g. an
 * out-of-range shift or a division by zero, so this ordering is sufficient
 * on both compilers this library builds with). In order: every power-of-two
 * exponent is small enough that a 1 << exponent is well-defined;
 * largest_size_power_of_two is genuinely larger than
 * smallest_size_power_of_two; their difference stays small enough that the
 * widest shift CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE performs stays
 * well-defined; number_of_smallest_size_elems_power_of_two is at least
 * largest_size_power_of_two - smallest_size_power_of_two (the exact
 * precondition CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE's own division
 * requires to be mathematically exact, matching what
 * r_mempool_create's own input validation separately enforces at runtime);
 * then each of the two terms CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE
 * sums is checked for overflow via division rather than by forming the
 * (possibly overflowing) product directly, followed by a check that the sum
 * of those two terms does not itself overflow size_t.
 */
#define _ccol_rmempool_buffer_params_fit(SS, LS, SC)                           \
  ((size_t)(SS) < (sizeof(size_t) * CHAR_BIT) &&                               \
   (size_t)(LS) < (sizeof(size_t) * CHAR_BIT) &&                               \
   (size_t)(SC) < (sizeof(size_t) * CHAR_BIT) && (LS) > (SS) &&                \
   ((size_t)(LS) - (size_t)(SS)) < (sizeof(size_t) * CHAR_BIT) - 1 &&          \
   (size_t)(SC) >= ((size_t)(LS) - (size_t)(SS)) &&                            \
   ((size_t)1 << (SC)) <= SIZE_MAX / ((size_t)(LS) - (size_t)(SS) + 1) &&      \
   ((size_t)1 << (SS)) <=                                                      \
       SIZE_MAX / (((size_t)(LS) - (size_t)(SS) + 1) * ((size_t)1 << (SC))) && \
   offsetof(__internal_entry_header, next) <=                                  \
       SIZE_MAX / (((size_t)1 << (SC)) >> ((size_t)(LS) - (size_t)(SS))) &&    \
   (((size_t)1 << ((size_t)(LS) - (size_t)(SS) + 1)) - 1) <=                   \
       SIZE_MAX / ((((size_t)1 << (SC)) >> ((size_t)(LS) - (size_t)(SS))) *    \
                   offsetof(__internal_entry_header, next)) &&                 \
   (((size_t)(LS) - (size_t)(SS) + 1) * ((size_t)1 << (SC)) *                  \
    ((size_t)1 << (SS))) <=                                                    \
       SIZE_MAX - ((((size_t)1 << (SC)) >> ((size_t)(LS) - (size_t)(SS))) *    \
                   offsetof(__internal_entry_header, next) *                   \
                   (((size_t)1 << ((size_t)(LS) - (size_t)(SS) + 1)) - 1)))

/**
 * @brief Declare a preallocated buffer for a ranged memory pool
 *
 * Macro that declares a uint8_t array sized correctly for a ranged memory pool
 * with the specified parameters. The buffer can then be passed to
 * r_mempool_create_from_preallocated_buffer().
 *
 * @param name Variable name for the buffer
 * @param smallest_size_power_of_two log2 of smallest element size
 * @param largest_size_power_of_two log2 of largest element size
 * @param number_of_smallest_size_elems_power_of_two log2 of element count in
 * smallest pool
 *
 * @note Automatically includes header overhead in size calculation
 * @note Useful for embedded systems or avoiding heap allocation
 * @note Buffer contains all sub-pools in contiguous memory
 * @note The declared buffer is aligned suitably for __internal_entry_header
 * (whose fields include a size_t and a pointer), so it can be handed directly
 * to r_mempool_create_from_preallocated_buffer() without any extra alignment
 * considerations on the caller's part; every individual sub-pool segment
 * within the buffer stays correctly aligned as a consequence
 * @note Rejected at compile time (via a _Static_assert), rather than
 * silently producing a wrongly-sized array, if the three parameters would
 * make the pool's own required buffer size overflow size_t, or if
 * number_of_smallest_size_elems_power_of_two is smaller than
 * largest_size_power_of_two - smallest_size_power_of_two (the same
 * precondition r_mempool_create's own input validation enforces at runtime,
 * required here too for CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE's own
 * division to be exact rather than silently truncated)
 *
 * @see CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE
 * @see r_mempool_create_from_preallocated_buffer
 *
 * Example:
 * @code
 * DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(my_buffer, 4, 8, 10);
 * r_mempool *rmp = r_mempool_create_from_preallocated_buffer(
 *     my_buffer, sizeof(my_buffer), 4, 8, 10,
 *     fallback_disabled, false, NULL, NULL);
 * @endcode
 */
#define DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(                            \
    name, smallest_size_power_of_two, largest_size_power_of_two,         \
    number_of_smallest_size_elems_power_of_two)                          \
  _Alignas(__internal_entry_header)                                      \
      uint8_t name[CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE(          \
          (smallest_size_power_of_two), (largest_size_power_of_two),     \
          (number_of_smallest_size_elems_power_of_two))];                \
  _Static_assert(                                                        \
      _ccol_rmempool_buffer_params_fit(                                  \
          (smallest_size_power_of_two), (largest_size_power_of_two),     \
          (number_of_smallest_size_elems_power_of_two)),                 \
      "DECLARE_PREALLOCATED_RMEMPOOL_BUFFER: parameters would make the " \
      "pool's own required buffer size overflow size_t, or violate "     \
      "number_of_smallest_size_elems_power_of_two >= "                   \
      "largest_size_power_of_two - smallest_size_power_of_two")

/**
 * @brief Create a ranged memory pool from a preallocated buffer
 *
 * Creates a ranged memory pool using a user-provided buffer instead of
 * allocating from the heap. The buffer is divided into contiguous segments
 * for each size pool. Useful for embedded systems.
 *
 * @param buffer Pointer to preallocated buffer
 * @param buf_size Size of buffer in bytes (must match calculated size exactly)
 * @param smallest_size_power_of_two log2 of smallest element size
 * @param largest_size_power_of_two log2 of largest element size
 * @param number_of_smallest_size_elems_power_of_two log2 of element count in
 * smallest pool
 * @param fb_policy Fallback policy when pools are exhausted
 * @param single_threaded If true, omit locking (faster but not thread-safe)
 * @param mmgmt_procs Custom memory management procedures, or NULL for default
 * malloc/free
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created ranged memory pool, or NULL on failure
 *
 * @note Buffer size must exactly match
 * CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE
 * @note Buffer is not freed by r_mempool_destroy() (user manages buffer
 * lifetime)
 * @note Pool structs are still allocated via mmgmt_procs
 * @note Buffer contains all sub-pools in adjacent segments
 * @note Returns NULL with error if buffer is not sufficiently aligned for
 * __internal_entry_header (a buffer declared via
 * DECLARE_PREALLOCATED_RMEMPOOL_BUFFER is always properly aligned; a
 * hand-rolled buffer must be aligned to at least _Alignof(max_align_t) or
 * explicitly to __internal_entry_header's own alignment requirement)
 *
 * @see DECLARE_PREALLOCATED_RMEMPOOL_BUFFER
 * @see r_mempool_create
 * @see r_mempool_destroy
 */
r_mempool *r_mempool_create_from_preallocated_buffer(
    void *buffer, size_t buf_size, uint8_t smallest_size_power_of_two,
    uint8_t largest_size_power_of_two,
    uint8_t number_of_smallest_size_elems_power_of_two,
    r_memory_fallback_policy_t fb_policy, bool single_threaded,
    ccol_memmgmt_procs_t *mmgmt_procs, char **err);

/**
 * @brief Destroy a ranged memory pool (internal function)
 *
 * @param rmp Ranged memory pool to destroy
 *
 * @warning Do not call directly - use r_mempool_destroy() macro instead
 */
void _r_mempool_destroy(r_mempool *rmp);

/**
 * @brief Destroy a ranged memory pool and set pointer to NULL
 *
 * Frees all resources associated with the ranged memory pool including all
 * sub-pools. For pools created from preallocated buffers, the buffer itself
 * is not freed.
 *
 * @param rmp Ranged memory pool to destroy (will be set to NULL after
 * destruction)
 *
 * @warning Unfreed, dynamically allocated pointers via the fallback memory
 * management mechanism will make this function assert to make sure a potential
 * leak does not go unnoticed. If no fallback is requested during the creation
 * of this pool, such an assert does not happen. As a rule of thumb, the
 * allocated buffers should always be freed.
 * @note Safe to call with NULL pointer
 * @note For preallocated pools, user must manage buffer lifetime
 */
#define r_mempool_destroy(rmp) \
  do {                         \
    _r_mempool_destroy((rmp)); \
    rmp = NULL;                \
  } while (0)

/**
 * @brief Get number of used entries for a specific size
 *
 * Returns the number of currently allocated entries from the pool that would
 * service the given size. Does not include dynamic allocations.
 *
 * @param rmp Ranged memory pool to query
 * @param size Size in bytes to query (maps to a specific sub-pool)
 *
 * @return Number of entries in use for this size, or 0 on error
 *
 * @note Returns 0 if size is invalid (0 or > largest_size)
 * @note Size is rounded up to next power-of-2 pool
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Dynamic allocations are tracked separately
 *
 * @see r_mempool_total_capacity
 * @see r_mempool_dynamic_allocs_count
 */
size_t r_mempool_used_count(r_mempool *rmp, size_t size);

/**
 * @brief Get total capacity for a specific size
 *
 * Returns the total number of entries in the pool that services the given
 * size. Does not include dynamic allocations.
 *
 * @param rmp Ranged memory pool to query
 * @param size Size in bytes to query (maps to a specific sub-pool)
 *
 * @return Total capacity for this size, or 0 on error
 *
 * @note Returns 0 if size is invalid (0 or > largest_size)
 * @note Size is rounded up to next power-of-2 pool
 * @note Thread-safe if pool was created with single_threaded=false
 *
 * @see r_mempool_used_count
 * @see r_mempool_dynamic_allocs_count
 */
size_t r_mempool_total_capacity(r_mempool *rmp, size_t size);

/**
 * @brief Get number of dynamic allocations for a specific size
 *
 * Returns the number of heap-allocated entries for the given size. Behavior
 * depends on fallback policy.
 *
 * @param rmp Ranged memory pool to query
 * @param size Size in bytes to query
 *
 * @return Number of dynamic allocations, or 0 on error or if disabled
 *
 * @note Returns 0 if fallback_disabled
 * @note With fallback_at_first_exhaustion, tracks per-pool dynamic allocations
 * @note With fallback_at_last_exhaustion, tracks all dynamic allocations
 * @note Returns 0 if size is invalid (0 or > largest_size)
 * @note Thread-safe if pool was created with single_threaded=false
 *
 * @see r_mempool_used_count
 * @see r_mempool_total_capacity
 */
size_t r_mempool_dynamic_allocs_count(r_mempool *rmp, size_t size);

/**
 * @brief Allocate an entry from the ranged pool
 *
 * Allocates memory of the requested size from the most appropriate sub-pool.
 * The size is rounded up to the nearest power-of-2 pool. If the optimal pool
 * is exhausted, tries larger pools. Fallback behavior depends on policy.
 *
 * @param rmp Ranged memory pool to allocate from
 * @param size Size in bytes to allocate (must be > 0 and <= largest_size)
 *
 * @return Pointer to allocated entry, or NULL on failure
 *
 * @note O(1) in common case, O(number_of_pools) worst case
 * @note Returned memory is uninitialized (use r_mempool_calloc_entry() for
 * zeroed)
 * @note Size is rounded up to next power-of-2 pool size
 * @note Tries progressively larger pools if optimal pool is exhausted
 * @note Fallback behavior depends on fb_policy
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Returns NULL if size is 0 or > largest_size
 * @note Entry must be freed with r_mempool_free_entry()
 *
 * @see r_mempool_calloc_entry
 * @see r_mempool_realloc_entry
 * @see r_mempool_free_entry
 */
void *r_mempool_alloc_entry(r_mempool *rmp, size_t size);

/**
 * @brief Allocate a zero-initialized entry from the ranged pool
 *
 * Like r_mempool_alloc_entry(), but zeros the requested number of bytes
 * before returning.
 *
 * @param rmp Ranged memory pool to allocate from
 * @param size Size in bytes to allocate
 *
 * @return Pointer to zero-initialized entry, or NULL on failure
 *
 * @note Zeros exactly 'size' bytes (not the full pool element)
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Will assert if rmp is NULL
 *
 * @see r_mempool_alloc_entry
 * @see r_mempool_realloc_entry
 */
void *r_mempool_calloc_entry(r_mempool *rmp, size_t size);

/**
 * @brief Reallocate an entry to a different size
 *
 * Changes the size of an allocated entry. If the new size does not require
 * moving to a differently-sized underlying allocation, returns the original
 * pointer unchanged (no copy); this applies uniformly whether addr is
 * served by one of the pool's own fixed-size tiers or by the dynamic/heap
 * fallback path. Otherwise, allocates a new entry sized for the request,
 * copies min(old_size, new_size) bytes, and frees the old entry.
 *
 * @param rmp Ranged memory pool
 * @param addr Existing entry to reallocate, or NULL to allocate new
 * @param size New size in bytes (must be > 0 and <= largest_size)
 *
 * @return Pointer to reallocated entry, or NULL on failure
 *
 * @note If addr is NULL, equivalent to r_mempool_alloc_entry()
 * @note If the new size does not require a differently-sized allocation,
 * returns the original pointer unchanged (no copy)
 * @note Otherwise, allocates new, copies data, frees old
 * @note Copies min(old_user_size, new_user_size) bytes
 * @note Old entry is automatically freed if reallocation succeeds
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Returns NULL if size is 0 or > largest_size, WITHOUT touching addr:
 * a non-NULL addr is neither freed nor moved when size itself is invalid,
 * exactly as if the call had never been made; a genuinely failed
 * reallocation (a valid size that simply cannot be satisfied) leaves addr
 * equally untouched, so both failure modes share the same "original
 * pointer still valid, still owned by the caller" contract
 * @note Will assert if addr is non-NULL and was not obtained from this
 * r_mempool (corruption/foreign-pointer detection, mirroring
 * mempool_free_entry())
 *
 * @see r_mempool_alloc_entry
 * @see r_mempool_free_entry
 */
void *r_mempool_realloc_entry(r_mempool *rmp, void *addr, size_t size);

/**
 * @brief Free an entry back to the ranged pool
 *
 * Returns an allocated entry to the appropriate sub-pool's free list.
 * Macro wrapper around mempool_free_entry() that sets pointer to NULL.
 *
 * @param entry Entry to free (will be set to NULL after freeing)
 *
 * @note Works for entries from any sub-pool in the ranged pool
 * @note Also works for dynamically allocated entries (fallback)
 * @note Safe to call with NULL
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Performs corruption detection via assertions
 *
 * @see mempool_free_entry
 * @see r_mempool_alloc_entry
 */
#define r_mempool_free_entry(entry) mempool_free_entry((entry))
