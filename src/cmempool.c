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
#include <internal/ctlsmodel.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* This is the other half of the link-time layout check in cmempool.h. Exactly
   one of these exists in a given build. A translation unit compiled with the
   other setting names an object that is not here. */
/* The tag is a scalar and not a one-element array. Nothing reads its value;
 * only its NAME carries meaning, and an array conveys nothing here that a
 * scalar does not. An array does carry one thing that this object should not:
 * a DWARF subrange, whose index type GCC spells `unsigned int` and Clang
 * spells `__ARRAY_SIZE_TYPE__`. The ABI corpus records that spelling, so the
 * array form made the recorded corpus differ between two compilers for an
 * object whose size, alignment and linkage are identical in both. */
#if CCOL_MEMPOOL_COMPACT_LAYOUT
const char _ccol_mempool_built_with_compact_layout = 0;
#else
const char _ccol_mempool_built_with_fast_layout = 0;
#endif

const char *_ccol_mempool_mark = "ccol_mempool";

typedef uintptr_t *addr_t;

struct ccol_mp_magazine;

/* A pool-owned entry carries no header at all. The address that the caller
 * receives is the entry itself. Its index is
 * (entry - lower_addr_limit) >> entry_shift. Its state stays in the status
 * array that begins at upper_addr_limit. The first bytes of a free entry hold
 * the free-list link. This is why the minimum element size is
 * sizeof(uintptr_t).
 *
 * The user-visible size of a pool entry is its whole stride. The rounding
 * belongs to the pool, but the bytes belong to whoever holds the entry. */
#if CCOL_MEMPOOL_COMPACT_LAYOUT
#define MP_HAS_CACHE(mp) ((mp)->has_cache)
/* The widest product that the reciprocal needs. The offsets of a 32-bit target
   fit in 32 bits, so a 64-bit intermediate keeps the high half. The offsets of
   a 64-bit target do not fit, and __uint128_t is the only way to hold the
   product. */
#if UINTPTR_MAX > 0xFFFFFFFFu
typedef __uint128_t mp_wide_t;
#define MP_WIDE_BITS 64
#else
typedef uint64_t mp_wide_t;
#define MP_WIDE_BITS 32
#endif
#define ENTRY_INDEX(mp, c_entry)                                          \
  (size_t)((uintptr_t)(((mp_wide_t)((c_entry) - (mp)->lower_addr_limit) * \
                        (mp)->entry_index_magic) >>                       \
                       MP_WIDE_BITS) >>                                   \
           (mp)->entry_index_shift)
#else
#define MP_HAS_CACHE(mp) ((mp)->max_magazines != 0)
#define ENTRY_INDEX(mp, c_entry) \
  (size_t)(((c_entry) - (mp)->lower_addr_limit) >> (mp)->entry_shift)
#endif

#define ENTRY_STATUS(mp, idx) (((uint8_t *)(mp)->upper_addr_limit)[(idx)])

/* A dynamic fallback entry is not in the buffer of any pool. It therefore
 * cannot be indexed and cannot use the status array. It keeps a header of its
 * own, immediately before the address that goes to the caller. That header
 * records two things. The first is the state that a pool entry keeps in the
 * status array. The second is the user-visible size that a pool entry recovers
 * from the fixed stride of its pool.
 *
 * The code reads this header only for an address that the registry of live
 * dynamic entries of the pool holds (see mp_dynamic_set_insert_locked). Any
 * other address is not one of those entries, whatever lies in front of it, and
 * a read in front of it can land outside every allocation. */
typedef struct {
  size_t user_size;
  size_t elem_status;
} mp_dynamic_header;

/* This value is rounded to the alignment that every entry is sure to meet. It
 * is not left at sizeof(mp_dynamic_header). The address that goes to the
 * caller sits this many bytes past the block that the allocator returned. A
 * prefix that is not itself a multiple of the alignment therefore carries the
 * misalignment straight through to the caller. This is true no matter how well
 * the underlying block is aligned.
 *
 * This assumes that the allocator behind m_procs returns memory that is
 * aligned for any object type. That is what malloc() guarantees, and what a
 * replacement should match. The guarantee for a fallback entry can be no
 * stronger than what that allocator gives. */
#define DYNAMIC_ENTRY_PREFIX_SIZE \
  _ccol_mempool_align_up(sizeof(mp_dynamic_header))

#define DYNAMIC_ENTRY_HEADER(entry) \
  ((mp_dynamic_header *)((uint8_t *)(entry) - DYNAMIC_ENTRY_PREFIX_SIZE))

#define DYNAMIC_ENTRY_RAW_BLOCK(entry) \
  ((void *)((uint8_t *)(entry) - DYNAMIC_ENTRY_PREFIX_SIZE))

/* This function is defined below, next to the free path that it mainly serves.
 * It is declared here because the magazine and the size helper above it both
 * ask the same question. */
static inline bool valid_mempool_addr(ccol_mempool *mp, uintptr_t c_entry);

/* This function is defined next to ccol_mempool_alloc_entry. The
 * thread-cache path above that definition serves requests through it. */
static inline __attribute__((always_inline)) void *mempool_serve_locked(
    ccol_mempool *mp);

#define DYNAMIC_ENTRY_USER_SIZE(entry) (DYNAMIC_ENTRY_HEADER(entry)->user_size)

struct ccol_mempool {
  /* ---- one cache line, touched by every allocation and free -------------
   * This group is exactly 64 bytes on LP64. The grouping is measured and not
   * guessed. free_inst and free_elem_count belong here although they are
   * mutable. A single-threaded pool, and any pool without a thread cache,
   * reaches them on every operation with no lock in between. A move to a
   * second line therefore costs those pools directly. max_magazines belongs
   * here because it is the branch that every operation starts with.
   * magazine_capacity does not belong here. The code reads it once, when a
   * thread first claims a magazine, and each magazine keeps its own copy after
   * that. */
  const char *ccol_mempool_mark;  // The code uses this field for sanity checks
#if CCOL_MEMPOOL_COMPACT_LAYOUT
  /* This turns the byte offset of an entry into its ordinal. The compact
     stride is not a power of two. This field therefore stands in for the shift
     that the default layout uses. It takes the eight bytes that max_magazines
     gives up below. */
  uintptr_t entry_index_magic;
#else
  size_t max_magazines;
#endif
  size_t extended_elem_size;
  uintptr_t lower_addr_limit;
  uintptr_t upper_addr_limit;
  void *free_inst;
  size_t free_elem_count;
  bool should_use_locks;
  bool fallback_to_dynamic_memory;
  bool is_preallocated;
  /* This is log2 of extended_elem_size in the default layout, where the stride
   * is a power of two. The free path therefore turns the offset of an entry
   * into its index with a shift and not a division. The field is zero under
   * CCOL_MEMPOOL_COMPACT_LAYOUT. There the stride is not a power of two, and
   * entry_index_magic carries the conversion instead. The field is a uint8_t
   * among the flags, and that position is deliberate. It lands in padding that
   * this line already had, so the group described above stays exactly 64
   * bytes.
   *
   * The status array for each entry needs no field of its own. The code carves
   * it immediately after the entries, so it begins at upper_addr_limit. This
   * same path has already loaded that value for the bounds check. */
  uint8_t entry_shift;
#if CCOL_MEMPOOL_COMPACT_LAYOUT
  /* The hot path asks only whether this pool caches at all. How many magazines
     it can hand out is a question for construction time, and that count stays
     below. This is what frees the eight bytes that entry_index_magic needs on
     this line. */
  bool has_cache;
  uint8_t entry_index_shift;
#endif

  /* ---- cold: construction, teardown, introspection --------------------- */
#if CCOL_MEMPOOL_COMPACT_LAYOUT
  size_t max_magazines;
#endif
  size_t magazine_capacity;
  /* This is what the caller asked for. total_elem_count is that count plus the
   * thread-cache reserve, so the two differ for a pool with a cache. Every
   * public count is stated in terms of this one. The reserve exists so that
   * the pool never refuses a thread while the cache of another thread holds
   * entries that the first thread could have had. That guarantee is what
   * bounds the other direction only roughly. Concurrent callers can hold a few
   * more than this many entries at one time, for a short time. They never hold
   * more than total_elem_count. The reason is that an entry leaves a thread
   * cache with no lock held, and therefore with no count to consult. See
   * mempool_magazine_refill_locked for why the two cannot both be exact. */
  size_t advertised_elem_count;
  size_t total_elem_count;
  /* This counter is atomic for one reason. A ranged pool must decide whether
   * an address that matched no tier can be one of its dynamic entries. It
   * answers that by reading this counter in every sub-pool, and it cannot hold
   * the lock of each of those pools to do so. Every change to the counter
   * happens under the lock of its own pool. The load is therefore an acquire
   * and each change is a release. Both are ordinary instructions on x86-64,
   * and this field is off the hot path in any case. */
  _Atomic size_t active_dynamic_memory_buffer_count;
  /* live_magazines enforces max_magazines. The code must decrement it on an
   * unlink, and not merely increment it on a link. Without the decrement,
   * ordinary thread churn fills the budget with magazines that belong to
   * threads which exited long ago. The pool then refuses every later thread.
   * It quietly goes back to taking the lock on every operation, and it still
   * passes every functional test.
   *
   * This field is a plain size_t on purpose. The code reads and writes it only
   * under the lock of its own pool. An _Atomic qualifier would let the refusal
   * check below read it without the lock. Measured against its own in-run
   * malloc comparison, that qualifier costs the cached allocate-and-free path
   * about seventeen percent, although that path never reads this field. The
   * question under the lock instead costs one uncontended lock and unlock, on
   * a path that is already out of line. */
  size_t live_magazines;
  struct ccol_mp_magazine *magazines;
  ccol_memmgmt_procs_t *m_procs;
  void *objects;
  /* The addresses of the live dynamic fallback entries of this pool, as an
   * open-addressing hash set. active_dynamic_memory_buffer_count is the number
   * of addresses it holds. The lock of the pool guards it. See the registry
   * section below for why it exists, and for where it keeps its own size. The
   * table is NULL until the first dynamic entry, and it stays allocated until
   * the pool is destroyed. It takes the place of no other field, so the
   * struct has the same size and the same offsets on every target. */
  uintptr_t *dynamic_entries;
  ccol_mutex_t lock;
};

/* The status array stores one byte for each entry. It therefore cannot use the
 * word-wide sentinels below. Those values are chosen to stand out in a memory
 * dump, and they truncate to an ordinary-looking value in a single byte. A
 * pool entry is only ever in one of two states. A byte encoding of its own is
 * therefore both enough and clear.
 *
 * The word-wide sentinels stay, and stay word-wide, for a dynamic fallback
 * entry. Such an entry keeps a header of its own. There the value that stands
 * out is still worth having, and there is room for it. */
typedef enum { mp_status_free = 0, mp_status_taken = 1 } mp_entry_status;

/* These are static because nothing outside this file names them. An
 * unprefixed definition with external linkage collides with an application
 * symbol of the same name at static-link time. -fvisibility=hidden does not
 * prevent that, because it governs the dynamic symbol table alone. */
#if UINTPTR_MAX == UINT32_MAX
static const size_t elem_is_not_a_pool_member = 0xfadeface;
static const size_t elem_is_freed_dynamic_member = 0xdeadc0de;
#elif UINTPTR_MAX == UINT64_MAX
static const size_t elem_is_not_a_pool_member = 0xfadefacefadeface;
static const size_t elem_is_freed_dynamic_member = 0xdeadc0dedeadc0de;
#else
#error "Unexpected pointer size"
#endif

/* ==========================================================================
 *                 REGISTRY OF LIVE DYNAMIC FALLBACK ENTRIES
 * ==========================================================================
 *
 * A free of an address outside the buffer of a pool is either the free of one
 * of its dynamic fallback entries, or a caller error. The header of a dynamic
 * entry sits in front of the address, so a read of it for an address that is
 * not such an entry reads memory that the pool never allocated. The address
 * can be an entry of another pool at the very start of that pool's buffer,
 * and the bytes in front of it then belong to no allocation at all. The
 * registry answers the question from the address alone, before anything reads
 * through it. The free path then reaches the documented assert cleanly for
 * every foreign pointer.
 *
 * The set uses open addressing with linear probing and backward-shift
 * deletion, so it keeps no tombstones and its probe sequences stay short
 * however many entries come and go. Its load stays at or below one half. The
 * index is the high bits of a multiplicative hash. The low bits of an address
 * are constant, because every entry is aligned, so a reduction that read them
 * would put every address in a handful of slots. The lock of the pool guards
 * the set, and only the paths of dynamic entries touch it. A pool that never
 * falls back never allocates it, and an entry of the buffer of the pool never
 * consults it.
 *
 * The table keeps its own size in the word in front of its first slot, as the
 * shift that reduces a hash to a slot index. The pool struct therefore carries
 * one pointer for the whole registry. */

#define MP_DYNAMIC_SET_MIN_CAPACITY 16u
#define MP_ADDR_BITS (sizeof(uintptr_t) * CHAR_BIT)
#if UINTPTR_MAX == UINT32_MAX
#define MP_ADDR_HASH_MUL ((uintptr_t)0x9E3779B9u)
#else
#define MP_ADDR_HASH_MUL ((uintptr_t)0x9E3779B97F4A7C15ull)
#endif

/* The shift that reduces a hash to a slot index. The table holds
 * 1 << (address bits - shift) slots, and at least MP_DYNAMIC_SET_MIN_CAPACITY
 * of them, so the shift stays below the address width. */
static inline unsigned mp_dynamic_set_shift(const ccol_mempool *mp) {
  return (unsigned)mp->dynamic_entries[-1];
}

static inline size_t mp_dynamic_set_home(const ccol_mempool *mp,
                                         uintptr_t key) {
  return (size_t)((key * MP_ADDR_HASH_MUL) >> mp_dynamic_set_shift(mp));
}

/* The slot count of the table, or 0 when the pool has none. */
static inline size_t mp_dynamic_set_capacity(const ccol_mempool *mp) {
  if (!mp->dynamic_entries) return 0;
  return (size_t)1 << (MP_ADDR_BITS - mp_dynamic_set_shift(mp));
}

/* Places key into a table that has room for it and does not hold it. */
static void mp_dynamic_set_place(ccol_mempool *mp, uintptr_t key) {
  size_t mask = mp_dynamic_set_capacity(mp) - 1;
  size_t i = mp_dynamic_set_home(mp, key);
  while (mp->dynamic_entries[i] != 0) i = (i + 1) & mask;
  mp->dynamic_entries[i] = key;
}

/* Returns the slot that holds key, or the capacity when the set does not hold
 * it. The caller holds mp->lock. */
static size_t mp_dynamic_set_find_locked(const ccol_mempool *mp,
                                         uintptr_t key) {
  size_t cap = mp_dynamic_set_capacity(mp);
  if (cap == 0) return 0;
  size_t mask = cap - 1;
  for (size_t i = mp_dynamic_set_home(mp, key);; i = (i + 1) & mask) {
    uintptr_t k = mp->dynamic_entries[i];
    if (k == key) return i;
    if (k == 0) return cap;
  }
}

static bool mp_dynamic_set_contains_locked(const ccol_mempool *mp,
                                           uintptr_t key) {
  return mp_dynamic_set_find_locked(mp, key) != mp_dynamic_set_capacity(mp);
}

/* Records key, which is the address of a dynamic entry that the pool is
 * about to hand out. The caller holds mp->lock, and the count of live dynamic
 * entries does not include key yet. It returns false when the table must grow
 * and the allocator refuses. The caller then gives the entry back and
 * reports the allocation as failed. */
