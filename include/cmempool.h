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

/* Everything that this header declares from here to its end is part of the
 * public ABI of libccollections, and the shared library exports all of it.
 * The library itself is built with -fvisibility=hidden, so any function or
 * object that one of these blocks does not cover stays internal to the
 * library and is absent from its dynamic symbol table: the application that
 * links against the library cannot interpose it, and a symbol of the same
 * name in that application cannot collide with it. */
#pragma GCC visibility push(default)

/**
 * @file cmempool.h
 * @brief Fixed-size and ranged memory pool allocators for efficient memory
 * management
 *
 * This header gives you two types of memory pools:
 * - ccol_mempool: Fixed-size element pool with optional dynamic fallback
 * - ccol_r_mempool: Ranged pool that serves power-of-2 sizes and selects the
 * best pool for each request
 *
 * Key features:
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
 * This is at least what malloc() guarantees, so the caller can store in a
 * pool entry any object that it can store in heap memory. The library rounds
 * the stride between entries up to a multiple of this value, which extends
 * the guarantee from the first entry to every entry: a buffer that starts at
 * a correctly aligned address, but whose stride is not a multiple of the
 * alignment, hands out misaligned entries all the same, because the offset
 * drifts one stride at a time.
 *
 * This is a fixed constant rather than _Alignof(max_align_t), because it is
 * part of the interface between the code of a caller and the shared library,
 * and the same compiler does not have to build both of them. The
 * preallocated-buffer macros size an array of the caller with this value,
 * while the library derives its stride from it, so a compiler that disagrees
 * about max_align_t mis-sizes every preallocated pool that is built against a
 * library another compiler produced. GCC and Clang do disagree about it on
 * i386, where they report 16 and 8. Sixteen is what the malloc of glibc
 * returns on every target that this library supports, so a pinned value
 * costs nothing that a caller can see, and it removes the toolchain from the
 * contract.
 */
#define _ccol_mempool_entry_align 16

/* There is one way the constant above can be wrong: on a target whose widest
 * fundamental type needs more than the pinned value, the pool would hand out
 * entries that cannot legally store that type. Such a target fails to compile
 * instead. */
_Static_assert(_ccol_mempool_entry_align >= _Alignof(max_align_t),
               "_ccol_mempool_entry_align must be at least the alignment this "
               "target requires for any object type");

/**
 * @brief True only when _ccol_mempool_align_up(x) is well-defined for x, that
 * is, when the addition that rounds x up does not overflow size_t.
 *
 * Do not use this macro directly. ccol_mempool_create(),
 * ccol_mempool_create_from_preallocated_buffer(), and
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER all check it, either directly or
 * through _ccol_mempool_buffer_params_fit, before they use the result of
 * _ccol_mempool_align_up().
 */
#define _ccol_mempool_align_up_fits(x) \
  ((x) <= SIZE_MAX - (_ccol_mempool_entry_align - 1))

/**
 * @brief Rounds x up to the nearest multiple of _ccol_mempool_entry_align.
 *
 * Every entry in the contiguous backing buffer of a ccol_mempool sits
 * extended_elem_size bytes after the entry before it, both in a heap buffer
 * and in a buffer that comes from
 * ccol_mempool_create_from_preallocated_buffer or
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER. The start address of the buffer is
 * aligned to _ccol_mempool_entry_align, which the constructor validates on its
 * own, but that alignment keeps only entry 0 aligned. The alignment of every
 * later entry depends on extended_elem_size ITSELF being a multiple of that
 * same alignment; if it is not, the offset of each entry drifts one stride at
 * a time.
 *
 * Two things depend on that, and the one that faces the caller is the more
 * important. An entry that a caller receives must be aligned for any object
 * type that it can hold, matching what malloc() guarantees, so that a long
 * double or a vector type is as valid in a pool entry as it is in heap
 * memory. As a result, the reads and writes of the pool through an entry are
 * correctly aligned too, since a free entry holds the free-list link in its
 * own first bytes. In both cases a drifting offset is undefined behavior and
 * a real fault risk on strict-alignment architectures; the loads and stores
 * of x86 and x86_64 tolerate misalignment, so the fault is invisible there.
 *
 * The caller must first confirm that x fits with
 * _ccol_mempool_align_up_fits(x), because otherwise the addition below can
 * overflow.
 */
#define _ccol_mempool_align_up(x)            \
  (((x) + (_ccol_mempool_entry_align - 1)) & \
   ~(size_t)(_ccol_mempool_entry_align - 1))

