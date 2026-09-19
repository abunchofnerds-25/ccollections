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

/* Everything declared from here to the end of this header is part of the
 * public ABI of libccollections and is exported from the shared library.
 * The library itself is built with -fvisibility=hidden, so any function or
 * object that is not covered by one of these blocks stays internal to the
 * library, is absent from its dynamic symbol table, and cannot be
 * interposed by, or collide with, a symbol of the same name in the
 * application that links against it. */
#pragma GCC visibility push(default)

/**
 * @file cmempool.h
 * @brief Fixed-size and ranged memory pool allocators for efficient memory
 * management
 *
 * Provides two types of memory pools:
 * - ccol_mempool: Fixed-size element pool with optional dynamic fallback
 * - ccol_r_mempool: Ranged pool supporting power-of-2 sizes with intelligent
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
typedef struct ccol_mempool ccol_mempool;

/**
 * @brief The alignment every entry a pool hands out is guaranteed to meet.
 *
 * At least what malloc() guarantees, so any object a caller can store in heap
 * memory can be stored in a pool entry. The stride between entries is rounded
 * up to a multiple of this, which is what extends the guarantee from the first
 * entry to every entry: a buffer whose start is correctly aligned still hands
 * out misaligned entries if the stride is not itself a multiple of the
 * alignment, since the offset drifts one stride at a time.
 *
 * A fixed constant rather than _Alignof(max_align_t), because it is part of the
 * interface between a caller's code and the shared library and the two are not
 * necessarily built by the same compiler. The preallocated-buffer macros size a
 * caller's array with this value while the library derives its stride from it,
 * so a compiler that disagrees about max_align_t mis-sizes every preallocated
 * pool built against a library another compiler produced; GCC and Clang do
 * disagree about it on i386, reporting 16 and 8. Sixteen is what glibc's own
 * malloc returns on every target this library supports, so pinning it costs
 * nothing a caller could observe and removes the toolchain from the contract.
 */
#define _ccol_mempool_entry_align 16

/* A target whose widest fundamental type needs more than the pinned value would
 * be handed entries it cannot legally store that type in, which is the one way
 * the constant above can be wrong. It fails to compile instead. */
_Static_assert(_ccol_mempool_entry_align >= _Alignof(max_align_t),
               "_ccol_mempool_entry_align must be at least the alignment this "
               "target requires for any object type");

/**
 * @brief True iff _ccol_mempool_align_up(x) is well-defined for x, i.e. the
 * rounding-up addition it performs does not overflow size_t.
 *
 * Not meant to be used directly; ccol_mempool_create(),
 * ccol_mempool_create_from_ preallocated_buffer(), and
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER all check this (directly or via
 * _ccol_mempool_buffer_params_fit) before relying on _ccol_mempool_align_up()'s
 * result.
 */
#define _ccol_mempool_align_up_fits(x) \
  ((x) <= SIZE_MAX - (_ccol_mempool_entry_align - 1))

/**
 * @brief Rounds x up to the nearest multiple of _ccol_mempool_entry_align.
 *
 * Every entry in a ccol_mempool's contiguous backing buffer (whether heap
 * allocated or supplied via ccol_mempool_create_from_preallocated_buffer /
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER) sits extended_elem_size bytes after
 * the previous one. The buffer's own starting address being aligned to
 * _ccol_mempool_entry_align (validated separately at construction) is only
 * enough to keep entry 0 aligned; every later entry's alignment depends on
 * extended_elem_size ITSELF being a multiple of that same alignment, or the
 * per-entry offset drifts one stride at a time.
 *
 * Two things depend on that, and the caller-facing one is the more important.
 * An entry a caller receives must be aligned for any object type it might hold,
 * matching what malloc() guarantees, so that storing a long double or a vector
 * type in a pool entry is as valid as storing one in heap memory. The pool's
 * own reads and writes through an entry (the free-list link a free entry holds
 * in its own first bytes) are then correctly aligned as a consequence. Either
 * way a drifting offset is undefined behavior
 * and a real fault risk on strict-alignment architectures, invisible on
 * x86/x86_64's alignment-tolerant loads and stores.
 *
 * Callers must first confirm x fits via _ccol_mempool_align_up_fits(x), since
 * the addition below can otherwise overflow.
 */
#define _ccol_mempool_align_up(x)            \
  (((x) + (_ccol_mempool_entry_align - 1)) & \
   ~(size_t)(_ccol_mempool_entry_align - 1))

/**
 * @brief Create a fixed-size memory pool
 *
 * Creates a memory pool that manages a fixed number of fixed-size elements.
 * All allocations return elements of the same size. Provides O(1) allocation
 * and deallocation using a free list.
 *
 * @param elem_count Number of elements in the pool (must be > 0)
 * @param elem_size Size of each element in bytes (must be > 0, minimum
 * sizeof(uintptr_t), maximum SIZE_MAX minus the alignment and power-of-two
 * rounding the stride adds)
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
 * @note The stride between entries is elem_size rounded up to
 * _ccol_mempool_entry_align and then, unless the library was built with
 * CCOL_MEMPOOL_COMPACT_LAYOUT, on up to a power of two, so every entry in the
 * pool's contiguous buffer, not just the first, lands on an address aligned for
 * any object type; one status byte per entry follows the entry array in the
 * same block
 * @note If elem_size < sizeof(uintptr_t), it is rounded up
 * @note Returns NULL with error if elem_size would overflow size_t once that
 * rounding is applied
 * @note Thread-safe if single_threaded is false (uses a mutex)
 * @note With fallback enabled, pool never fails allocation (until system OOM)
 * @note The pool must be destroyed with ccol_mempool_destroy() when done
 *
 * @see ccol_mempool_create_from_preallocated_buffer
 * @see ccol_mempool_destroy
 * @see ccol_mempool_alloc_entry
 */
ccol_mempool *ccol_mempool_create(size_t elem_count, size_t elem_size,
                                  bool fallback_to_dynamic_memory,
                                  bool single_threaded,
                                  ccol_memmgmt_procs_t *mmgmt_procs,
                                  char **err);