static __attribute__((noinline)) bool mp_dynamic_set_insert_locked(
    ccol_mempool *mp, uintptr_t key) {
  size_t live = atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                                     memory_order_relaxed);
  size_t old_cap = mp_dynamic_set_capacity(mp);
  if ((live + 1) * 2 > old_cap) {
    /* A table never reaches half the address space: the entries it records
     * are distinct allocations. The check only keeps the arithmetic below
     * honest. */
    if (old_cap > SIZE_MAX / 4 / sizeof(uintptr_t)) return false;
    size_t new_cap = old_cap ? old_cap * 2 : MP_DYNAMIC_SET_MIN_CAPACITY;
    uintptr_t new_shift = old_cap ? mp->dynamic_entries[-1] - 1
                                  : MP_ADDR_BITS - 4u; /* log2(16) == 4 */
    uintptr_t *fresh = (uintptr_t *)_ccol_mem_calloc(mp->m_procs, new_cap + 1,
                                                     sizeof(uintptr_t));
    if (!fresh) return false;
    fresh[0] = new_shift;
    uintptr_t *old = mp->dynamic_entries;
    mp->dynamic_entries = fresh + 1;
    for (size_t i = 0; i < old_cap; ++i) {
      if (old[i] != 0) mp_dynamic_set_place(mp, old[i]);
    }
    if (old) _ccol_mem_free(mp->m_procs, old - 1);
  }
  mp_dynamic_set_place(mp, key);
  return true;
}

/* Removes key. It returns false when the set does not hold key, which means
 * that key is not a live dynamic entry of this pool. The caller holds
 * mp->lock. */
static __attribute__((noinline)) bool mp_dynamic_set_remove_locked(
    ccol_mempool *mp, uintptr_t key) {
  size_t cap = mp_dynamic_set_capacity(mp);
  size_t i = mp_dynamic_set_find_locked(mp, key);
  if (i == cap) return false;
  size_t mask = cap - 1;
  /* Backward shift: pull each later member of the cluster into the hole when
   * the hole lies on its probe path from its home slot. */
  for (size_t j = (i + 1) & mask;; j = (j + 1) & mask) {
    uintptr_t k = mp->dynamic_entries[j];
    if (k == 0) break;
    size_t home = mp_dynamic_set_home(mp, k);
    /* k can fill the hole at i exactly when i lies cyclically in [home, j). */
    if (((j - home) & mask) >= ((j - i) & mask)) {
      mp->dynamic_entries[i] = k;
      i = j;
    }
  }
  mp->dynamic_entries[i] = 0;
  return true;
}

/* Frees the table. Only a destroy calls it, once no dynamic entry is live. */
static void mp_dynamic_set_release(ccol_mempool *mp) {
  if (mp->dynamic_entries) _ccol_mem_free(mp->m_procs, mp->dynamic_entries - 1);
  mp->dynamic_entries = NULL;
}

/* Hands out one dynamic fallback entry of user_size bytes, owned by mp. The
 * caller holds mp->lock when the pool uses locks. It returns NULL when the
 * allocator refuses, or when the size cannot carry the header. */
static __attribute__((noinline)) void *mempool_alloc_dynamic_locked(
    ccol_mempool *mp, size_t user_size) {
  if (user_size > SIZE_MAX - DYNAMIC_ENTRY_PREFIX_SIZE) return NULL;
  void *raw_block =
      _ccol_mem_alloc(mp->m_procs, DYNAMIC_ENTRY_PREFIX_SIZE + user_size);
  if (!raw_block) return NULL;
  void *result = (uint8_t *)raw_block + DYNAMIC_ENTRY_PREFIX_SIZE;
  if (!mp_dynamic_set_insert_locked(mp, (uintptr_t)result)) {
    _ccol_mem_free(mp->m_procs, raw_block);
    return NULL;
  }
  mp_dynamic_header *dyn = DYNAMIC_ENTRY_HEADER(result);
  dyn->user_size = user_size;
  dyn->elem_status = elem_is_not_a_pool_member;
  atomic_store_explicit(
      &mp->active_dynamic_memory_buffer_count,
      atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                           memory_order_relaxed) +
          1,
      memory_order_release);
  return result;
}

/* ==========================================================================
 *                      THREAD CACHE (MAGAZINES)
 * ==========================================================================
 *
 * Each thread keeps a small magazine of free entries for each pool. An
 * allocation pops from it and a free pushes to it. Neither takes the pool
 * lock, so the common path runs no lock and no read-modify-write atomic at
 * all. Only two operations touch the shared free list: a refill of an empty
 * magazine, and a flush of a full one. Both hold the lock of the pool.
 *
 * The pool keeps its capacity. It does not trade the capacity away. A pool
 * that is asked for N entries allocates N + R slots. R is the hard ceiling on
 * how much can ever sit in thread caches. At least N entries therefore stay
 * available to any thread, no matter how much other threads cached. R is
 * bounded by N/2. It falls below that proportion once a pool is large enough
 * for the capacity clamp of each magazine to bind. R is N/4 at 8192 entries,
 * N/8 at 16384, and a few percent or less from about 65536 upward.
 */

#define CCOL_MP_MAGAZINE_LIMIT 16u /* most magazines any one pool hands out */
#define CCOL_MP_CAPACITY_MAX 128u  /* most entries any one magazine holds    */
#define CCOL_MP_MIN_CACHED_POOL 8u /* pools below this get no cache at all */
/* The pool holds spare entries equal to its element count divided by this
 * value. That is the ceiling on what every thread cache of the pool can hold
 * between them. It is therefore also the ceiling on the extra memory that a
 * cached pool costs. Depth is what a bursty caller pays for. A magazine that
 * reaches its refill or flush boundary often enough makes the branch at that
 * boundary cost more than the lock that it avoids. The rate of those
 * boundaries falls as the reciprocal of the depth. */
#define CCOL_MP_RESERVE_DIVISOR 2u
/* The map is direct, so a ccol_r_mempool that allocates across size tiers does
 * not thrash a single-entry cache on every call. The count is a power of two,
 * and that is what lets mempool_tls_slot() end in a mask. */
#define CCOL_MP_TLS_SLOTS 8u
_Static_assert(CCOL_MP_TLS_SLOTS > 0u &&
                   (CCOL_MP_TLS_SLOTS & (CCOL_MP_TLS_SLOTS - 1u)) == 0,
               "mempool_tls_slot() reduces with a mask, which only covers the "
               "whole slot array while this is a power of two; zero satisfies "
               "the power-of-two test on its own and must be excluded, since "
               "it would make the mask every bit");

typedef struct ccol_mp_magazine {
  /* Whoever unlinks this magazine from the list of its pool writes this field.
   * The owning thread reads it. The ordering is release and acquire, and not
   * relaxed. The owning thread frees an orphaned magazine as soon as it sees
   * one. A read of NULL must therefore also make the unlink before it visible.
   * Without that, the thread that unlinks still holds a pointer to memory that
   * the owner just freed. */
  _Atomic(ccol_mempool *) pool;
  /* This field is a relaxed atomic and not a plain size_t. The only reason is
   * that the introspection functions must read it from another thread without
   * a data race. Only the owning thread ever writes it. A relaxed load or
   * store compiles to an ordinary load or store, with no lock prefix and no
   * fence, so the fast path does not change. What the atomic buys is an exact
   * answer from ccol_mempool_used_count(). That function then reports the
   * entries that the pool truly handed out, and not the entries that the pool
   * merely pulled out of the shared list. Without it, a single allocation
   * over-reports by a whole refill batch. */
  _Atomic size_t count;
  size_t capacity;
  struct ccol_mp_magazine *next_in_pool;
  struct ccol_mp_magazine *prev_in_pool;
  struct ccol_mp_magazine *next_in_thread;
  /* This field holds the value. It does not reach it through the pool. An
   * orphaned magazine outlives the pool that it came from, so mp->m_procs is
   * already gone when the code frees the magazine. NULL means that the pool
   * used the default allocator. _ccol_r_mempool_destroy follows the same
   * convention, for the same reason. */
  ccol_free_t free_proc;
  /* This array has the size of the magazine capacity of this pool. It does not
   * have the size of the ceiling above that capacity. The ceiling is what a
   * large pool can reach. The magazine of a small pool holds a handful of
   * entries, and its size matches that. A higher ceiling therefore costs a
   * small pool nothing. */
  void *slots[];
} ccol_mp_magazine;

/* The struct needs no padding against false sharing between magazines. The
 * slots array alone makes the struct larger than a cache line. Two magazines
 * from two separate allocations therefore cannot place their hot fields on one
 * line. */

/* This is one thread-local object and not four separate ones. In a shared
 * library built with -fPIC, each distinct __thread variable resolves through
 * its own __tls_get_addr call. A split of this state across several variables
 * would therefore put a dozen function calls on a path whose whole purpose is
 * to avoid a lock. One object leaves one resolution for each call. The code
 * reaches everything else as an offset from that object.
 *
 * armed exists for the following reason. A pthread key destructor runs only
 * for a thread whose value for that key is non-NULL. All the real state stays
 * here, in __thread storage that the key never sees. Without a value on the
 * key, the destructor never runs and every magazine leaks. Under
 * _CCOL_EMULATE_DARWIN_TLS the state itself is the value of the key, and
 * armed is not used. */
typedef struct {
  ccol_mp_magazine *mags;
  /* A magazine names its own pool. The reap below clears every slot that
   * points at a magazine before it frees that magazine. A slot therefore never
   * holds a dangling pointer, and the pool field of the magazine is the whole
   * hit test. A parallel array of pool keys answers the same question one load
   * later. */
  ccol_mp_magazine *cache[CCOL_MP_TLS_SLOTS];
  bool armed;
} mp_tls_state_t;

/* The model is initial-exec and not the default general model. In a shared
 * library, the general model resolves every thread-local access through a
 * __tls_get_addr call. On this path that is one function call for each
 * allocation and each free, and it costs about 25 percent of the thread-safe
 * throughput of the pool. initial-exec resolves at load time instead. It costs
 * one reserved slot in the static thread-local block of the process. A library
 * that loads in the normal way is not affected. A library that dlopen() loads
 * into a process which already filled that block fails to load. Build with
 * -DCCOL_MEMPOOL_DYNAMIC_TLS=1 to select the general model. That is for a
 * caller who needs the dlopen() case to work. */
#if defined(_CCOL_EMULATE_DARWIN_TLS)
/* No __thread object: the state is the value of the key; see _mp_tls_cur(). */
#elif defined(CCOL_MEMPOOL_DYNAMIC_TLS) && CCOL_MEMPOOL_DYNAMIC_TLS
static __thread mp_tls_state_t _mp_tls;
#else
static __thread mp_tls_state_t _mp_tls
    __attribute__((tls_model("initial-exec")));
#endif

static ccol_thread_ls_key_t _mp_tls_key;
/* This lock serializes the exit-time drain of a thread against a concurrent
 * pool destroy. Without it, the drain can read a non-NULL pool pointer, and a
 * destroy can then free that pool before the drain locks it. The code takes
 * this lock before any pool lock, and never the other way round. It also
 * serializes the arming of a thread against the deletion of the key in
 * _mp_cache_fini. */
static ccol_mutex_t _mp_registry_lock;
/* True while the key exists and a thread may still arm it. It goes false,
 * under the registry lock, when the module unloads. */
static atomic_bool _mp_cache_live;
static ccol_once_flag_t _mp_cache_once = CCOL_ONCE_INIT;

static void _mp_tls_drain(void *arg);

/* The thread cache state of the calling thread. Under
 * _CCOL_EMULATE_DARWIN_TLS (see ctlsmodel.h) it lives in a heap block whose
 * pointer is the value of the key, and this gives NULL for a thread that has
 * none yet or when the key does not exist. Everywhere else it is the __thread
 * object, so the NULL tests of the callers fold away. */
#if defined(_CCOL_EMULATE_DARWIN_TLS)
static inline __attribute__((always_inline)) mp_tls_state_t *_mp_tls_cur(void) {
  if (!atomic_load_explicit(&_mp_cache_live, memory_order_acquire)) return NULL;
  return (mp_tls_state_t *)ccol_thread_ls_get(_mp_tls_key);
}
#else
static inline __attribute__((always_inline)) mp_tls_state_t *_mp_tls_cur(void) {
  return &_mp_tls;
}
#endif

static void _mp_cache_init(void) {
  if (ccol_mutex_init(_mp_registry_lock) != 0) return;
  if (ccol_thread_ls_key_create(_mp_tls_key, _mp_tls_drain) != 0) return;
  atomic_store(&_mp_cache_live, true);
}

/* This is true once the key and the registry lock exist, until the module
 * unloads. A pool created while it is false runs without a cache. That is a
 * performance outcome and not a failure. The creation of the key can fail for
 * real reasons. One such reason is a process that already reached
 * PTHREAD_KEYS_MAX. That must not fail a pool creation which the caller had
 * every reason to expect to succeed. */
static bool _mp_cache_available(void) {
  ccol_call_once(_mp_cache_once, _mp_cache_init);
  return atomic_load(&_mp_cache_live);
}

/* This derives the cache geometry, and with it the reserve that keeps the
 * capacity guarantee whole. The N < CCOL_MP_MIN_CACHED_POOL gate is
 * load-bearing. Do not fold it into the clamp below it. A lower clamp of 1 on
 * its own forces a magazine onto every pool, a one-element pool included. That
 * contradicts the floor. It also drives the reserve past N/2, up to N itself
 * for a pool of one element. */
static void mempool_cache_geometry(size_t elem_count, size_t *max_magazines,
                                   size_t *capacity, size_t *reserve) {
  *max_magazines = 0;
  *capacity = 0;
  *reserve = 0;
  if (elem_count < CCOL_MP_MIN_CACHED_POOL) return;

  size_t mags = elem_count / CCOL_MP_MIN_CACHED_POOL;
  if (mags > CCOL_MP_MAGAZINE_LIMIT) mags = CCOL_MP_MAGAZINE_LIMIT;
  if (mags == 0) mags = 1;

  size_t cap = elem_count / (CCOL_MP_RESERVE_DIVISOR * mags);
  if (cap > CCOL_MP_CAPACITY_MAX) cap = CCOL_MP_CAPACITY_MAX;
  if (cap == 0) cap = 1;

  /* This guards the N + R of the caller against a wrap. R is the addition of
   * the library. If the arithmetic cannot hold it, the code builds the pool
   * without a cache. It does not fail the build of the pool. */
  if (mags > SIZE_MAX / cap) return;
  size_t r = mags * cap;
  if (r > SIZE_MAX - elem_count) return;

  *max_magazines = mags;
  *capacity = cap;
  *reserve = r;
}

/* This gives the entries that the pool holds out to callers now. It is
 * everything that left the shared free list, less everything that sits in a
 * thread cache. The caller holds mp->lock.
 *
 * The walk over the magazines costs one relaxed load for each magazine. The
 * magazine limit of the pool bounds it. It runs only on paths that already
 * hold the lock. A pool without a cache has no magazines to walk. */
static size_t mempool_live_count_locked(ccol_mempool *mp) {
  size_t live = mp->total_elem_count - mp->free_elem_count;
  for (ccol_mp_magazine *mag = mp->magazines; mag; mag = mag->next_in_pool) {
    size_t cached = atomic_load_explicit(&mag->count, memory_order_relaxed);
    live = (live > cached) ? (live - cached) : 0;
  }
  return live;
}

/* This moves entries from the free list of the pool into the magazine, and
 * fills it. A refill takes the whole magazine and not half of it. Both choices
 * give the same hysteresis against a caller that oscillates at the full mark.
 * But a complete fill spreads a burst of allocations over the whole capacity
 * of the magazine instead of half of it. That matters most on a small pool,
 * where the capacity is only a couple of entries.
 *
 * The caller holds mp->lock. This function reports corruption through
 * *corrupt. It does not assert here, so that the caller can release the lock
 * first. Every other corruption path in this file does the same. */
