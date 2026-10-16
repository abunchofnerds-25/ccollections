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

#include "common.h"

/* Everything this header declares from here to its end belongs to the
 * public ABI of libccollections, and the shared library exports all of it.
 * The library is built with -fvisibility=hidden, so any function or object
 * outside these blocks stays internal and isn't in the dynamic symbol table:
 * an application linking against the library can't interpose it, and a
 * symbol of the same name in that application can't collide with it. */
#pragma GCC visibility push(default)

/**
 * @file cmempool.h
 * @brief Fixed-size and ranged memory pool allocators for efficient memory
 * management
 *
 * Two kinds of memory pool:
 * - ccol_mempool: Fixed-size element pool with optional dynamic fallback
 * - ccol_r_mempool: Ranged pool that serves power-of-2 sizes and picks the
 * best pool for each request
 *
 * Main features:
 * - O(1) allocation and deallocation
 * - Thread-safe or single-threaded operation
 * - Support for a preallocated buffer, for embedded systems
 * - Corruption detection with assertions
 * - Fallback to dynamic allocation, which you can configure
 * - Zero external fragmentation (fixed-size pools)
 */

/* ========================================================================== */
/*                         BASIC MEMORY POOL                                  */
/* ========================================================================== */

/** @brief Opaque handle to a fixed-size memory pool */
typedef struct ccol_mempool ccol_mempool;

/**
 * @brief The alignment that every entry a pool hands out is sure to meet.
 *
 * It's at least what malloc() guarantees, so you can store in a pool entry
 * any object you could store in heap memory. The stride between entries is
 * rounded up to a multiple of this value, which carries the guarantee from
 * the first entry to all of them: a buffer that starts at a properly aligned
 * address but whose stride isn't a multiple of the alignment hands out
 * misaligned entries anyway, since the offset drifts one stride at a time.
 *
 * It's a fixed constant and not _Alignof(max_align_t) because it is part of
 * the interface between the caller's code and the shared library, and the
 * same compiler doesn't have to build both. The preallocated-buffer macros
 * size an array of the caller with this value while the library derives its
 * stride from it, so a compiler that disagrees about max_align_t would
 * mis-size every preallocated pool built against a library made by another
 * compiler. GCC and Clang really do disagree on i386, where they report 16
 * and 8. Sixteen is what glibc's malloc returns on every target we support,
 * so pinning the value costs a caller nothing visible and takes the
 * toolchain out of the contract.
 */
#define _ccol_mempool_entry_align 16

/* The constant above can only be wrong one way: on a target whose widest
 * fundamental type needs more than the pinned value, the pool would hand out
 * entries that can't legally hold that type. Such a target fails to compile
 * instead. */
_Static_assert(_ccol_mempool_entry_align >= _Alignof(max_align_t),
               "_ccol_mempool_entry_align must be at least the alignment this "
               "target requires for any object type");

/**
 * @brief True only when _ccol_mempool_align_up(x) is well-defined for x,
 * i.e. when the addition that rounds x up doesn't overflow size_t.
 *
 * Don't use this macro directly. ccol_mempool_create(),
 * ccol_mempool_create_from_preallocated_buffer() and
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER all check it, directly or through
 * _ccol_mempool_buffer_params_fit, before they use the result of
 * _ccol_mempool_align_up().
 */
#define _ccol_mempool_align_up_fits(x) \
  ((x) <= SIZE_MAX - (_ccol_mempool_entry_align - 1))

/**
 * @brief Rounds x up to the nearest multiple of _ccol_mempool_entry_align.
 *
 * Every entry in a ccol_mempool's contiguous backing buffer sits
 * extended_elem_size bytes after the one before it, both in a heap buffer
 * and in a buffer from ccol_mempool_create_from_preallocated_buffer or
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER. The constructor validates on its
 * own that the buffer's start address is aligned to
 * _ccol_mempool_entry_align, but that only keeps entry 0 aligned. Every
 * later entry is aligned only if extended_elem_size ITSELF is a multiple of
 * that same alignment; otherwise each entry's offset drifts by one stride at
 * a time.
 *
 * Two things depend on that, and the one the caller sees matters more. An
 * entry a caller gets has to be aligned for any object type it can hold,
 * like malloc() guarantees, so a long double or a vector type is as valid
 * in a pool entry as in heap memory. Following from that, the pool's own
 * reads and writes through an entry are aligned too, since a free entry
 * keeps the free-list link in its first bytes. In both cases a drifting
 * offset is undefined behavior and a real fault risk on strict-alignment
 * architectures; x86 and x86_64 loads and stores tolerate misalignment, so
 * there the fault never shows.
 *
 * The caller has to confirm first that x fits with
 * _ccol_mempool_align_up_fits(x), otherwise the addition below can
 * overflow.
 */
#define _ccol_mempool_align_up(x)            \
  (((x) + (_ccol_mempool_entry_align - 1)) & \
   ~(size_t)(_ccol_mempool_entry_align - 1))