/**
 * @brief Declare a preallocated buffer for a memory pool
 *
 * Macro that declares a uint8_t array sized correctly for a memory pool with
 * the specified parameters. The buffer can then be passed to
 * ccol_mempool_create_from_preallocated_buffer().
 *
 * @param name Variable name for the buffer
 * @param elem_count Number of elements the pool will hold
 * @param elem_size Size of each element in bytes
 *
 * @note Automatically includes the pool's own status bytes in the size
 * calculation
 * @note Useful for embedded systems or avoiding heap allocation
 * @note The declared buffer is aligned to _ccol_mempool_entry_align, the
 * alignment every entry the pool hands out is guaranteed to meet, so it can be
 * handed directly to ccol_mempool_create_from_preallocated_buffer() without any
 * extra alignment considerations on the caller's part
 * @note elem_size smaller than sizeof(uintptr_t) is rounded up to fit the
 * free-list pointer before the buffer's size is computed, the same way
 * ccol_mempool_create_from_preallocated_buffer() itself rounds it; this keeps
 * the declared buffer's element count in agreement with what that constructor
 * will actually carve it into, rather than the buffer being sized for the
 * caller's smaller, unrounded elem_size while the constructor divides it up
 * using the larger, rounded one
 * @note The per-element stride is the rounded elem_size taken up to
 * _ccol_mempool_entry_align and then, unless the library was built with
 * CCOL_MEMPOOL_COMPACT_LAYOUT, on up to a power of two, again matching
 * ccol_mempool_create_from_preallocated_buffer()'s own stride exactly; this
 * keeps every entry in the resulting pool aligned for any object type, not
 * just entry 0
 * @note One status byte per element is reserved after the elements, in the same
 * buffer, so the pool needs no allocation of its own for them
 * @note elem_count must be nonzero; rejected at compile time (via a
 * _Static_assert), rather than silently producing an undersized (or, for
 * elem_count == 0, zero-length) array, if elem_count is zero or if
 * elem_count * (the stride plus that element's own status byte) would
 * overflow size_t
 *
 * @see ccol_mempool_create_from_preallocated_buffer
 *
 * Example:
 * @code
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER(my_buffer, 100, 64);
 * ccol_mempool *mp = ccol_mempool_create_from_preallocated_buffer(
 *     my_buffer, sizeof(my_buffer), 64, false, false, NULL, NULL);
 * @endcode
 */
/**
 * @brief Selects how far the distance between entries is rounded up.
 *
 * The distance from one entry to the next is at least the element size raised
 * to hold a free entry's own list link and rounded up to
 * _ccol_mempool_entry_align. This switch decides whether it is rounded further.
 *
 * Left at 0, the default, it is rounded on up to a power of two, so recovering
 * an entry's position in the pool from its address is a shift. Defined to 1
 * (e.g. -DCCOL_MEMPOOL_COMPACT_LAYOUT=1) it is left at the aligned element
 * size, and the position is recovered with a multiply against a reciprocal
 * computed when the pool is built. That costs a little on every allocation and
 * release, and can save a great deal of memory: an element size just above a
 * power of two very nearly doubles under the default and does not move at all
 * under the compact setting. For an element size that is already a power of two
 * the two settings produce identical pools.
 *
 * It is a build-wide switch rather than a per-pool argument because a choice
 * made per pool would put that choice on the path every allocation and release
 * takes; measured, that costs roughly 8 percent for a pool with a thread cache
 * and 20 percent for a single-threaded one, paid by every caller including
 * those whose element sizes make the two layouts identical.
 *
 * @warning This value is part of the interface between an application and the
 * library, not only a build detail of the library. The buffer-declaring macros
 * below size an array in the application's own translation unit while the
 * library derives its stride from the same setting, so a library built with one
 * value and an application compiled against another disagree about how large a
 * preallocated buffer has to be, and every pool built on one is mis-sized.
 * Whatever value is chosen must be used for the library and for every
 * translation unit that includes this header.
 */
#ifndef CCOL_MEMPOOL_COMPACT_LAYOUT
#define CCOL_MEMPOOL_COMPACT_LAYOUT 0
#endif

/* Enforces the warning above at link time rather than leaving it to be read.
 *
 * The library defines exactly one of these two objects, named for the setting
 * it was built with, and CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER takes the
 * address of the one its own setting names. Build the two sides differently and
 * the link fails with an undefined reference naming the layout the declaring
 * code expected, instead of producing a buffer that is quietly the wrong size.
 *
 * Only that macro carries the reference, because only that macro is at risk:
 * nothing else here depends on the setting. No function signature, type or
 * struct does, the ranged buffer macro does not (every tier's element size is a
 * power of two at or above the entry alignment, where the two roundings agree),
 * and a translation unit that merely includes this header has nothing that
 * could be mis-sized. Putting the reference on every includer would make a file
 * using only the macros fail to link without the library, for a risk it does
 * not carry. A file that declares such a buffer has to link the library in any
 * case, since it exists to be handed to a constructor. */
#if CCOL_MEMPOOL_COMPACT_LAYOUT
extern const char _ccol_mempool_built_with_compact_layout[];
#define _ccol_mempool_layout_tag _ccol_mempool_built_with_compact_layout
#else
extern const char _ccol_mempool_built_with_fast_layout[];
#define _ccol_mempool_layout_tag _ccol_mempool_built_with_fast_layout
#endif

/**
 * @brief Smallest power of two that is at least x, as a constant expression.
 *
 * The buffer-declaring macros size a static array with this, so it cannot be a
 * function or a builtin the compiler is merely likely to fold; it is a
 * conditional chain. Its first entry is the alignment itself rather than a
 * fixed constant: the value handed to it is always already rounded up to that
 * alignment, so a chain that floored at anything else would disagree with the
 * runtime's own stride on a target where the two differ. It yields 0 above
 * 2^40, which
 * _ccol_mempool_buffer_params_fit() rejects, so an absurd element size fails at
 * compile time rather than silently wrapping.
 *
 * The same holds at every width: a chain entry beyond what a size_t can hold
 * (every entry from 2^32 up, on a 32-bit one) converts to exactly 0, which is
 * the same "no stride fits" answer the chain's own ceiling gives, so the entry
 * above which a size is rejected simply moves down with the width.
 */
/* Split by width: every entry above 2^31 is a comparison against a constant a
   32-bit size_t cannot hold, which clang rejects outright under -Werror rather
   than folding, so those entries are not emitted at all on such a target. A
   size that reaches past the chain's last entry yields 0, which
   _ccol_mempool_buffer_params_fit() and the runtime both read as "no stride
   fits"; the point at which that happens simply follows the width. */
#if SIZE_MAX > 0xFFFFFFFFu
#define _ccol_mp_pow2_ceil_wide(x)                                                     \
  ((x) <= 4294967296u                                                                  \
       ? (size_t)4294967296                                                            \
       : ((x) <= 8589934592u                                                           \
              ? (size_t)8589934592                                                     \
              : ((x) <= 17179869184u                                                   \
                     ? (size_t)17179869184                                             \
                     : ((x) <= 34359738368u                                            \
                            ? (size_t)34359738368                                      \
                            : ((x) <= 68719476736u                                     \
                                   ? (size_t)68719476736                               \
                                   : ((x) <= 137438953472u                             \
                                          ? (size_t)137438953472                       \
                                          : ((x) <= 274877906944u                      \
                                                 ? (size_t)274877906944                \
                                                 : ((x) <= 549755813888u               \
                                                        ? (size_t)549755813888         \
                                                        : ((x) <= 1099511627776u       \
                                                               ? (size_t)1099511627776 \
                                                               : (size_t)0)))))))))
#else
#define _ccol_mp_pow2_ceil_wide(x) (size_t)0
#endif