static void mempool_magazine_refill_locked(ccol_mempool *mp,
                                           ccol_mp_magazine *mag,
                                           bool *corrupt) {
  /* Every entry granted here can become live later, with no further lock. What
   * must stay within the advertised count is therefore everything that already
   * left the shared free list. It is not only what is live at this instant. For
   * this decision, an entry that sits in the magazine of ANOTHER thread is as
   * good as handed out. Nothing consults this budget again before that
   * thread pops it. total_elem_count - free_elem_count is exactly that
   * quantity. The reserve is total_elem_count - advertised_elem_count. The room
   * that is left is therefore free_elem_count - reserve.
   *
   * Do not charge only the holdings of this magazine. That lets several
   * magazines each get a batch that is inside the remainder on its own, and
   * past the remainder together. Sixteen threads that race to empty a
   * 1024-entry pool then hold about 1180 entries at one time, and not 1024.
   *
   * The bound is still not exact, and no rule here can make it exact. The
   * locked path in ccol_mempool_alloc_entry must keep serving while callers
   * truly hold fewer than advertised_elem_count entries. Without that, the
   * pool refuses a caller entries that another thread merely cached, and that
   * is the guarantee which the reserve exists to give. The two cannot both be
   * exact. This code therefore keeps the overshoot to a handful of entries
   * instead of the whole reserve. The documentation of
   * ccol_mempool_total_capacity() states what the pool promises.
   */
  size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
  size_t reserve = mp->total_elem_count - mp->advertised_elem_count;
  if (mp->free_elem_count <= reserve) return;
  size_t budget = mp->free_elem_count - reserve;

  while (budget > 0 && count < mag->capacity && mp->free_inst) {
    void *entry = mp->free_inst;
    /* The entry comes from the free list of this pool, so it is in range by
       construction. The check here is that nothing corrupted the list into a
       pointer at something which is not a free entry. */
    if (!valid_mempool_addr(mp, (uintptr_t)entry) ||
        ENTRY_STATUS(mp, ENTRY_INDEX(mp, (uintptr_t)entry)) != mp_status_free) {
      *corrupt = true;
      return;
    }
    --budget;
    mp->free_inst = *(void **)entry;
    --mp->free_elem_count;
    mag->slots[count++] = entry;
    atomic_store_explicit(&mag->count, count, memory_order_relaxed);
  }
}

/* This returns how_many of the entries of the magazine to the free list of the
 * pool. A flush of everything lets a caller that oscillates across the full
 * mark flush and refill at once. Such a caller takes the lock on every second
 * operation. An entry in a magazine is already marked free, and that is what
 * keeps a double free of a cached entry detectable. Only the list links change
 * here.
 *
 * The caller holds mp->lock. */
static void mempool_magazine_flush_locked(ccol_mempool *mp,
                                          ccol_mp_magazine *mag,
                                          size_t how_many) {
  size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
  while (how_many-- > 0 && count > 0) {
    void *entry = mag->slots[--count];
    atomic_store_explicit(&mag->count, count, memory_order_relaxed);
    *(void **)entry = mp->free_inst;
    mp->free_inst = entry;
    ++mp->free_elem_count;
  }
}

static void mempool_magazine_free(ccol_mp_magazine *mag) {
  /* This calls the stored proc directly and not through _ccol_mem_free. The
   * NULL check of _ccol_mem_free tests the address of a local, and that test
   * is therefore always true. */
  if (mag->free_proc)
    mag->free_proc(mag);
  else
    ccol_mem_free(mag);
}

/* This drops an orphaned magazine from the list of this thread and frees it.
 * It takes no lock, and that is deliberate. The unlink of an orphaned magazine
 * from the list of its pool happens before the clear of its pool pointer. This
 * thread is therefore its only remaining owner. The function also does not
 * touch live_magazines, and that too is deliberate. That count belongs to
 * whoever links or unlinks against the list of the pool, and the destroy
 * already counted this one. */
static void mempool_tls_reap(mp_tls_state_t *tls, ccol_mp_magazine **link,
                             ccol_mp_magazine *mag) {
  *link = mag->next_in_thread;
  for (size_t i = 0; i < CCOL_MP_TLS_SLOTS; ++i) {
    /* The lookup cache can still point at what this code is about to free. */
    if (tls->cache[i] == mag) tls->cache[i] = NULL;
  }
  mempool_magazine_free(mag);
}

/* This finds the magazine of this thread for mp. It creates one if the pool
 * still has a free slot. It returns NULL in three cases: the pool has no
 * cache, every slot is taken, or the allocation of the magazine fails. All
 * three mean "use the locked path". It reaps any orphaned magazine that it
 * walks past. */
/* The hit on the direct map. Every call after the first touch of a pool by a
 * thread takes this path. The compiler inlines it into the fast paths, and
 * that is deliberate. It is one masked index, two compares and a load. A call
 * boundary around it costs more than the work that it does. The miss path
 * stays out of line below, so the callers keep the code shape that they have
 * when the cache is not in play.
 *
 * Every access is an offset from one resolved thread-local base. The lookup
 * therefore costs one TLS resolution, and not one for each field that the
 * code touches. */
/* This gives the slot of the direct map that a pool occupies.
 *
 * The map exists so that a ccol_r_mempool which allocates across size tiers
 * keeps one slot for each tier, instead of thrashing one slot. The tiers are
 * separate allocations, so their addresses differ by a stride. Which address
 * bits the index reads decides entirely whether that stride spreads or
 * aliases. The lowest bits above the alignment are the worst choice that is
 * available. Bits 3 to 5 reach a single slot of eight at several of the
 * strides that a pool struct realistically lands on. A ranged pool that cycles
 * across tiers then misses on almost every call.
 *
 * The shift below reads bits 8 to 10. Two measures decide that, because
 * neither one alone settles the question. The first is how the eight slots
 * fill over the real addresses that the tiers of a ranged pool land on. That
 * measure runs at six through fifteen tiers, on LP64 and on ILP32. On ILP32 the
 * struct is smaller and the strides differ. The second is the same measure over
 * synthetic uniform strides. These bits are at or near the best on every one of
 * those measures. They never leave more than half the tiers in one slot, and
 * each neighbouring shift does leave more than half somewhere. The shift costs
 * one shift and one mask, the same as any other single run of bits, and that is
 * what keeps this on the inlined hit path.
 *
 * CCOL_MP_TLS_SLOTS is a power of two, so the reduction is a mask and not a
 * division. */
static inline __attribute__((always_inline)) size_t
mempool_tls_slot(const ccol_mempool *mp) {
  return (size_t)(((uintptr_t)mp >> 8) & (uintptr_t)(CCOL_MP_TLS_SLOTS - 1));
}

static inline __attribute__((always_inline)) ccol_mp_magazine *
mempool_tls_magazine_hit(ccol_mempool *mp) {
  mp_tls_state_t *tls = _mp_tls_cur();
  if (!tls) return NULL;
  size_t slot = mempool_tls_slot(mp);
  ccol_mp_magazine *mag = tls->cache[slot];
  if (__builtin_expect(
          mag && atomic_load_explicit(&mag->pool, memory_order_acquire) == mp,
          1))
    return mag;
  return NULL;
}

/* The miss path of the direct map. It walks the list of this thread, reaps
 * any magazine whose pool is destroyed, and returns the magazine of this
 * thread for mp. It returns NULL when this thread holds none. It takes no
 * lock. */
static ccol_mp_magazine *mempool_tls_magazine_find(ccol_mempool *mp) {
  mp_tls_state_t *tls = _mp_tls_cur();
  if (!tls) return NULL;
  ccol_mp_magazine *mag;

  ccol_mp_magazine **link = &tls->mags;
  while ((mag = *link) != NULL) {
    ccol_mempool *owner =
        atomic_load_explicit(&mag->pool, memory_order_acquire);
    if (!owner) {
      mempool_tls_reap(tls, link, mag);
      /* The reap calls the allocator of the reaped magazine, which can be
       * built on another cached pool and so re-enter this walk on this
       * thread. That inner walk can reap, and free, the magazine that link
       * points into. The walk therefore starts again from the head. */
      link = &tls->mags;
      continue;
    }
    if (owner == mp) {
      tls->cache[mempool_tls_slot(mp)] = mag;
      return mag;
    }
    link = &mag->next_in_thread;
  }
  return NULL;
}

/* Builds a magazine for this thread and claims one of the magazines that mp
 * grants. The caller learned under the lock, a moment ago, that mp had one
 * left. This function releases that lock before the allocation, so it asks
 * again when it claims: another thread can take the last one in between. It
 * returns NULL when that happens, or when the allocation fails. Both mean "use
 * the locked path".
 *
 * A thread reaches this at most once for each magazine that it ends up
 * holding. A thread that the pool refuses never gets here. Its callers answer
 * the budget question in the critical section that then serves the request,
 * so a refused thread pays one lock for each operation, exactly as a pool
 * with no cache does, and allocates nothing. */
static __attribute__((noinline)) ccol_mp_magazine *mempool_tls_magazine_create(
    ccol_mempool *mp) {
  mp_tls_state_t *tls = _mp_tls_cur();

  /* A thread arms the key before it builds its first magazine, and it builds
   * none while the key carries no value. A magazine that the thread held
   * without a value on the key is never drained at thread exit: its entries
   * are lost to the pool, live_magazines is never given back, and the
   * magazine itself leaks. pthread_setspecific can fail, and the key is gone
   * once the module unloads, so this can refuse. The caller then takes the
   * locked path, and the next call asks again. The registry lock orders the
   * arming against the deletion of the key in _mp_cache_fini. A value set on
   * a deleted key can land on a key that another component created since,
   * and the destructor of that component then receives it. */
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  /* Here the state itself is the value of the key, so a thread with no state
   * makes one and sets it under the registry lock. */
  if (!tls) {
    tls = (mp_tls_state_t *)calloc(1, sizeof(*tls));
    if (!tls) return NULL;
    bool set = false;
    ccol_mutex_lock(_mp_registry_lock);
    if (atomic_load_explicit(&_mp_cache_live, memory_order_relaxed) &&
        ccol_thread_ls_set(_mp_tls_key, tls) == 0)
      set = true;
    ccol_mutex_unlock(_mp_registry_lock);
    if (!set) {
      free(tls);
      return NULL;
    }
  }
#else
  if (!tls->armed) {
    ccol_mutex_lock(_mp_registry_lock);
    if (atomic_load_explicit(&_mp_cache_live, memory_order_relaxed) &&
        ccol_thread_ls_set(_mp_tls_key, (void *)1) == 0)
      tls->armed = true;
    ccol_mutex_unlock(_mp_registry_lock);
    if (!tls->armed) return NULL;
  }
#endif

  /* This allocation runs outside the pool lock because nothing here needs the
   * lock for it, and a thread-cache miss should not lengthen the critical
   * section. The dynamic fallback path does call the allocator of the caller
   * with the pool lock held, which is why the documented contract forbids an
   * allocator that calls back into the same pool. */
  ccol_mp_magazine *mag = _ccol_mem_calloc(
      mp->m_procs, 1, sizeof(*mag) + mp->magazine_capacity * sizeof(void *));
  if (!mag) return NULL;

  /* This code fills in everything that a magazine carries before it links the
   * magazine, while the magazine is still private to this thread. The teardown
   * walk assumes that a magazine which it reaches from mp->magazines is
   * complete. That walk clears the pool pointer of every entry that it finds.
   * A magazine linked before that pointer is written would have the write land
   * afterward, and the pointer would then name a pool that is going away. */
  mag->capacity = mp->magazine_capacity;
  mag->free_proc = mp->m_procs ? mp->m_procs->free : NULL;
  atomic_store_explicit(&mag->pool, mp, memory_order_release);

  bool claimed = false;
  ccol_mutex_lock(mp->lock);
  if (mp->live_magazines < mp->max_magazines) {
    ++mp->live_magazines;
    mag->next_in_pool = mp->magazines;
    if (mp->magazines) mp->magazines->prev_in_pool = mag;
    mp->magazines = mag;
    claimed = true;
  }
  ccol_mutex_unlock(mp->lock);

  if (!claimed) {
    mempool_magazine_free(mag);
    return NULL;
  }

  mag->next_in_thread = tls->mags;
  tls->mags = mag;
  tls->cache[mempool_tls_slot(mp)] = mag;
  return mag;
}

/* This unlinks a magazine from the list of its pool. The caller holds
 * mp->lock. */
static void mempool_magazine_unlink_locked(ccol_mempool *mp,
                                           ccol_mp_magazine *mag) {
  if (mag->prev_in_pool)
    mag->prev_in_pool->next_in_pool = mag->next_in_pool;
  else
    mp->magazines = mag->next_in_pool;
  if (mag->next_in_pool) mag->next_in_pool->prev_in_pool = mag->prev_in_pool;
  mag->next_in_pool = NULL;
  mag->prev_in_pool = NULL;
  --mp->live_magazines;
}

/* This runs on thread exit, for every magazine that this thread still owns. It
 * returns the entries that the magazine holds, so that the pool does not lose
 * them for the rest of the process. It then frees the magazine itself. That
 * second step matters, because a magazine is reachable from thread-local
 * storage. Without the free, any leak checker that treats still-reachable
 * memory as an error reports it. */
static void _mp_tls_drain(void *arg) {
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  /* The block that arg names. The C library has already cleared the value of
   * the key, so a magazine that a nested free builds during this drain goes
   * into a new block, which sets the value again and gets a drain of its own
   * in the next round of destructors. */
  mp_tls_state_t *tls = (mp_tls_state_t *)arg;
  if (!tls) return;
#else
  mp_tls_state_t *tls = &_mp_tls;
  (void)arg;
#endif
  /* The free of a magazine goes to the allocator of its pool, and that
   * allocator can itself be built on another pool with a thread cache. Such a
   * free runs on this thread while the drain is under way, finds no magazine
   * of that other pool on the emptied list, and builds a new one there. The
   * outer loop therefore takes the list again until nothing refilled it;
   * otherwise that magazine and the entries in it leak, and its pool never
   * gets back the magazine that it granted. Each round frees magazines one
   * allocator level further down, so the loop ends. armed stays true for the
   * whole drain, so such a nested build does not set the key again. */
  ccol_mp_magazine *mag;
  while ((mag = tls->mags) != NULL) {
    tls->mags = NULL;
    for (size_t i = 0; i < CCOL_MP_TLS_SLOTS; ++i) tls->cache[i] = NULL;
    while (mag) {
      ccol_mp_magazine *next = mag->next_in_thread;
      /* The registry lock is what stops a destroy from freeing the pool
       * between the read of its pointer here and the lock on it below. */
      ccol_mutex_lock(_mp_registry_lock);
      ccol_mempool *mp = atomic_load_explicit(&mag->pool, memory_order_acquire);
      if (mp) {
        ccol_mutex_lock(mp->lock);
        mempool_magazine_flush_locked(
            mp, mag, atomic_load_explicit(&mag->count, memory_order_relaxed));
        mempool_magazine_unlink_locked(mp, mag);
        ccol_mutex_unlock(mp->lock);
      }
      ccol_mutex_unlock(_mp_registry_lock);
      mempool_magazine_free(mag);
      mag = next;
    }
  }
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  free(tls);
#else
  tls->armed = false;
#endif
}

/* This runs when the module unloads, on a dlclose() and at process exit. It
 * drains the calling thread. It then stops every thread from arming the key
 * and deletes the key, both under the registry lock. The C library runs the
 * destructor of a key only while that key exists, so a thread that outlives
 * a dlclose() never calls _mp_tls_drain at its exit, whose code is then no
 * longer mapped. The magazines of such a thread are leaked at that point and
 * not drained: nothing that could drain them is still mapped. A pool that
 * exists after this runs keeps working through its locked paths, and a pool
 * created after it has no cache. At process exit no key destructor runs for
 * any thread in any case, so the deletion changes nothing there. A program
 * that leaves threads running should join them before it exits or unloads the
 * library, which also lets their magazines drain. The registry lock keeps
 * this code from racing a concurrent destroy or a concurrent arming. The key
 * is never deleted without a live cache, so a second run of this function
 * returns at once. */
__attribute__((destructor)) static void _mp_cache_fini(void) {
  if (!atomic_load(&_mp_cache_live)) return;
#if defined(_CCOL_EMULATE_DARWIN_TLS)
  mp_tls_state_t *tls = _mp_tls_cur();
  if (tls) {
    (void)ccol_thread_ls_set(_mp_tls_key, NULL);
    _mp_tls_drain(tls);
  }
#else
  _mp_tls_drain(NULL);
#endif
  ccol_mutex_lock(_mp_registry_lock);
  atomic_store(&_mp_cache_live, false);
  ccol_thread_ls_key_delete(_mp_tls_key);
  ccol_mutex_unlock(_mp_registry_lock);
}

