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

// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal.
#pragma GCC visibility push(default)

// Fixed-size and ranged memory pools. A cmempool hands out entries of one
// size from one block in O(1), with or without a lock, on the heap or on a
// buffer you give it, and can fall back to the heap when it runs out. A
// crmempool groups cmempools of power-of-two sizes and serves each request
// from the smallest one that fits. Corruption, double frees and foreign
// pointers are caught with assertions.

// The opaque handle of a fixed-size pool.
typedef struct cmempool cmempool;

// The alignment every entry meets, at least what malloc() gives, so an entry
// can hold any object. It's a fixed 16 and not _Alignof(max_align_t) because
// the buffer macros size the caller's array with it while the library derives
// its stride from it, and GCC and Clang disagree about max_align_t on i386.
#define cmempool__entry_align 16

// A target whose widest type needs more than 16 fails to compile instead of
// getting misaligned entries.
_Static_assert(cmempool__entry_align >= _Alignof(max_align_t),
               "cmempool__entry_align must be at least the alignment this "
               "target requires for any object type");

// Internal: evaluates to true when cmempool__align_up(x) doesn't overflow
// size_t. The constructors and the buffer macro check it before they round a
// caller's size.
#define cmempool__align_up_fits(x) \
  ((x) <= SIZE_MAX - (cmempool__entry_align - 1))

// Internal: evaluates to x rounded up to a multiple of cmempool__entry_align.
// The stride is rounded this way so every entry, not only the first, stays
// aligned; an unaligned stride drifts one stride at a time, which faults on
// strict-alignment targets. Check cmempool__align_up_fits(x) first.
#define cmempool__align_up(x) \
  (((x) + (cmempool__entry_align - 1)) & ~(size_t)(cmempool__entry_align - 1))

// Creates a pool of elem_count entries of elem_size bytes (both above 0), the
// heap-backed constructor. An elem_size below sizeof(uintptr_t) is rounded up,
// and the stride is that size rounded to cmempool__entry_align and, unless
// CMEMPOOL_COMPACT_LAYOUT is 1, to a power of two. One status byte per entry
// follows the entries in the same block. With fallback_to_dynamic_memory, an
// empty pool allocates from the heap and fails only when the system is out of
// memory. single_threaded drops the lock. mmgmt_procs is your allocator, or
// NULL for malloc and free; the pool calls it with its own lock held, so it
// must not call back into the same pool. Gives the pool, or NULL (bad sizes,
// overflow, no memory) and an error string in *err when err isn't NULL.
// Destroy it with cmempool_destroy().
cmempool *cmempool_create_mp(size_t elem_count, size_t elem_size,
                             bool fallback_to_dynamic_memory,
                             bool single_threaded,
                             ccol_memmgmt_procs *mmgmt_procs, char **err);

// Creates a pool with the default allocator; it evaluates to
// cmempool_create_mp() with NULL procs.
#define cmempool_create(elem_count, elem_size, fallback_to_dynamic_memory,    \
                        single_threaded, err)                                 \
  cmempool_create_mp((elem_count), (elem_size), (fallback_to_dynamic_memory), \
                     (single_threaded), NULL, (err))

// How far the stride is rounded. At 0 (the default) it goes up to a power of
// two, so a shift turns an address into an index. At 1 it stays at the aligned
// element size and a precomputed reciprocal does that job; that costs a little
// on every call but can nearly halve the memory of sizes just above a power of
// two. It's a build-wide switch because a per-pool choice slows every call.
// The buffer macros size your array with it, so build the library and every
// file that includes this header with the same value.
#ifndef CMEMPOOL_COMPACT_LAYOUT
#define CMEMPOOL_COMPACT_LAYOUT 0
#endif

// Enforces that rule at link time. The library defines only the tag object of
// its own layout, and CMEMPOOL_DECLARE_PREALLOCATED_BUFFER takes the address
// of the one its own setting names, so a mismatch is an undefined reference
// that names the expected layout. Only that macro is at risk, so only it
// carries the reference.
#if CMEMPOOL_COMPACT_LAYOUT
extern const char cmempool__built_with_compact_layout;
#define cmempool__layout_tag (&cmempool__built_with_compact_layout)
#else
extern const char cmempool__built_with_fast_layout;
#define cmempool__layout_tag (&cmempool__built_with_fast_layout)
#endif