/**
 * @brief Create a fixed-size memory pool
 *
 * Creates a memory pool that manages a fixed number of fixed-size elements,
 * so every allocation gives an element of the same size. Allocation and
 * deallocation are O(1) through a free list.
 *
 * @param elem_count Number of elements in the pool (must be > 0)
 * @param elem_size Size of each element in bytes. It must be > 0. The minimum
 * is sizeof(uintptr_t), and the maximum is SIZE_MAX less the alignment and
 * the power-of-two rounding that the stride adds
 * @param fallback_to_dynamic_memory If true, allocate from the heap when the
 * pool is empty
 * @param single_threaded If true, do not lock (faster but not thread-safe)
 * @param mmgmt_procs Custom memory management procedures, or NULL for the
 * default malloc and free
 * @param err Optional pointer that receives an error string on failure (pass
 * NULL to ignore)
 *
 * @return Pointer to the new memory pool, or NULL on failure
 *
 * @note The stride between entries is elem_size rounded up to
 * _ccol_mempool_entry_align and then, unless the library was built with
 * CCOL_MEMPOOL_COMPACT_LAYOUT, rounded further up to a power of two. So
 * every entry in the pool's contiguous buffer, not just the first, lands on
 * an address aligned for any object type. One status byte per entry follows
 * the entry array in the same block
 * @note An elem_size below sizeof(uintptr_t) is rounded up
 * @note Returns NULL with an error if elem_size would overflow size_t after
 * that rounding
 * @note Thread-safe if single_threaded is false (it uses a mutex)
 * @note The pool calls mmgmt_procs' functions for its dynamic fallback
 * entries, and for its own bookkeeping of them, in the middle of updating
 * its own state and with its lock held when it has one. So those functions
 * must not call back into this same pool: with a lock that deadlocks, and
 * without one it corrupts that state. An allocator built on a pool must
 * draw from a different pool
 * @note With fallback turned on, an allocation from the pool doesn't fail
 * until the system runs out of memory
 * @note Destroy the pool with ccol_mempool_destroy()
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
 * Declares a uint8_t array of the right size for a memory pool with the
 * given parameters, so you can pass the buffer to
 * ccol_mempool_create_from_preallocated_buffer().
 *
 * @param name Variable name for the buffer
 * @param elem_count Number of elements that the pool holds
 * @param elem_size Size of each element in bytes
 *
 * @note The computed size includes the pool's status bytes
 * @note Handy for embedded systems, or when you don't want a heap
 * allocation
 * @note The declared buffer is aligned to _ccol_mempool_entry_align, the
 * alignment every entry the pool hands out is sure to meet, so you can pass
 * it straight to ccol_mempool_create_from_preallocated_buffer() without
 * thinking about alignment
 * @note An elem_size smaller than sizeof(uintptr_t) is rounded up to hold
 * the free-list pointer. That happens before the buffer size is computed,
 * exactly as in ccol_mempool_create_from_preallocated_buffer(), which keeps
 * the element count of the declared buffer in agreement with what that
 * constructor carves it into. Otherwise the buffer would be sized for the
 * caller's smaller, unrounded elem_size while the constructor divides it up
 * with the larger, rounded one
 * @note The stride per element is the rounded elem_size taken up to
 * _ccol_mempool_entry_align and then, unless the library was built with
 * CCOL_MEMPOOL_COMPACT_LAYOUT, on up to a power of two. That matches the
 * stride of ccol_mempool_create_from_preallocated_buffer() exactly, and
 * keeps every entry of the resulting pool aligned for any object type, not
 * just entry 0
 * @note One status byte per element is reserved after the elements in the
 * same buffer, so the pool needs no allocation of its own for them
 * @note elem_count must be nonzero. A _Static_assert rejects two cases at
 * compile time: an elem_count of zero, and an elem_count * (the stride plus
 * that element's status byte) that would overflow size_t. Without it the
 * macro would quietly produce an array that is too small, or a zero-length
 * array when elem_count is 0
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
 * The distance from one entry to the next is at least the element size: the
 * size is raised to hold a free entry's list link and then rounded up to
 * _ccol_mempool_entry_align. This switch decides whether it gets rounded
 * any further.
 *
 * At 0, the default, it is rounded on up to a power of two so that a shift
 * gets an entry's position in the pool back from its address. At 1 (for
 * example -DCCOL_MEMPOOL_COMPACT_LAYOUT=1) it stays at the aligned element
 * size, and a multiply by a reciprocal computed when the pool is built gets
 * the position back instead. The multiply costs a little on every
 * allocation and release, but it can save a lot of memory: an element size
 * a little above a power of two nearly doubles under the default and
 * doesn't move at all under the compact setting. For an element size that is
 * a power of two already the two settings make identical pools.
 *
 * It's a build-wide switch rather than an argument per pool, because a
 * per-pool choice would sit on the path of every allocation and release.
 * Measured, that costs about 8 percent for a pool with a thread cache and 20
 * percent for a single-threaded one, and every caller would pay it, even
 * those whose element sizes make the two layouts identical.
 *
 * @warning This value is part of the interface between an application and the
 * library, not just a build detail of the library. The buffer-declaring
 * macros below size an array in the application's translation unit, while
 * the library derives its stride from the same setting. If the library is
 * built with one value and an application compiled against another, the two
 * disagree about how big a preallocated buffer must be, and every pool built
 * on one has the wrong size. Use the same value for the library and for
 * every translation unit that includes this header.
 */
#ifndef CCOL_MEMPOOL_COMPACT_LAYOUT
#define CCOL_MEMPOOL_COMPACT_LAYOUT 0
#endif

/* This enforces the warning above at link time, so nobody has to obey it on
 * trust.
 *
 * The library defines exactly one of these two objects, and that object's
 * name says which setting the library was built with.
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER takes the address of the one its
 * own setting names, so when the two sides are built differently the link
 * fails with an undefined reference naming the layout the declaring code
 * expected. Without it the build would quietly produce a buffer of the wrong
 * size.
 *
 * Only that macro carries the reference, because only that macro is at
 * risk. Nothing else here depends on the setting: no function signature,
 * type or struct, and not the ranged buffer macro either, since every
 * tier's element size is a power of two at or above the entry alignment,
 * where the two roundings agree. A translation unit that only includes this
 * header has nothing that could get the wrong size.
 *
 * A reference on every includer would make a file that uses only the macros
 * fail to link without the library, although that file isn't at risk. A
 * file that declares such a buffer has to link the library anyway, since the
 * buffer is there to go to a constructor. */
/* The tag is a scalar, so the reference below takes its address explicitly
 * instead of relying on array-to-pointer decay. The note next to its
 * definition in cmempool.c says why it isn't a one-element array. */
#if CCOL_MEMPOOL_COMPACT_LAYOUT
extern const char _ccol_mempool_built_with_compact_layout;
#define _ccol_mempool_layout_tag (&_ccol_mempool_built_with_compact_layout)
#else
extern const char _ccol_mempool_built_with_fast_layout;
#define _ccol_mempool_layout_tag (&_ccol_mempool_built_with_fast_layout)
#endif

/**
 * @brief Smallest power of two that is at least x, as a constant expression.
 *
 * The buffer-declaring macros size a static array with this, so it can't be
 * a function, or a builtin that the compiler only probably folds; it must
 * be a conditional chain. Its first entry is the alignment itself and not
 * some other fixed constant, because the value that reaches it is always
 * rounded up to that alignment already, and a chain with a different floor
 * would disagree with the runtime's stride on a target where the two
 * differ. The chain gives 0 above 2^40, which
 * _ccol_mempool_buffer_params_fit() rejects, so an absurd element size fails
 * at compile time instead of wrapping quietly.
 *
 * That holds at every width: a chain entry beyond what a size_t can hold
 * converts to exactly 0 (on a 32-bit size_t, every entry from 2^32 up),
 * which is the same "no stride fits" answer the chain's ceiling gives. So
 * the point above which a size is rejected moves down with the width.
 */
/* The chain is split by width because every entry above 2^31 is a
   comparison against a constant a 32-bit size_t can't hold, and Clang
   rejects such a comparison outright under -Werror rather than folding it,
   so on such a target those entries aren't emitted at all. A size past the
   chain's last entry gives 0, which both _ccol_mempool_buffer_params_fit()
   and the runtime read as "no stride fits", and since the chain is split by
   width, the point where that happens follows the width. */
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
 * An entry has no header, so the stride is the caller's element size,
 * raised to hold the free-list link a free entry keeps in its first bytes
 * and then rounded to the alignment every entry is sure to meet. In the
 * default layout it is rounded on up to a power of two, so that a shift and
 * not a division turns an entry's address into its index.
 *
 * CCOL_MEMPOOL_COMPACT_LAYOUT stops at the alignment and keeps the stride
 * the element size needs. It trades that shift for a multiply by a
 * reciprocal computed in advance, and spends no bytes on the rounding.
 */
#if CCOL_MEMPOOL_COMPACT_LAYOUT
#define _ccol_mempool_stride(elem_size) \
  _ccol_mempool_align_up(_ccol_max((elem_size), sizeof(uintptr_t)))
#else
#define _ccol_mempool_stride(elem_size) \
  _ccol_mp_pow2_ceil(                   \
      _ccol_mempool_align_up(_ccol_max((elem_size), sizeof(uintptr_t))))
#endif

/**
 * @brief Bytes that a pool of elem_count elements of elem_size needs.
 *
 * The block holds the entries and then one status byte per entry. With both
 * parts in the same block a preallocated buffer holds everything the pool
 * needs, and a heap pool takes exactly one allocation.
 */