/* The paths of a pool that uses locks and cannot serve a request from its
 * thread cache. They are defined next to the entry points. */
static void *mempool_alloc_locked(ccol_mempool *mp);
static void mempool_free_locked(ccol_mempool *mp, void *entry);

/* These are the thread-cache halves of alloc and free. They are kept out of
 * line on purpose. An inline copy inside ccol_mempool_alloc_entry and
 * __ccol_mempool_free_entry disturbs the code that those functions generate
 * for every other caller. Measured against an otherwise identical build, an
 * inline copy of this code costs a pool with no cache more than ten percent.
 * That pool runs not one instruction of it. Out of line, a pool with no cache
 * sees only the branch that skips the call. */

/* This returns an entry for a thread that the inlined pop could not serve.
 * There is no magazine in the direct map, or the magazine is empty. The caller
 * has established that the pool has a cache, and it holds no lock.
 *
 * Every case here costs at most one critical section, except the one-time
 * creation of a magazine. A thread without a magazine asks whether the pool
 * has one left to give, and when it has none, the same critical section
 * serves the request. An empty magazine is refilled, and when the refill
 * yields nothing because the reserve bound applies, the same critical section
 * serves the request through the ordinary locked path. Both answers therefore
 * cost what an allocation from a pool with no cache costs: one lock.
 *
 * Whatever it cannot serve, it hands to the locked path of the pool, and
 * returns what that path returns. It hands over a request that the locked
 * path already refused only for an exhausted pool, which is the failure
 * case, and the answer is then the same. The entry point therefore ends in a
 * tail jump to this function, and needs no frame of its own. */
static __attribute__((noinline)) void *mempool_alloc_from_cache_slow(
    ccol_mempool *mp, ccol_mp_magazine *mag) {
  void *served;
  if (!mag) {
    mag = mempool_tls_magazine_find(mp);
    if (!mag) {
      ccol_mutex_lock(mp->lock);
      bool budget_available = mp->live_magazines < mp->max_magazines;
      served = budget_available ? NULL : mempool_serve_locked(mp);
      ccol_mutex_unlock(mp->lock);
      if (!budget_available) return served ? served : mempool_alloc_locked(mp);
      mag = mempool_tls_magazine_create(mp);
      if (!mag) return mempool_alloc_locked(mp);
    }
  }

  if (atomic_load_explicit(&mag->count, memory_order_relaxed) == 0) {
    bool corrupt = false;
    served = NULL;
    ccol_mutex_lock(mp->lock);
    mempool_magazine_refill_locked(mp, mag, &corrupt);
    bool refilled =
        atomic_load_explicit(&mag->count, memory_order_relaxed) != 0;
    if (!corrupt && !refilled) served = mempool_serve_locked(mp);
    ccol_mutex_unlock(mp->lock);
    /* The assert runs after the code releases the lock. Every other
     * corruption path in this file does the same. */
    if (corrupt) {
      ccol_assert(false);
    }
    if (!refilled) return served ? served : mempool_alloc_locked(mp);
  }

  size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
  void *entry = mag->slots[--count];
  atomic_store_explicit(&mag->count, count, memory_order_relaxed);
  ENTRY_STATUS(mp, ENTRY_INDEX(mp, (uintptr_t)entry)) =
      (uint8_t)mp_status_taken;
  return entry;
}

static inline __attribute__((always_inline)) void *mempool_alloc_from_cache(
    ccol_mempool *mp) {
  /* The hit is the common case, and both tests say so to the compiler. It
   * then lays the pop out straight after the lookup, contiguous with the
   * entry of the function, which starts on a 64-byte boundary. Left to
   * itself it places the pop out of line, as the target of a taken branch,
   * and that block then falls wherever the rest of the function puts it: a
   * start offset that splits the 52-byte pop across two 64-byte lines costs
   * about 10 percent on every allocation. Every other case leaves through a
   * tail jump, so the function keeps no frame. */
  ccol_mp_magazine *mag = mempool_tls_magazine_hit(mp);
  if (__builtin_expect(mag != NULL, 1)) {
    size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
    if (__builtin_expect(count != 0, 1)) {
      void *entry = mag->slots[--count];
      atomic_store_explicit(&mag->count, count, memory_order_relaxed);
      ENTRY_STATUS(mp, ENTRY_INDEX(mp, (uintptr_t)entry)) =
          (uint8_t)mp_status_taken;
      return entry;
    }
  }
  return mempool_alloc_from_cache_slow(mp, mag);
}

/* This parks an entry in the magazine of this thread. Anything that it cannot
 * park goes to the locked path, through a tail jump. The caller has
 * established that the pool has a cache, and it holds no lock.
 *
 * The order of the tests below matches the locked path exactly. The code
 * checks the address against the block of the pool first. That check is
 * also what rejects a dynamic fallback entry. The address of such an entry is
 * never inside that block, and a cached copy of one would hand a freed block
 * back out as a pool entry. Only after that check does the code read the
 * taken-or-free status of the entry. A different order changes which
 * corruption the code detects, or whether it detects any at all.
 *
 * Everything that the bounds check reads is immutable after construction, so
 * that check needs no lock. The status byte is not immutable. The code reads
 * and writes it here with no lock, and that is what leaves a double free by
 * two threads at the same instant undetected. Such a call is a data race in
 * the program of the caller in either case. It is also the same limit that a
 * general purpose allocator with a thread cache has. See the note on
 * ccol_mempool_free_entry in the header. */
/* This holds every case that a push can meet which is not "there is room".
 * Either this thread has no magazine for this pool yet, or the magazine that
 * it has is full and must give half back under the lock. The function is out
 * of line, so that the push itself stays a handful of instructions. mag is
 * whatever the lookup in the direct map found. It is NULL when the caller
 * missed that lookup entirely. What it cannot park goes to the locked path. */
static __attribute__((noinline)) void mempool_free_to_cache_slow(
    ccol_mempool *mp, void *entry, size_t idx, ccol_mp_magazine *mag) {
  if (!mag) {
    mag = mempool_tls_magazine_find(mp);
    if (!mag) {
      /* The same single critical section as the allocation side. When the
       * pool has no magazine left for this thread, the entry goes straight
       * back to the shared list under this lock, and the caller is done.
       * The status byte is read again under the lock, as the locked path
       * reads it. */
      bool corrupt = false;
      ccol_mutex_lock(mp->lock);
      bool budget_available = mp->live_magazines < mp->max_magazines;
      if (!budget_available) {
        if (ENTRY_STATUS(mp, idx) != mp_status_taken) {
          corrupt = true;
        } else {
          ENTRY_STATUS(mp, idx) = (uint8_t)mp_status_free;
          *(void **)entry = mp->free_inst;
          mp->free_inst = entry;
          ++mp->free_elem_count;
        }
      }
      ccol_mutex_unlock(mp->lock);
      if (corrupt) {
        ccol_assert(false);
      }
      if (!budget_available) return;
      mag = mempool_tls_magazine_create(mp);
      if (!mag) {
        mempool_free_locked(mp, entry);
        return;
      }
    }
  }

  size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
  if (count == mag->capacity) {
    size_t half = mag->capacity / 2;
    if (half == 0) half = 1;
    ccol_mutex_lock(mp->lock);
    mempool_magazine_flush_locked(mp, mag, half);
    ccol_mutex_unlock(mp->lock);
    count = atomic_load_explicit(&mag->count, memory_order_relaxed);
  }
  if (count >= mag->capacity) {
    mempool_free_locked(mp, entry);
    return;
  }

  ENTRY_STATUS(mp, idx) = (uint8_t)mp_status_free;
  mag->slots[count++] = entry;
  atomic_store_explicit(&mag->count, count, memory_order_relaxed);
}

static inline __attribute__((always_inline)) void mempool_free_to_cache(
    ccol_mempool *mp, void *entry, uintptr_t c_entry) {
  /* Ownership comes first, because there is no header to consult before it. An
     address outside the buffer of this pool is not a pool entry. It belongs on
     the locked path, which owns the dynamic fallback. */
  if (__builtin_expect(!valid_mempool_addr(mp, c_entry), 0)) {
    mempool_free_locked(mp, entry);
    return;
  }

  size_t idx = ENTRY_INDEX(mp, c_entry);
  if (__builtin_expect(ENTRY_STATUS(mp, idx) != mp_status_taken, 0)) {
    /* This is a double free. It includes a double free of an entry that sits
     * in a magazine. Such an entry is marked free for exactly this reason, so
     * that the check stays able to detect it. The code holds no lock here, so
     * there is nothing to release first. */
    ccol_assert(false);
  }

  /* The push is the common case; see mempool_alloc_from_cache for why the
   * hints matter. */
  ccol_mp_magazine *mag = mempool_tls_magazine_hit(mp);
  if (__builtin_expect(mag != NULL, 1)) {
    size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
    if (__builtin_expect(count < mag->capacity, 1)) {
      ENTRY_STATUS(mp, idx) = (uint8_t)mp_status_free;
      mag->slots[count++] = entry;
      atomic_store_explicit(&mag->count, count, memory_order_relaxed);
      return;
    }
  }
  mempool_free_to_cache_slow(mp, entry, idx, mag);
}

#ifdef RUNNING_UNIT_TESTS
/* These are white-box accessors for the thread-cache regression tests. The
 * compiler builds them only under RUNNING_UNIT_TESTS. They are therefore
 * absent from the shipped library, and they never reach its dynamic symbol
 * table. The behaviours that they expose have no public API, because they are
 * not a concern of the caller. But they are exactly the properties whose
 * failure is silent. This is why the tests assert on them directly, and do not
 * infer them from timing or from throughput. */

/* This overwrites what a pool records about one of its entries.
 *
 * An entry carries no header. Its recorded state therefore stays in the status
 * array of the pool, and is not reachable from outside the module at all. The
 * corruption tests must put an entry into a state that no correct sequence of
 * calls can produce. Those tests exist to prove that the pool detects exactly
 * that state. The reach-in therefore lives here, and the test does not invent
 * a way to compute the address itself. */
void _ccol_mempool_corrupt_entry_status_for_tests(ccol_mempool *mp, void *entry,
                                                  unsigned char value) {
  if (!mp || !entry) return;
  if (!valid_mempool_addr(mp, (uintptr_t)entry)) return;
  ENTRY_STATUS(mp, ENTRY_INDEX(mp, (uintptr_t)entry)) = value;
}

/* The magazines that this pool holds out now. This tests that the exit of a
 * thread returns its slot. Without that, thread churn empties the budget. The
 * pool then quietly stops caching, and it still passes every functional
 * test. */
size_t _ccol_mempool_live_magazines_for_tests(ccol_mempool *mp) {
  if (!mp) return 0;
  size_t n;
  if (mp->should_use_locks) ccol_mutex_lock(mp->lock);
  n = mp->live_magazines;
  if (mp->should_use_locks) ccol_mutex_unlock(mp->lock);
  return n;
}

/* The magazines that the calling thread holds, across every pool. This tests
 * that the code reaps a magazine which a destroyed pool orphaned, and does not
 * let such magazines pile up. They stay reachable from thread-local storage,
 * so no leak checker reports them while the memory use climbs. */
size_t _ccol_mempool_thread_magazines_for_tests(void) {
  size_t n = 0;
  mp_tls_state_t *tls = _mp_tls_cur();
  for (ccol_mp_magazine *mag = tls ? tls->mags : NULL; mag;
       mag = mag->next_in_thread)
    n++;
  return n;
}

/* Runs what the module runs when it unloads, while the code stays mapped. A
 * test can then check what a pool and a thread do after the unload. */
void _ccol_mempool_unload_cache_for_tests(void) { _mp_cache_fini(); }

/* The slots that the pool allocated above the advertised count. This is zero
 * for a pool with no cache. */
size_t _ccol_mempool_reserve_for_tests(ccol_mempool *mp) {
  if (!mp) return 0;
  return mp->total_elem_count - mp->advertised_elem_count;
}
#endif /* RUNNING_UNIT_TESTS */

/* This destroys the memory pool. The call asserts when the dynamic fallback
 * was on and some dynamic entries are still not free. The assert makes that
 * leak visible. A loud crash is better than a silent loss of memory. The
 * function frees the backing objects buffer only when the caller did not give
 * that buffer as a preallocated one. */
void _ccol_mempool_destroy(ccol_mempool *mp) {
  if (mp) {
    /* Cut every magazine that is still out loose before anything else. Some
     * other thread owns each of them. That thread can be running right now,
     * and it frees the magazine from its own exit handler.
     *
     * Three things here are load-bearing. First, the code takes the registry
     * lock first. That lock is what stops the exit-time drain of a thread from
     * reading the pointer of this pool and then locking a mutex that this
     * function already destroyed. Second, the code unlinks each magazine
     * BEFORE it clears the pool pointer of that magazine. The owning thread
     * frees an orphaned magazine the moment that it sees NULL. In the other
     * order, that thread frees memory which is still on this list. Third, the
     * code does not drain the entries that a magazine holds, and that is
     * deliberate. The owning thread writes those entries with no
     * synchronization, so a read of them from here is a real data race. The
     * buffer that they point into is about to be freed in any case. */
    if (mp->max_magazines != 0) {
      ccol_mutex_lock(_mp_registry_lock);
      ccol_mutex_lock(mp->lock);
      ccol_mp_magazine *mag = mp->magazines;
      while (mag) {
        ccol_mp_magazine *next = mag->next_in_pool;
        mempool_magazine_unlink_locked(mp, mag);
        atomic_store_explicit(&mag->pool, NULL, memory_order_release);
        mag = next;
      }
      ccol_mutex_unlock(mp->lock);
      ccol_mutex_unlock(_mp_registry_lock);
    }

    if (mp->fallback_to_dynamic_memory) {
      if (ccol_mempool_dynamic_allocs_count(mp) > 0) {
        // This pool has dynamic entries that are still not free.
        // This is a leak. Make it noticed.
        ccol_assert(false);
      }
    }

    mp_dynamic_set_release(mp);

    if (mp->should_use_locks) {
      ccol_mutex_destroy(mp->lock);
    }

    if (!mp->is_preallocated && mp->objects) {
      _ccol_mem_free(mp->m_procs, mp->objects);
    }

    if (mp->m_procs) {
      ccol_free_t free_func = mp->m_procs->free;
      free_func(mp->m_procs);
      free_func(mp);
    } else {
      ccol_mem_free(mp);
    }
  }
}

/* This gives the distance between one entry and the next. It computes that
 * distance at run time, from the element size of the caller. It matches the
 * _ccol_mempool_stride() macro that the public buffer-declaring macros use. A
 * preallocated buffer and the pool built on it therefore always agree. The
 * macro exists on its own only because the size of a static array must be a
 * constant expression.
 *
 * This function returns 0 when no stride fits. Every caller treats that 0 as a
 * rejected elem_size, and no value wraps. */
static size_t mp_stride_for(size_t elem_size) {
  if (elem_size < sizeof(addr_t)) elem_size = sizeof(addr_t);
  if (!_ccol_mempool_align_up_fits(elem_size)) return 0;
  size_t base = _ccol_mempool_align_up(elem_size);
#if CCOL_MEMPOOL_COMPACT_LAYOUT
  return base;
#else
  size_t stride = 1;
  while (stride < base) {
    if (stride > SIZE_MAX / 2) return 0;
    stride <<= 1;
  }
  return stride;
#endif
}

/* This gives the bytes that one pool block takes. That is one stride for each
 * slot, then one status byte for each slot.
 *
 * The function reports the product through an out-parameter and does not
 * return it. It answers false and never a wrapped value. The reason is that
 * this code forms the size here and gives the allocator a single count that is
 * already multiplied. An allocator that is asked for count * size detects the
 * overflow itself. An allocator that is asked for one size cannot. A wrapped
 * product is therefore a small allocation, and a layout loop then walks
 * elem_count entries through it.
 *
 * _ccol_mempool_buffer_params_fit() checks the same expression at compile time
 * for a preallocated buffer. A count and size that the macro rejects are
 * therefore rejected here too. */