// Internal: evaluate to the smallest power of two that is >= x, as a constant
// expression, because the buffer macros size a static array with them. The
// chain starts at the entry alignment, matching the runtime stride, and gives
// 0 above 2^40 (or above what size_t holds), which
// cmempool__buffer_params_fit() rejects. The entries above 2^31 exist only for
// a 64-bit size_t, because Clang refuses their comparisons on a 32-bit one.
#if SIZE_MAX > 0xFFFFFFFFu
#define ccol__mp_pow2_ceil_wide(x)                 \
  ((x) <= 4294967296u      ? (size_t)4294967296    \
   : (x) <= 8589934592u    ? (size_t)8589934592    \
   : (x) <= 17179869184u   ? (size_t)17179869184   \
   : (x) <= 34359738368u   ? (size_t)34359738368   \
   : (x) <= 68719476736u   ? (size_t)68719476736   \
   : (x) <= 137438953472u  ? (size_t)137438953472  \
   : (x) <= 274877906944u  ? (size_t)274877906944  \
   : (x) <= 549755813888u  ? (size_t)549755813888  \
   : (x) <= 1099511627776u ? (size_t)1099511627776 \
                           : (size_t)0)
#else
#define ccol__mp_pow2_ceil_wide(x) (size_t)0
#endif

#define ccol__mp_pow2_ceil(x)                                   \
  ((x) <= cmempool__entry_align ? (size_t)cmempool__entry_align \
   : (x) <= 16u                 ? (size_t)16                    \
   : (x) <= 32u                 ? (size_t)32                    \
   : (x) <= 64u                 ? (size_t)64                    \
   : (x) <= 128u                ? (size_t)128                   \
   : (x) <= 256u                ? (size_t)256                   \
   : (x) <= 512u                ? (size_t)512                   \
   : (x) <= 1024u               ? (size_t)1024                  \
   : (x) <= 2048u               ? (size_t)2048                  \
   : (x) <= 4096u               ? (size_t)4096                  \
   : (x) <= 8192u               ? (size_t)8192                  \
   : (x) <= 16384u              ? (size_t)16384                 \
   : (x) <= 32768u              ? (size_t)32768                 \
   : (x) <= 65536u              ? (size_t)65536                 \
   : (x) <= 131072u             ? (size_t)131072                \
   : (x) <= 262144u             ? (size_t)262144                \
   : (x) <= 524288u             ? (size_t)524288                \
   : (x) <= 1048576u            ? (size_t)1048576               \
   : (x) <= 2097152u            ? (size_t)2097152               \
   : (x) <= 4194304u            ? (size_t)4194304               \
   : (x) <= 8388608u            ? (size_t)8388608               \
   : (x) <= 16777216u           ? (size_t)16777216              \
   : (x) <= 33554432u           ? (size_t)33554432              \
   : (x) <= 67108864u           ? (size_t)67108864              \
   : (x) <= 134217728u          ? (size_t)134217728             \
   : (x) <= 268435456u          ? (size_t)268435456             \
   : (x) <= 536870912u          ? (size_t)536870912             \
   : (x) <= 1073741824u         ? (size_t)1073741824            \
   : (x) <= 2147483648u         ? (size_t)2147483648            \
                                : ccol__mp_pow2_ceil_wide(x))

// Internal: evaluates to the distance between two entries for elem_size, the
// constant-expression twin of the runtime stride, so a buffer macro and the
// pool built on it agree. Entries have no header, so it's elem_size raised to
// hold the free-list link, aligned, and in the default layout rounded up to a
// power of two.
#if CMEMPOOL_COMPACT_LAYOUT
#define cmempool__stride(elem_size) \
  cmempool__align_up(ccol__max((elem_size), sizeof(uintptr_t)))
#else
#define cmempool__stride(elem_size) \
  ccol__mp_pow2_ceil(               \
      cmempool__align_up(ccol__max((elem_size), sizeof(uintptr_t))))
#endif