#define _ccol_mempool_buffer_bytes(elem_count, elem_size) \
  ((elem_count) * _ccol_mempool_stride(elem_size) + (elem_count))

#define _ccol_mempool_buffer_params_fit(elem_count, elem_size)               \
  ((elem_count) > 0 &&                                                       \
   _ccol_mempool_align_up_fits(_ccol_max((elem_size), sizeof(uintptr_t))) && \
   _ccol_mempool_stride(elem_size) > 0 &&                                    \
   _ccol_mempool_stride(elem_size) <=                                        \
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
 * Creates a memory pool on a buffer that the caller gives instead of
 * allocating from the heap, which is handy for embedded systems and
 * whenever you don't want a heap allocation. The buffer must have the right
 * size: use CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER, or compute the size by
 * hand.
 *
 * @param buffer Pointer to the preallocated buffer
 * @param buf_size Size of the buffer in bytes
 * @param elem_size Size of each element in bytes. It must be > 0. If it is
 * smaller than sizeof(uintptr_t) it is rounded up, as in
 * ccol_mempool_create(). The maximum is SIZE_MAX less the stride rounding
 * @param fallback_to_dynamic_memory If true, allocate from the heap when the
 * pool is empty
 * @param single_threaded If true, do not lock (faster but not thread-safe)
 * @param mmgmt_procs Custom memory management procedures, or NULL for the
 * default malloc and free
 * @param err Optional pointer that receives an error string on failure
 *
 * @return Pointer to the new memory pool, or NULL on failure
 *
 * @note The element count is buf_size / (stride + 1). An entry has no
 * header; the stride is elem_size rounded up to _ccol_mempool_entry_align,
 * and then on up to a power of two unless the library was built with
 * CCOL_MEMPOOL_COMPACT_LAYOUT. So every entry, not only the first, lands on
 * an address aligned for any object type, and a shift turns an address into
 * an index. The one extra byte per element is that element's status byte,
 * which sits in the same buffer after the entries. _ccol_mempool_stride()
 * computes exactly that stride, and CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER
 * sizes its own buffer with the same arithmetic, so that macro is the way to
 * get the size right without writing the formula a second time
 * @note ccol_mempool_destroy() doesn't free the buffer, its lifetime belongs
 * to the caller
 * @note The pool struct itself does come from mmgmt_procs
 * @note The pool calls mmgmt_procs' functions for its dynamic fallback
 * entries, and for its own bookkeeping of them, in the middle of updating
 * its own state and with its lock held when it has one. So those functions
 * must not call back into this same pool: with a lock that deadlocks, and
 * without one it corrupts that state. An allocator built on a pool has to
 * draw from a different pool
 * @note Returns NULL with an error if elem_size is zero
 * @note A nonzero elem_size below sizeof(uintptr_t) is rounded up, as
 * ccol_mempool_create() does
 * @note Returns NULL with an error if elem_size would overflow size_t after
 * the alignment and the power-of-two rounding
 * @note Returns NULL with an error if the buffer isn't aligned enough. A
 * buffer from CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER is always aligned
 * right, while a buffer you build by hand must be aligned to at least
 * _ccol_mempool_entry_align, which is 16 and is what the constructor checks
 * for
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
 * @warning Do not call this function directly. Use the
 * ccol_mempool_destroy() macro instead
 *
 * @note A custom allocator's free function can be called after this function
 * returns, even after main() returns. A pool that threads share keeps a
 * small cache of entries per thread, and destroying the pool marks those
 * caches dead instead of freeing them, because a cache belongs to the thread
 * holding it and no other thread may free it under that thread. Each cache
 * is freed later at one of three points: when the owning thread next has to
 * look up a magazine for a pool its own cache doesn't hold already; when
 * that thread exits; or at process exit, or when dlclose() unloads the
 * library, if that thread is the one running the library's unload handler.
 * Each free calls the free function the pool was created with. None of the
 * three points is guaranteed to come: a thread that keeps using the same few
 * pools answers every request from its cache, never looks a magazine up,
 * and so holds an orphaned cache for ever. A program that needs its
 * allocator to see every free should join such threads. Once dlclose()
 * unloads the library, the cache of every other running thread stays
 * allocated for the rest of the process, and that thread exits cleanly.
 *
 * So an allocator that is itself torn down at a known point has to outlive
 * that point. An allocator that draws from an arena can get called after the
 * end of main() frees that arena, and the same goes for an allocator drawing
 * from storage that the application's exit handler frees. An allocator built
 * on malloc and free isn't affected, and neither is one built on storage
 * that lives for the whole process. A pool created with single_threaded
 * keeps no such cache and has no such tail.
 */
void _ccol_mempool_destroy(ccol_mempool *mp);

/**
 * @brief Destroy a memory pool and set pointer to NULL
 *
 * Frees every resource of the memory pool. For a pool created from a
 * preallocated buffer only the pool struct is freed, not the buffer itself.
 *
 * @param mp Memory pool to destroy. It is set to NULL afterward
 *
 * @warning It asserts when the dynamic fallback still holds pointers that
 * nobody freed, so a possible leak doesn't go unnoticed. There is no such
 * assert if you didn't ask for a fallback when you created the pool. As a
 * rule, always free the allocated buffers.
 * @note Calling it with a NULL pointer is safe
 * @note For a preallocated pool the buffer's lifetime belongs to the caller
 * @note mp is evaluated exactly once. It must be a modifiable lvalue, such
 * as a variable or an element of an array.
 */
#define ccol_mempool_destroy(mp) \
  _ccol_mempool_destroy_impl(    \
      mp, _ccol_uniq(__ccol_mempool_destroy_slot, __COUNTER__))

/* Internal: the body of ccol_mempool_destroy. slot is a name from
 * _ccol_uniq(), so the macro can nest inside the argument of another destroy
 * macro and still be -Wshadow clean. The argument is evaluated exactly
 * once. */
#define _ccol_mempool_destroy_impl(mp, slot) \
  do {                                       \
    __typeof__(mp) *slot = &(mp);            \
    _ccol_mempool_destroy(*slot);            \
    *slot = NULL;                            \
  } while (0)

/**
 * @brief Allocate an entry from the pool
 *
 * Gives a pointer to a free entry of the pool, or allocates from the heap
 * when the pool is empty and the fallback is on. The contents of the entry
 * are uninitialized.
 *
 * @param mp Memory pool to allocate from
 *
 * @return Pointer to the allocated entry, or NULL when the pool is empty and
 * there is no fallback
 *
 * @note O(1) complexity
 * @note The memory you get is uninitialized; use ccol_mempool_calloc_entry()
 * for zeroed memory
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note It asserts if mp is NULL
 * @note Free the entry with ccol_mempool_free_entry()
 * @note With the fallback on it returns NULL only when the system runs out
 * of memory
 *
 * @see ccol_mempool_calloc_entry
 * @see ccol_mempool_free_entry
 */
void *ccol_mempool_alloc_entry(ccol_mempool *mp);

/**
 * @brief Allocate a zero-initialized entry from the pool
 *
 * Like ccol_mempool_alloc_entry(), but the memory is zeroed before it comes
 * back.
 *
 * @param mp Memory pool to allocate from
 *
 * @return Pointer to the zeroed entry, or NULL when the pool is empty and
 * there is no fallback
 *
 * @note O(1) allocation, plus the cost of the memory set
 * @note The whole element is zeroed
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note It asserts if mp is NULL
 *
 * @see ccol_mempool_alloc_entry
 * @see ccol_mempool_free_entry
 */
void *ccol_mempool_calloc_entry(ccol_mempool *mp);

/**
 * @brief Free an entry back to the pool (internal function)
 *
 * @param entry Entry to free. It comes from ccol_mempool_alloc_entry or
 * ccol_mempool_calloc_entry
 *
 * @warning Do not call this function directly. Use the
 * ccol_mempool_free_entry() macro instead
 */
void _ccol_mempool_free_entry(ccol_mempool *mp, void *entry);

/**
 * @brief Free an entry back to the pool and set pointer to NULL
 *
 * Puts an allocated entry back on the pool's free list for reuse, with
 * careful corruption detection through assertions. Calling it with NULL is
 * safe.
 *
 * @param entry Entry to free. It is set to NULL afterward
 *
 * @note O(1) complexity
 * @note With NULL it does nothing, like free()
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note Assertions catch a double free, with one exception: two threads that
 * free the same entry at the same instant, since the check runs outside the
 * pool lock. Such a call is a data race in the caller's program anyway.
 * @note Ownership is decided from the address alone, before anything is read
 * through the caller's pointer. An entry the pool owns has no header; its
 * state is one byte in the pool's status array. The pool also records the
 * address of every dynamic fallback entry it hands out until that entry is
 * freed, and checks an address outside its buffer against that record
 * @note A dynamic entry from the fallback is freed to the heap
 * @note It asserts on a double free and on an address this pool doesn't own,
 * an entry of another pool or a dynamic entry that is already free
 * included, and it reads nothing through such an address
 *
 * @see ccol_mempool_alloc_entry
 * @see ccol_mempool_calloc_entry
 *
 * @note Each argument is evaluated exactly once. entry must be a modifiable
 * lvalue, such as a variable or an element of an array.
 */
#define ccol_mempool_free_entry(mp, entry) \
  _ccol_mempool_free_entry_impl(           \
      (mp), entry, _ccol_uniq(__ccol_mempool_free_entry_slot, __COUNTER__))

/* Internal: the body of ccol_mempool_free_entry. slot is a name from
 * _ccol_uniq(), so the macro can nest inside the argument of another destroy
 * macro and still be -Wshadow clean. The argument is evaluated exactly
 * once. */
#define _ccol_mempool_free_entry_impl(mp, entry, slot) \
  do {                                                 \
    __typeof__(entry) *slot = &(entry);                \
    _ccol_mempool_free_entry((mp), *slot);             \
    *slot = NULL;                                      \
  } while (0)

/**
 * @brief Get total capacity of the pool
 *
 * Gives the total number of fixed-size entries the pool was created with,
 * without the dynamic entries.
 *
 * @param mp Memory pool to query
 *
 * @return Total number of pool entries. The count does not include dynamic
 * entries
 *
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note It asserts if mp is NULL
 * @note Dynamic entries are tracked separately
 * @note A pool with per-thread caches holds a reserve above this count,
 * which keeps this count available to any thread however much other threads
 * have cached. An entry leaves a cache with no lock held, so with no count
 * to look at, and in exchange concurrent callers can briefly hold a few
 * entries more than this count at once, but never more than this count plus
 * that reserve. A pool with no cache (a single_threaded one, or one built on
 * a preallocated buffer) is exact in both directions
 *
 * @see ccol_mempool_used_count
 * @see ccol_mempool_allocated_bytes
 * @see ccol_mempool_dynamic_allocs_count
 */
size_t ccol_mempool_total_capacity(ccol_mempool *mp);

/**
 * @brief Get the memory the pool holds for its entries
 *
 * Gives the size in bytes of the single block the pool uses for its entries
 * and their per-entry state, which is what the pool costs.
 * ccol_mempool_total_capacity() deliberately doesn't report that cost: a
 * pool that gets a thread cache holds a reserve above the count it was
 * created with, so its memory is more than its usable capacity. Use this
 * function to size a pool against a memory budget.
 *
 * @param mp Memory pool to query
 *
 * @return Size in bytes of the entry block of the pool
 *
 * @note Only the entry block is counted. The handle itself is a small, fixed
 * allocation, each thread's cache is a separate allocation made when that
 * thread first uses the pool, and an entry the dynamic fallback serves isn't
 * part of this block at all
 * @note For a pool built on a caller's buffer this is the part of that
 * buffer the pool divided into entries, none of which the pool allocated
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note It asserts if mp is NULL
 *
 * @see ccol_mempool_total_capacity
 * @see ccol_mempool_dynamic_allocs_count
 */
size_t ccol_mempool_allocated_bytes(ccol_mempool *mp);

/**
 * @brief Get number of currently allocated pool entries
 *
 * Gives the number of pool entries the pool has handed out to callers right
 * now. An entry sitting in a thread's cache (it left the shared free list
 * but the pool gave it to nobody) isn't counted, and neither is a dynamic
 * entry.
 *
 * @param mp Memory pool to query
 *
 * @return Number of pool entries currently in use
 *
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note It asserts if mp is NULL
 * @note Dynamic entries are tracked separately
 * @note On a quiet pool the count is exact and never exceeds
 * ccol_mempool_total_capacity(). On a pool with per-thread caches,
 * concurrent allocation can push it a few entries above that, but never
 * above the slot count that ccol_mempool_allocated_bytes() reports. See
 * ccol_mempool_total_capacity
 *
 * @see ccol_mempool_total_capacity
 * @see ccol_mempool_dynamic_allocs_count
 */
size_t ccol_mempool_used_count(ccol_mempool *mp);

/**
 * @brief Get number of dynamically allocated entries
 *
 * Gives the number of entries that came from the heap because the pool was
 * empty, which can be non-zero only when fallback_to_dynamic_memory was on.
 *
 * @param mp Memory pool to query
 *
 * @return Number of heap entries that are currently active
 *
 * @note Returns 0 if the fallback was off at creation
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note It asserts if mp is NULL
 * @note Free these entries the usual way, with ccol_mempool_free_entry()
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
 * Decides when and how the ranged memory pool falls back to dynamic
 * allocation once a single sub-pool is empty.
 */
typedef enum ccol_r_memory_fallback_policy_t {
  ccol_fallback_disabled = 0,        /**< Never use dynamic allocation */
  ccol_fallback_at_first_exhaustion, /**< Each size pool has its own fallback */
  ccol_fallback_at_last_exhaustion,  /**< Fall back only after every pool is
                                        empty */
  _ccol_fallback_end_place_holder    /**< Sentinel value (internal use) */
} ccol_r_memory_fallback_policy_t;

/**
 * @brief Create a ranged memory pool
 *
 * Creates a group of fixed-size memory pools covering a range of power-of-2
 * sizes, and each allocation is served by the smallest pool that can hold
 * the requested size. Going up the sizes, the pool sizes double and the
 * element counts halve.
 *
 * For example, smallest_size=4, largest_size=6, elem_count=8 creates:
 * - Pool 0: 2^4=16 bytes, 2^8=256 elements
 * - Pool 1: 2^5=32 bytes, 2^7=128 elements
 * - Pool 2: 2^6=64 bytes, 2^6=64 elements
 *
 * @param smallest_size_power_of_two log2 of the smallest element size (for
 * example 4 for 16 bytes)
 * @param largest_size_power_of_two log2 of the largest element size (for
 * example 10 for 1024 bytes)
 * @param smallest_elem_count_power_of_two log2 of the element count in the
 * smallest pool
 * @param fb_policy Fallback policy for when the pools are empty
 * @param single_threaded If true, do not lock (faster but not thread-safe)
 * @param mmgmt_procs Custom memory management procedures, or NULL for the
 * default malloc and free
 * @param err Optional pointer that receives an error string on failure
 *
 * @return Pointer to the new ranged memory pool, or NULL on failure
 *
 * @note Every parameter must be > 0
 * @note largest_size_power_of_two must be > smallest_size_power_of_two
 * @note smallest_elem_count_power_of_two must be >= (largest - smallest)
 * @note The smallest size must be >= 16 bytes (min_allowed_smallest_size)
 * @note The largest size must be <= 2^63 bytes (max_allowed_largest_size)
 * @note The number of pools is (largest - smallest + 1)
 * @note Thread-safe if single_threaded is false
 * @note Each sub-pool calls mmgmt_procs' functions for its dynamic fallback
 * entries, and for its own bookkeeping of them, in the middle of updating
 * its own state and with its lock held when it has one. So those functions
 * must not call back into this same ranged pool: with a lock that can
 * deadlock, and without one it corrupts that state. An allocator built on a
 * pool must draw from a different pool
 * @note Destroy the pool with ccol_r_mempool_destroy()
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
 * The internal macro behind CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER, which
 * works out the exact buffer size a ranged memory pool with the given
 * parameters needs.
 *
 * @param SS smallest_size_power_of_two
 * @param LS largest_size_power_of_two
 * @param SC smallest_elem_count_power_of_two
 *
 * @return Size in bytes that the buffer needs
 *
 * @note It is for internal use. Use CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER
 * instead
 *
 * Sub-pool i holds 2^(SC - i) entries of 2^(SS + i) bytes, for i from 0 to
 * N - 1 with N = LS - SS + 1, and needs one stride and one status byte per
 * entry. ccol_r_mempool_create() requires the smallest element size to be at
 * least 16 bytes, so every element size here is a power of two at or above
 * the alignment every entry is sure to meet already, and the stride is the
 * element size itself. So the entry bytes add up to N * 2^(SS + SC), the
 * first term below, and the status bytes to 2 * 2^SC * (2^N - 1) / 2^N, the
 * second term.
 *
 * Each sub-pool's segment holds its entries and then its status bytes, and
 * the next segment starts where that one ends, so a segment whose length
 * isn't a multiple of the alignment would give the next sub-pool a
 * misaligned first entry. A tier's entry bytes are always a whole number of
 * strides and so aligned already, but there is one status byte per entry,
 * and for the last few tiers, where the element count has halved down into
 * single digits, that count falls below the alignment. The status regions of
 * those tiers are rounded up, which is the third term below. The rounding is
 * pure slack that nothing reads or writes, and it comes to a few dozen bytes
 * for any configuration.
 *
 * The parameters must already pass the validation of
 * ccol_r_mempool_create(), which is SC >= LS - SS, i.e. SC + 1 >= N, and
 * which makes the division exact. We don't form the full, unreduced product
 * 2 * 2^SC * (2^N - 1) and divide it down afterward, because that product
 * can overflow size_t well before the final division brings the value back
 * into range, and it would then quietly wrap to a wrong, too small buffer
 * size. Instead the 2 * 2^SC / 2^N factor is reduced first with a single
 * right shift, which is exact under the same precondition and always
 * well-defined, since the validation of ccol_r_mempool_create() keeps
 * (LS - SS) below the width of size_t, and then the much smaller remaining
 * factor is multiplied in. That can't avoid an overflow for parameters big
 * enough to make the requested buffer itself too large for a size_t; it only
 * makes sure no further overflow piles up on top of that inherent limit.
 */
/* One tier's share of the padding above: the bytes added to that tier's
 * status region so the next tier's entries stay aligned. C is a candidate
 * element count. The term is zero unless that count really is one of the
 * tiers of this configuration, and zero again for any count that is at or
 * above the alignment already. */
/* The leading guard keeps the shift below from getting a negative count
 * when the parameters are invalid. The declaring macro asserts the
 * parameters, but a size macro expands inside a declarator and the compiler
 * diagnoses that declarator before it gets to the assertion. Without the
 * guard, the first thing a caller sees for a bad parameter is a page of
 * -Wshift-count-negative from inside this header, which this project's
 * -Werror baseline turns into errors. */
/* The tiers between the smallest and the largest size. The guard makes an
 * inverted pair shift by zero rather than by a negative count. */
#define _ccol_rmempool_tier_span(SS, LS) ((LS) >= (SS) ? (LS) - (SS) : 0)

/* Clamps a shift count into range. A short circuit governs evaluation, not
 * diagnosis: a compiler diagnoses a shift it can see is out of range even
 * on a branch it never reaches. Without the clamp an invalid triple gives
 * the declaring macro's _Static_assert message AND a page of shift
 * diagnostics from inside this header, which this project's -Werror
 * baseline turns into errors. One clamp covers both ways of going out of
 * range, because a negative count casts to a size_t above the width. We
 * clamp rather than reduce modulo the width, and that choice is
 * load-bearing; _ccol_rmempool_buffer_params_fit below explains why. For any
 * triple the predicate accepts at all the clamp changes no value, since
 * both counts are below the width there already. */
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

/* This chain names every element count that can need padding. A count is a
 * power of two, and only a count below _ccol_mempool_entry_align gets
 * rounded up at all, so the chain is complete for any alignment up to 128, a
 * limit that the assertion below pins. */
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

/* Internal, as the leading underscore says. It's the size arithmetic that
 * CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER applies for the caller, and it
 * sits in this installed header only because that public macro expands into
 * it in the caller's translation unit. Declare a buffer with
 * CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER and pass sizeof() on it; the name
 * and the expression here can change in any release.
 *
 * Every shift below is guarded against a NEGATIVE count. The declaring macro
 * rejects a triple with an inverted pair (LS below SS, or SC below LS - SS),
 * and the guards make such a triple give that macro's _Static_assert message
 * and nothing else. This expression sits in an array declarator, which a
 * compiler diagnoses before it gets to the assertion, so an unguarded shift
 * there would bury the explanation under a page of -Wshift-count-negative,
 * which this project's -Werror baseline turns into the first errors the
 * caller sees.
 *
 * The guards do NOT cover an exponent wider than size_t itself: the macro
 * rejects such a triple, but the caller sees shift-count diagnostics next to
 * the assertion. A guard for that case would cost more than it saves, since
 * the counts here are the caller's own literals and the assertion names the
 * real problem anyway. The guards change no value for any triple the macro
 * accepts.
 */
#define _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE(SS, LS, SC)         \
  ((size_t)(_ccol_rmempool_tier_span((SS), (LS)) + 1) * ((size_t)1 << (SC)) * \
       ((size_t)1 << (SS)) +                                                  \
   ((((size_t)1 << (SC)) >> _ccol_rmempool_tier_span((SS), (LS))) *           \
    (((size_t)1 << (_ccol_rmempool_tier_span((SS), (LS)) + 1)) - 1)) +        \
   _ccol_rmempool_status_padding((SS), (LS), (SC)))

/**
 * @brief Compile-time guard for CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER
 *
 * True only when the three power-of-two parameters give a well-defined
 * _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE result that fits in a
 * size_t. Don't use it directly;
 * CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER is the public entry point.
 *
 * The order of the sub-conditions below is deliberate: a term is evaluated
 * only after an earlier, short-circuited && operand has confirmed every
 * condition that term needs to be well-defined. The short-circuited side of
 * && or || isn't evaluated, so an out-of-range shift or a division by zero
 * written there doesn't make the condition undefined. That order governs
 * EVALUATION and not diagnosis though: a compiler is free to warn about a
 * shift count it can see is too wide for the type, even on an operand the
 * short circuit never reaches. GCC does that at both widths once a build
 * instruments its shifts, and Clang doesn't. So the shift counts below are
 * clamped wherever a rejected shape can push them out of range; see
 * _ccol_rmempool_shift_count.
 *
 * The conditions come in this order. First, every power-of-two exponent is
 * small enough for a 1 << exponent to be well-defined. Second,
 * largest_size_power_of_two really is larger than
 * smallest_size_power_of_two. Third, their difference is small enough to keep
 * the widest shift of _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE
 * well-defined. Fourth, number_of_smallest_size_elems_power_of_two is at
 * least largest_size_power_of_two - smallest_size_power_of_two, which is the
 * exact precondition for the division inside
 * _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE to be mathematically
 * exact, and matches what ccol_r_mempool_create's input validation enforces
 * at run time. Fifth, each of the two terms that
 * _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE adds is checked for
 * overflow with a division instead of forming the product, which could
 * overflow. Last, the sum of those two terms must not overflow size_t
 * either.
 */
/* Brings a shift count into the range of size_t's width. The predicate below
 * only needs it for the two counts that come from (LS) - (SS), because those
 * are the only counts that a shape the predicate rejects can push out of
 * range: an inverted pair makes that unsigned subtraction underflow to a
 * huge value.
 *
 * For any shape the predicate ACCEPTS the clamp changes nothing, since an
 * earlier && operand has already established that the difference is below
 * the width. The clamp is there for the rejected shape. The short circuit
 * means the operand is never evaluated, but a compiler can still diagnose a
 * shift count it can see exceeds the type. Measured, GCC does so once shifts
 * are instrumented, at both widths, and Clang doesn't at either width.
 * Without the clamp a caller's -Werror build fails hard on exactly the input
 * this macro is there to answer "no" for.
 *
 * We CLAMP rather than reduce modulo the width, and the difference is
 * load-bearing. A modulo maps a count of exactly the width to zero, which
 * collapses the surrounding (1 << count) - 1 to zero, and GCC then reports
 * the comparison it sits in as always true under -Wtype-limits. That swaps
 * one diagnostic for another and breaks an otherwise clean -Werror build for
 * every shape whose tier span is exactly the width. A clamp to one below the
 * width gives a legal shift that folds to nothing in particular, so no such
 * comparison shows up. The clamp changes no value: over every exponent
 * triple from 0 to 70, at both widths, the predicate gives the same answer
 * with it and without it.
 *
 * Every shift the predicate does is clamped, the (SS) and (SC) ones too.
 * Don't leave those out on the grounds that an earlier clause bounds them
 * already, because that's exactly the trap this whole block is about: an
 * earlier clause bounds what gets EVALUATED, but a compiler diagnoses a
 * shift it can see is out of range whether the branch is taken or not.
 * Without the clamp on the (SS) shift, a triple such as
 * (33, 35, 35) at 32 bits gives -Wshift-count-overflow there. */

/* The smallest element size a ranged pool serves, as a power of two.
   ccol_r_mempool_create refuses anything below it at run time, so a buffer
   declared for such a shape could never go to a pool that takes it. The
   predicate below has the same floor, which turns the mistake into a
   compile-time rejection instead of a buffer that just can never be used. */
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
 * Declares a uint8_t array of the right size for a ranged memory pool with
 * the given parameters, so you can pass the buffer to
 * ccol_r_mempool_create_from_preallocated_buffer().
 *
 * @param name Variable name for the buffer
 * @param smallest_size_power_of_two log2 of the smallest element size
 * @param largest_size_power_of_two log2 of the largest element size
 * @param number_of_smallest_size_elems_power_of_two log2 of element count in
 * smallest pool
 *
 * @note The computed size includes the status bytes of every sub-pool
 * @note Handy for embedded systems, and when you don't want a heap
 * allocation
 * @note The buffer holds every sub-pool in contiguous memory
 * @note The declared buffer is aligned to _ccol_mempool_entry_align, the
 * alignment every entry the pool hands out is sure to meet, so you can pass
 * it straight to ccol_r_mempool_create_from_preallocated_buffer() without
 * thinking about alignment, and every sub-pool segment inside the buffer
 * stays properly aligned
 * @note A _Static_assert rejects three cases at compile time. The first is a
 * set of three parameters that would make the pool's required buffer size
 * overflow size_t. The second is a number_of_smallest_size_elems_power_of_two
 * smaller than largest_size_power_of_two - smallest_size_power_of_two, the
 * same precondition ccol_r_mempool_create's input validation enforces at
 * run time, which is needed here too to keep the division inside
 * _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE exact rather than quietly
 * truncated. The third is a smallest_size_power_of_two below 4: the smallest
 * element a ranged pool serves is 16 bytes, so a buffer declared for
 * anything smaller could only go to a constructor that refuses it. Without
 * the assert the macro would quietly produce an array of the wrong size
 *
 * @see _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE
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
#define CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(                             \
    name, smallest_size_power_of_two, largest_size_power_of_two,               \
    number_of_smallest_size_elems_power_of_two)                                \
  _Alignas(_ccol_mempool_entry_align)                                          \
      uint8_t name[_CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE(          \
          (smallest_size_power_of_two), (largest_size_power_of_two),           \
          (number_of_smallest_size_elems_power_of_two))];                      \
  /* Every rule a caller can name gets its own assertion, ahead of the         \
   * catch-all below. One assertion over the whole predicate could only list   \
   * every rule and leave the reader to work out which one was broken, and it  \
   * would list them from a copy that has to be kept in step with the          \
   * predicate by hand; a separate assertion per rule says which rule failed.  \
   * Each condition below is one clause of _ccol_rmempool_buffer_params_fit,   \
   * so none of them can reject a triple the predicate accepts. */             \
  _Static_assert(                                                              \
      (smallest_size_power_of_two) >= _ccol_rmempool_min_smallest_size_power,  \
      "CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER: "                            \
      "smallest_size_power_of_two must be at least 4. The smallest element "   \
      "a ranged pool serves is 16 bytes");                                     \
  _Static_assert(                                                              \
      (largest_size_power_of_two) > (smallest_size_power_of_two),              \
      "CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER: "                            \
      "largest_size_power_of_two must be strictly greater than "               \
      "smallest_size_power_of_two. A ranged pool spans at least two size "     \
      "tiers; use CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER for one size");     \
  _Static_assert(                                                              \
      (size_t)(number_of_smallest_size_elems_power_of_two) >=                  \
          (size_t)((largest_size_power_of_two) -                               \
                   (smallest_size_power_of_two)),                              \
      "CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER: "                            \
      "number_of_smallest_size_elems_power_of_two must be at least "           \
      "largest_size_power_of_two - smallest_size_power_of_two, so that every " \
      "tier ends up with at least one element");                               \
  _Static_assert(                                                              \
      _ccol_rmempool_buffer_params_fit(                                        \
          (smallest_size_power_of_two), (largest_size_power_of_two),           \
          (number_of_smallest_size_elems_power_of_two)),                       \
      "CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER: these parameters would "     \
      "make the pool's own required buffer size overflow size_t, or name a "   \
      "power of two that a size_t on this target cannot hold")