/**
 * @brief Create a fixed-size memory pool
 *
 * Creates a memory pool that manages a fixed number of fixed-size elements,
 * so every allocation returns an element of the same size. The pool gives
 * O(1) allocation and deallocation with a free list.
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
 * CCOL_MEMPOOL_COMPACT_LAYOUT, rounded further up to a power of two. So every
 * entry in the contiguous buffer of the pool, not only the first one, lands
 * on an address that is aligned for any object type. One status byte for
 * each entry follows the entry array in the same block
 * @note If elem_size < sizeof(uintptr_t), the library rounds it up
 * @note Returns NULL with an error if elem_size would overflow size_t after
 * that rounding
 * @note Thread-safe if single_threaded is false (it uses a mutex)
 * @note The pool calls the functions of mmgmt_procs for its dynamic fallback
 * entries, and for its own bookkeeping of them, in the middle of an update of
 * its own state, with its lock held when it has one. So those functions must
 * not call back into this same pool: with a lock the call deadlocks, and
 * without one it corrupts that state. An allocator built on a pool must draw
 * from a different pool
 * @note With fallback turned on, an allocation from the pool never fails until
 * the system runs out of memory
 * @note The caller must destroy the pool with ccol_mempool_destroy()
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
 * This macro declares a uint8_t array with the correct size for a memory pool
 * that has the given parameters, so that the caller can give the buffer to
 * ccol_mempool_create_from_preallocated_buffer().
 *
 * @param name Variable name for the buffer
 * @param elem_count Number of elements that the pool holds
 * @param elem_size Size of each element in bytes
 *
 * @note The macro includes the status bytes of the pool in the size that it
 * computes
 * @note This is useful for embedded systems, or when you do not want a heap
 * allocation
 * @note The macro aligns the declared buffer to _ccol_mempool_entry_align,
 * the alignment that every entry the pool hands out is sure to meet, so the
 * caller can give the buffer directly to
 * ccol_mempool_create_from_preallocated_buffer() without thinking about
 * alignment
 * @note An elem_size smaller than sizeof(uintptr_t) is rounded up to hold the
 * free-list pointer. The macro rounds it before it computes the size of the
 * buffer, in the same way as
 * ccol_mempool_create_from_preallocated_buffer(), which keeps the element
 * count of the declared buffer in agreement with what that constructor carves
 * it into. Without this, the buffer would have the size for the caller's
 * smaller elem_size, which nothing rounded, while the constructor divides it
 * up with the larger, rounded one
 * @note The stride for each element is the rounded elem_size taken up to
 * _ccol_mempool_entry_align and then, unless the library was built with
 * CCOL_MEMPOOL_COMPACT_LAYOUT, on up to a power of two. This again matches
 * the stride of ccol_mempool_create_from_preallocated_buffer() exactly, and
 * keeps every entry in the resulting pool aligned for any object type, not
 * only entry 0
 * @note The macro reserves one status byte for each element after the
 * elements, in the same buffer, so the pool needs no allocation of its own
 * for them
 * @note elem_count must be nonzero. A _Static_assert rejects two cases at
 * compile time: an elem_count of zero, and an elem_count * (the stride plus
 * the status byte of that element) that would overflow size_t. Without the
 * assert, the macro silently produces an array that is too small, or a
 * zero-length array when elem_count is 0
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
 * @brief Selects how far the library rounds up the distance between entries.
 *
 * The distance from one entry to the next is at least the element size: the
 * library raises that size to hold the list link of a free entry and then
 * rounds it up to _ccol_mempool_entry_align. This switch decides whether the
 * library rounds it further.
 *
 * At 0, the default, the library rounds it on up to a power of two, so that a
 * shift recovers the position of an entry in the pool from its address. At 1
 * (for example -DCCOL_MEMPOOL_COMPACT_LAYOUT=1) the library leaves it at the
 * aligned element size, and a multiply against a reciprocal, which the
 * library computes when it builds the pool, recovers the position instead.
 * The multiply costs a little on every allocation and release, but it can
 * save a great deal of memory: an element size a little above a power of two
 * almost doubles under the default and does not move at all under the
 * compact setting. For an element size that is already a power of two, the
 * two settings produce identical pools.
 *
 * This is a build-wide switch rather than a per-pool argument, because a
 * choice made for each pool would put that choice on the path of every
 * allocation and release. Measured, that costs about 8 percent for a pool
 * with a thread cache and 20 percent for a single-threaded one, and every
 * caller pays it, including the callers whose element sizes make the two
 * layouts identical.
 *
 * @warning This value is part of the interface between an application and the
 * library, not only a build detail of the library. The macros below that
 * declare a buffer size an array in the translation unit of the application,
 * while the library derives its stride from the same setting. If the library
 * is built with one value and an application is compiled against another, the
 * two disagree about how large a preallocated buffer has to be, and every
 * pool built on one has the wrong size. Use the same value for the library
 * and for every translation unit that includes this header.
 */
#ifndef CCOL_MEMPOOL_COMPACT_LAYOUT
#define CCOL_MEMPOOL_COMPACT_LAYOUT 0
#endif

/* This enforces the warning above at link time, so that a reader does not
 * have to obey it on trust.
 *
 * The library defines exactly one of these two objects, and the name of that
 * object states the setting that the library was built with.
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER takes the address of the one that
 * its own setting names, so if the two sides are built differently the link
 * fails with an undefined reference that names the layout the declaring code
 * expected. Without this, the build quietly produces a buffer of the wrong
 * size.
 *
 * Only that macro carries the reference, because only that macro is at risk.
 * Nothing else here depends on the setting: no function signature, type or
 * struct, and not the ranged buffer macro either, because the element size
 * of every tier is a power of two at or above the entry alignment, where the
 * two roundings agree. A translation unit that only includes this header has
 * nothing that can get the wrong size.
 *
 * A reference on every includer would make a file that uses only the macros
 * fail to link without the library, although that file does not carry the
 * risk. A file that declares such a buffer must link the library in any case,
 * because the buffer exists to go to a constructor. */
/* The tag is a scalar, so the reference below takes its address explicitly
 * rather than relying on array-to-pointer decay. See the note beside the
 * definition in cmempool.c for why it is not a one-element array. */
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
 * The macros that declare a buffer size a static array with this macro, so it
 * cannot be a function, or a builtin that the compiler is only likely to
 * fold; it is a conditional chain. Its first entry is the alignment itself
 * rather than some other fixed constant, because the value that reaches it is
 * always already rounded up to that alignment, and a chain with a different
 * floor would disagree with the stride of the runtime on a target where the
 * two differ. The chain gives 0 above 2^40, which
 * _ccol_mempool_buffer_params_fit() rejects, so an absurd element size fails
 * at compile time instead of wrapping in silence.
 *
 * The same is true at every width: a chain entry beyond what a size_t can
 * hold converts to exactly 0 (on a 32-bit size_t, every entry from 2^32 up),
 * which is the same "no stride fits" answer that the ceiling of the chain
 * gives. The entry above which a size is rejected therefore moves down with
 * the width.
 */