// Internal: evaluates to the bytes a pool of elem_count elements needs: the
// entries, then one status byte per entry, all in one block. The buffer macro
// sizes its array with it.
#define cmempool__buffer_bytes(elem_count, elem_size) \
  ((elem_count) * cmempool__stride(elem_size) + (elem_count))

// Internal: evaluates to true when elem_count is above 0 and the buffer size
// fits in a size_t, so the buffer macro can refuse bad parameters at compile
// time.
#define cmempool__buffer_params_fit(elem_count, elem_size)               \
  ((elem_count) > 0 &&                                                   \
   cmempool__align_up_fits(ccol__max((elem_size), sizeof(uintptr_t))) && \
   cmempool__stride(elem_size) > 0 &&                                    \
   cmempool__stride(elem_size) <= (SIZE_MAX - (elem_count)) / (elem_count))

// Declares name as a uint8_t array, aligned to cmempool__entry_align, of the
// exact size a pool of elem_count elements of elem_size bytes needs (status
// bytes included), for code that builds a pool without a heap. It rounds with
// the same arithmetic as cmempool_create_from_preallocated_buffer_mp() and
// carries the layout check. A zero elem_count or a size that overflows size_t
// fails a _Static_assert. Example:
//   CMEMPOOL_DECLARE_PREALLOCATED_BUFFER(my_buffer, 100, 64);
//   cmempool *mp = cmempool_create_from_preallocated_buffer(
//       my_buffer, sizeof(my_buffer), 64, false, false, NULL);
#define CMEMPOOL_DECLARE_PREALLOCATED_BUFFER(name, elem_count, elem_size)  \
  _Static_assert(                                                          \
      cmempool__buffer_params_fit((elem_count), (elem_size)),              \
      "CMEMPOOL_DECLARE_PREALLOCATED_BUFFER: elem_count must be nonzero, " \
      "and elem_count * elem_size (rounded up for the minimum element "    \
      "size and for the alignment every entry is guaranteed to meet) "     \
      "must not overflow size_t");                                         \
  _Alignas(cmempool__entry_align)                                          \
      uint8_t name[cmempool__buffer_bytes((elem_count), (elem_size))];     \
  static const char *const name##__cmempool_layout_ref                     \
      __attribute__((used, unused)) = cmempool__layout_tag

// Creates a pool on your buffer instead of the heap, for embedded code or a
// pool on the stack. The element count is buf_size / (stride + 1), with the
// stride of cmempool_create_mp(); declare the buffer with
// CMEMPOOL_DECLARE_PREALLOCATED_BUFFER to get the size right. The buffer must
// be aligned to cmempool__entry_align (16) and stays yours: cmempool_destroy()
// doesn't free it. The pool struct and the fallback entries come from
// mmgmt_procs, which must not call back into this pool. Gives the pool, or
// NULL with *err set for a zero or overflowing elem_size, a misaligned buffer
// or a failed allocation.
cmempool *cmempool_create_from_preallocated_buffer_mp(
    void *buffer, size_t buf_size, size_t elem_size,
    bool fallback_to_dynamic_memory, bool single_threaded,
    ccol_memmgmt_procs *mmgmt_procs, char **err);

// Creates a pool on your buffer with the default allocator; it evaluates to
// cmempool_create_from_preallocated_buffer_mp() with NULL procs.
#define cmempool_create_from_preallocated_buffer(buffer, buf_size, elem_size, \
                                                 fallback_to_dynamic_memory,  \
                                                 single_threaded, err)        \
  cmempool_create_from_preallocated_buffer_mp(                                \
      (buffer), (buf_size), (elem_size), (fallback_to_dynamic_memory),        \
      (single_threaded), NULL, (err))

// Internal: destroys mp, the body of cmempool_destroy(); use the macro.
// A pool shared by threads keeps a small cache per thread, and destroying the
// pool only marks those caches dead, because no other thread may free them. A
// cache is freed when its thread next looks up a magazine for a pool it
// doesn't cache, when it exits, or at exit or dlclose() if it runs the unload
// handler. So your allocator's free function can run after this returns, even
// after main(), and an allocator torn down at a known point must outlive it. A
// single_threaded pool has no such tail.
void cmempool__destroy(cmempool *mp);