/**
 * @brief Create a ranged memory pool from a preallocated buffer
 *
 * Creates a ranged memory pool on a buffer the caller gives instead of
 * allocating from the heap, which is handy for embedded systems. The buffer
 * is divided into contiguous segments, one per size pool.
 *
 * @param buffer Pointer to the preallocated buffer
 * @param buf_size Size of the buffer in bytes. It must match the computed size
 * exactly
 * @param smallest_size_power_of_two log2 of the smallest element size
 * @param largest_size_power_of_two log2 of the largest element size
 * @param number_of_smallest_size_elems_power_of_two log2 of element count in
 * smallest pool
 * @param fb_policy Fallback policy for when the pools are empty
 * @param single_threaded If true, do not lock (faster but not thread-safe)
 * @param mmgmt_procs Custom memory management procedures, or NULL for the
 * default malloc and free
 * @param err Optional pointer that receives an error string on failure
 *
 * @return Pointer to the new ranged memory pool, or NULL on failure
 *
 * @note The buffer size must match
 * _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE exactly
 * @note ccol_r_mempool_destroy() doesn't free the buffer, its lifetime
 * belongs to the caller
 * @note The pool structs themselves do come from mmgmt_procs
 * @note Each sub-pool calls mmgmt_procs' functions for its dynamic fallback
 * entries, and for its own bookkeeping of them, in the middle of updating
 * its own state and with its lock held when it has one. So those functions
 * must not call back into this same ranged pool: with a lock that can
 * deadlock, and without one it corrupts that state. An allocator built on a
 * pool has to draw from a different pool
 * @note The buffer holds every sub-pool in adjacent segments
 * @note Returns NULL with an error if the buffer isn't aligned enough. A
 * buffer from CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER is always aligned
 * right, while a buffer you build by hand must be aligned to at least
 * _ccol_mempool_entry_align, which is 16 and is what the constructor checks
 * for
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
 * @warning Do not call this function directly. Use the
 * ccol_r_mempool_destroy() macro instead
 */