static bool mp_block_bytes(size_t count, size_t stride, size_t *out) {
  if (count == 0 || stride > (SIZE_MAX - count) / count) return false;
  *out = count * stride + count;
  return true;
}

#if CCOL_MEMPOOL_COMPACT_LAYOUT
/* This turns a division by the stride of a compact pool into a multiply and a
 * shift. It then proves the result before it accepts it.
 *
 * A reciprocal that is slightly wrong does not fail. It returns a neighbouring
 * ordinal. The pool then reads and writes the status byte of another entry,
 * for the life of the process. The check below is exhaustive and not
 * algebraic. Floor division and the multiply-shift both never decrease as the
 * offset rises. Agreement at every multiple of the stride, and at the byte
 * immediately below it, therefore forces agreement everywhere in between. That
 * is exactly what the bounds test needs. The bounds test must reject an
 * address that is inside the buffer but not on an entry boundary.
 *
 * This function returns false when no shift works. The code then does not
 * build the pool at all. It does not build a pool on arithmetic that nobody
 * can vouch for. */
static bool mp_build_reciprocal(size_t stride, size_t count, uintptr_t *magic,
                                uint8_t *shift) {
  if (stride == 0 || count == 0) return false;
  for (uint8_t s = 0; s < MP_WIDE_BITS; ++s) {
    mp_wide_t candidate = (((mp_wide_t)1 << (MP_WIDE_BITS + s)) / stride) + 1;
    if ((candidate >> MP_WIDE_BITS) != 0) continue; /* wider than a uintptr_t */
    uintptr_t m = (uintptr_t)candidate;

    bool ok = true;
    for (size_t i = 0; i <= count && ok; ++i) {
      uintptr_t at = (uintptr_t)i * (uintptr_t)stride;
      uintptr_t high_at = (uintptr_t)(((mp_wide_t)at * m) >> MP_WIDE_BITS);
      if ((size_t)(high_at >> s) != i) {
        ok = false;
      } else if (i > 0) {
        uintptr_t high_below =
            (uintptr_t)(((mp_wide_t)(at - 1) * m) >> MP_WIDE_BITS);
        if ((size_t)(high_below >> s) != i - 1) ok = false;
      }
    }
    if (ok) {
      *magic = m;
      *shift = s;
      return true;
    }
  }
  return false;
}
#endif

#if !CCOL_MEMPOOL_COMPACT_LAYOUT
static uint8_t mp_log2_exact(size_t v) {
  uint8_t r = 0;
  while ((size_t)1 << r < v) ++r;
  return r;
}
#endif

/* This lays out one contiguous block. The block holds elem_count entries of
 * stride bytes, then elem_count status bytes. The function threads every entry
 * onto the free list through the first bytes of that entry. The code does
 * not store the position of the status array. That array begins where the
 * entries end, which is upper_addr_limit. */
/* This returns false only under the compact layout, and only when no
   reciprocal for this stride and count can be proved. */
static bool ccol_mempool_init_internal_scalars(
    ccol_mempool *mp, size_t elem_count, size_t extended_elem_size,
    bool fallback_to_dynamic_memory) {
  uintptr_t base = (uintptr_t)mp->objects;

  mp->ccol_mempool_mark = _ccol_mempool_mark;
  mp->extended_elem_size = extended_elem_size;
#if CCOL_MEMPOOL_COMPACT_LAYOUT
  mp->entry_shift = 0;
  if (!mp_build_reciprocal(extended_elem_size, elem_count,
                           &mp->entry_index_magic, &mp->entry_index_shift)) {
    return false;
  }
#else
  mp->entry_shift = mp_log2_exact(extended_elem_size);
#endif
  mp->total_elem_count = elem_count;
  mp->fallback_to_dynamic_memory = fallback_to_dynamic_memory;
  mp->active_dynamic_memory_buffer_count = 0;
  mp->lower_addr_limit = base;
  mp->upper_addr_limit = base + extended_elem_size * elem_count;
  mp->free_elem_count = elem_count;

  uint8_t *status = (uint8_t *)mp->upper_addr_limit;
  for (size_t i = 0; i < elem_count; ++i) {
    void *entry = (void *)(base + i * extended_elem_size);
    status[i] = (uint8_t)mp_status_free;
    *(void **)entry = (i == elem_count - 1)
                          ? NULL
                          : (void *)(base + (i + 1) * extended_elem_size);
  }

  mp->free_inst = mp->objects;
  return true;
}

/* This creates a fixed-size memory pool for elem_count elements of elem_size
 * bytes each. An entry carries no header. The stride is therefore elem_size
 * itself, raised to sizeof(addr_t) when it is smaller, then aligned, then
 * rounded to a power of two. The raise is needed because a free entry stores
 * the free-list link in its own first bytes. One status byte for each entry
 * follows the entry array in the same block. */
ccol_mempool *ccol_mempool_create(size_t elem_count, size_t elem_size,
                                  bool fallback_to_dynamic_memory,
                                  bool single_threaded,
                                  ccol_memmgmt_procs_t *mmgmt_procs,
                                  char **err) {
  if (err) {
    *err = NULL;
  }

  if (elem_count == 0 || elem_size == 0) {
    if (err) {
      *err = CCOL_ERR_STR("elem_count or elem_size is zero");
    }
    return NULL;
  } else if (mp_stride_for(elem_size) == 0) {
    // Two roundings apply to elem_size here. The first is up to
    // _ccol_mempool_entry_align. Every entry past the first one in the
    // contiguous buffer of the pool then lands on an address that is aligned
    // for any object type. See the doc comment of _ccol_mempool_align_up() for
    // why that rounding is needed at all. The second is up to a power of two.
    // For this elem_size the two roundings overflow size_t. The stride wraps to
    // a value smaller than elem_size itself, and every entry index that the
    // free path computes then lands outside the allocated buffer.
    if (err) {
      *err = CCOL_ERR_STR(
          "elem_size is too large: the stride rounding would "
          "overflow size_t");
    }
    return NULL;
  } else if (elem_size < sizeof(addr_t)) {
    elem_size = sizeof(addr_t);
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  ccol_mempool *mp =
      (ccol_mempool *)_ccol_mem_calloc(mmgmt_procs, 1, sizeof(ccol_mempool));
  if (!mp) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate memory pool struct");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(mp, mmgmt_procs, err)) {
    _ccol_mem_free(mmgmt_procs, mp);
    return NULL;
  }

  size_t extended_elem_size = mp_stride_for(elem_size);
  if (extended_elem_size == 0) {
    if (err) {
      *err = CCOL_ERR_STR("elem_size is too large for a pool entry");
    }
    ccol_mempool_destroy(mp);
    return NULL;
  }

  /* One block holds the entries and, immediately after them, one status byte
     for each entry. The code therefore stores the position of the status array
     nowhere. The size is computed here, before the code derives any cache
     geometry. A count and size that together cannot be laid out at all are
     therefore rejected as a bad request from the caller. They are not reported
     as a failure to allocate. */
  size_t asked_bytes = 0;
  if (!mp_block_bytes(elem_count, extended_elem_size, &asked_bytes)) {
    if (err) {
      *err = CCOL_ERR_STR(
          "elem_count and elem_size are too large: the entries and their "
          "status bytes would overflow size_t");
    }
    ccol_mempool_destroy(mp);
    return NULL;
  }

  /* A thread cache needs a lock to refill against. The process-wide key that
   * it hangs off can also fail to be created, for a real reason such as a
   * process that already reached PTHREAD_KEYS_MAX. In either case the code
   * builds the pool without a cache. It does not fail to build the pool. */
  size_t max_magazines = 0, magazine_capacity = 0, reserve = 0;
  if (!single_threaded && _mp_cache_available())
    mempool_cache_geometry(elem_count, &max_magazines, &magazine_capacity,
                           &reserve);

  /* The reserve is what keeps the promise of the pool whole while entries sit
   * in thread caches. With at most `reserve` entries cached, the elem_count of
   * the caller stays available, no matter which thread asks. */
  size_t slot_count = elem_count + reserve;
  size_t slot_bytes = asked_bytes;
  if (reserve > 0 &&
      !mp_block_bytes(slot_count, extended_elem_size, &slot_bytes)) {
    /* The reserve is the addition of this library. A count whose block
     * fits only without the reserve must therefore still get the pool that it
     * asked for. The retry below, after a failed allocation, follows the same
     * reasoning. */
    max_magazines = 0;
    magazine_capacity = 0;
    reserve = 0;
    slot_count = elem_count;
    slot_bytes = asked_bytes;
  }

  mp->objects = _ccol_mem_calloc(mp->m_procs, 1, slot_bytes);
  if (!mp->objects && reserve > 0) {
    /* The reserve is the addition of this library. The code must not deny
     * a pool to a caller near its memory ceiling when that caller could
     * otherwise have had one. Drop the cache and retry at exactly what the
     * caller asked for. */
    max_magazines = 0;
    magazine_capacity = 0;
    reserve = 0;
    slot_count = elem_count;
    slot_bytes = asked_bytes;
    mp->objects = _ccol_mem_calloc(mp->m_procs, 1, slot_bytes);
  }
  if (!mp->objects) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate memory pool data area");
    }
    ccol_mempool_destroy(mp);
    return NULL;
  }

  mp->should_use_locks = !single_threaded;

  if (mp->should_use_locks) {
    if (ccol_mutex_init(mp->lock) != 0) {
      if (err) {
        *err = CCOL_ERR_STR("failed to initialize the mutex");
      }
      mp->should_use_locks = false;
      ccol_mempool_destroy(mp);
      return NULL;
    }
  }

  mp->max_magazines = max_magazines;
#if CCOL_MEMPOOL_COMPACT_LAYOUT
  mp->has_cache = (max_magazines != 0);
#endif
  mp->magazine_capacity = magazine_capacity;
  mp->advertised_elem_count = elem_count;

  if (!ccol_mempool_init_internal_scalars(mp, slot_count, extended_elem_size,
                                          fallback_to_dynamic_memory)) {
    if (err) {
      *err = CCOL_ERR_STR(
          "no verified reciprocal exists for this element size and count");
    }
    ccol_mempool_destroy(mp);
    return NULL;
  }

  return mp;
}

/* This is like ccol_mempool_create, but it uses a buffer from outside as the
 * element store. That buffer can be a static array, for example. The code
 * derives elem_count from buf_size divided by the extended element size. The
 * pool never frees the buffer. The caller stays responsible for its lifetime.
 * This is useful for an embedded pool, and for a pool on the stack.
 */
ccol_mempool *ccol_mempool_create_from_preallocated_buffer(
    void *buffer, size_t buf_size, size_t elem_size,
    bool fallback_to_dynamic_memory, bool single_threaded,
    ccol_memmgmt_procs_t *mmgmt_procs, char **err) {
  if (err) {
    *err = NULL;
  }

  /* This checks the pointer only. There is no fixed minimum size. A buffer has
     to hold one stride plus the status byte of that entry. That amount depends
     on elem_size, and the count computation below states it. A separate
     constant here could only disagree with that computation. */
  if (!buffer) {
    if (err) {
      *err = CCOL_ERR_STR("buffer is not acceptable");
    }
    return NULL;
  }

  // Entry 0 starts at the address of the buffer. The buffer therefore
  // decides whether the entries that this pool hands out meet the alignment
  // which the pool guarantees for them. The stride rounding only carries that
  // property from entry 0 to the rest. A buffer that comes from
  // CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER already meets this test, because
  // the macro aligns it to the same constant that the code checks here. A
  // weaker buffer would make the pool hand back entries that a caller cannot
  // store every object type in. That is undefined behavior, and a real fault
  // risk on strict-alignment architectures.
  if ((uintptr_t)buffer % _ccol_mempool_entry_align != 0) {
    if (err) {
      *err = CCOL_ERR_STR("buffer must be aligned to at least 16 bytes");
    }
    return NULL;
  }

  // This matches the three-way elem_size handling of ccol_mempool_create
  // exactly, and it matches the ORDER of the checks too. A real zero is a
  // mistake of the caller, and it deserves its own distinct error. An
  // elem_size too large for the stride rounding to apply without an overflow
  // of size_t comes next. Only after both of those are ruled out does the code
  // quietly round a small but nonzero size up, so that it holds the free-list
  // pointer. In practice the zero-size range and the overflow range can never
  // overlap the round-up range, because sizeof(addr_t) is tiny. But an
  // identical check order here, and not merely an equivalent one, means that
  // the two functions can never quietly drift apart when either threshold
  // changes.
  if (elem_size == 0) {
    if (err) {
      *err = CCOL_ERR_STR("elem_size is zero");
    }
    return NULL;
  } else if (mp_stride_for(elem_size) == 0) {
    // The code rounds elem_size up to _ccol_mempool_entry_align, and then up
    // to a power of two. For this elem_size those roundings overflow size_t.
    // The stride wraps to a value smaller than elem_size itself, and every
    // entry index that the free path computes then lands outside the buffer of
    // the caller. See the identical check in ccol_mempool_create, and the doc
    // comment of _ccol_mempool_align_up(), for the full explanation.
    if (err) {
      *err = CCOL_ERR_STR(
          "elem_size is too large: the stride rounding would "
          "overflow size_t");
    }
    return NULL;
  } else if (elem_size < sizeof(addr_t)) {
    elem_size = sizeof(addr_t);
  }

  size_t extended_elem_size = mp_stride_for(elem_size);
  /* Each element costs its stride plus the one status byte that sits in the
     same buffer. That is exactly what
     CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER sizes for. A buffer from that
     macro therefore gives exactly the element count that it was declared
     with. */
  size_t elem_count = buf_size / (extended_elem_size + 1);
  if (elem_count == 0) {
    if (err) {
      *err = CCOL_ERR_STR("calculated elem_count is zero");
    }
    return NULL;
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  ccol_mempool *mp =
      (ccol_mempool *)_ccol_mem_calloc(mmgmt_procs, 1, sizeof(ccol_mempool));
  if (!mp) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate ccol_mempool struct");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(mp, mmgmt_procs, err)) {
    _ccol_mem_free(mmgmt_procs, mp);
    return NULL;
  }

  mp->is_preallocated = true;
  mp->objects = buffer;

  mp->should_use_locks = !single_threaded;

  if (mp->should_use_locks) {
    if (ccol_mutex_init(mp->lock) != 0) {
      if (err) {
        *err = CCOL_ERR_STR("failed to init the rw lock");
      }
      mp->should_use_locks = false;
      ccol_mempool_destroy(mp);
      return NULL;
    }
  }

  mp->advertised_elem_count = elem_count;

  if (!ccol_mempool_init_internal_scalars(mp, elem_count, extended_elem_size,
                                          fallback_to_dynamic_memory)) {
    if (err) {
      *err = CCOL_ERR_STR(
          "no verified reciprocal exists for this element size and count");
    }
    ccol_mempool_destroy(mp);
    return NULL;
  }

  return mp;
}

/* The free-list half of an allocation: it pops the head of the free list, or
 * it makes a dynamic fallback entry when the pool is empty and the fallback is
 * on. locked says whether the caller holds mp->lock, which only a pool that
 * uses locks does. Every caller passes a constant, so each instantiation
 * carries no test of it.
 *
 * A pool with a thread cache owns slots above its advertised count. Its free
 * list can therefore be non-empty while the pool already handed out the whole
 * advertised count. The code treats that state as empty. This is what stops
 * the reserve from leaking into ordinary allocation, and stops the pool from
 * serving more entries than it promised. The reserve exists only to cover
 * entries that sit in the caches of other threads. A pool with no cache has no
 * reserve. There a non-empty free list already means that the count is not
 * used up, and the short circuit below keeps such a pool from paying for the
 * check. */