// Destroys the pool and sets mp to NULL, so the caller can't use it again. mp
// is evaluated once and must be a modifiable lvalue; NULL is fine. A
// preallocated buffer isn't freed. It asserts when fallback entries are still
// allocated, so free every entry first.
#define cmempool_destroy(mp) \
  cmempool__destroy_impl(mp, ccol__uniq(cmempool__destroy_slot, __COUNTER__))

// Internal: the body of cmempool_destroy(). slot comes from ccol__uniq(), so
// destroy macros can nest without -Wshadow warnings.
#define cmempool__destroy_impl(mp, slot) \
  do {                                   \
    __typeof__(mp) *slot = &(mp);        \
    cmempool__destroy(*slot);            \
    *slot = NULL;                        \
  } while (0)

// Allocates an uninitialized entry from mp in O(1), or a heap entry when the
// pool is empty and the fallback is on. Gives NULL when the pool is empty
// without a fallback (or the heap is out of memory with one). Asserts on a
// NULL mp. Free it with cmempool_free_entry().
void *cmempool_alloc_entry(cmempool *mp);

// Allocates an entry with the whole element zeroed, for callers that need
// zeroed memory; otherwise as cmempool_alloc_entry().
void *cmempool_calloc_entry(cmempool *mp);

// Internal: frees entry to mp, the body of cmempool_free_entry(); use the
// macro.
void cmempool__free_entry(cmempool *mp, void *entry);

// Gives entry back to the pool in O(1) and sets entry to NULL, so a dangling
// copy isn't left behind; NULL does nothing. Each argument is evaluated once
// and entry must be a modifiable lvalue. Ownership is decided from the address
// alone before anything is read through it: a pool entry's state is one byte
// in the status array, and every live fallback entry is recorded. A double
// free or an address this pool doesn't own asserts; two threads freeing the
// same entry at the same instant can slip past, which is a data race in the
// caller anyway. A fallback entry goes back to the heap.
#define cmempool_free_entry(mp, entry) \
  cmempool__free_entry_impl(           \
      (mp), entry, ccol__uniq(cmempool__free_entry_slot, __COUNTER__))

// Internal: the body of cmempool_free_entry(). slot comes from ccol__uniq(),
// so the macros can nest without -Wshadow warnings.
#define cmempool__free_entry_impl(mp, entry, slot) \
  do {                                             \
    __typeof__(entry) *slot = &(entry);            \
    cmempool__free_entry((mp), *slot);             \
    *slot = NULL;                                  \
  } while (0)

// Gives the number of entries mp was created with, so you can check how close
// it is to running out; fallback entries aren't counted. A pool with
// per-thread caches holds a reserve above this count, so any thread can get
// this many however much others cache, and concurrent callers may briefly
// hold a few more (never more than the reserve). A single_threaded or
// preallocated pool is exact. Asserts on a NULL mp.
size_t cmempool_total_capacity(cmempool *mp);

// Gives the size in bytes of mp's entry block (entries and status bytes,
// reserve included), which is what to measure against a memory budget. The
// handle, the thread caches and fallback entries aren't counted. For a
// preallocated pool it's the part of your buffer the pool uses. Asserts on a
// NULL mp.
size_t cmempool_allocated_bytes(cmempool *mp);

// Gives the pool entries handed out right now, for monitoring and leak
// checks. Entries in a thread cache and fallback entries aren't counted.
// Concurrent allocation on a cached pool can push it a few above
// cmempool_total_capacity(), never above the slots that
// cmempool_allocated_bytes() covers. Asserts on a NULL mp.
size_t cmempool_used_count(cmempool *mp);

// Gives the live fallback (heap) entries, which tells you the pool ran out
// at some point; always 0 without the fallback. Asserts on a NULL mp.
size_t cmempool_dynamic_allocs_count(cmempool *mp);

// The opaque handle of a ranged pool.
typedef struct crmempool crmempool;

// When a ranged pool falls back to the heap: never, as soon as the tier for a
// request runs out (each tier has its own fallback), or only after every tier
// that could serve the request is empty.
typedef enum crmempool_fallback_policy {
  crmempool_fallback_disabled = 0,
  crmempool_fallback_at_first_exhaustion,
  crmempool_fallback_at_last_exhaustion,
  // Internal sentinel.
  crmempool__fallback_end_place_holder
} crmempool_fallback_policy;