/* The chain is split by width, because every entry above 2^31 is a comparison
   against a constant that a 32-bit size_t cannot hold, and Clang rejects such
   a comparison outright under -Werror instead of folding it. So those entries
   are not emitted at all on such a target. A size that reaches past the last
   entry of the chain gives 0, which both _ccol_mempool_buffer_params_fit()
   and the runtime read as "no stride fits". Because the chain is split by
   width, the point at which that happens follows the width. */
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
 * An entry carries no header, so the stride is the element size of the
 * caller, raised to hold the free-list link that a free entry stores in its
 * own first bytes, and then rounded to the alignment that every entry is sure
 * to meet. In the default layout the library rounds it on up to a power of
 * two, so that a shift, not a division, turns the address of an entry into
 * its index.
 *
 * CCOL_MEMPOOL_COMPACT_LAYOUT stops at the alignment and keeps the stride
 * that the element size needs: it trades that shift for a multiply by a
 * reciprocal that the library computes in advance, and it spends no bytes on
 * the rounding.
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
 * @brief Bytes that a pool of elem_count elements of elem_size needs.
 *
 * The block holds the entries themselves and then one status byte for each
 * entry. Because both parts stay in the same block, a preallocated buffer
 * holds everything that the pool needs, and a heap pool takes exactly one
 * allocation.
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
 * Creates a memory pool that uses a buffer from the caller instead of
 * allocating from the heap, which is useful for embedded systems and when you
 * do not want a heap allocation. The buffer must have the correct size: use
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER, or compute the size by hand.
 *
 * @param buffer Pointer to the preallocated buffer
 * @param buf_size Size of the buffer in bytes
 * @param elem_size Size of each element in bytes. It must be > 0. If it is
 * smaller than sizeof(uintptr_t), the library rounds it up, in the same way as
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
 * @note The element count is buf_size / (stride + 1). An entry carries no
 * header; the stride is elem_size rounded up to _ccol_mempool_entry_align,
 * and then rounded on up to a power of two unless the library was built with
 * CCOL_MEMPOOL_COMPACT_LAYOUT. So every entry, not only the first one, lands
 * on an address that is aligned for any object type, and a shift converts an
 * address to an index. The one extra byte for each element is the status
 * byte of that element, which stays in the same buffer, after the entries.
 * _ccol_mempool_stride() computes exactly that stride, and
 * CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER sizes its own buffer with the same
 * arithmetic, which makes it the way to get the size right without a second
 * statement of the formula
 * @note ccol_mempool_destroy() does not free the buffer; the caller owns the
 * lifetime of the buffer
 * @note The pool struct itself does come from mmgmt_procs
 * @note The pool calls the functions of mmgmt_procs for its dynamic fallback
 * entries, and for its own bookkeeping of them, in the middle of an update of
 * its own state, with its lock held when it has one. So those functions must
 * not call back into this same pool: with a lock the call deadlocks, and
 * without one it corrupts that state. An allocator built on a pool must draw
 * from a different pool
 * @note Returns NULL with an error if elem_size is zero
 * @note If elem_size is nonzero but < sizeof(uintptr_t), the library rounds it
 * up, as ccol_mempool_create() does
 * @note Returns NULL with an error if elem_size would overflow size_t after
 * the alignment and the power-of-two rounding
 * @note Returns NULL with an error if the buffer is not aligned enough. A
 * buffer that comes from CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER is always
 * aligned correctly, while a buffer that you build by hand must be aligned to
 * at least _ccol_mempool_entry_align, which is 16 and is what the constructor
 * checks for
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
 * @note The library can call the free function of a custom allocator after
 * this function returns, and even after main() returns. A pool that threads
 * share keeps a small cache of entries for each thread, and a destroy of the
 * pool marks those caches dead instead of freeing them, because a cache
 * belongs to the thread that holds it and another thread must not free it
 * under that thread. The library frees each cache later, at one of three
 * points: when the owning thread next has to look a magazine up for a pool
 * that its own cache does not already hold; when that thread exits; or at
 * process exit, or when dlclose() unloads the library, if that thread is the
 * one that runs the unload handler of the library. Each free calls the free
 * function that the pool was created with. None of these three points is
 * sure to happen: a thread that keeps using the same few pools answers every
 * request from its cache, never looks a magazine up, and so holds an orphaned
 * cache for ever. A program that needs its allocator to see every free
 * should join such threads. Once dlclose() unloads the library, the cache of
 * every other thread that is running stays allocated for the rest of the
 * process, and that thread exits cleanly.
 *
 * So an allocator that is itself torn down at a known point must outlive
 * that point. The library can call an allocator that draws from an arena
 * after the end of main() frees that arena, and the same is true for an
 * allocator that draws from storage which the exit handler of the
 * application frees. An allocator built on malloc and free is not affected,
 * and neither is one built on storage that lives for the whole process. A
 * pool created with single_threaded keeps no such cache and has no such tail.
 */
void _ccol_mempool_destroy(ccol_mempool *mp);

/**
 * @brief Destroy a memory pool and set pointer to NULL
 *
 * Frees every resource of the memory pool. For a pool that was created from a
 * preallocated buffer, this macro frees only the pool struct, not the buffer
 * itself.
 *
 * @param mp Memory pool to destroy. The macro sets it to NULL afterward
 *
 * @warning This function asserts when the dynamic fallback holds pointers
 * that nobody freed, so that a possible leak does not go unnoticed. Such an
 * assert does not happen if you did not ask for a fallback when you created
 * this pool. As a rule, always free the allocated buffers.
 * @note It is safe to call this macro with a NULL pointer
 * @note For a preallocated pool, the caller owns the lifetime of the buffer
 * @note The macro evaluates mp exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array.
 */
#define ccol_mempool_destroy(mp) \
  _ccol_mempool_destroy_impl(    \
      mp, _ccol_uniq(__ccol_mempool_destroy_slot, __COUNTER__))

/* Internal: the body of ccol_mempool_destroy. slot is a name from
 * _ccol_uniq(), so the macro nests inside the argument of another destroy
 * macro and stays -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_mempool_destroy_impl(mp, slot) \
  do {                                       \
    __typeof__(mp) *slot = &(mp);            \
    _ccol_mempool_destroy(*slot);            \
    *slot = NULL;                            \
  } while (0)

/**
 * @brief Allocate an entry from the pool
 *
 * Returns a pointer to a free entry from the pool, allocating from the heap
 * when the pool is empty and the fallback is on. The contents of the entry
 * are uninitialized.
 *
 * @param mp Memory pool to allocate from
 *
 * @return Pointer to the allocated entry, or NULL when the pool is empty and
 * there is no fallback
 *
 * @note O(1) complexity
 * @note The memory that comes back is uninitialized; use
 * ccol_mempool_calloc_entry() for zeroed memory
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note This function asserts if mp is NULL
 * @note The caller must free the entry with ccol_mempool_free_entry()
 * @note With the fallback on, the function returns NULL only when the system
 * runs out of memory
 *
 * @see ccol_mempool_calloc_entry
 * @see ccol_mempool_free_entry
 */
void *ccol_mempool_alloc_entry(ccol_mempool *mp);