#define _ccol_mp_pow2_ceil(x)                                                                                                                                                                                                                                                                                                                                                                               \
  ((x) <= _ccol_mempool_entry_align                                                                                                                                                                                                                                                                                                                                                                         \
       ? (size_t)_ccol_mempool_entry_align                                                                                                                                                                                                                                                                                                                                                                  \
       : ((x) <= 16u                                                                                                                                                                                                                                                                                                                                                                                        \
              ? (size_t)16                                                                                                                                                                                                                                                                                                                                                                                  \
              : ((x) <= 32u                                                                                                                                                                                                                                                                                                                                                                                 \
                     ? (size_t)32                                                                                                                                                                                                                                                                                                                                                                           \
                     : ((x) <= 64u                                                                                                                                                                                                                                                                                                                                                                          \
                            ? (size_t)64                                                                                                                                                                                                                                                                                                                                                                    \
                            : ((x) <= 128u                                                                                                                                                                                                                                                                                                                                                                  \
                                   ? (size_t)128                                                                                                                                                                                                                                                                                                                                                            \
                                   : ((x) <= 256u                                                                                                                                                                                                                                                                                                                                                           \
                                          ? (size_t)256                                                                                                                                                                                                                                                                                                                                                     \
                                          : ((x) <= 512u                                                                                                                                                                                                                                                                                                                                                    \
                                                 ? (size_t)512                                                                                                                                                                                                                                                                                                                                              \
                                                 : ((x) <= 1024u                                                                                                                                                                                                                                                                                                                                            \
                                                        ? (size_t)1024                                                                                                                                                                                                                                                                                                                                      \
                                                        : ((x) <= 2048u                                                                                                                                                                                                                                                                                                                                     \
                                                               ? (size_t)2048                                                                                                                                                                                                                                                                                                                               \
                                                               : ((x) <= 4096u                                                                                                                                                                                                                                                                                                                              \
                                                                      ? (size_t)4096                                                                                                                                                                                                                                                                                                                        \
                                                                      : ((x) <= 8192u                                                                                                                                                                                                                                                                                                                       \
                                                                             ? (size_t)8192                                                                                                                                                                                                                                                                                                                 \
                                                                             : ((x) <= 16384u                                                                                                                                                                                                                                                                                                               \
                                                                                    ? (size_t)16384                                                                                                                                                                                                                                                                                                         \
                                                                                    : ((x) <= 32768u                                                                                                                                                                                                                                                                                                        \
                                                                                           ? (size_t)32768                                                                                                                                                                                                                                                                                                  \
                                                                                           : ((x) <= 65536u ? (size_t)65536                                                                                                                                                                                                                                                                                 \
                                                                                                            : ((x) <= 131072u ? (size_t)131072                                                                                                                                                                                                                                                              \
                                                                                                                              : (                                                                                                                                                                                                                                                                           \
                                                                                                                                    (x) <= 262144u ? (size_t)262144                                                                                                                                                                                                                                         \
                                                                                                                                                   : (                                                                                                                                                                                                                                                      \
                                                                                                                                                         (x) <= 524288u ? (size_t)524288                                                                                                                                                                                                                    \
                                                                                                                                                                        : (                                                                                                                                                                                                                                 \
                                                                                                                                                                              (x) <= 1048576u ? (size_t)1048576                                                                                                                                                                                             \
                                                                                                                                                                                              : ((x) <= 2097152u                                                                                                                                                                                            \
                                                                                                                                                                                                     ? (size_t)2097152                                                                                                                                                                                      \
                                                                                                                                                                                                     : (                                                                                                                                                                                                    \
                                                                                                                                                                                                           (x) <= 4194304u ? (size_t)4194304                                                                                                                                                                \
                                                                                                                                                                                                                           : ((x) <= 8388608u ? (size_t)8388608                                                                                                                                             \
                                                                                                                                                                                                                                              : ((x) <= 16777216u ? (size_t)16777216                                                                                                                        \
                                                                                                                                                                                                                                                                  : ((x) <= 33554432u ? (size_t)33554432                                                                                                    \
                                                                                                                                                                                                                                                                                      : ((x) <= 67108864u ? (size_t)67108864                                                                                \
                                                                                                                                                                                                                                                                                                          : (                                                                                               \
                                                                                                                                                                                                                                                                                                                (x) <= 134217728u                                                                           \
                                                                                                                                                                                                                                                                                                                    ? (size_t)134217728                                                                     \
                                                                                                                                                                                                                                                                                                                    : (                                                                                     \
                                                                                                                                                                                                                                                                                                                          (x) <= 268435456u                                                                 \
                                                                                                                                                                                                                                                                                                                              ? (size_t)268435456                                                           \
                                                                                                                                                                                                                                                                                                                              : ((x) <= 536870912u                                                          \
                                                                                                                                                                                                                                                                                                                                     ? (size_t)536870912                                                    \
                                                                                                                                                                                                                                                                                                                                     : ((x) <= 1073741824u ? (size_t)1073741824                             \
                                                                                                                                                                                                                                                                                                                                                           : ((x) <= 2147483648u ? (size_t)2147483648       \
                                                                                                                                                                                                                                                                                                                                                                                 : _ccol_mp_pow2_ceil_wide( \
                                                                                                                                                                                                                                                                                                                                                                                       x))))))))))))))))))))))))))))))

/**
 * @brief The distance between one entry and the next.
 *
 * An entry carries no header, so the stride is the caller's element size,
 * raised to hold the free-list link a free entry stores in its own first bytes,
 * rounded to the alignment every entry is guaranteed to meet, and then, in the
 * default layout, rounded on up to a power of two so that turning an entry's
 * address into its index is a shift rather than a division.
 *
 * CCOL_MEMPOOL_COMPACT_LAYOUT stops at the alignment and keeps the stride the
 * element size needs, trading that shift for a multiply by a precomputed
 * reciprocal and spending no bytes on the rounding.
 */
#if CCOL_MEMPOOL_COMPACT_LAYOUT
#define _ccol_mempool_stride(elem_size) \
  _ccol_mempool_align_up(ccol_max((elem_size), sizeof(uintptr_t)))
#else
#define _ccol_mempool_stride(elem_size) \
  _ccol_mp_pow2_ceil(                   \
      _ccol_mempool_align_up(ccol_max((elem_size), sizeof(uintptr_t))))
#endif

/**
 * @brief Bytes a pool of elem_count elements of elem_size needs.
 *
 * The entries themselves, followed by one status byte per entry. Both live in
 * the same block, so a preallocated buffer holds everything the pool needs and
 * a heap-allocated pool takes exactly one allocation.
 */
#define _ccol_mempool_buffer_bytes(elem_count, elem_size) \
  ((elem_count) * _ccol_mempool_stride(elem_size) + (elem_count))

#define _ccol_mempool_buffer_params_fit(elem_count, elem_size)              \
  ((elem_count) > 0 &&                                                      \
   _ccol_mempool_align_up_fits(ccol_max((elem_size), sizeof(uintptr_t))) && \
   _ccol_mempool_stride(elem_size) > 0 &&                                   \
   _ccol_mempool_stride(elem_size) <=                                       \
       (SIZE_MAX - (elem_count)) / (elem_count))