void _ccol_r_mempool_destroy(ccol_r_mempool *rmp);

/**
 * @brief Destroy a ranged memory pool and set pointer to NULL
 *
 * Frees every resource of the ranged memory pool, all sub-pools included.
 * For a pool created from a preallocated buffer the buffer itself isn't
 * freed.
 *
 * @param rmp Ranged memory pool to destroy. It is set to NULL afterward
 *
 * @warning It asserts when the dynamic fallback still holds pointers that
 * nobody freed, so a possible leak doesn't go unnoticed. There is no such
 * assert if you didn't ask for a fallback when you created the pool. As a
 * rule, always free the allocated buffers.
 * @note Calling it with a NULL pointer is safe
 * @note For a preallocated pool the buffer's lifetime belongs to the caller
 * @note rmp is evaluated exactly once. It must be a modifiable lvalue, such
 * as a variable or an element of an array.
 */
#define ccol_r_mempool_destroy(rmp) \
  _ccol_r_mempool_destroy_impl(     \
      rmp, _ccol_uniq(__ccol_r_mempool_destroy_slot, __COUNTER__))

/* Internal: the body of ccol_r_mempool_destroy. slot is a name from
 * _ccol_uniq(), so the macro can nest inside the argument of another destroy
 * macro and still be -Wshadow clean. The argument is evaluated exactly
 * once. */