/**
 * @brief Allocate a zero-initialized entry from the pool
 *
 * This function is like ccol_mempool_alloc_entry(), but it zeros the memory
 * before it returns.
 *
 * @param mp Memory pool to allocate from
 *
 * @return Pointer to the zeroed entry, or NULL when the pool is empty and
 * there is no fallback
 *
 * @note O(1) allocation, plus the cost of the memory set
 * @note The function zeros the whole element
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note This function asserts if mp is NULL
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
 * Returns an allocated entry to the free list of the pool for reuse, with
 * careful corruption detection through assertions. It is safe to call with
 * NULL.
 *
 * @param entry Entry to free. The macro sets it to NULL afterward
 *
 * @note O(1) complexity
 * @note It is safe to call this macro with NULL, in which case it does
 * nothing, like free()
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note Assertions detect a double free, with one exception: two threads
 * that free the same entry at the same instant, because the check runs
 * outside the pool lock. Such a call is a data race in the program of the
 * caller in any case.
 * @note The macro establishes ownership from the address alone, before it
 * reads anything through the pointer of the caller. A pool-owned entry
 * carries no header; its state is one byte in the status array of the pool.
 * The pool also records the address of every dynamic fallback entry that it
 * hands out, until that entry is freed, and it checks an address outside its
 * buffer against that record
 * @note For a dynamic entry from the fallback, the macro frees to the heap
 * @note The macro asserts on a double free and on an address that this pool
 * does not own, including an entry of another pool and a dynamic entry that
 * is already free, and it reads nothing through such an address
 *
 * @see ccol_mempool_alloc_entry
 * @see ccol_mempool_calloc_entry
 *
 * @note The macro evaluates each argument exactly once. entry must be a
 * modifiable lvalue, such as a variable or an element of an array.
 */
#define ccol_mempool_free_entry(mp, entry) \
  _ccol_mempool_free_entry_impl(           \
      (mp), entry, _ccol_uniq(__ccol_mempool_free_entry_slot, __COUNTER__))

/* Internal: the body of ccol_mempool_free_entry. slot is a name from
 * _ccol_uniq(), so the macro nests inside the argument of another destroy
 * macro and stays -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_mempool_free_entry_impl(mp, entry, slot) \
  do {                                                 \
    __typeof__(entry) *slot = &(entry);                \
    _ccol_mempool_free_entry((mp), *slot);             \
    *slot = NULL;                                      \
  } while (0)

/**
 * @brief Get total capacity of the pool
 *
 * Returns the total number of fixed-size entries that the pool was created
 * with, not counting dynamic entries.
 *
 * @param mp Memory pool to query
 *
 * @return Total number of pool entries. The count does not include dynamic
 * entries
 *
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note This function asserts if mp is NULL
 * @note The pool tracks dynamic entries on their own
 * @note A pool with a cache for each thread holds a reserve above this
 * count, which keeps this count available to any thread no matter how much
 * other threads cached. Because an entry leaves a cache with no lock held,
 * and so with no count to consult, concurrent callers can in exchange hold a
 * few entries more than this count at one time, for a short time, but never
 * more than this count plus that reserve. A pool with no cache (one that is
 * single_threaded, or one built on a preallocated buffer) is exact in both
 * directions
 *
 * @see ccol_mempool_used_count
 * @see ccol_mempool_allocated_bytes
 * @see ccol_mempool_dynamic_allocs_count
 */
size_t ccol_mempool_total_capacity(ccol_mempool *mp);

/**
 * @brief Get the memory the pool holds for its entries
 *
 * Returns the size in bytes of the single block that the pool uses for its
 * entries and for the state of each entry, which is what the pool costs.
 * ccol_mempool_total_capacity() deliberately does not report that cost: a
 * pool that receives a thread cache holds a reserve above the count that it
 * was created with, so its memory exceeds its usable capacity. Use this
 * function to size a pool against a memory budget.
 *
 * @param mp Memory pool to query
 *
 * @return Size in bytes of the entry block of the pool
 *
 * @note The function counts the entry block only. The handle itself is a
 * small, fixed allocation, the cache for each thread is a separate allocation
 * that the library makes when that thread first uses the pool, and an entry
 * that the dynamic fallback serves is not part of this block at all
 * @note For a pool built on a buffer from the caller, this is the part of
 * that buffer that the pool divided into entries, none of which the pool
 * allocated
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note This function asserts if mp is NULL
 *
 * @see ccol_mempool_total_capacity
 * @see ccol_mempool_dynamic_allocs_count
 */
size_t ccol_mempool_allocated_bytes(ccol_mempool *mp);

/**
 * @brief Get number of currently allocated pool entries
 *
 * Returns the number of entries from the pool that the pool currently holds
 * out to callers. The count does not include an entry that sits in the cache
 * of a thread (which left the shared free list, but which the pool gave to
 * nobody), and it does not include a dynamic entry either.
 *
 * @param mp Memory pool to query
 *
 * @return Number of pool entries currently in use
 *
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note This function asserts if mp is NULL
 * @note The pool tracks dynamic entries on their own
 * @note The count is exact on a quiet pool, where it never exceeds
 * ccol_mempool_total_capacity(). On a pool with a cache for each thread,
 * concurrent allocation can put the count a few entries above that, but it
 * never goes above the slot count that ccol_mempool_allocated_bytes()
 * reports. See ccol_mempool_total_capacity
 *
 * @see ccol_mempool_total_capacity
 * @see ccol_mempool_dynamic_allocs_count
 */
size_t ccol_mempool_used_count(ccol_mempool *mp);

/**
 * @brief Get number of dynamically allocated entries
 *
 * Returns the number of entries that came from the heap because the pool was
 * empty, which is non-zero only when fallback_to_dynamic_memory was on.
 *
 * @param mp Memory pool to query
 *
 * @return Number of heap entries that are currently active
 *
 * @note Returns 0 if the fallback was off at creation
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note This function asserts if mp is NULL
 * @note The caller frees these entries in the usual way, with
 * ccol_mempool_free_entry()
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
 * This policy decides when and how the ranged memory pool falls back to
 * dynamic allocation after a single sub-pool becomes empty.
 */
typedef enum ccol_r_memory_fallback_policy_t {
  ccol_fallback_disabled = 0,        /**< Never use dynamic allocation */
  ccol_fallback_at_first_exhaustion, /**< Each size pool has its own fallback */
  ccol_fallback_at_last_exhaustion,  /**< Fall back only after every pool is
                                        empty */
  ccol_fallback_end_place_holder     /**< Sentinel value (internal use) */
} ccol_r_memory_fallback_policy_t;