// Creates a ranged pool, which serves requests of many sizes from tiers of
// 2^smallest to 2^largest bytes. Each tier doubles the element size and halves
// the count, starting from 2^number_of_smallest_size_elems_power_of_two
// elements; for example 4, 6, 8 gives 256 x 16, 128 x 32 and 64 x 64 bytes.
// smallest must be at least 4, largest must be above smallest, and the count
// power at least largest - smallest. fb_policy says when to fall back to the
// heap, single_threaded drops the locks. mmgmt_procs is called with a tier's
// lock held, so it must not call back into this pool. Gives the pool, or NULL
// with *err set. Destroy it with crmempool_destroy().
crmempool *crmempool_create_mp(
    uint8_t smallest_size_power_of_two, uint8_t largest_size_power_of_two,
    uint8_t number_of_smallest_size_elems_power_of_two,
    crmempool_fallback_policy fb_policy, bool single_threaded,
    ccol_memmgmt_procs *mmgmt_procs, char **err);

// Creates a ranged pool with the default allocator; it evaluates to
// crmempool_create_mp() with NULL procs.
#define crmempool_create(smallest_size_power_of_two,                 \
                         largest_size_power_of_two,                  \
                         number_of_smallest_size_elems_power_of_two, \
                         fb_policy, single_threaded, err)            \
  crmempool_create_mp((smallest_size_power_of_two),                  \
                      (largest_size_power_of_two),                   \
                      (number_of_smallest_size_elems_power_of_two),  \
                      (fb_policy), (single_threaded), NULL, (err))

// Internal: the arithmetic of CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER.
// Tier i holds 2^(SC - i) entries of 2^(SS + i) bytes, and every element size
// is a power of two of 16 or more, so the stride is the element size. The
// entries add up to N * 2^(SS + SC) and the status bytes to
// (2^SC >> (N - 1)) * (2^N - 1), reduced by a shift first so the product
// doesn't overflow. The status region of a tier with fewer than 16 elements is
// padded so the next tier stays aligned.
//
// Evaluates to the tier span LS - SS, or 0 for an inverted pair so no shift
// count goes negative.
#define crmempool__tier_span(SS, LS) ((LS) >= (SS) ? (LS) - (SS) : 0)

// Evaluates to the shift count x clamped below the width of size_t. A compiler
// diagnoses an out-of-range shift even on a branch that is never evaluated, so
// without the clamp a bad triple gives a page of -Werror shift errors next to
// the _Static_assert message. It's a clamp and not a modulo, because a modulo
// turns a count equal to the width into 0, and GCC then flags the comparison
// around it under -Wtype-limits. It changes no value for an accepted triple.
#define crmempool__shift_count(x)                    \
  ((size_t)(x) < (size_t)(sizeof(size_t) * CHAR_BIT) \
       ? (size_t)(x)                                 \
       : (size_t)(sizeof(size_t) * CHAR_BIT - 1))

// Evaluates to the padding one tier's status region gets when C is one of the
// tier element counts and is below the alignment, else 0.
#define crmempool__status_pad_term(SS, LS, SC, C)                              \
  (((SC) >= crmempool__tier_span((SS), (LS)) &&                                \
    ((size_t)1 << crmempool__shift_count(                                      \
         (size_t)((SC) - crmempool__tier_span((SS), (LS))))) <= (size_t)(C) && \
    (size_t)(C) <= ((size_t)1 << crmempool__shift_count((size_t)(SC))))        \
       ? (cmempool__align_up((size_t)(C)) - (size_t)(C))                       \
       : (size_t)0)

// Evaluates to the padding of every tier, which the buffer size adds. Only
// counts below the alignment are padded, so the list is complete while the
// alignment stays at or below 128.
#define crmempool__status_padding(SS, LS, SC)         \
  (crmempool__status_pad_term((SS), (LS), (SC), 1) +  \
   crmempool__status_pad_term((SS), (LS), (SC), 2) +  \
   crmempool__status_pad_term((SS), (LS), (SC), 4) +  \
   crmempool__status_pad_term((SS), (LS), (SC), 8) +  \
   crmempool__status_pad_term((SS), (LS), (SC), 16) + \
   crmempool__status_pad_term((SS), (LS), (SC), 32) + \
   crmempool__status_pad_term((SS), (LS), (SC), 64))