#define CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER(name, elem_count, elem_size)  \
  _Static_assert(                                                              \
      _ccol_mempool_buffer_params_fit((elem_count), (elem_size)),              \
      "CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER: elem_count must be nonzero, " \
      "and elem_count * elem_size (rounded up for the minimum element "        \
      "size and for the alignment every entry is guaranteed to meet) "         \
      "must not overflow size_t");                                             \
  _Alignas(_ccol_mempool_entry_align)                                          \
      uint8_t name[_ccol_mempool_buffer_bytes((elem_count), (elem_size))];     \
  static const char *const name##__ccol_mempool_layout_ref                     \
      __attribute__((used, unused)) = _ccol_mempool_layout_tag

/**
 * @brief Create a memory pool from a preallocated buffer
 *
 * Creates a memory pool using a user-provided buffer instead of allocating
 * from the heap. Useful for embedded systems or when heap allocation is
 * undesirable. The buffer must be properly sized using
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER or manual calculation.
 *
 * @param buffer Pointer to preallocated buffer
 * @param buf_size Size of buffer in bytes
 * @param elem_size Size of each element in bytes (must be > 0; if smaller
 * than sizeof(uintptr_t) it is rounded up, mirroring ccol_mempool_create();
 * maximum SIZE_MAX minus the stride rounding)
 * @param fallback_to_dynamic_memory If true, allocate from heap when pool
 * exhausted
 * @param single_threaded If true, omit locking (faster but not thread-safe)
 * @param mmgmt_procs Custom memory management procedures, or NULL for default
 * malloc/free
 * @param err Optional pointer to receive error string on failure
 *
 * @return Pointer to newly created memory pool, or NULL on failure
 *
 * @note Element count is calculated as buf_size / (stride + 1). An entry
 * carries no header; stride is elem_size rounded up to
 * _ccol_mempool_entry_align and then, unless the library was built with
 * CCOL_MEMPOOL_COMPACT_LAYOUT, on up to a power of two (so every entry, not
 * just the first, lands on an address aligned for any object type, and an
 * address converts to an index with a shift), and the one extra byte per
 * element is that element's own status byte, which lives in the same buffer
 * after the entries. _ccol_mempool_stride() computes exactly that stride;
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER sizes its own buffer with the same
 * arithmetic, and is the way to get it right without restating it
 * @note Buffer is not freed by ccol_mempool_destroy() (user manages buffer
 * lifetime)
 * @note Pool struct itself is still allocated via mmgmt_procs
 * @note Returns NULL with error if elem_size is zero
 * @note If elem_size is nonzero but < sizeof(uintptr_t), it is rounded up
 * (same as ccol_mempool_create())
 * @note Returns NULL with error if elem_size would overflow size_t once the
 * alignment and power-of-two rounding are applied
 * @note Returns NULL with error if the buffer is not sufficiently aligned (a
 * buffer declared via
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER is always properly aligned; a
 * hand-rolled buffer must be aligned to at least _ccol_mempool_entry_align,
 * which is 16 and is what the constructor checks for)
 *
 * @see CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER
 * @see ccol_mempool_create
 * @see ccol_mempool_destroy
 */
ccol_mempool *ccol_mempool_create_from_preallocated_buffer(
    void *buffer, size_t buf_size, size_t elem_size,
    bool fallback_to_dynamic_memory, bool single_threaded,
    ccol_memmgmt_procs_t *mmgmt_procs, char **err);

/**
 * @brief Destroy a memory pool (internal function)
 *
 * @param mp Memory pool to destroy
 *
 * @warning Do not call directly - use ccol_mempool_destroy() macro instead
 *
 * @note A custom allocator's free function can still be called after this
 * returns, and after main() returns. A pool shared between threads keeps a
 * small per-thread cache of entries; destroying the pool marks those caches
 * dead but does not free them, because a cache belongs to the thread holding
 * it and another thread must not free it underneath that thread. Each is
 * released later: when its owning thread next has to look up a magazine for a
 * pool its own per-thread cache does not already hold, when that thread exits,
 * or, if that thread is the one that runs the library's process-exit handler,
 * at process exit. Each release calls the free function the pool was created
 * with. None of those is guaranteed to happen: a thread that keeps using the
 * same few pools answers every request from its cache without ever looking a
 * magazine up, and so holds an orphaned one indefinitely. A program that needs
 * its allocator to see every free should join such threads.
 *
 * An allocator that is itself torn down at a known point therefore has to
 * outlive that: one drawing from an arena released at the end of main(), or
 * from storage freed by the application's own exit handler, can be called after
 * it is gone. Allocators built on malloc/free, or on storage that lives for the
 * process, are unaffected. A pool created with single_threaded set keeps no
 * such cache and has no such tail.
 */