/**
 * @brief Create a ranged memory pool
 *
 * Creates a group of fixed-size memory pools that cover a range of power-of-2
 * sizes, where the smallest pool that can hold the requested size serves each
 * allocation. As the sizes rise, the pool sizes double and the element counts
 * halve.
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
 * @param fb_policy Fallback policy for the time when the pools are empty
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
 * @note Each sub-pool calls the functions of mmgmt_procs for its dynamic
 * fallback entries, and for its own bookkeeping of them, in the middle of an
 * update of its own state, with its lock held when it has one. So those
 * functions must not call back into this same ranged pool: with a lock the
 * call can deadlock, and without one it corrupts that state. An allocator
 * built on a pool must draw from a different pool
 * @note The caller must destroy the pool with ccol_r_mempool_destroy()
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
 * This internal macro, which CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER uses,
 * computes the exact buffer size that a ranged memory pool with the given
 * parameters needs.
 *
 * @param SS smallest_size_power_of_two
 * @param LS largest_size_power_of_two
 * @param SC smallest_elem_count_power_of_two
 *
 * @return Size in bytes that the buffer needs
 *
 * @note This macro is for internal use. Use
 * CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER instead
 *
 * Sub-pool i holds 2^(SC - i) entries of 2^(SS + i) bytes, for i from 0 to
 * N - 1, where N = LS - SS + 1, and needs one stride and one status byte for
 * each entry. ccol_r_mempool_create() needs the smallest element size to be
 * at least 16 bytes, so every element size here is already a power of two at
 * or above the alignment that every entry is sure to meet, and the stride is
 * the element size itself. The entry bytes therefore total N * 2^(SS + SC),
 * which is the first term below, and the status bytes total
 * 2 * 2^SC * (2^N - 1) / 2^N, which is the second term.
 *
 * The segment of each sub-pool holds its entries and then its status bytes,
 * and the next segment begins where that segment ends, so a segment whose
 * length is not a multiple of the alignment would give the sub-pool after it
 * a misaligned first entry. The entry bytes of a tier are always a whole
 * number of strides, and so already aligned, but its status bytes are one for
 * each entry, and that count falls below the alignment for the last few
 * tiers, where the element count has halved down into single digits. The
 * macro rounds the status regions of those tiers up, which is the third term
 * below. The rounding is pure slack that nothing reads or writes, and it
 * totals a few dozen bytes for any configuration.
 *
 * The parameters of this macro must already satisfy the validation of
 * ccol_r_mempool_create(), which is SC >= LS - SS, that is, SC + 1 >= N, and
 * which guarantees that the division is exact. The macro does not form the
 * full, un-reduced product 2 * 2^SC * (2^N - 1) and divide it down
 * afterward, because such a product can overflow size_t well before the final
 * division brings the value back into range, and it then wraps in silence to
 * a wrong buffer size that is too small. Instead, the macro first reduces the
 * 2 * 2^SC / 2^N factor with a single right shift, which is exact under the
 * same precondition and always well-defined, because the validation of
 * ccol_r_mempool_create() keeps (LS - SS) below the width of size_t. The
 * macro then multiplies by the much smaller factor that remains. This cannot
 * avoid an overflow for parameters that are large enough to make the
 * requested buffer itself too big for a size_t; it only makes sure that no
 * further overflow arrives on top of that inherent limit.
 */
/* The share of one tier in the padding above: the bytes that the macro adds
 * to the status region of that tier so that the entries of the next tier stay
 * aligned. C is a candidate element count. The term is zero unless that count
 * is truly one of the tiers of this configuration, and it is zero again for
 * any count that is already at or above the alignment. */
/* The leading guard keeps the shift below from taking a negative count when
 * the parameters are invalid. The declaring macro asserts the parameters, but
 * a size macro expands inside a declarator, and the compiler diagnoses that
 * declarator before it reaches the assertion. Without this guard, the first
 * thing a caller sees for a bad parameter is a page of
 * -Wshift-count-negative from inside this header, which the -Werror baseline
 * of this project turns into errors. */
/* The tiers between the smallest and the largest size. The guard makes an
 * inverted pair shift by zero instead of by a negative count. */
#define _ccol_rmempool_tier_span(SS, LS) ((LS) >= (SS) ? (LS) - (SS) : 0)

/* This macro clamps a shift count into range. A short circuit governs
 * evaluation, not diagnosis: a compiler diagnoses a shift that it can see is
 * out of range even on a branch that it never reaches. Without the clamp, an
 * invalid triple produces the _Static_assert message of the declaring macro
 * AND a page of shift diagnostics from inside this header, which the -Werror
 * baseline of this project turns into errors. One clamp covers both ways to
 * go out of range, because a negative count casts to a size_t above the
 * width. The macro clamps instead of reducing modulo the width, and that
 * choice is load-bearing; see _ccol_rmempool_buffer_params_fit below for the
 * reason. The clamp changes no value for any triple that the predicate
 * accepts at all, because there both counts are already below the width. */
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
 * power of two, and only a count below _ccol_mempool_entry_align rounds up at
 * all, so the chain is complete for any alignment up to 128, a limit that the
 * assertion below pins. */
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

/* This macro is internal, as the leading underscore says. It is the size
 * arithmetic that CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER applies for the
 * caller, and it sits in this installed header only because that public macro
 * expands into it in the translation unit of the caller. Declare a buffer with
 * CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER and pass sizeof() on it; the name
 * and the expression here can change in any release.
 *
 * A guard protects every shift below from a NEGATIVE count. The declaring
 * macro rejects a triple with an inverted pair (LS below SS, or SC below
 * LS - SS), and the guards make such a triple produce the _Static_assert
 * message of that macro and nothing else. This expression sits in an array
 * declarator, which a compiler diagnoses before it reaches the assertion, so
 * an unguarded shift there buries the explanation under a page of
 * -Wshift-count-negative, which the -Werror baseline of this project turns
 * into the first errors that the caller sees.
 *
 * The guards do NOT cover an exponent wider than size_t itself: the macro
 * rejects such a triple, but the caller sees shift-count diagnostics beside
 * the assertion. A guard for that case costs more than it saves, because the
 * counts here are the literals of the caller and the assertion names the
 * real problem all the same. The guards change no value for any triple that
 * the macro accepts.
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
 * This predicate is true only when the three power-of-two parameters produce a
 * well-defined _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE result that
 * fits in a size_t. Do not use it directly;
 * CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER is the public entry point.
 *
 * The order of the sub-conditions below is deliberate: a term is evaluated
 * only after an earlier, short-circuited && operand confirms every condition
 * that the term needs to be well-defined. Because the short-circuited side of
 * && or || is not evaluated, an out-of-range shift or a division by zero
 * written there does not make the condition undefined. That order governs
 * EVALUATION, not diagnosis, though: a compiler is free to warn about a shift
 * count that it can see is too wide for the type, even on an operand that
 * the short circuit never reaches. GCC does that, at both widths, once a
 * build instruments its shifts; Clang does not. So the shift counts below
 * are clamped wherever a rejected shape can push them out of range; see
 * _ccol_rmempool_shift_count.
 *
 * The conditions come in this order. First, every power-of-two exponent is
 * small enough to make a 1 << exponent well-defined. Second,
 * largest_size_power_of_two is truly larger than smallest_size_power_of_two.
 * Third, their difference stays small enough to keep the widest shift of
 * _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE well-defined. Fourth,
 * number_of_smallest_size_elems_power_of_two is at least
 * largest_size_power_of_two - smallest_size_power_of_two, which is the exact
 * precondition that the division inside
 * _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE needs to be
 * mathematically exact, and which matches what the input validation of
 * ccol_r_mempool_create enforces on its own at run time. Fifth, the predicate
 * checks each of the two terms that
 * _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE adds for overflow, with a
 * division instead of forming the product, which can overflow. Last, it
 * checks that the sum of those two terms does not itself overflow size_t.
 */