_Static_assert(cmempool__entry_align <= 128,
               "crmempool__status_padding enumerates every element count "
               "that can be rounded up, which is complete only while the "
               "alignment stays at or below 128");

// Internal: evaluates to the buffer size of a ranged pool with these
// parameters, which CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER applies for you;
// declare with that macro and use sizeof(). Its shifts are guarded against
// negative counts so a bad triple shows only the assertion of that macro.
#define CRMEMPOOL__CALCULATE_PREALLOCATED_BUFFER_SIZE(SS, LS, SC)         \
  ((size_t)(crmempool__tier_span((SS), (LS)) + 1) * ((size_t)1 << (SC)) * \
       ((size_t)1 << (SS)) +                                              \
   ((((size_t)1 << (SC)) >> crmempool__tier_span((SS), (LS))) *           \
    (((size_t)1 << (crmempool__tier_span((SS), (LS)) + 1)) - 1)) +        \
   crmempool__status_padding((SS), (LS), (SC)))

// The smallest element size a ranged pool serves, as a power of two. The
// constructor refuses smaller ones, so the buffer macro does too.
#define crmempool__min_smallest_size_power 4

// Internal: evaluates to true when the triple gives a well-defined buffer size
// that fits in a size_t, so the buffer macro can refuse it at compile time.
// The clauses are ordered so each one is evaluated only after the earlier
// ones made it well-defined: the exponents fit, LS > SS, the span is small
// enough, SC >= LS - SS (which makes the size arithmetic exact), each term
// fits, and so does their sum.
#define crmempool__buffer_params_fit(SS, LS, SC)                            \
  ((SS) >= crmempool__min_smallest_size_power &&                            \
   (size_t)(SS) < (sizeof(size_t) * CHAR_BIT) &&                            \
   (size_t)(LS) < (sizeof(size_t) * CHAR_BIT) &&                            \
   (size_t)(SC) < (sizeof(size_t) * CHAR_BIT) && (LS) > (SS) &&             \
   ((size_t)(LS) - (size_t)(SS)) < (sizeof(size_t) * CHAR_BIT) - 1 &&       \
   (size_t)(SC) >= ((size_t)(LS) - (size_t)(SS)) &&                         \
   ((size_t)1 << crmempool__shift_count((size_t)(SC))) <=                   \
       SIZE_MAX / ((size_t)(LS) - (size_t)(SS) + 1) &&                      \
   ((size_t)1 << crmempool__shift_count((size_t)(SS))) <=                   \
       SIZE_MAX / (((size_t)(LS) - (size_t)(SS) + 1) *                      \
                   ((size_t)1 << crmempool__shift_count((size_t)(SC)))) &&  \
   (((size_t)1 << crmempool__shift_count(crmempool__tier_span((SS), (LS)) + \
                                         1)) -                              \
    1) <= SIZE_MAX /                                                        \
              (((size_t)1 << crmempool__shift_count((size_t)(SC))) >>       \
               crmempool__shift_count(crmempool__tier_span((SS), (LS)))) && \
   (((size_t)(LS) - (size_t)(SS) + 1) *                                     \
    ((size_t)1 << crmempool__shift_count((size_t)(SC))) *                   \
    ((size_t)1 << crmempool__shift_count((size_t)(SS)))) <=                 \
       SIZE_MAX -                                                           \
           ((((size_t)1 << crmempool__shift_count((size_t)(SC))) >>         \
             crmempool__shift_count((size_t)(LS) - (size_t)(SS))) *         \
            (((size_t)1 << crmempool__shift_count((size_t)(LS) -            \
                                                  (size_t)(SS) + 1)) -      \
             1)) -                                                          \
           crmempool__status_padding((SS), (LS), (SC)))