static inline __attribute__((always_inline)) void *mempool_serve(
    ccol_mempool *mp, bool locked) {
  void *result = NULL;

  bool can_serve = mp->free_inst != NULL;
  if (can_serve && MP_HAS_CACHE(mp)) {
    can_serve = mempool_live_count_locked(mp) < mp->advertised_elem_count;
  }

  if (__builtin_expect(can_serve, 1)) {
    void *entry = mp->free_inst;

    /* The entry comes from the free list of this pool, so it is in range by
       construction. The check here is that nothing corrupted the list into a
       pointer at something which is not a free entry of this pool. */
    if (__builtin_expect(
            !valid_mempool_addr(mp, (uintptr_t)entry) ||
                ENTRY_STATUS(mp, ENTRY_INDEX(mp, (uintptr_t)entry)) !=
                    mp_status_free,
            0)) {
      // There is a corruption.
      if (locked) {
        ccol_mutex_unlock(mp->lock);
      }
      ccol_assert(false);
    }

    mp->free_inst = *(void **)entry;
    ENTRY_STATUS(mp, ENTRY_INDEX(mp, (uintptr_t)entry)) =
        (uint8_t)mp_status_taken;
    result = entry;
    --mp->free_elem_count;
  } else if (mp->fallback_to_dynamic_memory) {
    // The pool used up its buffers, and the caller asked for a fallback to
    // the dynamic memory allocation mechanisms. A prefix on the allocation
    // holds the user-visible size of the entry. See
    // DYNAMIC_ENTRY_USER_SIZE. A later ccol_r_mempool_realloc_entry call can
    // therefore recover that size exactly.
    result = mempool_alloc_dynamic_locked(mp, mp->extended_elem_size);
  }

  return result;
}

/* The thread-cache path serves through this inside a critical section that it
 * already holds. */
static inline __attribute__((always_inline)) void *mempool_serve_locked(
    ccol_mempool *mp) {
  return mempool_serve(mp, true);
}

/* The two free-list paths of ccol_mempool_alloc_entry, each in a function of
 * its own that starts on a 64-byte boundary.
 *
 * The time of an allocation depends on how its hot instructions fall across
 * 64-byte lines, and not only on how many there are. Inline in the entry
 * point, each of these paths starts wherever the blocks ahead of it happen to
 * end, so an edit anywhere in the function, or a change of compiler, moves
 * it: the same instructions measured more than ten percent apart at two
 * offsets. Here each path starts at byte 0 of a line, whatever surrounds it.
 * The entry point reaches it with a tail jump, and the constant lock argument
 * removes every test of should_use_locks from the path, so the move costs
 * nothing that the path runs. */
static __attribute__((noinline, aligned(64))) void *mempool_alloc_unlocked(
    ccol_mempool *mp) {
  return mempool_serve(mp, false);
}

static __attribute__((noinline, aligned(64))) void *mempool_alloc_locked(
    ccol_mempool *mp) {
  ccol_mutex_lock(mp->lock);
  void *result = mempool_serve(mp, true);
  ccol_mutex_unlock(mp->lock);
  return result;
}

/* This allocates one element from the pool in O(1) time. It pops the head of
 * the internal free list. When the pool is empty and
 * fallback_to_dynamic_memory is on, the code makes a fresh heap allocation
 * instead. It tags that allocation as elem_is_not_a_pool_member, so that the
 * return path routes it through free.
 *
 * The function starts on a 64-byte boundary, and the thread-cache hit is its
 * straight-line fall-through, so the hit path occupies the same lines in every
 * build. */
__attribute__((aligned(64))) void *ccol_mempool_alloc_entry(ccol_mempool *mp) {
  if (!mp) {
    ccol_assert(false);
  }

  if (!mp->should_use_locks) {
    return mempool_alloc_unlocked(mp);
  }

  /* This test sits inside the lock test and not ahead of it. A single-threaded
   * pool never has a cache, and it never reaches this test at all. */
  if (MP_HAS_CACHE(mp)) {
    return mempool_alloc_from_cache(mp);
  }
  return mempool_alloc_locked(mp);
}

/* This allocates one element and zeros it before it returns. An entry carries
 * no header. The whole stride therefore belongs to the caller, and the
 * function zeros all of it. */
void *ccol_mempool_calloc_entry(ccol_mempool *mp) {
  void *result = ccol_mempool_alloc_entry(mp);

  if (result) {
    memset(result, 0, mp->extended_elem_size);
  }

  return result;
}

/* This checks whether c_entry falls inside the object buffer of the pool, and
 * whether it lands on a valid element boundary. A valid boundary is a multiple
 * of extended_elem_size from the base. The free path uses this to tell a
 * pool-owned entry from a dynamic fallback entry. */
/* Is this address one of the entries of mp, and exactly on an entry
 * boundary?
 *
 * The stride is a power of two, so the boundary test is a mask and not the
 * division that it would otherwise be. The offset which produces that mask is
 * the same one that the caller goes on to shift into an index. An address
 * below the buffer wraps when the subtraction is unsigned, so one upper
 * comparison covers both ends. The code keeps both comparisons. The
 * pseudo_pool has a range of zero width, and the explicit lower test states
 * that intent instead of a reliance on the wrap. */
/* Is c_entry the start of one of the entries of this pool?
 *
 * A position inside the buffer is not enough. The code must reject an address
 * partway into an entry too. Without that, the ordinal from such an address
 * names a neighbour, and the code reads the wrong status byte. The stride of
 * the default layout is a power of two, so there the test is one mask. The
 * compact layout multiplies the ordinal back out and compares, and the
 * verified reciprocal makes that comparison exact. */
static inline bool valid_mempool_addr(ccol_mempool *mp, uintptr_t c_entry) {
  if (c_entry < mp->lower_addr_limit || c_entry >= mp->upper_addr_limit) {
    return false;
  }
  uintptr_t off = c_entry - mp->lower_addr_limit;
#if CCOL_MEMPOOL_COMPACT_LAYOUT
  return (uintptr_t)ENTRY_INDEX(mp, c_entry) *
             (uintptr_t)mp->extended_elem_size ==
         off;
#else
  return (off & (mp->extended_elem_size - 1)) == 0;
#endif
}

/* Releases a dynamic fallback entry that the registry of mp has already
 * given up. The caller holds mp->lock when the pool uses locks.
 *
 * The one legitimate way that an address outside the buffer of a pool
 * reaches a free is a live dynamic fallback entry that this pool handed out,
 * and the registry of those entries answers that from the address alone. Any
 * other address, whether a stray pointer, an entry of another pool or a
 * dynamic entry that is already free, is not ours, and the code reads nothing
 * through it. The removal from the registry is the check: a second free of
 * the same entry finds it gone. Only after that check does this function read
 * the header in front of the entry. */
static inline __attribute__((always_inline)) void
mempool_release_dynamic_locked(ccol_mempool *mp, void *entry) {
  mp_dynamic_header *dyn = DYNAMIC_ENTRY_HEADER(entry);

  atomic_store_explicit(
      &mp->active_dynamic_memory_buffer_count,
      atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                           memory_order_relaxed) -
          1,
      memory_order_release);
  // The code marks the header before the block goes back to the allocator.
  // The registry already refuses a second free of the same block. The mark
  // keeps the freed state visible in a memory dump of a block that nothing
  // reused yet.
  dyn->elem_status = elem_is_freed_dynamic_member;
  _ccol_mem_free(mp->m_procs, DYNAMIC_ENTRY_RAW_BLOCK(entry));
}

/* The same check and release, for the ranged pool, which must ask each of
 * its pools in turn. It returns false, having touched nothing, when entry is
 * not a live dynamic entry of mp. */
static __attribute__((noinline)) bool mempool_free_dynamic_locked(
    ccol_mempool *mp, void *entry) {
  if (!mp_dynamic_set_remove_locked(mp, (uintptr_t)entry)) return false;
  mempool_release_dynamic_locked(mp, entry);
  return true;
}

/* The free path.
 *
 * The code establishes ownership from the address, before it reads anything
 * through that address. An entry carries no bookkeeping of its own to consult
 * first. An address inside the buffer of this pool is a pool entry, and its
 * state is in the status array. Any other address can only be a dynamic
 * fallback entry. The code enters that branch only once it knows that this
 * pool holds live dynamic entries. It therefore rejects a stray pointer
 * without a dereference of whatever the caller passed.
 */
/* The free-list half of a free. locked says whether the caller holds
 * mp->lock, which only a pool that uses locks does, and this function then
 * releases it. Every caller passes a constant. */
static inline __attribute__((always_inline)) void mempool_free_body(
    ccol_mempool *mp, void *entry, bool locked) {
  uintptr_t c_entry = (uintptr_t)entry;

  if (__builtin_expect(valid_mempool_addr(mp, c_entry), 1)) {
    size_t idx = ENTRY_INDEX(mp, c_entry);
    if (__builtin_expect(ENTRY_STATUS(mp, idx) != mp_status_taken, 0)) {
      // This is a double free when the entry is already marked free. Any
      // other status is corruption. Both are fatal, so the code asserts here
      // and does not branch further.
      if (locked) {
        ccol_mutex_unlock(mp->lock);
      }
      ccol_assert(false);
    }

    ENTRY_STATUS(mp, idx) = (uint8_t)mp_status_free;
    *(void **)entry = mp->free_inst;
    mp->free_inst = entry;
    ++mp->free_elem_count;
    if (locked) {
      ccol_mutex_unlock(mp->lock);
    }
    return;
  }

  /* The address is outside the buffer of this pool. The one legitimate way
   * that happens is a live dynamic fallback entry that this pool handed out.
   * A pool that holds no such entry rejects the address here. Otherwise the
   * registry decides, before anything reads through the address. */
  if (!mp->fallback_to_dynamic_memory ||
      atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                           memory_order_relaxed) == 0) {
    if (locked) {
      ccol_mutex_unlock(mp->lock);
    }
    ccol_assert(false);
  }

  if (!mp_dynamic_set_remove_locked(mp, c_entry)) {
    if (locked) {
      ccol_mutex_unlock(mp->lock);
    }
    ccol_assert(false);
  }

  mempool_release_dynamic_locked(mp, entry);

  if (locked) {
    ccol_mutex_unlock(mp->lock);
  }
}

/* The two free-list paths of a free, each in a function of its own that
 * starts on a 64-byte boundary, for the reason given at
 * mempool_alloc_unlocked. */
static __attribute__((noinline, aligned(64))) void mempool_free_unlocked(
    ccol_mempool *mp, void *entry) {
  mempool_free_body(mp, entry, false);
}

static __attribute__((noinline, aligned(64))) void mempool_free_locked(
    ccol_mempool *mp, void *entry) {
  ccol_mutex_lock(mp->lock);
  mempool_free_body(mp, entry, true);
}

static inline __attribute__((always_inline)) void __ccol_mempool_free_entry(
    ccol_mempool *mp, void *entry) {
  if (!mp) {
    ccol_assert(false);
  }

  if (!mp->should_use_locks) {
    mempool_free_unlocked(mp, entry);
    return;
  }

  /* This test sits inside the lock test for the same reason as in
   * ccol_mempool_alloc_entry. A pool with no cache must not pay for one. */
  if (MP_HAS_CACHE(mp)) {
    mempool_free_to_cache(mp, entry, (uintptr_t)entry);
    return;
  }
  mempool_free_locked(mp, entry);
}

/* The public entry point for a free. It accepts a NULL pointer and does not
 * assert, which matches the behaviour of the standard free. It verifies with
 * the ccol_mempool_mark sentinel that the caller named a real pool. It then
 * calls __ccol_mempool_free_entry, which does the real release. */
/* The function starts on a 64-byte boundary, and the thread-cache push is its
 * straight-line fall-through, so the hit path occupies the same lines in every
 * build. */
__attribute__((aligned(64))) void _ccol_mempool_free_entry(ccol_mempool *mp,
                                                           void *entry) {
  if (!entry) {
    // This follows the dynamic memory allocation approach. A free of a NULL
    // pointer is acceptable.
    return;
  }

  // A NULL pool is an error of the caller, and not a tolerated no-op. A NULL
  // entry has a free()-like reading, and a NULL pool has none. To continue
  // here means to guess which pool the caller meant.
  if (!mp) {
    ccol_assert(false);
  }

  if (mp->ccol_mempool_mark != _ccol_mempool_mark) {
    ccol_assert(false);
  }

  // The function below establishes ownership from the address. It reads
  // nothing through the pointer of the caller first.
  __ccol_mempool_free_entry(mp, entry);
}

/* This returns the total number of elements that the pool was sized for. That
 * is the free ones plus the ones in use. The function takes the read lock when
 * the pool is in multi-threaded mode. */
size_t ccol_mempool_total_capacity(ccol_mempool *mp) {
  if (!mp) {
    ccol_assert(false);
  }

  size_t result = 0;

  if (mp->should_use_locks) {
    ccol_mutex_lock(mp->lock);
  }

  /* This is the count that the caller asked for, and not the physical slot
   * count. A pool with a thread cache holds a reserve above this count. It
   * never hands out more than this many entries at one time. A report of the
   * physical count would therefore promise capacity that nobody can use. */
  result = mp->advertised_elem_count;

  if (mp->should_use_locks) {
    ccol_mutex_unlock(mp->lock);
  }

  return result;
}

/* This returns the number of pool-owned elements in use now, which is total
 * less free. The count does not include a dynamic fallback entry. */
size_t ccol_mempool_used_count(ccol_mempool *mp) {
  if (!mp) {
    ccol_assert(false);
  }

  size_t result = 0;

  if (mp->should_use_locks) {
    ccol_mutex_lock(mp->lock);
  }

  /* An entry that sits in a thread cache left the shared free list, and the
   * pool gave it to nobody. Such an entry is therefore neither free nor in
   * use. The subtraction of those entries is what keeps this function
   * reporting the entries that a caller truly holds. Without it, a single
   * allocation reports a whole refill batch as used. For a large pool that is
   * wrong by more than an order of magnitude.
   *
   * The code reads each count with a relaxed atomic load. The value can be
   * stale when its owning thread allocates at the same time. That staleness is
   * inherent in a count taken while the thing that it counts moves. The value
   * is never torn. It is exact whenever the pool is quiet. */
  result = mempool_live_count_locked(mp);

  if (mp->should_use_locks) {
    ccol_mutex_unlock(mp->lock);
  }

  return result;
}

/* This returns the number of dynamic fallback entries that are outstanding
 * now. A non-zero count means that the pool entries ran out at some point. */
size_t ccol_mempool_dynamic_allocs_count(ccol_mempool *mp) {
  if (!mp) {
    ccol_assert(false);
  }

  size_t result = 0;

  if (mp->should_use_locks) {
    ccol_mutex_lock(mp->lock);
  }

  result = atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                                memory_order_relaxed);

  if (mp->should_use_locks) {
    ccol_mutex_unlock(mp->lock);
  }

  return result;
}

// Ranged memory pool implementation starts
static const size_t min_allowed_smallest_size = 16;
// This is the highest power of two that a size_t can hold. It is
// ccol_max_power_of_two_size_t from common.h. That is 2^63 on a 64-bit size_t,
// and 2^31 on a 32-bit one. It is not SIZE_MAX / 2. SIZE_MAX itself is odd, so
// SIZE_MAX / 2 is one less than that power of two, for example 2^63 - 1 and
// not 2^63 on a 64-bit platform. SIZE_MAX / 2 therefore quietly rejects the
// single largest power-of-two size that the documented "must be <= 2^63 bytes"
// contract of this module promises.
static const size_t max_allowed_largest_size = ccol_max_power_of_two_size_t;

struct ccol_r_mempool {
  ccol_mempool **mem_pools;  // The real memory pools
  ccol_mempool pseudo_pool;
  ccol_r_memory_fallback_policy_t fb_policy;
  bool should_use_locks;
  ccol_memmgmt_procs_t *m_procs;
  size_t number_of_mempools;
  size_t smallest_size;
  size_t largest_size;
  size_t smallest_elem_count;
  // This sits beside smallest_size, which is its own linear value, 1 << this.
  // ccol_r_mempool_pool_index_for_size can therefore turn the "/smallest_size"
  // division that every size-to-tier lookup needs into an exact right shift.
  uint8_t smallest_size_power_of_two;
};

/* This destroys the ranged memory pool. The function asserts when the fallback
 * policy is ccol_fallback_at_last_exhaustion and outstanding dynamic entries
 * exist. That assert exposes the leak. _ccol_mempool_destroy follows the same
 * philosophy. The function destroys each internal sub-pool one at a time,
 * before it frees the sub-pool array. */