void _ccol_mempool_destroy(ccol_mempool *mp);

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
#define ccol_mempool_destroy(mp) \
  do {                           \
    _ccol_mempool_destroy((mp)); \
    mp = NULL;                   \
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
 * @note Returned memory is uninitialized (use ccol_mempool_calloc_entry() for
 * zeroed)
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Will assert if mp is NULL
 * @note Entry must be freed with ccol_mempool_free_entry()
 * @note With fallback enabled, only returns NULL on system OOM
 *
 * @see ccol_mempool_calloc_entry
 * @see ccol_mempool_free_entry
 */
void *ccol_mempool_alloc_entry(ccol_mempool *mp);

/**
 * @brief Allocate a zero-initialized entry from the pool
 *
 * Like ccol_mempool_alloc_entry(), but zeros the memory before returning.
 *
 * @param mp Memory pool to allocate from
 *
 * @return Pointer to zero-initialized entry, or NULL if pool exhausted and no
 * fallback
 *
 * @note O(1) allocation + mem_set cost
 * @note Zeros the whole element
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Will assert if mp is NULL
 *
 * @see ccol_mempool_alloc_entry
 * @see ccol_mempool_free_entry
 */
void *ccol_mempool_calloc_entry(ccol_mempool *mp);

/**
 * @brief Free an entry back to the pool (internal function)
 *
 * @param entry Entry to free (obtained from ccol_mempool_alloc_entry or
 * ccol_mempool_calloc_entry)
 *
 * @warning Do not call directly - use ccol_mempool_free_entry() macro instead
 */
void _ccol_mempool_free_entry(ccol_mempool *mp, void *entry);

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
 * @note Detects double-free via assertions. Two threads freeing the same entry
 * at the same instant are the exception, since the check runs outside the pool
 * lock; that is a data race in the calling program in any case.
 * @note Ownership is established from the address alone, before anything is
 * read through the caller's pointer: a pool-owned entry carries no header, and
 * its state is one byte in the pool's own status array. A dynamic fallback
 * entry is the exception and does carry a header, with its own sentinel
 * distinct from a pool-owned entry's
 * @note For dynamically allocated entries (fallback), frees to heap
 * @note Will assert on: double free, an address this pool does not own, and,
 * for a dynamic entry, a corrupted header
 *
 * @see ccol_mempool_alloc_entry
 * @see ccol_mempool_calloc_entry
 */
#define ccol_mempool_free_entry(mp, entry)   \
  do {                                       \
    _ccol_mempool_free_entry((mp), (entry)); \
    entry = NULL;                            \
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
 * @note A pool with per-thread caches holds a reserve beyond this count, which
 * keeps this count obtainable by any thread however much other threads have
 * cached. An entry leaves a cache with no lock held and so with no count to
 * consult, so in exchange concurrent callers can briefly hold a few entries
 * more than this at once, never more than this plus that reserve. A pool with
 * no cache (single_threaded, or built on a preallocated buffer) is exact in
 * both directions
 *
 * @see ccol_mempool_used_count
 * @see ccol_mempool_allocated_bytes
 * @see ccol_mempool_dynamic_allocs_count
 */
size_t ccol_mempool_total_capacity(ccol_mempool *mp);

/**
 * @brief Get the memory the pool holds for its entries
 *
 * Returns the size in bytes of the single block the pool uses for its entries
 * and their per-entry state. This is what the pool actually costs, which
 * ccol_mempool_total_capacity() deliberately does not report: a pool that
 * receives a thread cache holds a reserve beyond the count it was created with,
 * so its memory exceeds its usable capacity. Use this to size a pool against a
 * memory budget.
 *
 * @param mp Memory pool to query
 *
 * @return Size in bytes of the pool's entry block
 *
 * @note Counts the entry block only. The handle itself is a fixed, small
 * allocation, per-thread caches are separate allocations made lazily as threads
 * first use the pool, and entries served by the dynamic fallback are not part
 * of this block at all
 * @note For a pool built on a caller-supplied buffer, this is the part of that
 * buffer the pool divided into entries, and the pool allocated none of it
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Will assert if mp is NULL
 *
 * @see ccol_mempool_total_capacity
 * @see ccol_mempool_dynamic_allocs_count
 */
size_t ccol_mempool_allocated_bytes(ccol_mempool *mp);

/**
 * @brief Get number of currently allocated pool entries
 *
 * Returns the number of entries from the pool that are currently handed out to
 * callers. Entries sitting in a per-thread cache have been taken off the shared
 * free list but given to nobody, and are not counted. Does not include dynamic
 * allocations.
 *
 * @param mp Memory pool to query
 *
 * @return Number of pool entries currently in use
 *
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Will assert if mp is NULL
 * @note Dynamic allocations are tracked separately
 * @note Exact on a quiescent pool, where it never exceeds
 * ccol_mempool_total_capacity(). On a pool with per-thread caches, concurrent
 * allocation can put it a few entries above that and never above the slot count
 * ccol_mempool_allocated_bytes() accounts for; see ccol_mempool_total_capacity
 *
 * @see ccol_mempool_total_capacity
 * @see ccol_mempool_dynamic_allocs_count
 */
size_t ccol_mempool_used_count(ccol_mempool *mp);

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
 * @note These entries are freed normally with ccol_mempool_free_entry()
 *
 * @see ccol_mempool_total_capacity
 * @see ccol_mempool_used_count
 */
size_t ccol_mempool_dynamic_allocs_count(ccol_mempool *mp);

/* ========================================================================== */
/*                         RANGED MEMORY POOL                                 */
/* ========================================================================== */

/** @brief Opaque handle to a ranged memory pool */
typedef struct ccol_r_mempool ccol_r_mempool;

/**
 * @brief Fallback policy for ranged memory pools
 *
 * Determines when and how the ranged memory pool falls back to dynamic
 * allocation when individual pools are exhausted.
 */
typedef enum ccol_r_memory_fallback_policy_t {
  ccol_fallback_disabled = 0,        /**< Never use dynamic allocation */
  ccol_fallback_at_first_exhaustion, /**< Each size pool has its own fallback */
  ccol_fallback_at_last_exhaustion, /**< Only fallback after all pools exhausted
                                     */
  ccol_fallback_end_place_holder    /**< Sentinel value (internal use) */
} ccol_r_memory_fallback_policy_t;

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
 * @note The pool must be destroyed with ccol_r_mempool_destroy() when done
 *
 * @see ccol_r_mempool_create_from_preallocated_buffer
 * @see ccol_r_mempool_destroy
 * @see ccol_r_mempool_alloc_entry
 */
ccol_r_mempool *ccol_r_mempool_create(
    uint8_t smallest_size_power_of_two, uint8_t largest_size_power_of_two,
    uint8_t number_of_smallest_size_elems_power_of_two,
    ccol_r_memory_fallback_policy_t fb_policy, bool single_threaded,
    ccol_memmgmt_procs_t *mmgmt_procs, char **err);

/**
 * @brief Calculate buffer size for preallocated ranged memory pool
 *
 * Internal macro for calculating the exact buffer size needed for a ranged
 * memory pool with given parameters. Used by
 * CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER.
 *
 * @param SS smallest_size_power_of_two
 * @param LS largest_size_power_of_two
 * @param SC smallest_elem_count_power_of_two
 *
 * @return Size in bytes required for the buffer
 *
 * @note For internal use -use CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER instead
 *
 * Sub-pool i holds 2^(SC - i) entries of 2^(SS + i) bytes, for i from 0 to
 * N - 1 where N = LS - SS + 1, and needs one stride per entry plus one status
 * byte per entry. ccol_r_mempool_create() requires the smallest element size to
 * be at least 16 bytes, so every element size here is already a power of two at
 * or above the alignment every entry is guaranteed to meet, and the stride is
 * the element size itself. The entry bytes therefore total N * 2^(SS + SC) (the
 * first term below) and the status bytes total 2 * 2^SC * (2^N - 1) / 2^N (the
 * second).
 *
 * Each sub-pool's segment is laid out as its entries followed by its status
 * bytes, and the next segment begins where it ends, so a segment whose length
 * is not a multiple of the alignment would hand the following sub-pool a
 * misaligned first entry. A tier's entry bytes are always a whole number of
 * strides and so already aligned; its status bytes are one per entry, which
 * falls below the alignment for the last few tiers, where the element count has
 * halved down into single digits. Those tiers' status regions are rounded up,
 * which is the third term below; the rounding is pure slack, never read or
 * written, and totals a few dozen bytes for any configuration.
 *
 * The ccol_r_mempool_create() validation this macro's own parameters must
 * already satisfy (SC >= LS - SS, i.e. SC + 1 >= N) guarantees that division is
 * exact. Rather than forming the full, un-reduced product 2 * 2^SC * (2^N - 1)
 * and dividing it down afterward (which can overflow size_t well before the
 * final division would have brought the value back into range, silently
 * wrapping to a wrong, too-small buffer size), the 2 * 2^SC / 2^N factor is
 * reduced first via a single right shift (exact under the same precondition,
 * and always well-defined since (LS - SS) is itself bounded below size_t's
 * width by ccol_r_mempool_create()'s own validation) before multiplying by the
 * much smaller remaining factor. This does not (and cannot) avoid overflow for
 * parameters large enough that the requested buffer itself is not
 * representable in a size_t; it only ensures no further overflow is
 * introduced on top of that inherent limit.
 */
/* One tier's own share of the padding described above: the bytes its status
 * region is rounded up by so the next tier's entries stay aligned. C is a
 * candidate element count; the term is zero unless that count is genuinely one
 * of this configuration's tiers, and zero again for any count already at or
 * above the alignment. */
/* The leading guard keeps the shift below from having a negative count when the
 * parameters are invalid. The declaring macro asserts them, but a size macro is
 * expanded in a declarator that the compiler diagnoses before it reaches that
 * assertion, so without this the first thing a caller sees for a bad parameter
 * is a page of -Wshift-count-negative from inside this header, which the
 * project's own -Werror baseline turns into errors. */
/* Tiers between the smallest and largest size, guarded so an inverted pair
 * shifts by zero instead of by a negative count. */
#define _ccol_rmempool_tier_span(SS, LS) ((LS) >= (SS) ? (LS) - (SS) : 0)

/* Clamps a shift count into range. Short-circuiting governs evaluation, not
 * diagnosis: a compiler still diagnoses a shift it can see is out of range on a
 * branch it never reaches, so an invalid triple would answer the declaring
 * macro's _Static_assert AND a page of shift diagnostics from inside this
 * header, which the project's -Werror baseline turns into errors. The two ways
 * out of range are covered by one clamp, because a negative count casts to a
 * size_t above the width. Clamping rather than reducing modulo the width is
 * load-bearing for the reason given at _ccol_rmempool_buffer_params_fit below.
 * It is a value no-op for every triple that is accepted at all, where both
 * counts are already below the width. */
#define _ccol_rmempool_shift_count(x)                \
  ((size_t)(x) < (size_t)(sizeof(size_t) * CHAR_BIT) \
       ? (size_t)(x)                                 \
       : (size_t)(sizeof(size_t) * CHAR_BIT - 1))

#define _ccol_rmempool_status_pad_term(SS, LS, SC, C)                       \
  (((SC) >= _ccol_rmempool_tier_span((SS), (LS)) &&                         \
    ((size_t)1 << _ccol_rmempool_shift_count(                               \
         (size_t)((SC) - _ccol_rmempool_tier_span((SS), (LS))))) <=         \
        (size_t)(C) &&                                                      \
    (size_t)(C) <= ((size_t)1 << _ccol_rmempool_shift_count((size_t)(SC)))) \
       ? (_ccol_mempool_align_up((size_t)(C)) - (size_t)(C))                \
       : (size_t)0)

/* Every element count that can need padding, enumerated. A count is a power of
 * two and only a count below _ccol_mempool_entry_align rounds up at all, so the
 * chain is complete for any alignment up to 128, which the assertion below
 * pins. */
#define _ccol_rmempool_status_padding(SS, LS, SC)         \
  (_ccol_rmempool_status_pad_term((SS), (LS), (SC), 1) +  \
   _ccol_rmempool_status_pad_term((SS), (LS), (SC), 2) +  \
   _ccol_rmempool_status_pad_term((SS), (LS), (SC), 4) +  \
   _ccol_rmempool_status_pad_term((SS), (LS), (SC), 8) +  \
   _ccol_rmempool_status_pad_term((SS), (LS), (SC), 16) + \
   _ccol_rmempool_status_pad_term((SS), (LS), (SC), 32) + \
   _ccol_rmempool_status_pad_term((SS), (LS), (SC), 64))

_Static_assert(_ccol_mempool_entry_align <= 128,
               "_ccol_rmempool_status_padding enumerates every element count "
               "that can be rounded up, which is complete only while the "
               "alignment stays at or below 128");

/* Every shift below is guarded against a NEGATIVE count, so that a triple the
 * declaring macro rejects for an inverted pair (LS below SS, or SC below
 * LS - SS) produces that macro's own _Static_assert message and nothing else.
 * This expression sits in an array declarator, which a compiler diagnoses
 * before it reaches the assertion, so an unguarded shift there would bury the
 * explanation under a page of -Wshift-count-negative, which this project's
 * -Werror baseline turns into the first errors the caller sees.
 *
 * An exponent wider than size_t itself is NOT covered: such a triple is
 * rejected, but the caller sees shift-count diagnostics alongside the
 * assertion. Guarding that case as well costs more than it saves, because the
 * counts here are the caller's own literals and the assertion still names the
 * real problem. The guards change no value for any triple the macro accepts. */
#define CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE(SS, LS, SC)          \
  ((size_t)(_ccol_rmempool_tier_span((SS), (LS)) + 1) * ((size_t)1 << (SC)) * \
       ((size_t)1 << (SS)) +                                                  \
   ((((size_t)1 << (SC)) >> _ccol_rmempool_tier_span((SS), (LS))) *           \
    (((size_t)1 << (_ccol_rmempool_tier_span((SS), (LS)) + 1)) - 1)) +        \
   _ccol_rmempool_status_padding((SS), (LS), (SC)))

/**
 * @brief Compile-time guard for CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER
 *
 * True iff the three power-of-two parameters produce a well-defined
 * CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE result that fits in a
 * size_t. Not meant to be used directly;
 * CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER is the public entry point.
 *
 * Every sub-condition below is ordered so that a term is only ever
 * evaluated once every condition it depends on for well-definedness has
 * already been confirmed true by an earlier, short-circuited && operand
 * (the short-circuited side of && / || is not evaluated, so an out-of-range
 * shift or a division by zero written there does not make the condition
 * undefined). That ordering governs EVALUATION, not diagnosis: a compiler is
 * still free to warn about a shift count it can see is too wide for the type
 * even on an operand the short circuit means it never reaches, and GCC does
 * exactly that, at both widths, once shifts are instrumented (clang does not).
 * The shift counts below are therefore clamped where a rejected shape could
 * push them out of range; see _ccol_rmempool_shift_count. In order: every
 * power-of-two exponent is small enough that a 1 << exponent is well-defined;
 * largest_size_power_of_two is genuinely larger than
 * smallest_size_power_of_two; their difference stays small enough that the
 * widest shift CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE performs stays
 * well-defined; number_of_smallest_size_elems_power_of_two is at least
 * largest_size_power_of_two - smallest_size_power_of_two (the exact
 * precondition CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE's own division
 * requires to be mathematically exact, matching what
 * ccol_r_mempool_create's own input validation separately enforces at runtime);
 * then each of the two terms CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE
 * sums is checked for overflow via division rather than by forming the
 * (possibly overflowing) product directly, followed by a check that the sum
 * of those two terms does not itself overflow size_t.
 */
/* Reduces a shift count modulo the width of size_t. Applied below to the two
 * counts derived from (LS) - (SS), and to nothing else, because those are the
 * only ones a shape the predicate rejects can push out of range: an inverted
 * pair makes that unsigned subtraction underflow to an enormous value.
 *
 * For any shape the predicate ACCEPTS the reduction changes nothing, since an
 * earlier && operand has already established the difference is smaller than
 * the width. Its whole purpose is the rejected shape: the short circuit means
 * the operand is never evaluated, but a compiler may still diagnose a shift
 * count it can see exceeds the type. Measured, GCC does so once shifts are
 * instrumented, at both widths (clang does not, at either), which would turn a
 * caller's -Werror build into a hard failure on exactly the input this macro
 * exists to answer "no" for.
 *
 * It CLAMPS rather than reducing modulo the width, and that distinction is
 * load-bearing: a modulo maps a count of exactly the width to zero, which
 * collapses the surrounding (1 << count) - 1 to zero, and GCC then reports the
 * comparison it sits in as always true under -Wtype-limits. That trades one
 * diagnostic for another and breaks a -Werror build that is otherwise clean,
 * for every shape whose tier span is exactly the width. Clamping to one below
 * the width yields a legal shift that folds to nothing in particular, so no
 * such comparison appears. The clamp is a value no-op: over every exponent
 * triple from 0 to 70, at both widths, the predicate answers identically with
 * it and without it.
 *
 * Applied to every shift the predicate performs, including the (SS) and (SC)
 * ones. Excluding those on the grounds that an earlier clause has already
 * bounded them is the mistake this whole block exists to describe: an earlier
 * clause bounds what is EVALUATED, and a compiler diagnoses a shift it can see
 * is out of range whether or not the branch is taken. Measured, a triple such
 * as (33, 35, 35) at 32 bits produces -Wshift-count-overflow on the (SS) shift
 * with the exclusion in place. */

/* The smallest element size a ranged pool will serve, as a power of two.
   ccol_r_mempool_create refuses anything below it at run time, so a buffer
   declared for such a shape could never be handed to a pool that accepted it;
   the predicate below carries the same floor so the mistake is a compile-time
   rejection rather than a buffer that is merely never usable. */
#define _ccol_rmempool_min_smallest_size_power 4

#define _ccol_rmempool_buffer_params_fit(SS, LS, SC)                           \
  ((SS) >= _ccol_rmempool_min_smallest_size_power &&                           \
   (size_t)(SS) < (sizeof(size_t) * CHAR_BIT) &&                               \
   (size_t)(LS) < (sizeof(size_t) * CHAR_BIT) &&                               \
   (size_t)(SC) < (sizeof(size_t) * CHAR_BIT) && (LS) > (SS) &&                \
   ((size_t)(LS) - (size_t)(SS)) < (sizeof(size_t) * CHAR_BIT) - 1 &&          \
   (size_t)(SC) >= ((size_t)(LS) - (size_t)(SS)) &&                            \
   ((size_t)1 << _ccol_rmempool_shift_count((size_t)(SC))) <=                  \
       SIZE_MAX / ((size_t)(LS) - (size_t)(SS) + 1) &&                         \
   ((size_t)1 << _ccol_rmempool_shift_count((size_t)(SS))) <=                  \
       SIZE_MAX / (((size_t)(LS) - (size_t)(SS) + 1) *                         \
                   ((size_t)1 << _ccol_rmempool_shift_count((size_t)(SC)))) && \
   (((size_t)1 << _ccol_rmempool_shift_count(                                  \
         _ccol_rmempool_tier_span((SS), (LS)) + 1)) -                          \
    1) <=                                                                      \
       SIZE_MAX / (((size_t)1 << _ccol_rmempool_shift_count((size_t)(SC))) >>  \
                   _ccol_rmempool_shift_count(                                 \
                       _ccol_rmempool_tier_span((SS), (LS)))) &&               \
   (((size_t)(LS) - (size_t)(SS) + 1) *                                        \
    ((size_t)1 << _ccol_rmempool_shift_count((size_t)(SC))) *                  \
    ((size_t)1 << _ccol_rmempool_shift_count((size_t)(SS)))) <=                \
       SIZE_MAX -                                                              \
           ((((size_t)1 << _ccol_rmempool_shift_count((size_t)(SC))) >>        \
             _ccol_rmempool_shift_count((size_t)(LS) - (size_t)(SS))) *        \
            (((size_t)1 << _ccol_rmempool_shift_count((size_t)(LS) -           \
                                                      (size_t)(SS) + 1)) -     \
             1)) -                                                             \
           _ccol_rmempool_status_padding((SS), (LS), (SC)))

/**
 * @brief Declare a preallocated buffer for a ranged memory pool
 *
 * Macro that declares a uint8_t array sized correctly for a ranged memory pool
 * with the specified parameters. The buffer can then be passed to
 * ccol_r_mempool_create_from_preallocated_buffer().
 *
 * @param name Variable name for the buffer
 * @param smallest_size_power_of_two log2 of smallest element size
 * @param largest_size_power_of_two log2 of largest element size
 * @param number_of_smallest_size_elems_power_of_two log2 of element count in
 * smallest pool
 *
 * @note Automatically includes every sub-pool's own status bytes in the
 * size calculation
 * @note Useful for embedded systems or avoiding heap allocation
 * @note Buffer contains all sub-pools in contiguous memory
 * @note The declared buffer is aligned to _ccol_mempool_entry_align, the
 * alignment every entry the pool hands out is guaranteed to meet, so it can be
 * handed directly to ccol_r_mempool_create_from_preallocated_buffer() without
 * any extra alignment considerations on the caller's part; every individual
 * sub-pool segment within the buffer stays correctly aligned as a consequence
 * @note Rejected at compile time (via a _Static_assert), rather than silently
 * producing a wrongly-sized array, if the three parameters would make the
 * pool's own required buffer size overflow size_t, or if
 * number_of_smallest_size_elems_power_of_two is smaller than
 * largest_size_power_of_two -smallest_size_power_of_two (the same precondition
 * ccol_r_mempool_create's own input validation enforces at runtime, required
 * here too for CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE's own division
 * to be exact rather than silently truncated), or if
 * smallest_size_power_of_two is below 4, since 16 bytes is the smallest
 * element a ranged pool serves and a buffer declared for anything smaller
 * could only ever be handed to a constructor that refuses it
 *
 * @see CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE
 * @see ccol_r_mempool_create_from_preallocated_buffer
 *
 * Example:
 * @code
 * CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(my_buffer, 4, 8, 10);
 * ccol_r_mempool *rmp = ccol_r_mempool_create_from_preallocated_buffer(
 *     my_buffer, sizeof(my_buffer), 4, 8, 10,
 *     ccol_fallback_disabled, false, NULL, NULL);
 * @endcode
 */
#define CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(                            \
    name, smallest_size_power_of_two, largest_size_power_of_two,              \
    number_of_smallest_size_elems_power_of_two)                               \
  _Alignas(_ccol_mempool_entry_align)                                         \
      uint8_t name[CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE(          \
          (smallest_size_power_of_two), (largest_size_power_of_two),          \
          (number_of_smallest_size_elems_power_of_two))];                     \
  _Static_assert(                                                             \
      _ccol_rmempool_buffer_params_fit(                                       \
          (smallest_size_power_of_two), (largest_size_power_of_two),          \
          (number_of_smallest_size_elems_power_of_two)),                      \
      "CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER: parameters would make the " \
      "pool's own required buffer size overflow size_t, or violate "          \
      "number_of_smallest_size_elems_power_of_two >= "                        \
      "largest_size_power_of_two - smallest_size_power_of_two, or name a "    \
      "smallest_size_power_of_two below 4, which is the smallest element "    \
      "size a ranged pool serves")

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
 * CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE
 * @note Buffer is not freed by ccol_r_mempool_destroy() (user manages buffer
 * lifetime)
 * @note Pool structs are still allocated via mmgmt_procs
 * @note Buffer contains all sub-pools in adjacent segments
 * @note Returns NULL with error if the buffer is not sufficiently aligned (a
 * buffer declared via
 * CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER is always properly aligned; a
 * hand-rolled buffer must be aligned to at least _ccol_mempool_entry_align,
 * which is 16 and is what the constructor checks for)
 *
 * @see CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER
 * @see ccol_r_mempool_create
 * @see ccol_r_mempool_destroy
 */
ccol_r_mempool *ccol_r_mempool_create_from_preallocated_buffer(
    void *buffer, size_t buf_size, uint8_t smallest_size_power_of_two,
    uint8_t largest_size_power_of_two,
    uint8_t number_of_smallest_size_elems_power_of_two,
    ccol_r_memory_fallback_policy_t fb_policy, bool single_threaded,
    ccol_memmgmt_procs_t *mmgmt_procs, char **err);

/**
 * @brief Destroy a ranged memory pool (internal function)
 *
 * @param rmp Ranged memory pool to destroy
 *
 * @warning Do not call directly - use ccol_r_mempool_destroy() macro instead
 */
void _ccol_r_mempool_destroy(ccol_r_mempool *rmp);

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
#define ccol_r_mempool_destroy(rmp) \
  do {                              \
    _ccol_r_mempool_destroy((rmp)); \
    rmp = NULL;                     \
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
 * @see ccol_r_mempool_total_capacity
 * @see ccol_r_mempool_dynamic_allocs_count
 */
size_t ccol_r_mempool_used_count(ccol_r_mempool *rmp, size_t size);

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
 * @see ccol_r_mempool_used_count
 * @see ccol_r_mempool_dynamic_allocs_count
 */
size_t ccol_r_mempool_total_capacity(ccol_r_mempool *rmp, size_t size);

/**
 * @brief Get the memory the ranged pool holds for its entries
 *
 * Returns the size in bytes of every tier's entry block added together, which
 * is what a ranged pool costs. Each tier that receives a thread cache holds a
 * reserve beyond its own nominal count, so this exceeds the sum of the tier
 * capacities.
 *
 * @param rmp Ranged memory pool to query
 *
 * @return Size in bytes of every tier's entry block, summed
 *
 * @note Counts the tiers' entry blocks only, on the same terms as
 * ccol_mempool_allocated_bytes()
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note Will assert if rmp is NULL
 *
 * @see ccol_mempool_allocated_bytes
 * @see ccol_r_mempool_total_capacity
 */
size_t ccol_r_mempool_allocated_bytes(ccol_r_mempool *rmp);

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
 * @note Returns 0 if ccol_fallback_disabled
 * @note With ccol_fallback_at_first_exhaustion, tracks per-pool dynamic
 * allocations
 * @note With ccol_fallback_at_last_exhaustion, tracks all dynamic allocations
 * @note Returns 0 if size is invalid (0 or > largest_size)
 * @note Thread-safe if pool was created with single_threaded=false
 *
 * @see ccol_r_mempool_used_count
 * @see ccol_r_mempool_total_capacity
 */
size_t ccol_r_mempool_dynamic_allocs_count(ccol_r_mempool *rmp, size_t size);

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
 * @note Returned memory is uninitialized (use ccol_r_mempool_calloc_entry() for
 * zeroed)
 * @note Size is rounded up to next power-of-2 pool size
 * @note Tries progressively larger pools if optimal pool is exhausted
 * @note Fallback behavior depends on fb_policy
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Returns NULL if size is 0 or > largest_size
 * @note Entry must be freed with ccol_r_mempool_free_entry()
 *
 * @see ccol_r_mempool_calloc_entry
 * @see ccol_r_mempool_realloc_entry
 * @see ccol_r_mempool_free_entry
 */
void *ccol_r_mempool_alloc_entry(ccol_r_mempool *rmp, size_t size);

/**
 * @brief Allocate a zero-initialized entry from the ranged pool
 *
 * Like ccol_r_mempool_alloc_entry(), but zeros the requested number of bytes
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
 * @see ccol_r_mempool_alloc_entry
 * @see ccol_r_mempool_realloc_entry
 */
void *ccol_r_mempool_calloc_entry(ccol_r_mempool *rmp, size_t size);

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
 * A pool-owned entry that the request still fits inside is returned unmoved
 * when that move cannot be made, rather than reported as a failure. An entry
 * sits in a tier at least as large as the size it was created for, and may sit
 * in a larger one than the request alone would choose, so a shrink (or a
 * resize to the size already held) can be honoured by the entry staying where
 * it is. The pointer that comes back then addresses at least the requested
 * bytes, as it does on every other successful path.
 *
 * @param rmp Ranged memory pool
 * @param addr Existing entry to reallocate, or NULL to allocate new
 * @param size New size in bytes (must be > 0 and <= largest_size)
 *
 * @return Pointer to reallocated entry, or NULL on failure
 *
 * @note If addr is NULL, equivalent to ccol_r_mempool_alloc_entry()
 * @note If the new size does not require a differently-sized allocation,
 * returns the original pointer unchanged (no copy)
 * @note Otherwise, allocates new, copies data, frees old; if that allocation
 * fails and addr is a pool-owned entry the request still fits inside, addr is
 * returned unmoved instead of NULL
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
 * ccol_r_mempool (corruption/foreign-pointer detection, mirroring
 * ccol_mempool_free_entry())
 *
 * @see ccol_r_mempool_alloc_entry
 * @see ccol_r_mempool_free_entry
 */
/**
 * @brief Return an entry to the ranged pool it came from (internal function)
 *
 * Prefer the ccol_r_mempool_free_entry() macro, which also NULLs the caller's
 * pointer. The ranged pool locates the sub-pool the entry belongs to; the
 * caller supplies the ranged pool, not that sub-pool, which it has no way to
 * know.
 *
 * @param rmp Ranged pool the entry was allocated from
 * @param entry Entry to release; a NULL entry is a no-op, matching free()
 *
 * @note Passing an entry that did not come from this ranged pool is a caller
 * error and is fatal, not silently tolerated
 *
 * @see ccol_r_mempool_free_entry
 * @see ccol_r_mempool_alloc_entry
 */
void _ccol_r_mempool_free_entry(ccol_r_mempool *rmp, void *entry);

void *ccol_r_mempool_realloc_entry(ccol_r_mempool *rmp, void *addr,
                                   size_t size);

/**
 * @brief Free an entry back to the ranged pool
 *
 * Returns an allocated entry to the appropriate sub-pool's free list.
 * Macro wrapper around ccol_mempool_free_entry() that sets pointer to NULL.
 *
 * @param entry Entry to free (will be set to NULL after freeing)
 *
 * @note Works for entries from any sub-pool in the ranged pool
 * @note Also works for dynamically allocated entries (fallback)
 * @note Safe to call with NULL
 * @note Thread-safe if pool was created with single_threaded=false
 * @note Performs corruption detection via assertions
 *
 * @see ccol_mempool_free_entry
 * @see ccol_r_mempool_alloc_entry
 */
#define ccol_r_mempool_free_entry(rmp, entry)   \
  do {                                          \
    _ccol_r_mempool_free_entry((rmp), (entry)); \
    entry = NULL;                               \
  } while (0)

#pragma GCC visibility pop