/* This brings a shift count into the range of the width of size_t. The
 * predicate below needs it only for the two counts that come from
 * (LS) - (SS), because those are the only counts that a shape the predicate
 * rejects can push out of range: an inverted pair makes that unsigned
 * subtraction underflow to an enormous value.
 *
 * For any shape that the predicate ACCEPTS, the clamp changes nothing,
 * because an earlier && operand has already established that the difference
 * is smaller than the width. The clamp exists for the rejected shape: the
 * short circuit means that the operand is never evaluated, but a compiler can
 * diagnose a shift count that it can see exceeds the type all the same.
 * Measured, GCC does so once shifts are instrumented, at both widths, while
 * Clang does not, at either width. Without the clamp, the -Werror build of a
 * caller fails hard on exactly the input that this macro exists to answer
 * "no" for.
 *
 * The macro CLAMPS instead of reducing modulo the width, and that difference
 * is load-bearing. A modulo maps a count of exactly the width to zero, which
 * collapses the (1 << count) - 1 around it to zero, and GCC then reports the
 * comparison that it sits in as always true, under -Wtype-limits. That trades
 * one diagnostic for another and breaks an otherwise clean -Werror build for
 * every shape whose tier span is exactly the width. A clamp to one below the
 * width gives a legal shift that folds to nothing in particular, so no such
 * comparison appears. The clamp changes no value: over every exponent triple
 * from 0 to 70, at both widths, the predicate answers the same with it and
 * without it.
 *
 * The predicate clamps every shift that it does, the (SS) and (SC) ones
 * included. Do not exclude those on the grounds that an earlier clause
 * already bounds them, which is the mistake this whole block describes: an
 * earlier clause bounds what is EVALUATED, but a compiler diagnoses a shift
 * that it can see is out of range whether or not the branch is taken.
 * Measured, with that exclusion in place, a triple such as (33, 35, 35) at 32
 * bits produces -Wshift-count-overflow on the (SS) shift. */

/* The smallest element size that a ranged pool serves, as a power of two.
   ccol_r_mempool_create refuses anything below it at run time, so a buffer
   declared for such a shape could never go to a pool that accepted it. The
   predicate below carries the same floor, which makes the mistake a
   compile-time rejection instead of a buffer that is merely never usable. */
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
 * This macro declares a uint8_t array with the correct size for a ranged
 * memory pool that has the given parameters, so that the caller can give the
 * buffer to ccol_r_mempool_create_from_preallocated_buffer().
 *
 * @param name Variable name for the buffer
 * @param smallest_size_power_of_two log2 of the smallest element size
 * @param largest_size_power_of_two log2 of the largest element size
 * @param number_of_smallest_size_elems_power_of_two log2 of element count in
 * smallest pool
 *
 * @note The macro includes the status bytes of every sub-pool in the size that
 * it computes
 * @note This is useful for embedded systems, and when you do not want a heap
 * allocation
 * @note The buffer holds every sub-pool in contiguous memory
 * @note The macro aligns the declared buffer to _ccol_mempool_entry_align,
 * the alignment that every entry the pool hands out is sure to meet, so the
 * caller can give the buffer directly to
 * ccol_r_mempool_create_from_preallocated_buffer() without thinking about
 * alignment, and every single sub-pool segment inside the buffer stays
 * correctly aligned
 * @note A _Static_assert rejects three cases at compile time. The first is a
 * set of three parameters that would make the required buffer size of the pool
 * overflow size_t. The second is a
 * number_of_smallest_size_elems_power_of_two that is smaller than
 * largest_size_power_of_two - smallest_size_power_of_two, the same
 * precondition that the input validation of ccol_r_mempool_create enforces at
 * run time, which is needed here too to keep the division inside
 * _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE exact rather than silently
 * truncated. The third is a smallest_size_power_of_two below 4: the smallest
 * element that a ranged pool serves is 16 bytes, so a buffer declared for
 * anything smaller could only go to a constructor that refuses it. Without
 * the assert, the macro produces an array of the wrong size in silence
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
  /* Each rule that a caller can name gets an assertion of its own, before the \
   * catch-all below. A single assertion over the whole predicate could only   \
   * recite every rule and leave the reader to work out which one was broken,  \
   * and it would recite them from a list that has to be kept in step with the \
   * predicate by hand, while a separate assertion per rule says which rule    \
   * failed. Each condition below is one clause of                             \
   * _ccol_rmempool_buffer_params_fit, so none of them can reject a triple     \
   * that the predicate accepts. */                                            \
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
 * Creates a ranged memory pool that uses a buffer from the caller instead of
 * allocating from the heap, which is useful for embedded systems. The
 * constructor divides the buffer into contiguous segments, one for each size
 * pool.
 *
 * @param buffer Pointer to the preallocated buffer
 * @param buf_size Size of the buffer in bytes. It must match the computed size
 * exactly
 * @param smallest_size_power_of_two log2 of the smallest element size
 * @param largest_size_power_of_two log2 of the largest element size
 * @param number_of_smallest_size_elems_power_of_two log2 of element count in
 * smallest pool
 * @param fb_policy Fallback policy for the time when the pools are empty
 * @param single_threaded If true, do not lock (faster but not thread-safe)
 * @param mmgmt_procs Custom memory management procedures, or NULL for the
 * default malloc and free
 * @param err Optional pointer that receives an error string on failure
 *
 * @return Pointer to the new ranged memory pool, or NULL on failure
 *
 * @note The buffer size must match
 * _CCOL_CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE exactly
 * @note ccol_r_mempool_destroy() does not free the buffer; the caller owns
 * the lifetime of the buffer
 * @note The pool structs themselves do come from mmgmt_procs
 * @note Each sub-pool calls the functions of mmgmt_procs for its dynamic
 * fallback entries, and for its own bookkeeping of them, in the middle of an
 * update of its own state, with its lock held when it has one. So those
 * functions must not call back into this same ranged pool: with a lock the
 * call can deadlock, and without one it corrupts that state. An allocator
 * built on a pool must draw from a different pool
 * @note The buffer holds every sub-pool in adjacent segments
 * @note Returns NULL with an error if the buffer is not aligned enough. A
 * buffer that comes from CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER is always
 * aligned correctly, while a buffer that you build by hand must be aligned to
 * at least _ccol_mempool_entry_align, which is 16 and is what the constructor
 * checks for
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
 * Frees every resource of the ranged memory pool, every sub-pool included. For
 * a pool that was created from a preallocated buffer, this macro does not free
 * the buffer itself.
 *
 * @param rmp Ranged memory pool to destroy. The macro sets it to NULL
 * afterward
 *
 * @warning This function asserts when the dynamic fallback holds pointers
 * that nobody freed, so that a possible leak does not go unnoticed. Such an
 * assert does not happen if you did not ask for a fallback when you created
 * this pool. As a rule, always free the allocated buffers.
 * @note It is safe to call this macro with a NULL pointer
 * @note For a preallocated pool, the caller owns the lifetime of the buffer
 * @note The macro evaluates rmp exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array.
 */