void _ccol_r_mempool_destroy(ccol_r_mempool *rmp) {
  if (rmp) {
    if (rmp->fb_policy == ccol_fallback_at_last_exhaustion) {
      if (ccol_mempool_dynamic_allocs_count(&rmp->pseudo_pool) > 0) {
        // There are dynamic pointers that are still not free.
        // That is a possible leak. Make it noticed.
        ccol_assert(false);
      }
    }

    if (rmp->mem_pools) {
      for (size_t i = 0; i < rmp->number_of_mempools; ++i) {
        if (rmp->mem_pools[i]) {
          ccol_mempool_destroy(rmp->mem_pools[i]);
        }
      }
      _ccol_mem_free(rmp->m_procs, rmp->mem_pools);
    }

    if (rmp->fb_policy == ccol_fallback_at_last_exhaustion) {
      mp_dynamic_set_release(&rmp->pseudo_pool);
      if (rmp->pseudo_pool.should_use_locks) {
        ccol_mutex_destroy(rmp->pseudo_pool.lock);
      }
    }

    if (rmp->m_procs) {
      ccol_free_t free_func = rmp->m_procs->free;
      free_func(rmp->m_procs);
      free_func(rmp);
    } else {
      ccol_mem_free(rmp);
    }
  }
}

/* This validates the power-of-two size parameters for a ccol_r_mempool. It
 * then derives the number of internal sub-pools. The constraint
 * smallest_elem_count_power_of_two >= (largest - smallest) makes sure that
 * each larger sub-pool in turn can have at least one element. The code divides
 * the count by 2 for each doubling of the size. The function fills in the rmp
 * fields on success. */
static bool assess_r_mempool_create_inputs(
    ccol_r_mempool *rmp, uint8_t smallest_size_power_of_two,
    uint8_t largest_size_power_of_two, uint8_t smallest_elem_count_power_of_two,
    ccol_r_memory_fallback_policy_t fb_policy, bool single_threaded,
    char **err) {
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

  if (fb_policy < 0 || fb_policy >= ccol_fallback_end_place_holder) {
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

/* This initializes the pseudo_pool inside rmp. That pseudo_pool is the
 * sentinel and the tracker for the global dynamic fallback entries under the
 * ccol_fallback_at_last_exhaustion policy. In that mode, the
 * active_dynamic_memory_buffer_count of the pseudo_pool tracks every dynamic
 * entry across every sub-pool. */
static bool init_r_mempool_pseudo_pool(ccol_r_mempool *rmp) {
  memset(&rmp->pseudo_pool, 0, sizeof(ccol_mempool));
  rmp->pseudo_pool.m_procs = rmp->m_procs;
  if (rmp->fb_policy == ccol_fallback_at_last_exhaustion) {
    if (rmp->should_use_locks) {
      if (ccol_mutex_init(rmp->pseudo_pool.lock) != 0) {
        return false;
      }
      rmp->pseudo_pool.should_use_locks = true;
    }
    rmp->pseudo_pool.fallback_to_dynamic_memory = true;
    rmp->pseudo_pool.ccol_mempool_mark = _ccol_mempool_mark;
  }

  return true;
}

/* This creates every sub-pool from smallest_size to largest_size. Each one
 * holds half as many elements as the one before it, which balances the twice
 * larger element size. The fallback policy of a sub-pool is
 * ccol_fallback_at_first_exhaustion only when the overall policy of the
 * ccol_r_mempool is first-exhaustion too. */
static bool init_r_mempool_internal_pools(ccol_r_mempool *rmp, char **err) {
  if (!init_r_mempool_pseudo_pool(rmp)) {
    if (err) {
      *err = CCOL_ERR_STR("failed to initialize the pseudo_pool");
    }
    return false;
  }

  rmp->mem_pools = (ccol_mempool **)_ccol_mem_alloc(
      rmp->m_procs, rmp->number_of_mempools * sizeof(ccol_mempool *));
  if (!rmp->mem_pools) {
    // The caller does the cleanup.
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate mem_pools array");
    }
    return false;
  }
  memset(rmp->mem_pools, 0, rmp->number_of_mempools * sizeof(ccol_mempool *));

  size_t first_size = rmp->smallest_size;
  size_t first_count = rmp->smallest_elem_count;

  for (size_t esize = first_size, ecount = first_count, index = 0;
       index < rmp->number_of_mempools; esize *= 2, ecount /= 2, ++index) {
    rmp->mem_pools[index] = ccol_mempool_create(
        ecount, esize, rmp->fb_policy == ccol_fallback_at_first_exhaustion,
        !rmp->should_use_locks, rmp->m_procs, err);
    if (!rmp->mem_pools[index]) {
      // The caller does the cleanup.
      return false;
    }
  }

  return true;
}

/* This maps an allocation size to the index of the sub-pool tier that should
 * serve it. It is true O(1): no table, no memory access at all, one shift and
 * one hardware bit scan. The size of every tier is smallest_size * 2^p. The
 * question "which tier does this size belong to" therefore reduces to "how
 * many bits does (size - 1) need, in units of smallest_size". That is exactly
 * a bit-length computation.
 *
 * A reverse lookup array computed in advance would answer the same question.
 * It would hold largest_size/smallest_size entries. That count is exponential
 * in the number of tiers, not linear. It is
 * 2^(largest_size_power_of_two - smallest_size_power_of_two). A caller that
 * picks a modest-looking size range, such as 16 bytes to 1 GiB over 26 tiers,
 * would need a lookup table of about 512 MiB to answer it. This formula needs
 * no table and no allocation. It therefore fails only in the cases that
 * ccol_r_mempool_alloc_entry and the other entry points already reject up
 * front. Those cases are size == 0 and size > largest_size. */
static inline size_t ccol_r_mempool_pool_index_for_size(ccol_r_mempool *rmp,
                                                        size_t size) {
  size_t t = (size - 1) >> rmp->smallest_size_power_of_two;
  if (t == 0) {
    // Here size <= smallest_size. That is always the smallest tier, index 0.
    return 0;
  }
  return (size_t)(sizeof(unsigned long long) * CHAR_BIT) -
         (size_t)__builtin_clzll((unsigned long long)t);
}

/* This creates a ranged memory pool. The pool covers the power-of-two sizes
 * from 2^smallest_size_power_of_two bytes to 2^largest_size_power_of_two
 * bytes. The pool with the smallest element size holds
 * 2^smallest_elem_count_power_of_two elements. A larger sub-pool holds
 * proportionally fewer elements. */
ccol_r_mempool *ccol_r_mempool_create(uint8_t smallest_size_power_of_two,
                                      uint8_t largest_size_power_of_two,
                                      uint8_t smallest_elem_count_power_of_two,
                                      ccol_r_memory_fallback_policy_t fb_policy,
                                      bool single_threaded,
                                      ccol_memmgmt_procs_t *mmgmt_procs,
                                      char **err) {
  if (err) {
    *err = NULL;
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  ccol_r_mempool *rmp = (ccol_r_mempool *)_ccol_mem_calloc(
      mmgmt_procs, 1, sizeof(ccol_r_mempool));
  if (!rmp) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate ccol_r_mempool struct");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(rmp, mmgmt_procs, err)) {
    _ccol_mem_free(mmgmt_procs, rmp);
    return NULL;
  }

  if (!assess_r_mempool_create_inputs(
          rmp, smallest_size_power_of_two, largest_size_power_of_two,
          smallest_elem_count_power_of_two, fb_policy, single_threaded, err)) {
    ccol_r_mempool_destroy(rmp);
    return NULL;
  }
  rmp->fb_policy = fb_policy;

  if (!init_r_mempool_internal_pools(rmp, err)) {
    ccol_r_mempool_destroy(rmp);
    return NULL;
  }

  return rmp;
}

/* This carves the preallocated_buffer into contiguous sub-buffer segments, one
 * for each sub-pool. It uses the same progression of sizes and counts as
 * init_r_mempool_internal_pools. It asserts that cumulative_size equals
 * preallocated_buffer_size. That assert makes sure that the caller gave a
 * buffer of exactly the right size. */
static bool init_preallocated_r_mempool_internal_pools(
    ccol_r_mempool *rmp, void *preallocated_buffer,
    size_t preallocated_buffer_size, char **err) {
  if (!init_r_mempool_pseudo_pool(rmp)) {
    if (err) {
      *err = CCOL_ERR_STR("failed to initialize the pseudo_pool");
    }
    return false;
  }

  size_t first_size = rmp->smallest_size;
  size_t first_count = rmp->smallest_elem_count;

  /* This validates the total buffer size before it touches any sub-buffer. A
   * buffer of the wrong size therefore never causes an out-of-bounds write
   * while the code initializes a sub-pool. The code checks the term of each
   * tier, and the running sum, for overflow one at a time, before it forms
   * them. For a huge and physically impossible set of parameters, the true
   * required buffer size can itself exceed SIZE_MAX. A plain multiply and
   * accumulate then wraps in silence. A preallocated_buffer_size that is too
   * small can therefore pass the expected_size comparison below, instead of
   * being rejected. The second loop further down recomputes esize and ecount
   * through the exact same deterministic progression. A confirmation here that
   * no term and no partial sum overflows is therefore enough to guarantee that
   * the arithmetic of the second loop cannot overflow either. */
  size_t expected_size = 0;
  for (size_t esize = first_size, ecount = first_count, index = 0;
       index < rmp->number_of_mempools; esize *= 2, ecount /= 2, ++index) {
    if (mp_stride_for(esize) == 0) {
      if (err) {
        *err = CCOL_ERR_STR(
            "requested configuration's buffer size overflows size_t");
      }
      return false;
    }
    size_t elem_extended_size = mp_stride_for(esize);
    if (!_ccol_mempool_align_up_fits(ecount)) {
      if (err) {
        *err = CCOL_ERR_STR(
            "requested configuration's buffer size overflows size_t");
      }
      return false;
    }
    size_t status_bytes = _ccol_mempool_align_up(ecount);
    if (ecount != 0 &&
        elem_extended_size > (SIZE_MAX - status_bytes) / ecount) {
      if (err) {
        *err = CCOL_ERR_STR(
            "requested configuration's buffer size overflows size_t");
      }
      return false;
    }
    size_t term = ecount * elem_extended_size + status_bytes;
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

  rmp->mem_pools = (ccol_mempool **)_ccol_mem_calloc(
      rmp->m_procs, rmp->number_of_mempools, sizeof(ccol_mempool *));
  if (!rmp->mem_pools) {
    // The caller does the cleanup.
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate mem_pools array");
    }
    return false;
  }

  size_t cumulative_size = 0;

  for (size_t esize = first_size, ecount = first_count, index = 0;
       index < rmp->number_of_mempools; esize *= 2, ecount /= 2, ++index) {
    // The code uses adjacent segments of the preallocated buffer, each with a
    // different size, to hold the different pools of memory.
    uint8_t *sub_buffer = (uint8_t *)preallocated_buffer + cumulative_size;
    /* This is one stride for each entry, plus one status byte for each entry.
       The code rounds the status region up, so that the first entry of the
       next sub-pool stays aligned. The rounding is slack that this sub-pool
       never reads. Its element count comes from the stride and the status byte
       alone. The extra bytes therefore cannot make it claim an entry that the
       segment does not hold. */
    size_t sub_buffer_size =
        ecount * mp_stride_for(esize) + _ccol_mempool_align_up(ecount);
    rmp->mem_pools[index] = ccol_mempool_create_from_preallocated_buffer(
        sub_buffer, sub_buffer_size, esize,
        rmp->fb_policy == ccol_fallback_at_first_exhaustion,
        !rmp->should_use_locks, rmp->m_procs, err);
    if (!rmp->mem_pools[index]) {
      // The caller does the cleanup.
      return false;
    }
    cumulative_size += sub_buffer_size;
  }

  return true;
}

/* This is like ccol_r_mempool_create, but it uses a buffer from outside for
 * the element storage of every sub-pool. The ccol_r_mempool does not free the
 * buffer. The caller is responsible for its lifetime. */
ccol_r_mempool *ccol_r_mempool_create_from_preallocated_buffer(
    void *buffer, size_t buf_size, uint8_t smallest_size_power_of_two,
    uint8_t largest_size_power_of_two, uint8_t smallest_elem_count_power_of_two,
    ccol_r_memory_fallback_policy_t fb_policy, bool single_threaded,
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

  // The code checks this once, up front. The reason is the same one that
  // ccol_mempool_create_from_preallocated_buffer has. Every entry that a
  // sub-pool hands out must meet _ccol_mempool_entry_align. The creation call
  // of each sub-pool would derive this again in the end. The offset of the own
  // segment of a sub-pool from the buffer start is always a multiple of that
  // alignment. A misalignment can therefore only come from the start
  // address of the buffer. But a check here gives one immediate error with a
  // clear cause. Without it, the failure appears from deep inside the
  // construction of a sub-pool.
  if ((uintptr_t)buffer % _ccol_mempool_entry_align != 0) {
    if (err) {
      *err = CCOL_ERR_STR("buffer must be aligned to at least 16 bytes");
    }
    return NULL;
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  ccol_r_mempool *rmp = (ccol_r_mempool *)_ccol_mem_calloc(
      mmgmt_procs, 1, sizeof(ccol_r_mempool));
  if (!rmp) {
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate ccol_r_mempool struct");
    }
    return NULL;
  }

  if (!ccol_populate_mem_mgmt_procs(rmp, mmgmt_procs, err)) {
    _ccol_mem_free(mmgmt_procs, rmp);
    return NULL;
  }

  if (!assess_r_mempool_create_inputs(
          rmp, smallest_size_power_of_two, largest_size_power_of_two,
          smallest_elem_count_power_of_two, fb_policy, single_threaded, err)) {
    ccol_r_mempool_destroy(rmp);
    return NULL;
  }
  rmp->fb_policy = fb_policy;

  if (!init_preallocated_r_mempool_internal_pools(rmp, buffer, buf_size, err)) {
    ccol_r_mempool_destroy(rmp);
    return NULL;
  }

  return rmp;
}

/* This allocates a dynamic entry through the pseudo_pool and tags it as
 * elem_is_not_a_pool_member. The pseudo_pool is the tracker for the global
 * fallback entries under the ccol_fallback_at_last_exhaustion policy. The
 * pseudo_pool itself has no object buffer. It keeps only the count and the
 * registry of its dynamic entries. The pseudo_pool has no fixed element size
 * of its own. A real sub-pool does, and an entry of a real sub-pool always
 * recovers its size from the extended_elem_size of that sub-pool. Every
 * pseudo_pool entry therefore records its own user-visible size in the header
 * ahead of it. A later ccol_r_mempool_realloc_entry call can then recover
 * exactly how many bytes are safe to copy out of it. It does not have to guess
 * the count, and it does not have to discard the data entirely.
 *
 * ccol_mempool_create and ccol_mempool_create_from_preallocated_buffer round
 * elem_size up to sizeof(addr_t). This function deliberately does NOT. That
 * rounding exists only so that a POOL-OWNED entry is always large enough to
 * serve as a free-list node. Such an entry holds the link to the next free
 * entry in its own first bytes while it sits free. A pseudo_pool entry is a
 * one-off heap allocation. Nothing ever links it into a free list. The code
 * hands it to _ccol_mem_free once the caller releases it. It therefore has no
 * such minimum size. A rounding here would also corrupt the size prefix of
 * the entry, against what the caller truly asked for.
 * ccol_r_mempool_realloc_entry has a fast path for "same size, no move
 * needed". For a pseudo_pool entry, that path compares the newly requested
 * size of the caller directly against the recorded size. A recorded size other
 * than exactly what the caller requested therefore makes that comparison fail
 * for any request below sizeof(addr_t) bytes. It fails even when nothing about
 * the request changed, and that quietly defeats the fast path on every such
 * call. */
static void *ccol_mempool_pseudo_alloc_entry(ccol_mempool *mp,
                                             size_t elem_size) {
  void *result = NULL;

  // The code checks this before it forms the allocation size. Take an
  // elem_size that is within DYNAMIC_ENTRY_PREFIX_SIZE of SIZE_MAX. The
  // addition of the prefix to it wraps to a small request, and that request
  // succeeds. The size prefix a few lines below still records the original,
  // huge elem_size. The result is a dangerously small block whose recorded
  // size lies about its real capacity to any later
  // ccol_r_mempool_realloc_entry call.
  if (elem_size > SIZE_MAX - DYNAMIC_ENTRY_PREFIX_SIZE) {
    return NULL;
  }

  if (mp->should_use_locks) {
    ccol_mutex_lock(mp->lock);
  }

  result = mempool_alloc_dynamic_locked(mp, elem_size);

  if (mp->should_use_locks) {
    ccol_mutex_unlock(mp->lock);
  }

  return result;
}

/* This allocates a buffer of at least size bytes.
 * ccol_r_mempool_pool_index_for_size finds the smallest sub-pool whose element
 * size fits size. When that sub-pool is empty, the code tries the next larger
 * one, and so on up the tiers. The ccol_fallback_at_last_exhaustion path runs
 * only when every sub-pool is empty. */
void *ccol_r_mempool_alloc_entry(ccol_r_mempool *rmp, size_t size) {
  if (!rmp) {
    ccol_assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return NULL;
  }

  size_t pool_index = ccol_r_mempool_pool_index_for_size(rmp, size);

  void *result = NULL;

  for (; pool_index < rmp->number_of_mempools; ++pool_index) {
    result = ccol_mempool_alloc_entry(rmp->mem_pools[pool_index]);
    if (result) {
      break;
    }
  }

  if (!result && rmp->fb_policy == ccol_fallback_at_last_exhaustion) {
    result = ccol_mempool_pseudo_alloc_entry(&rmp->pseudo_pool, size);
  }

  return result;
}

/* This allocates a buffer of at least size bytes from the ccol_r_mempool and
 * zeros it. */
void *ccol_r_mempool_calloc_entry(ccol_r_mempool *rmp, size_t size) {
  void *result = ccol_r_mempool_alloc_entry(rmp, size);

  if (result) {
    memset(result, 0, size);
  }

  return result;
}

/* An entry inside the buffer of the pool is exactly one stride wide. The
 * rounding belongs to the pool, but the bytes belong to whoever holds the
 * entry. Any other address that reaches here is a dynamic fallback entry. The
 * size of such an entry differs for each allocation, and its own header
 * records that size. */
static inline size_t entry_user_size(ccol_mempool *mp, void *entry) {
  if (valid_mempool_addr(mp, (uintptr_t)entry)) {
    return mp->extended_elem_size;
  }
  return DYNAMIC_ENTRY_USER_SIZE(entry);
}

/* Does this ranged pool hold any dynamic fallback entry at all? This covers
 * both policies. ccol_fallback_at_first_exhaustion keeps such entries in each
 * sub-pool. ccol_fallback_at_last_exhaustion keeps them on the pseudo_pool.
 * The code uses this to decide whether an address that matched no tier can
 * legitimately belong to this pool. It decides that before it reads anything
 * through the address. */
static bool rmp_holds_dynamic_entries(ccol_r_mempool *rmp) {
  if (atomic_load_explicit(&rmp->pseudo_pool.active_dynamic_memory_buffer_count,
                           memory_order_acquire) > 0) {
    return true;
  }
  for (size_t i = 0; i < rmp->number_of_mempools; ++i) {
    if (rmp->mem_pools[i] &&
        atomic_load_explicit(
            &rmp->mem_pools[i]->active_dynamic_memory_buffer_count,
            memory_order_acquire) > 0) {
      return true;
    }
  }
  return false;
}

/* Asks the registry of each pool of rmp that holds dynamic entries whether
 * c_entry is one of them. Each registry is read under the lock of its own
 * pool, one pool at a time, so no two of those locks are ever held together.
 * The answer can go stale once that lock is released. The free path asks its
 * own pool again under the lock, and that answer is the one it acts on. */
static ccol_mempool *rmp_dynamic_owner_of(ccol_r_mempool *rmp,
                                          uintptr_t c_entry) {
  for (size_t i = 0; i <= rmp->number_of_mempools; ++i) {
    ccol_mempool *mp =
        i < rmp->number_of_mempools ? rmp->mem_pools[i] : &rmp->pseudo_pool;
    if (!mp || !mp->fallback_to_dynamic_memory ||
        atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                             memory_order_acquire) == 0) {
      continue;
    }
    if (mp->should_use_locks) ccol_mutex_lock(mp->lock);
    bool found = mp_dynamic_set_contains_locked(mp, c_entry);
    if (mp->should_use_locks) ccol_mutex_unlock(mp->lock);
    if (found) return mp;
  }
  return NULL;
}