#define _ccol_r_mempool_destroy_impl(rmp, slot) \
  do {                                          \
    __typeof__(rmp) *slot = &(rmp);             \
    _ccol_r_mempool_destroy(*slot);             \
    *slot = NULL;                               \
  } while (0)

/**
 * @brief Get number of used entries for a specific size
 *
 * Gives the number of entries in use right now from the pool that serves
 * the given size, without the dynamic entries.
 *
 * @param rmp Ranged memory pool to query
 * @param size Size in bytes to query. It maps to one sub-pool
 *
 * @return Number of entries in use for this size, or 0 when the size is
 * invalid
 *
 * @note Returns 0 if the size is invalid, i.e. 0 or > largest_size
 * @note The size is rounded up to the next power-of-2 pool
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note Dynamic entries are tracked separately
 * @note It asserts if rmp is NULL
 *
 * @see ccol_r_mempool_total_capacity
 * @see ccol_r_mempool_dynamic_allocs_count
 */
size_t ccol_r_mempool_used_count(ccol_r_mempool *rmp, size_t size);

/**
 * @brief Get total capacity for a specific size
 *
 * Gives the total number of entries in the pool that serves the given size,
 * without the dynamic entries.
 *
 * @param rmp Ranged memory pool to query
 * @param size Size in bytes to query. It maps to one sub-pool
 *
 * @return Total capacity for this size, or 0 when the size is invalid
 *
 * @note Returns 0 if the size is invalid, i.e. 0 or > largest_size
 * @note The size is rounded up to the next power-of-2 pool
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note It asserts if rmp is NULL
 *
 * @see ccol_r_mempool_used_count
 * @see ccol_r_mempool_dynamic_allocs_count
 */