// Declares name as a uint8_t array, aligned to cmempool__entry_align, of the
// exact size a ranged pool with these parameters needs, every tier's status
// bytes included, for code that builds a ranged pool without a heap. Each rule
// a caller can break has its own _Static_assert, so the message says which one
// failed: smallest below 4, largest not above smallest, count power below
// largest - smallest, or a size that overflows. Example:
//   CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER(my_buffer, 4, 8, 10);
//   crmempool *rmp = crmempool_create_from_preallocated_buffer(
//       my_buffer, sizeof(my_buffer), 4, 8, 10, crmempool_fallback_disabled,
//       false, NULL);
#define CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER(                                 \
    name, smallest_size_power_of_two, largest_size_power_of_two,               \
    number_of_smallest_size_elems_power_of_two)                                \
  _Alignas(cmempool__entry_align)                                              \
      uint8_t name[CRMEMPOOL__CALCULATE_PREALLOCATED_BUFFER_SIZE(              \
          (smallest_size_power_of_two), (largest_size_power_of_two),           \
          (number_of_smallest_size_elems_power_of_two))];                      \
  _Static_assert(                                                              \
      (smallest_size_power_of_two) >= crmempool__min_smallest_size_power,      \
      "CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER: "                                \
      "smallest_size_power_of_two must be at least 4. The smallest element "   \
      "a ranged pool serves is 16 bytes");                                     \
  _Static_assert(                                                              \
      (largest_size_power_of_two) > (smallest_size_power_of_two),              \
      "CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER: "                                \
      "largest_size_power_of_two must be strictly greater than "               \
      "smallest_size_power_of_two. A ranged pool spans at least two size "     \
      "tiers; use CMEMPOOL_DECLARE_PREALLOCATED_BUFFER for one size");         \
  _Static_assert(                                                              \
      (size_t)(number_of_smallest_size_elems_power_of_two) >=                  \
          (size_t)((largest_size_power_of_two) -                               \
                   (smallest_size_power_of_two)),                              \
      "CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER: "                                \
      "number_of_smallest_size_elems_power_of_two must be at least "           \
      "largest_size_power_of_two - smallest_size_power_of_two, so that every " \
      "tier ends up with at least one element");                               \
  _Static_assert(                                                              \
      crmempool__buffer_params_fit(                                            \
          (smallest_size_power_of_two), (largest_size_power_of_two),           \
          (number_of_smallest_size_elems_power_of_two)),                       \
      "CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER: these parameters would "         \
      "make the pool's own required buffer size overflow size_t, or name a "   \
      "power of two that a size_t on this target cannot hold")

// Creates a ranged pool on your buffer, one segment per tier, for code that
// avoids the heap. buf_size must match the size
// CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER computes exactly, and the buffer must
// be aligned to cmempool__entry_align (16). The buffer stays yours; the pool
// structs come from mmgmt_procs, which must not call back into this pool.
// Gives the pool, or NULL with *err set; the other rules are those of
// crmempool_create_mp().
crmempool *crmempool_create_from_preallocated_buffer_mp(
    void *buffer, size_t buf_size, uint8_t smallest_size_power_of_two,
    uint8_t largest_size_power_of_two,
    uint8_t number_of_smallest_size_elems_power_of_two,
    crmempool_fallback_policy fb_policy, bool single_threaded,
    ccol_memmgmt_procs *mmgmt_procs, char **err);

// Creates a ranged pool on your buffer with the default allocator; it
// evaluates to crmempool_create_from_preallocated_buffer_mp() with NULL
// procs.
#define crmempool_create_from_preallocated_buffer(                           \
    buffer, buf_size, smallest_size_power_of_two, largest_size_power_of_two, \
    number_of_smallest_size_elems_power_of_two, fb_policy, single_threaded,  \
    err)                                                                     \
  crmempool_create_from_preallocated_buffer_mp(                              \
      (buffer), (buf_size), (smallest_size_power_of_two),                    \
      (largest_size_power_of_two),                                           \
      (number_of_smallest_size_elems_power_of_two), (fb_policy),             \
      (single_threaded), NULL, (err))

// Internal: destroys rmp, the body of crmempool_destroy(); use the macro.
void crmempool__destroy(crmempool *rmp);