/* Frees entry when one of the pools of rmp holds it as a live dynamic entry.
 * It locks each candidate once, one pool at a time, and the pool that holds
 * the entry frees it inside that same critical section. It returns false,
 * having read nothing through entry, when no pool holds it. */
static __attribute__((noinline)) bool rmp_free_dynamic(ccol_r_mempool *rmp,
                                                       void *entry) {
  for (size_t i = 0; i <= rmp->number_of_mempools; ++i) {
    ccol_mempool *mp =
        i < rmp->number_of_mempools ? rmp->mem_pools[i] : &rmp->pseudo_pool;
    if (!mp || !mp->fallback_to_dynamic_memory ||
        atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                             memory_order_acquire) == 0) {
      continue;
    }
    if (mp->should_use_locks) ccol_mutex_lock(mp->lock);
    bool freed = mempool_free_dynamic_locked(mp, entry);
    if (mp->should_use_locks) ccol_mutex_unlock(mp->lock);
    if (freed) return true;
  }
  return false;
}

/* This resolves an address to the sub-pool of rmp that owns it.
 *
 * The code identifies a tier from the address alone. It therefore reads
 * nothing through the pointer of the caller to find that tier. An address that
 * matches no tier can still be a dynamic fallback entry. That is only possible
 * when this ranged pool holds such entries. When it holds none, the answer is
 * "not ours". When it holds some, the registries of its pools answer from the
 * address alone as well. The code never touches the header of an address that
 * is not a live dynamic entry of rmp. *is_dynamic reports which of the two
 * kinds the function found. */
static ccol_mempool *rmp_owner_of(ccol_r_mempool *rmp, void *entry,
                                  bool *is_dynamic) {
  *is_dynamic = false;
  for (size_t i = 0; i < rmp->number_of_mempools; ++i) {
    ccol_mempool *sub = rmp->mem_pools[i];
    if (sub && valid_mempool_addr(sub, (uintptr_t)entry)) return sub;
  }

  if (!rmp_holds_dynamic_entries(rmp)) return NULL;

  ccol_mempool *owner = rmp_dynamic_owner_of(rmp, (uintptr_t)entry);
  if (!owner) return NULL;
  *is_dynamic = true;
  return owner;
}

#ifdef RUNNING_UNIT_TESTS
/* This is the ranged counterpart of
 * _ccol_mempool_corrupt_entry_status_for_tests. The corruption tests hold a
 * ccol_r_mempool and one of its entries. This function resolves which sub-pool
 * owns that entry, from the address, exactly as the free and realloc paths
 * do. */
void _ccol_r_mempool_corrupt_entry_status_for_tests(ccol_r_mempool *rmp,
                                                    void *entry,
                                                    unsigned char value) {
  if (!rmp || !entry) return;
  for (size_t i = 0; i < rmp->number_of_mempools; ++i) {
    ccol_mempool *sub = rmp->mem_pools[i];
    if (sub && valid_mempool_addr(sub, (uintptr_t)entry)) {
      ENTRY_STATUS(sub, ENTRY_INDEX(sub, (uintptr_t)entry)) = value;
      return;
    }
  }
}
#endif /* RUNNING_UNIT_TESTS */

void _ccol_r_mempool_free_entry(ccol_r_mempool *rmp, void *entry) {
  if (!entry) {
    return;
  }

  if (!rmp) {
    ccol_assert(false);
  }

  for (size_t i = 0; i < rmp->number_of_mempools; ++i) {
    ccol_mempool *sub = rmp->mem_pools[i];
    if (sub && valid_mempool_addr(sub, (uintptr_t)entry)) {
      __ccol_mempool_free_entry(sub, entry);
      return;
    }
  }

  /* The address matches no tier. It is either a live dynamic entry of one of
   * the pools of rmp, or it is not ours. */
  if (!rmp_free_dynamic(rmp, entry)) {
    ccol_assert(false);
  }
}

/* This reallocates addr to a buffer of at least size bytes. When the entry
 * already holds as many bytes as a move could give it, the function returns
 * the original pointer unchanged and copies nothing. In every other case it
 * allocates a new entry, copies the smaller of the old and new user sizes, and
 * frees the old entry.
 *
 * Both questions below are about the bytes that the entry truly holds. A
 * pool-owned entry and a dynamic fallback entry of the same capacity therefore
 * answer them in the same way. Neither kind is a special case. */
void *ccol_r_mempool_realloc_entry(ccol_r_mempool *rmp, void *addr,
                                   size_t size) {
  if (!rmp) {
    ccol_assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return NULL;
  }

  ccol_mempool *owner = NULL;
  bool owner_is_dynamic = false;
  size_t held = 0;

  if (addr) {
    /* The code validates everything before the fast path below, which moves
     * nothing and returns addr unchanged. That path can therefore never hand
     * back an entry which is already free, or an entry that never belonged to
     * this pool. Ownership comes from the address. The code then checks the
     * status of a pool-owned entry in the status array of its pool. The
     * resolve step already checked the status of a dynamic entry. */
    owner = rmp_owner_of(rmp, addr, &owner_is_dynamic);
    if (!owner) {
      ccol_assert(false);
    }
    if (!owner_is_dynamic &&
        ENTRY_STATUS(owner, ENTRY_INDEX(owner, (uintptr_t)addr)) !=
            mp_status_taken) {
      ccol_assert(false);
    }

    /* This is how many bytes the entry holds. A pool-owned entry holds the
     * whole stride of its tier. A dynamic fallback entry holds what its own
     * header records. That is the same stride when a tier handed the entry
     * out, and exactly the requested size when the pseudo_pool handed it out.
     * The code resolves this from owner_is_dynamic and not through
     * entry_user_size(). It therefore reuses the answer that rmp_owner_of()
     * already produced, and does not test the address against the bounds a
     * second time. */
    held = owner_is_dynamic ? DYNAMIC_ENTRY_USER_SIZE(addr)
                            : owner->extended_elem_size;

    /* A move is worth making only when it changes how many bytes the entry
     * holds. The tier that the request names serves it, and the stride of that
     * tier is new_ext_size. Once every tier is empty, a fallback block of
     * exactly size bytes serves it instead. An entry that already holds either
     * amount is as large as the move would make it. Such an entry stays where
     * it is, and the code copies no bytes. Every other request truly changes
     * the size. A shrink across a tier boundary gives back the slot of the
     * larger tier, or the larger heap block. A grow has no choice. */
    size_t new_ext_size =
        rmp->mem_pools[ccol_r_mempool_pool_index_for_size(rmp, size)]
            ->extended_elem_size;

    if (held == new_ext_size || held == size) {
      return addr;
    }
  }

  void *new_entry = ccol_r_mempool_alloc_entry(rmp, size);
  if (!new_entry && addr && size <= held) {
    /* The move failed, but it was never needed. This entry already has room
       for the requested size where it sits. The check above compares the IDEAL
       tier for the new size against the bytes that this entry holds. Every
       entry that holds more than that tier would give it therefore arrives
       here. An ordinary shrink across a tier boundary arrives here. So does an
       entry that moved up into a larger tier because its own tier was full.
       So does a dynamic fallback entry that a caller asks for fewer bytes than
       it received. A return of the entry unmoved is what realloc(3) does for a
       request that fits. It also keeps a shrink from failing for want of
       memory that it does not need. The entry keeps the capacity that it
       already had, and entry_user_size goes on reporting that capacity for
       it. */
    return addr;
  }
  if (new_entry && addr) {
    // The code reads both sizes back from the entries that are truly
    // involved. It never infers them from the *ideal* target tier for `size`.
    // This is therefore correct for every kind of entry. It is correct for a
    // pool-owned entry, for an entry that moved up into a larger tier, and for
    // an entry that went through the pseudo_pool under
    // ccol_fallback_at_last_exhaustion. That last entry can legitimately hold
    // far fewer bytes than the ideal tier for its own size suggests.
    bool new_is_dynamic = false;
    ccol_mempool *new_owner = rmp_owner_of(rmp, new_entry, &new_is_dynamic);
    size_t old_user_size = held;
    size_t new_user_size =
        new_owner ? entry_user_size(new_owner, new_entry) : 0;
    size_t copy_size =
        old_user_size < new_user_size ? old_user_size : new_user_size;

    memcpy(new_entry, addr, copy_size);
    /* This frees through the sub-pool that owns the entry, and not through
       rmp. It is an internal free of an entry that this function already
       resolved, so it needs no second ownership check. */
    ccol_mempool_free_entry(owner, addr);
  }

  return new_entry;
}

/* This returns the used element count of the sub-pool that serves allocations
 * of the given size. It returns 0 for a size outside the range of the pool. */
size_t ccol_r_mempool_used_count(ccol_r_mempool *rmp, size_t size) {
  if (!rmp) {
    ccol_assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return 0;
  }

  return ccol_mempool_used_count(
      rmp->mem_pools[ccol_r_mempool_pool_index_for_size(rmp, size)]);
}

/* This returns the total element capacity of the sub-pool that serves
 * allocations of the given size. It returns 0 for a size outside the range of
 * the pool. */
/* This is the size of the entry block in bytes. It is the stride of every
 * slot, plus the status byte of every slot. The formula uses total_elem_count
 * and not advertised_elem_count, and that is deliberate. This call exists to
 * report the memory that the pool truly takes, the reserve included. That is
 * the one number ccol_mempool_total_capacity() cannot report. Reporting it
 * there would promise capacity that the pool never hands out. */
size_t ccol_mempool_allocated_bytes(ccol_mempool *mp) {
  if (!mp) {
    ccol_assert(false);
  }

  size_t result = 0;

  if (mp->should_use_locks) {
    ccol_mutex_lock(mp->lock);
  }

  result = mp->total_elem_count * mp->extended_elem_size + mp->total_elem_count;

  if (mp->should_use_locks) {
    ccol_mutex_unlock(mp->lock);
  }

  return result;
}

/* This is the sum over every tier. The pseudo_pool carries no entry block of
 * its own, so it adds nothing. */
size_t ccol_r_mempool_allocated_bytes(ccol_r_mempool *rmp) {
  if (!rmp) {
    ccol_assert(false);
  }

  size_t result = 0;
  for (size_t i = 0; i < rmp->number_of_mempools; ++i) {
    if (rmp->mem_pools[i]) {
      result += ccol_mempool_allocated_bytes(rmp->mem_pools[i]);
    }
  }
  return result;
}

size_t ccol_r_mempool_total_capacity(ccol_r_mempool *rmp, size_t size) {
  if (!rmp) {
    ccol_assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return 0;
  }

  return ccol_mempool_total_capacity(
      rmp->mem_pools[ccol_r_mempool_pool_index_for_size(rmp, size)]);
}

/* Returns the number of outstanding dynamic fallback allocations for the
 * given size. For ccol_fallback_at_last_exhaustion, the count is held in the
 * single pseudo_pool regardless of size. For ccol_fallback_at_first_exhaustion,
 * the count is per-sub-pool. Returns 0 for ccol_fallback_disabled or
 * out-of-range sizes. */
size_t ccol_r_mempool_dynamic_allocs_count(ccol_r_mempool *rmp, size_t size) {
  if (!rmp) {
    ccol_assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return 0;
  }

  if (rmp->fb_policy == ccol_fallback_disabled) {
    return 0;
  }

  if (rmp->fb_policy == ccol_fallback_at_first_exhaustion) {
    return ccol_mempool_dynamic_allocs_count(
        rmp->mem_pools[ccol_r_mempool_pool_index_for_size(rmp, size)]);
  }

  return ccol_mempool_dynamic_allocs_count(&rmp->pseudo_pool);
}