#define ccol_r_mempool_destroy(rmp) \
  _ccol_r_mempool_destroy_impl(     \
      rmp, _ccol_uniq(__ccol_r_mempool_destroy_slot, __COUNTER__))

/* Internal: the body of ccol_r_mempool_destroy. slot is a name from
 * _ccol_uniq(), so the macro nests inside the argument of another destroy
 * macro and stays -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_r_mempool_destroy_impl(rmp, slot) \
  do {                                          \
    __typeof__(rmp) *slot = &(rmp);             \
    _ccol_r_mempool_destroy(*slot);             \
    *slot = NULL;                               \
  } while (0)

/**
 * @brief Get number of used entries for a specific size
 *
 * Returns the number of entries currently in use from the pool that serves
 * the given size, not counting dynamic entries.
 *
 * @param rmp Ranged memory pool to query
 * @param size Size in bytes to query. It maps to one sub-pool
 *
 * @return Number of entries in use for this size, or 0 on an error
 *
 * @note Returns 0 if the size is invalid, that is, 0 or > largest_size
 * @note The function rounds the size up to the next power-of-2 pool
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note The pool tracks dynamic entries on their own
 *
 * @see ccol_r_mempool_total_capacity
 * @see ccol_r_mempool_dynamic_allocs_count
 */
size_t ccol_r_mempool_used_count(ccol_r_mempool *rmp, size_t size);

/**
 * @brief Get total capacity for a specific size
 *
 * Returns the total number of entries in the pool that serves the given size,
 * not counting dynamic entries.
 *
 * @param rmp Ranged memory pool to query
 * @param size Size in bytes to query. It maps to one sub-pool
 *
 * @return Total capacity for this size, or 0 on an error
 *
 * @note Returns 0 if the size is invalid, that is, 0 or > largest_size
 * @note The function rounds the size up to the next power-of-2 pool
 * @note Thread-safe if the pool was created with single_threaded=false
 *
 * @see ccol_r_mempool_used_count
 * @see ccol_r_mempool_dynamic_allocs_count
 */
size_t ccol_r_mempool_total_capacity(ccol_r_mempool *rmp, size_t size);

/**
 * @brief Get the memory the ranged pool holds for its entries
 *
 * Returns the size in bytes of the entry block of every tier, added together,
 * which is what a ranged pool costs. Each tier that receives a thread cache
 * holds a reserve above its own nominal count, so this total exceeds the sum
 * of the tier capacities.
 *
 * @param rmp Ranged memory pool to query
 *
 * @return Size in bytes of the entry block of every tier, added together
 *
 * @note The function counts the entry blocks of the tiers only, on the same
 * terms as ccol_mempool_allocated_bytes()
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note This function asserts if rmp is NULL
 *
 * @see ccol_mempool_allocated_bytes
 * @see ccol_r_mempool_total_capacity
 */
size_t ccol_r_mempool_allocated_bytes(ccol_r_mempool *rmp);

/**
 * @brief Get number of dynamic allocations for a specific size
 *
 * Returns the number of heap entries for the given size; the behavior depends
 * on the fallback policy.
 *
 * @param rmp Ranged memory pool to query
 * @param size Size in bytes to query
 *
 * @return Number of dynamic entries. The function returns 0 on an error, and 0
 * when the fallback is off
 *
 * @note Returns 0 for ccol_fallback_disabled
 * @note With ccol_fallback_at_first_exhaustion, the function tracks the
 * dynamic entries of each pool
 * @note With ccol_fallback_at_last_exhaustion, the function tracks every
 * dynamic entry
 * @note Returns 0 if the size is invalid, that is, 0 or > largest_size
 * @note Thread-safe if the pool was created with single_threaded=false
 *
 * @see ccol_r_mempool_used_count
 * @see ccol_r_mempool_total_capacity
 */
size_t ccol_r_mempool_dynamic_allocs_count(ccol_r_mempool *rmp, size_t size);

/**
 * @brief Allocate an entry from the ranged pool
 *
 * Allocates memory of the requested size from the best sub-pool: the function
 * rounds the size up to the nearest power-of-2 pool and, if that pool is
 * empty, tries larger pools. The fallback behavior depends on the policy.
 *
 * @param rmp Ranged memory pool to allocate from
 * @param size Size in bytes to allocate. It must be > 0 and <= largest_size
 *
 * @return Pointer to the allocated entry, or NULL on failure
 *
 * @note O(1) in the common case, O(number_of_pools) in the worst case
 * @note The memory that comes back is uninitialized; use
 * ccol_r_mempool_calloc_entry() for zeroed memory
 * @note The function rounds the size up to the next power-of-2 pool size
 * @note The function tries larger and larger pools when the best pool is empty
 * @note The fallback behavior depends on fb_policy
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note Returns NULL if the size is 0 or > largest_size
 * @note The caller must free the entry with ccol_r_mempool_free_entry()
 *
 * @see ccol_r_mempool_calloc_entry
 * @see ccol_r_mempool_realloc_entry
 * @see ccol_r_mempool_free_entry
 */