// Destroys the ranged pool with all its tiers and sets rmp to NULL, so the
// caller can't use it again (evaluated once, a modifiable lvalue; NULL is
// fine). A preallocated buffer isn't freed. It asserts when fallback entries
// are still allocated.
#define crmempool_destroy(rmp) \
  crmempool__destroy_impl(rmp, ccol__uniq(crmempool__destroy_slot, __COUNTER__))

// Internal: the body of crmempool_destroy(). slot comes from ccol__uniq(), so
// destroy macros can nest without -Wshadow warnings.
#define crmempool__destroy_impl(rmp, slot) \
  do {                                     \
    __typeof__(rmp) *slot = &(rmp);        \
    crmempool__destroy(*slot);             \
    *slot = NULL;                          \
  } while (0)

// Gives the entries in use in the tier that serves size (rounded up to a power
// of two), for monitoring; fallback entries aren't counted. Gives 0 for a size
// of 0 or above the largest. Asserts on a NULL rmp.
size_t crmempool_used_count(crmempool *rmp, size_t size);

// Gives the capacity of the tier that serves size, for monitoring; 0 for an
// invalid size. Asserts on a NULL rmp.
size_t crmempool_total_capacity(crmempool *rmp, size_t size);

// Gives the bytes of every tier's entry block added together, what the ranged
// pool costs, on the terms of cmempool_allocated_bytes(). Asserts on a NULL
// rmp.
size_t crmempool_allocated_bytes(crmempool *rmp);

// Gives the live fallback entries for size, which tells you the tiers ran
// out: per tier under crmempool_fallback_at_first_exhaustion, all of them
// under crmempool_fallback_at_last_exhaustion, 0 when the fallback is off or
// the size is invalid. Asserts on a NULL rmp.
size_t crmempool_dynamic_allocs_count(crmempool *rmp, size_t size);

// Allocates an uninitialized entry of at least size bytes from the smallest
// tier that fits, trying larger tiers when it's empty and then the fallback
// the policy allows. Gives NULL for a size of 0 or above the largest, or when
// nothing can serve it. Asserts on a NULL rmp. Free it with
// crmempool_free_entry().
void *crmempool_alloc_entry(crmempool *rmp, size_t size);

// Allocates an entry with exactly size bytes zeroed, for callers that need
// zeroed memory; otherwise as crmempool_alloc_entry().
void *crmempool_calloc_entry(crmempool *rmp, size_t size);

// Resizes an entry, like realloc(3) for a ranged pool. A NULL addr allocates.
// When addr already holds exactly what the tier for size would give, or
// exactly size bytes, it comes back as it is. Otherwise a new entry gets
// min(old, new) bytes and the old one is freed; if that allocation fails but
// size fits in what addr holds, addr comes back unmoved. A pool entry and a
// tier fallback entry hold the tier's whole element size, and an entry of the
// shared fallback holds exactly its size. Gives the entry, or NULL on failure
// (including a size of 0 or above the largest), leaving addr untouched and
// yours. It asserts when addr isn't from this pool and on a NULL rmp.
void *crmempool_realloc_entry(crmempool *rmp, void *addr, size_t size);

// Internal: frees entry to rmp, the body of crmempool_free_entry(). It finds
// the tier the entry belongs to; NULL does nothing, and an entry from
// elsewhere is fatal.
void crmempool__free_entry(crmempool *rmp, void *entry);

// Gives entry back to its tier, or to the heap for a fallback entry, and sets
// entry to NULL so a dangling copy isn't left behind; NULL does nothing. Each
// argument is evaluated once, entry must be a modifiable lvalue, and
// corruption asserts.
#define crmempool_free_entry(rmp, entry) \
  crmempool__free_entry_impl(            \
      (rmp), entry, ccol__uniq(crmempool__free_entry_slot, __COUNTER__))

// Internal: the body of crmempool_free_entry(). slot comes from
// ccol__uniq(), so the macros can nest without -Wshadow warnings.
#define crmempool__free_entry_impl(rmp, entry, slot) \
  do {                                               \
    __typeof__(entry) *slot = &(entry);              \
    crmempool__free_entry((rmp), *slot);             \
    *slot = NULL;                                    \
  } while (0)

#pragma GCC visibility pop