size_t ccol_r_mempool_total_capacity(ccol_r_mempool *rmp, size_t size);

/**
 * @brief Get the memory the ranged pool holds for its entries
 *
 * Gives the size in bytes of every tier's entry block added together, which
 * is what a ranged pool costs. Each tier that gets a thread cache holds a
 * reserve above its own nominal count, so this total is more than the sum
 * of the tier capacities.
 *
 * @param rmp Ranged memory pool to query
 *
 * @return Size in bytes of the entry block of every tier, added together
 *
 * @note Only the tiers' entry blocks are counted, on the same terms as
 * ccol_mempool_allocated_bytes()
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note It asserts if rmp is NULL
 *
 * @see ccol_mempool_allocated_bytes
 * @see ccol_r_mempool_total_capacity
 */
size_t ccol_r_mempool_allocated_bytes(ccol_r_mempool *rmp);

/**
 * @brief Get number of dynamic allocations for a specific size
 *
 * Gives the number of heap entries for the given size; what it counts
 * depends on the fallback policy.
 *
 * @param rmp Ranged memory pool to query
 * @param size Size in bytes to query
 *
 * @return Number of dynamic entries, or 0 when the size is invalid or the
 * fallback is off
 *
 * @note Returns 0 for ccol_fallback_disabled
 * @note With ccol_fallback_at_first_exhaustion the dynamic entries of each
 * pool are tracked
 * @note With ccol_fallback_at_last_exhaustion every dynamic entry is tracked
 * @note Returns 0 if the size is invalid, i.e. 0 or > largest_size
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note It asserts if rmp is NULL
 *
 * @see ccol_r_mempool_used_count
 * @see ccol_r_mempool_total_capacity
 */