void *ccol_r_mempool_alloc_entry(ccol_r_mempool *rmp, size_t size);

/**
 * @brief Allocate a zero-initialized entry from the ranged pool
 *
 * This function is like ccol_r_mempool_alloc_entry(), but it zeros the
 * requested number of bytes before it returns.
 *
 * @param rmp Ranged memory pool to allocate from
 * @param size Size in bytes to allocate
 *
 * @return Pointer to the zeroed entry, or NULL on failure
 *
 * @note The function zeros exactly 'size' bytes, and not the full pool element
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note This function asserts if rmp is NULL
 *
 * @see ccol_r_mempool_alloc_entry
 * @see ccol_r_mempool_realloc_entry
 */
void *ccol_r_mempool_calloc_entry(ccol_r_mempool *rmp, size_t size);

/**
 * @brief Reallocate an entry to a different size
 *
 * Changes the size of an allocated entry. The original pointer comes back
 * unchanged, with no copy, whenever a move could not give the entry more bytes
 * than it already holds, which happens in two cases: when the entry already
 * holds exactly what the tier that serves the new size would give it, and
 * when it already holds exactly the number of bytes requested. In every other
 * case the function allocates a new entry sized for the request, copies
 * min(old_size, new_size) bytes into it, and frees the old one.
 *
 * Both of those questions are about the bytes that the entry holds, so a
 * pool-owned entry and a dynamic fallback entry that hold the same number of
 * bytes behave in the same way here. How many bytes an entry holds does
 * depend on where it came from, and that is the one difference which the two
 * representations truly force: a pool-owned entry holds the whole element
 * size of its tier, a dynamic fallback entry that a tier handed out after its
 * own slots ran out holds that same whole element size, and an entry from the
 * shared fallback that ccol_fallback_at_last_exhaustion uses holds exactly
 * the size that it was created for.
 *
 * When the function cannot make the move, it returns, unmoved, an entry that
 * the request fits inside, instead of reporting a failure. This too is the
 * same for both kinds of entry. A shrink does not fail for want of memory that
 * it does not need. The pointer that comes back then addresses at least the
 * requested bytes, as it does on every other successful path.
 *
 * @param rmp Ranged memory pool
 * @param addr Entry to reallocate, or NULL to allocate a new one
 * @param size New size in bytes. It must be > 0 and <= largest_size
 *
 * @return Pointer to the reallocated entry, or NULL on failure
 *
 * @note If addr is NULL, this function does the same as
 * ccol_r_mempool_alloc_entry()
 * @note If the entry already holds as many bytes as a move could give it, the
 * function returns the original pointer unchanged and copies nothing
 * @note In every other case the function allocates a new entry, copies the
 * data, and frees the old entry. If that allocation fails and the request
 * fits inside the bytes that addr already holds, the function returns addr
 * unmoved instead of NULL
 * @note The function copies min(old_user_size, new_user_size) bytes
 * @note The function frees the old entry when the reallocation succeeds
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note Returns NULL if the size is 0 or > largest_size, and does NOT touch
 * addr: for an invalid size, the function neither frees nor moves a non-NULL
 * addr, and the state is exactly as if the call had never happened. A real
 * failed reallocation (a valid size that the pool cannot serve) leaves addr
 * equally untouched, so both failure modes share one contract: the original
 * pointer remains valid, and the caller keeps owning it
 * @note This function asserts if addr is non-NULL and did not come from this
 * ccol_r_mempool, which detects corruption and a foreign pointer, matching
 * ccol_mempool_free_entry()
 *
 * @see ccol_r_mempool_alloc_entry
 * @see ccol_r_mempool_free_entry
 */
void *ccol_r_mempool_realloc_entry(ccol_r_mempool *rmp, void *addr,
                                   size_t size);

/**
 * @brief Return an entry to the ranged pool it came from (internal function)
 *
 * Prefer the ccol_r_mempool_free_entry() macro, which also sets the pointer
 * of the caller to NULL. The ranged pool finds the sub-pool that the entry
 * belongs to; the caller gives the ranged pool rather than that sub-pool,
 * because the caller has no way to know which sub-pool it is.
 *
 * @param rmp Ranged pool that the entry came from
 * @param entry Entry to free. A NULL entry does nothing, which matches free()
 *
 * @note An entry that did not come from this ranged pool is an error of the
 * caller, and it is fatal: the function does not tolerate it in silence
 *
 * @see ccol_r_mempool_free_entry
 * @see ccol_r_mempool_alloc_entry
 */
void _ccol_r_mempool_free_entry(ccol_r_mempool *rmp, void *entry);

/**
 * @brief Free an entry back to the ranged pool
 *
 * Returns an allocated entry to the free list of the correct sub-pool. This
 * macro calls _ccol_r_mempool_free_entry() and then sets the pointer to NULL.
 *
 * @param rmp Ranged pool that the entry came from
 * @param entry Entry to free. The macro sets it to NULL afterward
 *
 * @note The macro works for an entry from any sub-pool in the ranged pool
 * @note It also works for a dynamic entry from the fallback
 * @note It is safe to call this macro with NULL
 * @note Thread-safe if the pool was created with single_threaded=false
 * @note The macro does corruption detection with assertions
 *
 * @see ccol_mempool_free_entry
 * @see ccol_r_mempool_alloc_entry
 *
 * @note The macro evaluates each argument exactly once. entry must be a
 * modifiable lvalue, such as a variable or an element of an array.
 */
#define ccol_r_mempool_free_entry(rmp, entry) \
  _ccol_r_mempool_free_entry_impl(            \
      (rmp), entry, _ccol_uniq(__ccol_r_mempool_free_entry_slot, __COUNTER__))

/* Internal: the body of ccol_r_mempool_free_entry. slot is a name from
 * _ccol_uniq(), so the macro nests inside the argument of another destroy
 * macro and stays -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_r_mempool_free_entry_impl(rmp, entry, slot) \
  do {                                                    \
    __typeof__(entry) *slot = &(entry);                   \
    _ccol_r_mempool_free_entry((rmp), *slot);             \
    *slot = NULL;                                         \
  } while (0)

#pragma GCC visibility pop