size_t ccol_r_mempool_dynamic_allocs_count(ccol_r_mempool *rmp, size_t size);

/**
 * @brief Allocate an entry from the ranged pool
 *
 * Allocates memory of the requested size from the best sub-pool: the size
 * is rounded up to the nearest power-of-2 pool, and when that pool is empty
 * larger pools are tried. The fallback behavior depends on the policy.
 *
 * @param rmp Ranged memory pool to allocate from
 * @param size Size in bytes to allocate. It must be > 0 and <= largest_size
 *
 * @return Pointer to the allocated entry, or NULL on failure
 *
 * @note O(1) in the common case, O(number_of_pools) in the worst case
 * @note The memory you get is uninitialized; use
 * ccol_r_mempool_calloc_entry() for zeroed memory
 * @note The size is rounded up to the next power-of-2 pool size
 * @note Larger and larger pools are tried when the best pool is empty
 * @note The fallback behavior depends on fb_policy
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note Returns NULL if the size is 0 or > largest_size
 * @note Free the entry with ccol_r_mempool_free_entry()
 * @note It asserts if rmp is NULL
 *
 * @see ccol_r_mempool_calloc_entry
 * @see ccol_r_mempool_realloc_entry
 * @see ccol_r_mempool_free_entry
 */
void *ccol_r_mempool_alloc_entry(ccol_r_mempool *rmp, size_t size);

/**
 * @brief Allocate a zero-initialized entry from the ranged pool
 *
 * Like ccol_r_mempool_alloc_entry(), but the requested number of bytes is
 * zeroed before it comes back.
 *
 * @param rmp Ranged memory pool to allocate from
 * @param size Size in bytes to allocate
 *
 * @return Pointer to the zeroed entry, or NULL on failure
 *
 * @note Exactly 'size' bytes are zeroed, not the full pool element
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note It asserts if rmp is NULL
 *
 * @see ccol_r_mempool_alloc_entry
 * @see ccol_r_mempool_realloc_entry
 */
void *ccol_r_mempool_calloc_entry(ccol_r_mempool *rmp, size_t size);

/**
 * @brief Reallocate an entry to a different size
 *
 * Changes the size of an allocated entry. You get the original pointer back
 * unchanged, with no copy, whenever a move couldn't give the entry more
 * bytes than it holds already. That happens in two cases: when the entry
 * already holds exactly what the tier serving the new size would give it,
 * and when it already holds exactly the number of bytes requested. In every
 * other case a new entry sized for the request is allocated,
 * min(old_size, new_size) bytes are copied into it, and the old one is
 * freed.
 *
 * Both of those questions are about the bytes the entry holds, so a
 * pool-owned entry and a dynamic fallback entry holding the same number of
 * bytes behave the same here. How many bytes an entry holds does depend on
 * where it came from, and that's the one difference the two representations
 * really force: a pool-owned entry holds its tier's whole element size, a
 * dynamic fallback entry that a tier handed out after its own slots ran out
 * holds that same whole element size, and an entry from the shared fallback
 * that ccol_fallback_at_last_exhaustion uses holds exactly the size it was
 * created for.
 *
 * When the move can't be made, you get back, unmoved, an entry the request
 * fits inside, rather than a failure. That's the same for both kinds of
 * entry too. A shrink never fails for lack of memory it doesn't need. The
 * pointer you get then addresses at least the requested bytes, as on every
 * other successful path.
 *
 * @param rmp Ranged memory pool
 * @param addr Entry to reallocate, or NULL to allocate a new one
 * @param size New size in bytes. It must be > 0 and <= largest_size
 *
 * @return Pointer to the reallocated entry, or NULL on failure
 *
 * @note With a NULL addr it does the same as ccol_r_mempool_alloc_entry()
 * @note If the entry already holds as many bytes as a move could give it,
 * the original pointer comes back unchanged and nothing is copied
 * @note In every other case a new entry is allocated, the data copied and
 * the old entry freed. If that allocation fails and the request fits inside
 * the bytes addr already holds, addr comes back unmoved instead of NULL
 * @note min(old_user_size, new_user_size) bytes are copied
 * @note The old entry is freed when the reallocation succeeds
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note Returns NULL if the size is 0 or > largest_size, and does NOT touch
 * addr: for an invalid size a non-NULL addr is neither freed nor moved, and
 * the state is exactly as if the call never happened. A real failed
 * reallocation (a valid size the pool can't serve) leaves addr just as
 * untouched, so both failure modes share one contract: the original pointer
 * stays valid and the caller keeps owning it
 * @note It asserts if addr is non-NULL and didn't come from this
 * ccol_r_mempool, which catches corruption and a foreign pointer, matching
 * ccol_mempool_free_entry()
 * @note It asserts if rmp is NULL
 *
 * @see ccol_r_mempool_alloc_entry
 * @see ccol_r_mempool_free_entry
 */
void *ccol_r_mempool_realloc_entry(ccol_r_mempool *rmp, void *addr,
                                   size_t size);

/**
 * @brief Return an entry to the ranged pool it came from (internal function)
 *
 * Prefer the ccol_r_mempool_free_entry() macro, which also sets the caller's
 * pointer to NULL. The ranged pool finds the sub-pool the entry belongs to;
 * you give the ranged pool rather than that sub-pool because you have no way
 * of knowing which sub-pool it is.
 *
 * @param rmp Ranged pool that the entry came from
 * @param entry Entry to free. A NULL entry does nothing, which matches free()
 *
 * @note An entry that didn't come from this ranged pool is a caller error and
 * it is fatal; it is never quietly tolerated
 *
 * @see ccol_r_mempool_free_entry
 * @see ccol_r_mempool_alloc_entry
 */
void _ccol_r_mempool_free_entry(ccol_r_mempool *rmp, void *entry);

/**
 * @brief Free an entry back to the ranged pool
 *
 * Puts an allocated entry back on the free list of the right sub-pool, by
 * calling _ccol_r_mempool_free_entry() and then setting the pointer to NULL.
 *
 * @param rmp Ranged pool that the entry came from
 * @param entry Entry to free. It is set to NULL afterward
 *
 * @note It works for an entry from any sub-pool of the ranged pool
 * @note It also works for a dynamic entry from the fallback
 * @note Calling it with NULL is safe
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note Corruption is detected with assertions
 *
 * @see ccol_mempool_free_entry
 * @see ccol_r_mempool_alloc_entry
 *
 * @note Each argument is evaluated exactly once. entry must be a modifiable
 * lvalue, such as a variable or an element of an array.
 */
#define ccol_r_mempool_free_entry(rmp, entry) \
  _ccol_r_mempool_free_entry_impl(            \
      (rmp), entry, _ccol_uniq(__ccol_r_mempool_free_entry_slot, __COUNTER__))

/* Internal: the body of ccol_r_mempool_free_entry. slot is a name from
 * _ccol_uniq(), so the macro can nest inside the argument of another destroy
 * macro and still be -Wshadow clean. The argument is evaluated exactly
 * once. */
#define _ccol_r_mempool_free_entry_impl(rmp, entry, slot) \
  do {                                                    \
    __typeof__(entry) *slot = &(entry);                   \
    _ccol_r_mempool_free_entry((rmp), *slot);             \
    *slot = NULL;                                         \
  } while (0)

#pragma GCC visibility pop
