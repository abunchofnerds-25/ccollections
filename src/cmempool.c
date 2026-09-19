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
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The other half of the link-time layout check in cmempool.h. Exactly one of
   these exists in a given build, and a translation unit compiled with the other
   setting references a name that is not here. */
#if CCOL_MEMPOOL_COMPACT_LAYOUT
const char _ccol_mempool_built_with_compact_layout[1] = {0};
#else
const char _ccol_mempool_built_with_fast_layout[1] = {0};
#endif

const char *_ccol_mempool_mark = "ccol_mempool";

typedef uintptr_t *addr_t;

struct ccol_mp_magazine;

/* A pool-owned entry carries no header at all. The address the caller receives
 * is the entry itself, its index is (entry - lower_addr_limit) >> entry_shift,
 * and its state lives in the status array that begins at upper_addr_limit. A
 * free entry's own first bytes hold the free-list link, which is why the
 * minimum element size is sizeof(uintptr_t).
 *
 * A pool entry's user-visible size is its whole stride: the rounding is the
 * pool's, but the bytes belong to whoever holds the entry. */
#if CCOL_MEMPOOL_COMPACT_LAYOUT
#define MP_HAS_CACHE(mp) ((mp)->has_cache)
/* The widest product the reciprocal needs. A 32-bit target's offsets fit in 32
   bits so a 64-bit intermediate keeps the high half; a 64-bit target's do not,
   and __uint128_t is the only way to hold it. */
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

/* A dynamic fallback entry is not in any pool's buffer, so it cannot be
 * indexed and cannot use the status array. It keeps a header of its own,
 * immediately before the address handed to the caller, recording the state a
 * pool entry keeps in the status array plus the user-visible size that a pool
 * entry recovers from its pool's fixed stride.
 *
 * Only ever reached after an address has failed the owning pool's bounds test
 * AND that pool has been shown to hold live dynamic entries. Reading it for an
 * address that satisfies neither would be dereferencing whatever the caller
 * passed. */
typedef struct {
  size_t user_size;
  size_t elem_status;
  ccol_mempool *pool_ptr;
} mp_dynamic_header;

/* Rounded to the alignment every entry is guaranteed to meet, not left at
 * sizeof(mp_dynamic_header). The address handed to the caller sits this many
 * bytes past the block the allocator returned, so a prefix that is not itself a
 * multiple of the alignment carries the misalignment straight through to the
 * caller, however well aligned the underlying block was.
 *
 * This assumes the allocator behind m_procs returns memory aligned for any
 * object type, which is what malloc() guarantees and what a replacement is
 * expected to match; the guarantee for fallback entries can be no stronger than
 * what that allocator provides. */
#define DYNAMIC_ENTRY_PREFIX_SIZE \
  _ccol_mempool_align_up(sizeof(mp_dynamic_header))

#define DYNAMIC_ENTRY_HEADER(entry) \
  ((mp_dynamic_header *)((uint8_t *)(entry) - DYNAMIC_ENTRY_PREFIX_SIZE))

#define DYNAMIC_ENTRY_RAW_BLOCK(entry) \
  ((void *)((uint8_t *)(entry) - DYNAMIC_ENTRY_PREFIX_SIZE))

/* Defined below, next to the free path it primarily serves; declared here
 * because the magazine and the size helper above it both ask the question. */
static inline bool valid_mempool_addr(ccol_mempool *mp, uintptr_t c_entry);

#define DYNAMIC_ENTRY_USER_SIZE(entry) (DYNAMIC_ENTRY_HEADER(entry)->user_size)

struct ccol_mempool {
  /* ---- one cache line, touched by every allocation and free -------------
   * Exactly 64 bytes on LP64, and the grouping is measured rather than
   * guessed. free_inst and free_elem_count belong here despite being mutable:
   * a single-threaded pool, and any pool without a thread cache, reaches them
   * on every operation with no lock in between, so demoting them to a second
   * line costs those pools directly. max_magazines belongs here because it is
   * the branch every operation starts with. magazine_capacity does not: it is
   * read once when a thread first claims a magazine, and each magazine keeps
   * its own copy thereafter. */
  const char *ccol_mempool_mark;  // This field is used for sanity checks
#if CCOL_MEMPOOL_COMPACT_LAYOUT
  /* Turns an entry's byte offset into its ordinal. The compact stride is not a
     power of two, so this is what stands in for the shift the default layout
     uses, and it occupies the eight bytes max_magazines gives up below. */
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
  /* log2 of extended_elem_size in the default layout, where the stride is a
   * power of two, so the free path turns an entry's offset into its index with
   * a shift rather than a division. Zero under CCOL_MEMPOOL_COMPACT_LAYOUT,
   * where the stride is not a power of two and entry_index_magic carries the
   * conversion instead. A
   * uint8_t placed among the flags deliberately: it lands in padding this line
   * already had, so the grouping described above stays exactly 64 bytes.
   *
   * The per-entry status array needs no field of its own. It is carved
   * immediately after the entries, so it begins at upper_addr_limit, a value
   * this same path has already loaded for the bounds check. */
  uint8_t entry_shift;
#if CCOL_MEMPOOL_COMPACT_LAYOUT
  /* The hot path asks only whether this pool caches at all; how many magazines
     it may hand out is a construction-time question and lives below, which is
     what frees the eight bytes entry_index_magic needs on this line. */
  bool has_cache;
  uint8_t entry_index_shift;
#endif

  /* ---- cold: construction, teardown, introspection --------------------- */
#if CCOL_MEMPOOL_COMPACT_LAYOUT
  size_t max_magazines;
#endif
  size_t magazine_capacity;
  /* What the caller asked for. total_elem_count is that plus the thread-cache
   * reserve, so the two differ for a pool with a cache. Every public count is
   * expressed in terms of this one, and the reserve exists so that a thread is
   * never refused while another thread's cache holds entries it could have
   * had. That guarantee is what bounds the other direction only approximately:
   * concurrent callers can briefly hold a few more than this many entries at
   * once, never more than total_elem_count, because entries are handed out of
   * a thread cache with no lock and therefore no count to consult. See
   * mempool_magazine_refill_locked for why the two cannot both be exact. */
  size_t advertised_elem_count;
  size_t total_elem_count;
  /* Atomic because a ranged pool decides whether an address that matched no
   * tier could be one of its dynamic entries by reading this counter across
   * every sub-pool, and it cannot hold each of those pools' locks to do it. The
   * counter is otherwise only ever changed under its own pool's lock, so the
   * load is an acquire and each change a release; both are ordinary
   * instructions on x86-64 and this field is off the hot path in any case. */
  _Atomic size_t active_dynamic_memory_buffer_count;
  /* live_magazines enforces max_magazines and must be decremented on unlink,
   * not merely incremented on link: otherwise ordinary thread churn fills the
   * budget with magazines belonging to threads that exited long ago, every
   * later thread is refused, and the pool silently reverts to taking the lock
   * on every operation while still passing every functional test.
   *
   * Deliberately a plain size_t, read and written only under the pool's own
   * lock. Qualifying it _Atomic so the refusal check below could read it
   * without the lock costs the cached allocate-and-free path about seventeen
   * percent against its own in-run malloc comparison, even though that path
   * never reads this field; asking the question under the lock instead costs
   * one uncontended lock and unlock on a path that is already out of line. */
  size_t live_magazines;
  struct ccol_mp_magazine *magazines;
  ccol_memmgmt_procs_t *m_procs;
  void *objects;
  // The ccol_r_mempool that owns this sub-pool (or its embedded pseudo_pool),
  // or NULL for a standalone ccol_mempool created directly via
  // ccol_mempool_create()/ ccol_mempool_create_from_preallocated_buffer(). Set
  // once, at construction time, by
  // init_r_mempool_internal_pools/init_preallocated_r_mempool_
  // internal_pools/init_r_mempool_pseudo_pool. Lets entry_belongs_to_rmp()
  // answer "does this entry belong to THIS ccol_r_mempool" in O(1) via a direct
  // pointer comparison instead of scanning every one of the ccol_r_mempool's
  // own sub-pools; declared void* (rather than ccol_r_mempool*) purely to avoid
  // a forward declaration, since struct ccol_r_mempool is defined later in this
  // file.
  void *owner_rmp;
  ccol_mutex_t lock;
};

/* The per-entry status array stores one byte per entry, so it cannot use the
 * word-wide sentinels below: those are chosen to be conspicuous in a memory
 * dump and truncate to an ordinary-looking value in a byte. A pool entry is
 * only ever in one of two states, so a byte encoding of its own is both
 * sufficient and unambiguous.
 *
 * The word-wide sentinels remain, and remain word-wide, for dynamic fallback
 * entries: those keep a header of their own, where the conspicuous value is
 * still worth having and there is room for it. */
typedef enum { mp_status_free = 0, mp_status_taken = 1 } mp_entry_status;

/* Static: nothing outside this file names them, and an unprefixed definition
 * with external linkage would collide at static-link time with an application
 * symbol of the same name, which -fvisibility=hidden does not prevent because
 * it governs the dynamic symbol table alone. */
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
 *                      THREAD CACHE (MAGAZINES)
 * ==========================================================================
 *
 * Each thread keeps a small per-pool magazine of free entries. An allocation
 * pops from it and a free pushes to it, and neither takes the pool lock, so the
 * common path executes no lock and no read-modify-write atomic at all. Only
 * refilling an empty magazine and flushing a full one touch the shared free
 * list, under the pool's own lock.
 *
 * Capacity is preserved, not traded away. A pool asked for N entries allocates
 * N + R slots, where R is the hard ceiling on how much can ever sit in thread
 * caches, so at least N entries remain obtainable by any thread no matter how
 * much other threads have cached. R is bounded by N/2, and falls away from
 * that proportion once a pool is large enough for the per-magazine capacity
 * clamp to bind: N/4 at 8192 entries, N/8 at 16384, and a few percent or less
 * from roughly 65536 upward.
 */

#define CCOL_MP_MAGAZINE_LIMIT 16u /* most magazines any one pool hands out */
#define CCOL_MP_CAPACITY_MAX 128u  /* most entries any one magazine holds    */
#define CCOL_MP_MIN_CACHED_POOL 8u /* pools below this get no cache at all */
/* The pool is over-provisioned by its element count divided by this, which is
 * the ceiling on what every thread cache of the pool can hold between them and
 * so the ceiling on the extra memory a cached pool costs. Depth is what a
 * bursty caller pays for: a magazine that reaches its refill or flush boundary
 * often enough makes the branch at that boundary cost more than the lock it
 * avoids, and the boundary rate falls as the reciprocal of the depth. */
#define CCOL_MP_RESERVE_DIVISOR 2u
/* Direct-mapped, so a ccol_r_mempool allocating across size tiers does not
 * thrash a single-entry cache on every call. A power of two, which is what lets
 * mempool_tls_slot() end in a mask. */
#define CCOL_MP_TLS_SLOTS 8u
_Static_assert(CCOL_MP_TLS_SLOTS > 0u &&
                   (CCOL_MP_TLS_SLOTS & (CCOL_MP_TLS_SLOTS - 1u)) == 0,
               "mempool_tls_slot() reduces with a mask, which only covers the "
               "whole slot array while this is a power of two; zero satisfies "
               "the power-of-two test on its own and must be excluded, since "
               "it would make the mask every bit");

typedef struct ccol_mp_magazine {
  /* Written by whoever unlinks this magazine from its pool's list, read by the
   * owning thread. Release/acquire rather than relaxed: the owning thread frees
   * an orphaned magazine on sight, so observing NULL must also mean the unlink
   * that preceded it is visible, or the unlinking thread would still be holding
   * a pointer to memory the owner just released. */
  _Atomic(ccol_mempool *) pool;
  /* Relaxed-atomic rather than plain, purely so the introspection functions can
   * read it from another thread without a data race. Only the owning thread
   * ever writes it, and a relaxed load or store compiles to an ordinary load or
   * store with no lock prefix and no fence, so the fast path is unaffected;
   * what it buys is ccol_mempool_used_count() reporting entries actually handed
   * out rather than entries merely pulled out of the shared list, which for a
   * single allocation would otherwise over-report by a whole refill batch. */
  _Atomic size_t count;
  size_t capacity;
  struct ccol_mp_magazine *next_in_pool;
  struct ccol_mp_magazine *prev_in_pool;
  struct ccol_mp_magazine *next_in_thread;
  /* Held by value, not reached through the pool. An orphaned magazine outlives
   * the pool it came from, so mp->m_procs is already gone by the time it is
   * freed. NULL means the pool used the default allocator, matching the
   * convention _ccol_r_mempool_destroy follows for the same reason. */
  ccol_free_t free_proc;
  /* Sized to this pool's own magazine capacity, not to the ceiling above it.
   * The ceiling is what a large pool is allowed to reach; a small pool's
   * magazine holds a handful of entries and is allocated to match, so raising
   * the ceiling costs a small pool nothing. */
  void *slots[];
} ccol_mp_magazine;

/* No padding is needed against false sharing between magazines: the slots array
 * alone makes the struct larger than a cache line, so two separately allocated
 * magazines cannot place their hot fields on one. */

/* One thread-local object rather than four separate ones. In a shared library
 * built -fPIC each distinct __thread variable is resolved through its own
 * __tls_get_addr call, so splitting this state across several of them would
 * put a dozen function calls on a path whose whole purpose is to avoid a lock.
 * Gathering it here leaves a single resolution per call, with everything else
 * reached as an offset from it.
 *
 * armed exists because a pthread key destructor only runs for a thread whose
 * value for that key is non-NULL, and all the real state lives here in __thread
 * storage that the key never sees. Without setting something on the key, the
 * destructor never fires and every magazine leaks. */
typedef struct {
  ccol_mp_magazine *mags;
  /* A magazine names its own pool, and the reap below clears every slot
   * pointing at a magazine before freeing it, so a slot never holds a dangling
   * pointer and the magazine's own pool field is the whole hit test. A parallel
   * array of pool keys would answer the same question one load later. */
  ccol_mp_magazine *cache[CCOL_MP_TLS_SLOTS];
  bool armed;
} mp_tls_state_t;

/* initial-exec rather than the default general model. In a shared library the
 * general model resolves every thread-local access through a __tls_get_addr
 * call, which on this path is a function call per allocation and per free and
 * costs roughly 25 percent of the pool's thread-safe throughput. initial-exec
 * resolves at load time instead, at the cost of reserving a slot in the
 * process's static thread-local block: a library loaded normally is unaffected,
 * while one dlopen()ed into a process that has already exhausted that block
 * fails to load. Building with -DCCOL_MEMPOOL_DYNAMIC_TLS=1 selects the general
 * model for callers who need that case to work. */
#if defined(CCOL_MEMPOOL_DYNAMIC_TLS) && CCOL_MEMPOOL_DYNAMIC_TLS
static __thread mp_tls_state_t _mp_tls;
#else
static __thread mp_tls_state_t _mp_tls
    __attribute__((tls_model("initial-exec")));
#endif

static ccol_thread_ls_key_t _mp_tls_key;
/* Serializes a thread's exit-time drain against a concurrent pool destroy.
 * Without it the drain can read a non-NULL pool pointer, and the pool can then
 * be destroyed and freed before the drain locks it. Acquired before any pool
 * lock, never the other way round. */
static ccol_mutex_t _mp_registry_lock;
static atomic_bool _mp_cache_live;
static ccol_once_flag_t _mp_cache_once = CCOL_ONCE_INIT;

static void _mp_tls_drain(void *unused);

static void _mp_cache_init(void) {
  if (ccol_mutex_init(_mp_registry_lock) != 0) return;
  if (ccol_thread_ls_key_create(_mp_tls_key, _mp_tls_drain) != 0) return;
  atomic_store(&_mp_cache_live, true);
}

/* True once the key and registry lock exist. A pool created while this is false
 * simply runs uncached, which is a performance outcome rather than a failure:
 * key creation can fail for real reasons such as PTHREAD_KEYS_MAX already being
 * exhausted process-wide, and that must not fail a pool creation the caller had
 * every reason to expect to succeed. */
static bool _mp_cache_available(void) {
  ccol_call_once(_mp_cache_once, _mp_cache_init);
  return atomic_load(&_mp_cache_live);
}

/* Derives the cache geometry, and with it the reserve that keeps the capacity
 * guarantee intact. The N < CCOL_MP_MIN_CACHED_POOL gate is load-bearing and
 * must not be folded into the clamp below it: a lower clamp of 1 on its own
 * would force a magazine onto every pool including a one-element one, which
 * both contradicts the floor and drives the reserve past N/2, up to N itself
 * for a single-element pool. */
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

  /* Guards the caller's own N + R against wrapping. R is the library's own
   * addition, so if it cannot be afforded arithmetically the pool is simply
   * built without a cache rather than failing to be built. */
  if (mags > SIZE_MAX / cap) return;
  size_t r = mags * cap;
  if (r > SIZE_MAX - elem_count) return;

  *max_magazines = mags;
  *capacity = cap;
  *reserve = r;
}

/* Entries currently handed out to callers: everything taken off the shared free
 * list, less everything parked in a thread cache. Caller holds mp->lock.
 *
 * Walking the magazines costs one relaxed load each and is bounded by the
 * pool's magazine limit. It happens only on paths that already hold the lock,
 * and a pool without a cache has no magazines to walk. */
static size_t mempool_live_count_locked(ccol_mempool *mp) {
  size_t live = mp->total_elem_count - mp->free_elem_count;
  for (ccol_mp_magazine *mag = mp->magazines; mag; mag = mag->next_in_pool) {
    size_t cached = atomic_load_explicit(&mag->count, memory_order_relaxed);
    live = (live > cached) ? (live - cached) : 0;
  }
  return live;
}

/* Moves entries from the pool's free list into the magazine, filling it. Refill
 * takes the whole magazine rather than half of it: both choices give the same
 * hysteresis against a caller oscillating at the full mark, but filling
 * completely amortizes a burst of allocations over the magazine's full capacity
 * instead of half, which matters most on a small pool where the capacity is
 * only a couple of entries to begin with.
 *
 * Caller holds mp->lock. Reports corruption through *corrupt rather than
 * asserting here, so the caller can drop the lock first, as every other
 * corruption path in this file does. */
static void mempool_magazine_refill_locked(ccol_mempool *mp,
                                           ccol_mp_magazine *mag,
                                           bool *corrupt) {
  /* Every entry granted here can become live later without taking the lock
   * again, so what must stay within the advertised count is everything already
   * off the shared free list, not just what is live at this instant: an entry
   * parked in ANOTHER thread's magazine is as good as handed out as far as
   * this decision can tell, since nothing consults this budget again before
   * that thread pops it. total_elem_count - free_elem_count is exactly that
   * quantity, and the reserve is total_elem_count - advertised_elem_count, so
   * the room left is free_elem_count - reserve.
   *
   * Charging only this magazine's own holdings lets several magazines each be
   * granted a batch that is individually within the remainder and collectively
   * past it; sixteen threads racing to exhaust a 1024-entry pool then hold
   * about 1180 entries at once rather than 1024.
   *
   * The bound is still not exact, and no rule here can make it so: the locked
   * path in ccol_mempool_alloc_entry has to keep serving while fewer than
   * advertised_elem_count entries are actually held, or a caller would be
   * refused entries another thread merely has cached, which is the guarantee
   * the reserve exists to provide. The two cannot both be exact, so this keeps
   * the overshoot to a handful of entries instead of the whole reserve;
   * ccol_mempool_total_capacity()'s own documentation states what is promised.
   */
  size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
  size_t reserve = mp->total_elem_count - mp->advertised_elem_count;
  if (mp->free_elem_count <= reserve) return;
  size_t budget = mp->free_elem_count - reserve;

  while (budget > 0 && count < mag->capacity && mp->free_inst) {
    void *entry = mp->free_inst;
    /* Taken from this pool's own free list, so it is in range by construction;
       what is checked is that the list has not been corrupted into pointing at
       something that is not a free entry. */
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

/* Returns how_many of the magazine's entries to the pool's free list: flushing
 * everything would let a caller oscillating across the full mark flush and
 * immediately refill, taking the lock every other operation. Entries in a
 * magazine are already marked free, which is what keeps a double free
 * of a cached entry detectable, so only the list linkage changes here.
 *
 * Caller holds mp->lock. */
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
  /* The stored proc is called directly rather than through _ccol_mem_free,
   * whose NULL check would be against the address of a local and therefore
   * always true. */
  if (mag->free_proc)
    mag->free_proc(mag);
  else
    ccol_mem_free(mag);
}

/* Drops an orphaned magazine from this thread's list and frees it. Deliberately
 * takes no lock: an orphaned magazine was unlinked from its pool's list before
 * its pool pointer was cleared, so this thread is its only remaining owner.
 * Deliberately does not touch live_magazines either; that count belongs to
 * whoever links or unlinks against the pool's list, and destroy already
 * accounted for this one. */
static void mempool_tls_reap(mp_tls_state_t *tls, ccol_mp_magazine **link,
                             ccol_mp_magazine *mag) {
  *link = mag->next_in_thread;
  for (size_t i = 0; i < CCOL_MP_TLS_SLOTS; ++i) {
    /* The lookup cache may still point at what is about to be freed. */
    if (tls->cache[i] == mag) tls->cache[i] = NULL;
  }
  mempool_magazine_free(mag);
}

/* Finds this thread's magazine for mp, creating one if the pool still has a
 * slot free. Returns NULL when the pool is uncached, when every slot is taken,
 * or when the magazine cannot be allocated; all three mean "use the locked
 * path". Reaps any orphaned magazines it walks past. */
/* The direct-mapped hit, which is what every call after a thread's first touch
 * of a pool takes. Inlined into the fast paths deliberately: it is one masked
 * index, two compares and a load, and leaving it behind a call boundary costs
 * more than the work it does. The miss path stays out of line below, so the
 * callers keep the code shape they have when the cache is not in play.
 *
 * Every access is an offset from one resolved thread-local base, which keeps
 * the lookup to a single TLS resolution rather than one per field touched. */
/* Which of the direct-mapped slots a pool occupies.
 *
 * The map exists so that a ccol_r_mempool allocating across size tiers keeps a
 * slot per tier instead of thrashing one. Tiers are separate allocations, so
 * their addresses differ by a stride, and which address bits the index reads
 * decides entirely whether that stride spreads or aliases. Reading the lowest
 * bits above the alignment is the worst choice available: bits 3 to 5 reach a
 * single slot of eight at several of the strides a pool struct realistically
 * lands on, so a ranged pool cycling across tiers misses on nearly every call.
 *
 * The shift below reads bits 8 to 10. Two measures decide that, since neither
 * alone is conclusive: the occupancy of the eight slots over the real
 * addresses a ranged pool's tiers land on, at six through fifteen tiers, on
 * LP64 and on ILP32 (where the struct is smaller and the strides differ), and
 * the same over synthetic uniform strides. Those bits are at or near the best
 * on every one of those, and never leave more than half the tiers in one slot,
 * which the neighbouring shifts each do somewhere. It costs one shift and one
 * mask, the same as any other single run of bits, which is what keeps this on
 * the inlined hit path.
 *
 * CCOL_MP_TLS_SLOTS is a power of two, so the mask is a mask and not a
 * division. */
static inline __attribute__((always_inline)) size_t
mempool_tls_slot(const ccol_mempool *mp) {
  return (size_t)(((uintptr_t)mp >> 8) & (uintptr_t)(CCOL_MP_TLS_SLOTS - 1));
}

static inline __attribute__((always_inline)) ccol_mp_magazine *
mempool_tls_magazine_hit(ccol_mempool *mp) {
  mp_tls_state_t *tls = &_mp_tls;
  size_t slot = mempool_tls_slot(mp);
  ccol_mp_magazine *mag = tls->cache[slot];
  if (mag && atomic_load_explicit(&mag->pool, memory_order_acquire) == mp)
    return mag;
  return NULL;
}

/* The miss path: walk this thread's own list, reaping any magazine whose pool
 * has been destroyed, and build one if this thread has none for this pool. */
static __attribute__((noinline)) ccol_mp_magazine *mempool_tls_magazine_slow(
    ccol_mempool *mp) {
  mp_tls_state_t *tls = &_mp_tls;
  size_t slot = mempool_tls_slot(mp);
  ccol_mp_magazine *mag;

  ccol_mp_magazine **link = &tls->mags;
  while ((mag = *link) != NULL) {
    ccol_mempool *owner =
        atomic_load_explicit(&mag->pool, memory_order_acquire);
    if (!owner) {
      mempool_tls_reap(tls, link, mag);
      continue;
    }
    if (owner == mp) {
      tls->cache[slot] = mag;
      return mag;
    }
    link = &mag->next_in_thread;
  }

  /* Ask whether the pool has a magazine left to give before building one, and
   * ask under the lock, which is what keeps live_magazines an ordinary
   * size_t. A pool grants at most elem_count/8 magazines, so a modest pool
   * shared by more threads than that leaves the surplus threads here on every
   * allocation and every free; without this they each build a magazine, take
   * the lock, learn there is no room, and free it again, which drives the
   * caller's own allocator from the one path this module exists to keep
   * allocator traffic off. The check is repeated under the lock below, since
   * this one is released before the allocation: another thread can take the
   * last slot in between, and that is what the second test is for. */
  bool budget_available;
  ccol_mutex_lock(mp->lock);
  budget_available = mp->live_magazines < mp->max_magazines;
  ccol_mutex_unlock(mp->lock);
  if (!budget_available) return NULL;

  /* Allocated outside the pool lock on purpose: mp->m_procs may be a
   * caller-supplied allocator, and this module is documented as usable as
   * another container's allocator, so calling into one while holding the pool's
   * own lock is a reentrancy hazard. */
  mag = _ccol_mem_calloc(mp->m_procs, 1,
                         sizeof(*mag) + mp->magazine_capacity * sizeof(void *));
  if (!mag) return NULL;

  /* Everything a magazine carries is filled in before it is linked, while it is
   * still private to this thread. The teardown walk assumes a magazine
   * reachable from mp->magazines is fully formed: it clears the pool pointer of
   * every entry it finds, so one linked with that pointer not yet written would
   * have the write land afterwards and name a pool that is going away. */
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
  tls->cache[slot] = mag;

  /* Latched only once the key really carries a value. pthread_setspecific can
     fail, and a thread that recorded itself as armed without it would never
     have its key destructor run: every magazine it holds stays reachable from
     thread-local storage at exit (an error under a leak checker that treats
     still-reachable memory as one), the entries in them are lost to the pool,
     and live_magazines is never given back, which is the silent "the pool
     quietly stops caching" failure the counter exists to prevent. Retrying on
     the next call costs one store on a path that is already out of line. */
  if (!tls->armed && ccol_thread_ls_set(_mp_tls_key, (void *)1) == 0) {
    tls->armed = true;
  }
  return mag;
}

/* Unlinks a magazine from its pool's list. Caller holds mp->lock. */
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

/* Runs on thread exit, for every magazine this thread still owns. Returns the
 * entries it is holding so they are not lost to the pool for the rest of the
 * process, and frees the magazine itself, which matters because a magazine is
 * reachable from thread-local storage and would otherwise be reported by any
 * leak checker configured to treat still-reachable memory as an error. */
static void _mp_tls_drain(void *unused) {
  mp_tls_state_t *tls = &_mp_tls;
  (void)unused;
  ccol_mp_magazine *mag = tls->mags;
  tls->mags = NULL;
  for (size_t i = 0; i < CCOL_MP_TLS_SLOTS; ++i) tls->cache[i] = NULL;
  while (mag) {
    ccol_mp_magazine *next = mag->next_in_thread;
    /* The registry lock is what stops the pool from being destroyed and freed
     * between reading its pointer here and locking it below. */
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
  tls->armed = false;
}

/* Drains the calling thread on unload. No other thread's magazines can be
 * reached from here, and none of their destructors will run at process exit, so
 * a program that leaves threads running is expected to join them; the registry
 * lock keeps this from racing a concurrent destroy either way. */
__attribute__((destructor)) static void _mp_cache_fini(void) {
  if (!atomic_load(&_mp_cache_live)) return;
  _mp_tls_drain(NULL);
}

/* Defined below, with the rest of the ranged-pool internals. */
static bool entry_belongs_to_rmp(ccol_r_mempool *rmp, ccol_mempool *sub);

/* The thread-cache halves of alloc and free, deliberately kept out of line.
 * Inlining them into ccol_mempool_alloc_entry and __ccol_mempool_free_entry
 * perturbs the code those functions generate for every other caller: measured
 * against an otherwise identical build, merely having this code inlined costs
 * a pool with no cache more than ten percent, without executing a single
 * instruction of it. Out of line, an uncached pool sees only the branch that
 * skips the call. */

/* Returns an entry from this thread's magazine, or NULL to fall through to the
 * locked path, which owns the dynamic fallback. Caller has established that the
 * pool has a cache and holds no lock. */
/* The pop's counterpart to mempool_free_to_cache_slow: no magazine yet, or an
 * empty one that has to be refilled from the shared list under the lock. */
static __attribute__((noinline)) void *mempool_alloc_from_cache_slow(
    ccol_mempool *mp, ccol_mp_magazine *mag) {
  if (!mag) {
    mag = mempool_tls_magazine_slow(mp);
    if (!mag) return NULL;
  }

  if (atomic_load_explicit(&mag->count, memory_order_relaxed) == 0) {
    bool corrupt = false;
    ccol_mutex_lock(mp->lock);
    mempool_magazine_refill_locked(mp, mag, &corrupt);
    ccol_mutex_unlock(mp->lock);
    /* Asserted after the lock is released, as every other corruption path in
     * this file does. */
    if (corrupt) {
      ccol_assert(false);
    }
  }

  size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
  if (count == 0) return NULL;
  void *entry = mag->slots[--count];
  atomic_store_explicit(&mag->count, count, memory_order_relaxed);
  ENTRY_STATUS(mp, ENTRY_INDEX(mp, (uintptr_t)entry)) =
      (uint8_t)mp_status_taken;
  return entry;
}

static inline __attribute__((always_inline)) void *mempool_alloc_from_cache(
    ccol_mempool *mp) {
  ccol_mp_magazine *mag = mempool_tls_magazine_hit(mp);
  if (mag) {
    size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
    if (count != 0) {
      void *entry = mag->slots[--count];
      atomic_store_explicit(&mag->count, count, memory_order_relaxed);
      ENTRY_STATUS(mp, ENTRY_INDEX(mp, (uintptr_t)entry)) =
          (uint8_t)mp_status_taken;
      return entry;
    }
  }
  return mempool_alloc_from_cache_slow(mp, mag);
}

/* Parks an entry in this thread's magazine. Returns false to fall through to
 * the locked path. Caller has established that the pool has a cache and holds
 * no lock.
 *
 * The order of the tests below mirrors the locked path exactly: the address is
 * bounds-checked against the pool's own block first, which is also what rejects
 * a dynamic fallback entry (its address is never inside that block, and caching
 * one would hand a freed block back out as a pool entry); only then is the
 * entry's taken/free status read. Reordering them changes which corruption is
 * detected, or whether it is detected at all.
 *
 * Everything the bounds check reads is immutable after construction, so it
 * needs no lock. The status byte is not: it is read and written here without
 * one, which is what leaves a double free by two threads at the same instant
 * undetected. That is a data race in the calling program either way, and it is
 * the same limit a thread-caching general purpose allocator has; see
 * ccol_mempool_free_entry's own note in the header. */
/* Everything a push can run into that is not simply "there is room": this
 * thread has no magazine for this pool yet, or the one it has is full and has
 * to give half back under the lock. Out of line so the push itself stays a
 * handful of instructions. mag is whatever the direct-mapped lookup found,
 * which is NULL when the caller missed it entirely. */
static __attribute__((noinline)) bool mempool_free_to_cache_slow(
    ccol_mempool *mp, void *entry, size_t idx, ccol_mp_magazine *mag) {
  if (!mag) {
    mag = mempool_tls_magazine_slow(mp);
    if (!mag) return false;
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
  if (count >= mag->capacity) return false;

  ENTRY_STATUS(mp, idx) = (uint8_t)mp_status_free;
  mag->slots[count++] = entry;
  atomic_store_explicit(&mag->count, count, memory_order_relaxed);
  return true;
}

static inline __attribute__((always_inline)) bool mempool_free_to_cache(
    ccol_mempool *mp, void *entry, uintptr_t c_entry) {
  /* Ownership first, because there is no header to consult before it: an
     address outside this pool's buffer is not a pool entry and belongs on the
     locked path, which owns the dynamic fallback. */
  if (!valid_mempool_addr(mp, c_entry)) return false;

  size_t idx = ENTRY_INDEX(mp, c_entry);
  if (ENTRY_STATUS(mp, idx) != mp_status_taken) {
    /* A double free, including of an entry sitting in a magazine, which is
     * marked free exactly so this stays detectable. No lock is held here, so
     * there is nothing to release first. */
    ccol_assert(false);
  }

  ccol_mp_magazine *mag = mempool_tls_magazine_hit(mp);
  if (mag) {
    size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
    if (count < mag->capacity) {
      ENTRY_STATUS(mp, idx) = (uint8_t)mp_status_free;
      mag->slots[count++] = entry;
      atomic_store_explicit(&mag->count, count, memory_order_relaxed);
      return true;
    }
  }
  return mempool_free_to_cache_slow(mp, entry, idx, mag);
}

#ifdef RUNNING_UNIT_TESTS
/* White-box accessors for the thread-cache regression tests. Compiled only
 * under RUNNING_UNIT_TESTS, so they are absent from the shipped library and
 * never reach its dynamic symbol table. The behaviours they expose have no
 * public API because they are not a caller's concern; they are, however,
 * exactly the properties whose failure is silent, which is why the tests assert
 * on them directly rather than inferring them from timing or throughput. */

/* Overwrites what a pool records about one of its entries.
 *
 * An entry carries no header, so its recorded state lives in the pool's own
 * status array and is not reachable from outside the module at all. The
 * corruption tests need to be able to put an entry into a state no correct
 * sequence of calls can produce, which is precisely what they exist to prove is
 * detected, so the reach-in lives here rather than the test inventing a way to
 * compute the address itself. */
void _ccol_mempool_corrupt_entry_status_for_tests(ccol_mempool *mp, void *entry,
                                                  unsigned char value) {
  if (!mp || !entry) return;
  if (!valid_mempool_addr(mp, (uintptr_t)entry)) return;
  ENTRY_STATUS(mp, ENTRY_INDEX(mp, (uintptr_t)entry)) = value;
}

/* Magazines this pool has currently handed out. Tests that a thread's exit
 * returns its slot: without that, thread churn exhausts the budget and the
 * pool silently stops caching while still passing every functional test. */
size_t _ccol_mempool_live_magazines_for_tests(ccol_mempool *mp) {
  if (!mp) return 0;
  size_t n;
  if (mp->should_use_locks) ccol_mutex_lock(mp->lock);
  n = mp->live_magazines;
  if (mp->should_use_locks) ccol_mutex_unlock(mp->lock);
  return n;
}

/* Magazines the calling thread is holding, across all pools. Tests that
 * magazines orphaned by a destroyed pool are reaped rather than accumulating:
 * they stay reachable from thread-local storage, so no leak checker reports
 * them while memory climbs. */
size_t _ccol_mempool_thread_magazines_for_tests(void) {
  size_t n = 0;
  for (ccol_mp_magazine *mag = _mp_tls.mags; mag; mag = mag->next_in_thread)
    n++;
  return n;
}

/* Slots allocated beyond the advertised count. Zero for a pool with no cache.
 */
size_t _ccol_mempool_reserve_for_tests(ccol_mempool *mp) {
  if (!mp) return 0;
  return mp->total_elem_count - mp->advertised_elem_count;
}
#endif /* RUNNING_UNIT_TESTS */

/* Destroys the memory pool. If dynamic fallback was enabled and some
 * dynamically allocated entries have not been freed yet, the call asserts to
 * make that leak visible; we'd rather crash loudly than silently lose memory.
 * The backing objects buffer is only freed if it was not supplied by the caller
 * as a preallocated buffer. */
void _ccol_mempool_destroy(ccol_mempool *mp) {
  if (mp) {
    /* Cut every outstanding magazine loose before anything else. Each is owned
     * by some other thread, which may be running right now and which will free
     * it from its own exit handler.
     *
     * Three things about this are load-bearing. The registry lock is taken
     * first, and is what stops a thread's exit-time drain from reading this
     * pool's pointer and then locking a mutex this function has already
     * destroyed. Each magazine is unlinked BEFORE its pool pointer is cleared,
     * because the owning thread frees an orphaned magazine the moment it sees
     * NULL, and would otherwise free memory still on this list. And the
     * entries a magazine is holding are deliberately not drained: they are
     * written by their owning thread with no synchronization, so reading them
     * from here would be a genuine data race, and the buffer they point into is
     * about to be released anyway. */
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
        // This pool has dynamically allotated entries that have
        // not yet been freed. This is a leak, let's make it
        // noticed.
        ccol_assert(false);
      }
    }

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

/* The distance between one entry and the next, computed at run time from a
 * caller's element size. Mirrors the _ccol_mempool_stride() macro the public
 * buffer-declaring macros use, so a preallocated buffer and the pool built on
 * it always agree; the macro exists separately only because a static array's
 * size must be a constant expression.
 *
 * Returns 0 if no stride fits, which every caller treats as a rejected
 * elem_size rather than wrapping. */
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

/* Bytes one pool block occupies: one stride per slot, followed by one status
 * byte per slot.
 *
 * Reports the product through an out-parameter rather than returning it, and
 * answers false instead of a wrapped value, because this size is formed here
 * and handed to the allocator
 * as a single already-multiplied count. An allocator asked for count * size
 * detects the overflow itself; asked for one size, it cannot, so a wrapped
 * product would be a small allocation followed by a layout loop walking
 * elem_count entries through it.
 *
 * The same expression _ccol_mempool_buffer_params_fit() checks at compile time
 * for a preallocated buffer, so a count and size the macro rejects are rejected
 * here too. */
static bool mp_block_bytes(size_t count, size_t stride, size_t *out) {
  if (count == 0 || stride > (SIZE_MAX - count) / count) return false;
  *out = count * stride + count;
  return true;
}

#if CCOL_MEMPOOL_COMPACT_LAYOUT
/* Turns division by a compact pool's stride into a multiply and a shift, and
 * then proves the result before it is accepted.
 *
 * A reciprocal that is subtly wrong does not fail; it returns a neighbouring
 * ordinal, and the pool then reads and writes another entry's status byte for
 * the life of the process. The check below is exhaustive rather than algebraic:
 * floor division and the multiply-shift are both non-decreasing in the offset,
 * so agreeing at every multiple of the stride and at the byte immediately below
 * it forces agreement everywhere between, which is exactly what the bounds test
 * needs in order to reject an address that is inside the buffer but not on an
 * entry boundary.
 *
 * Returns false when no shift works, and the pool is then not built at all
 * rather than built on arithmetic that cannot be vouched for. */
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

/* Lays out one contiguous block as elem_count entries of stride bytes followed
 * by elem_count status bytes, and threads every entry onto the free list
 * through its own first bytes. The status array's position is not stored: it
 * begins where the entries end, which is upper_addr_limit. */
/* Returns false only under the compact layout, and only when no reciprocal for
   this stride and count could be proved. */
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

/* Creates a fixed-size memory pool for elem_count elements of elem_size bytes
 * each. An entry carries no header, so the stride is elem_size itself, bumped
 * to sizeof(addr_t) if smaller (a free entry stores the free-list link in its
 * own first bytes), aligned, and rounded to a power of two; one status byte per
 * entry follows the entry array in the same block. */
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
    // Rounding elem_size up to _ccol_mempool_entry_align (so every entry past
    // the first one in the pool's contiguous buffer lands on an address aligned
    // for any object type, not just the first; see _ccol_mempool_align_up()'s
    // own doc comment for why that rounding is necessary at all), and then up
    // to a power of two, would overflow size_t and wrap the stride to a value
    // smaller than elem_size itself, making every entry index the free path
    // computes land outside the allocated buffer.
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
     each; the status array's position is therefore not stored anywhere. Sized
     here, before any cache geometry is derived, so that a count and size which
     together cannot be laid out at all are rejected as the caller's own
     request rather than reported as a failure to allocate. */
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

  /* A thread cache needs a lock to refill against, and the process-wide key it
   * hangs off can legitimately fail to be created (PTHREAD_KEYS_MAX being
   * exhausted, say). Either way the pool is built without one rather than not
   * built at all. */
  size_t max_magazines = 0, magazine_capacity = 0, reserve = 0;
  if (!single_threaded && _mp_cache_available())
    mempool_cache_geometry(elem_count, &max_magazines, &magazine_capacity,
                           &reserve);

  /* The reserve is what keeps the pool's promise intact while entries sit in
   * thread caches: with at most `reserve` entries cached, the caller's own
   * elem_count remains obtainable no matter which thread asks. */
  size_t slot_count = elem_count + reserve;
  size_t slot_bytes = asked_bytes;
  if (reserve > 0 &&
      !mp_block_bytes(slot_count, extended_elem_size, &slot_bytes)) {
    /* The reserve is this library's own addition, so a count whose block can
     * only be sized without it must still get the pool it asked for; the same
     * reasoning the allocation-failure retry below follows. */
    max_magazines = 0;
    magazine_capacity = 0;
    reserve = 0;
    slot_count = elem_count;
    slot_bytes = asked_bytes;
  }

  mp->objects = _ccol_mem_calloc(mp->m_procs, 1, slot_bytes);
  if (!mp->objects && reserve > 0) {
    /* The reserve is this library's own addition, so a caller near its memory
     * ceiling must not be denied a pool it could otherwise have had. Drop the
     * cache and retry at exactly what was asked for. */
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

/* Like ccol_mempool_create but uses an externally supplied buffer (e.g. a
 * static array) as the element store. elem_count is derived from buf_size /
 * extended element size. The buffer is never freed by the pool; the caller
 * remains responsible for its lifetime. Useful for embedded or stack-allocated
 * pools.
 */
ccol_mempool *ccol_mempool_create_from_preallocated_buffer(
    void *buffer, size_t buf_size, size_t elem_size,
    bool fallback_to_dynamic_memory, bool single_threaded,
    ccol_memmgmt_procs_t *mmgmt_procs, char **err) {
  if (err) {
    *err = NULL;
  }

  /* Only the pointer is checked here. There is no fixed minimum size: what a
     buffer has to hold is one stride plus that entry's status byte, which
     depends on elem_size and is expressed by the element-count computation
     below, so a separate constant here could only disagree with it. */
  if (!buffer) {
    if (err) {
      *err = CCOL_ERR_STR("buffer is not acceptable");
    }
    return NULL;
  }

  // Entry 0 starts at the buffer's own address, so the buffer decides whether
  // the entries this pool hands out meet the alignment it guarantees for them;
  // the stride rounding only carries that property from entry 0 to the rest. A
  // buffer declared via CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER already
  // satisfies this, since the macro aligns it to the same constant checked
  // here. Accepting a weaker one would hand back entries a caller cannot store
  // every object type in: undefined behavior, and a real fault risk on
  // strict-alignment architectures.
  if ((uintptr_t)buffer % _ccol_mempool_entry_align != 0) {
    if (err) {
      *err = CCOL_ERR_STR("buffer must be aligned to at least 16 bytes");
    }
    return NULL;
  }

  // Mirrors ccol_mempool_create's own three-way elem_size handling exactly,
  // including check ORDER: a genuine zero is a caller mistake worth its own
  // distinct error; an elem_size too large for the stride rounding to be
  // applied without overflowing size_t is rejected next;
  // only once both of those are ruled out is a small-but-nonzero size silently
  // rounded up to fit the free-list pointer. The zero-size and overflow ranges
  // can never overlap with the round-up range in practice (sizeof(addr_t) is
  // tiny), but keeping this function's check order identical to
  // ccol_mempool_create's own, rather than merely equivalent, means the two can
  // never silently drift apart if either threshold changes.
  if (elem_size == 0) {
    if (err) {
      *err = CCOL_ERR_STR("elem_size is zero");
    }
    return NULL;
  } else if (mp_stride_for(elem_size) == 0) {
    // Rounding elem_size up to _ccol_mempool_entry_align and then to a power
    // of two (see ccol_mempool_create's identical check, and
    // _ccol_mempool_align_up()'s own doc comment, for the full explanation)
    // would overflow size_t and wrap the stride to a value smaller than
    // elem_size itself, making every entry index the free path computes land
    // outside the caller's buffer.
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
     same buffer, which is exactly what CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER
     sizes for, so a buffer from that macro yields precisely the element count
     it was declared with. */
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

/* Allocates one element from the pool in O(1) time by popping the head of the
 * internal free list. If the pool is exhausted and fallback_to_dynamic_memory
 * is enabled, a fresh heap allocation is made instead and tagged as
 * elem_is_not_a_pool_member so it is routed through free on return. */
void *ccol_mempool_alloc_entry(ccol_mempool *mp) {
  if (!mp) {
    ccol_assert(false);
  }

  void *result = NULL;

  if (mp->should_use_locks) {
    /* Nested inside the lock test rather than placed ahead of it, so that a
     * single-threaded pool executes exactly the instruction sequence a build
     * with no thread cache at all produces. A pool created with single_threaded
     * = true never has a cache, and testing max_magazines ahead of this branch
     * makes those pools measurably slower for a feature they opt out of.
     */
    if (MP_HAS_CACHE(mp)) {
      void *cached = mempool_alloc_from_cache(mp);
      if (cached) return cached;
      /* Magazine empty and the shared list had nothing to give: fall through,
       * since the locked path below owns the dynamic fallback. */
    }
    ccol_mutex_lock(mp->lock);
  }

  /* A pool with a thread cache owns slots beyond its advertised count, so its
   * free list can be non-empty while the advertised count is already fully
   * handed out. Treating that as exhausted is what stops the reserve leaking
   * into ordinary allocation and the pool serving more entries than it
   * promised; the reserve exists only to cover entries parked in other
   * threads' caches. A pool with no cache has no reserve, so a non-empty free
   * list already means the count is not exhausted, and the short-circuit below
   * keeps it from paying for the check. */
  bool can_serve = mp->free_inst != NULL;
  if (can_serve && MP_HAS_CACHE(mp)) {
    can_serve = mempool_live_count_locked(mp) < mp->advertised_elem_count;
  }

  if (can_serve) {
    void *entry = mp->free_inst;

    /* Taken from this pool's own free list, so it is in range by construction;
       what is checked is that the list has not been corrupted into pointing at
       something that is not a free entry of this pool. */
    if (!valid_mempool_addr(mp, (uintptr_t)entry) ||
        ENTRY_STATUS(mp, ENTRY_INDEX(mp, (uintptr_t)entry)) != mp_status_free) {
      // We have a corruption!
      if (mp->should_use_locks) {
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
    // Seems like we exhausted our buffers and
    // we are asked to fallback to the dynamic
    // memory allocation mechanisms. The allocation is prefixed with the
    // entry's own user-visible size (see DYNAMIC_ENTRY_USER_SIZE) so a
    // later ccol_r_mempool_realloc_entry call can recover it faithfully.
    if (mp->extended_elem_size <= SIZE_MAX - DYNAMIC_ENTRY_PREFIX_SIZE) {
      void *raw_block = _ccol_mem_alloc(
          mp->m_procs, DYNAMIC_ENTRY_PREFIX_SIZE + mp->extended_elem_size);
      if (raw_block) {
        result = (uint8_t *)raw_block + DYNAMIC_ENTRY_PREFIX_SIZE;
        mp_dynamic_header *dyn = DYNAMIC_ENTRY_HEADER(result);
        dyn->user_size = mp->extended_elem_size;
        dyn->elem_status = elem_is_not_a_pool_member;
        dyn->pool_ptr = mp;
        atomic_store_explicit(
            &mp->active_dynamic_memory_buffer_count,
            atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                                 memory_order_relaxed) +
                1,
            memory_order_release);
      }
    }
  }

  if (mp->should_use_locks) {
    ccol_mutex_unlock(mp->lock);
  }

  return result;
}

/* Allocates one element and zeroes it before returning. An entry carries no
 * header, so the whole stride belongs to the caller and all of it is zeroed. */
void *ccol_mempool_calloc_entry(ccol_mempool *mp) {
  void *result = ccol_mempool_alloc_entry(mp);

  if (result) {
    ccol_mem_zero(result, mp->extended_elem_size);
  }

  return result;
}

/* Checks whether c_entry falls within the pool's object buffer and lands on a
 * valid element boundary (i.e. is a multiple of extended_elem_size from the
 * base). Used to distinguish pool-owned entries from dynamic fallback entries
 * during free. */
/* Is this address one of mp's own entries, and exactly on an entry boundary?
 *
 * The stride is a power of two, so the boundary test is a mask rather than the
 * division it would otherwise be, and the offset that produces it is the same
 * one the caller goes on to shift into an index. An address below the buffer
 * wraps when the subtraction is done unsigned, so a single upper comparison
 * would cover both ends; both are kept because the pseudo_pool has a zero-width
 * range and the explicit lower test states that intent rather than relying on
 * the wrap. */
/* Is c_entry the start of one of this pool's entries?
 *
 * Landing inside the buffer is not enough: an address partway into an entry has
 * to be rejected too, or the ordinal derived from it names a neighbour and the
 * status byte examined is the wrong one. The default layout's stride is a power
 * of two, so that is one mask; the compact layout multiplies the ordinal back
 * out and compares, which the verified reciprocal makes exact. */
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

/* The free path.
 *
 * Ownership is established from the address before anything is read through it,
 * since an entry carries no bookkeeping of its own to consult first. An address
 * inside this pool's buffer is a pool entry and its state is in the status
 * array; anything else can only be a dynamic fallback entry, and that branch is
 * entered only once this pool is known to hold live ones, so a stray pointer is
 * rejected without dereferencing whatever the caller passed.
 */
static void __ccol_mempool_free_entry(ccol_mempool *mp, void *entry) {
  if (!mp) {
    ccol_assert(false);
  }

  uintptr_t c_entry = (uintptr_t)entry;

  if (mp->should_use_locks) {
    /* Nested for the same reason as in ccol_mempool_alloc_entry: a pool with no
     * cache must not pay for one. */
    if (MP_HAS_CACHE(mp) && mempool_free_to_cache(mp, entry, c_entry)) {
      return;
    }
    ccol_mutex_lock(mp->lock);
  }

  if (valid_mempool_addr(mp, c_entry)) {
    size_t idx = ENTRY_INDEX(mp, c_entry);
    if (ENTRY_STATUS(mp, idx) != mp_status_taken) {
      // Either a double-free (the entry is already marked free) or
      // corruption (any other status).
      // Both are fatal, so assert unconditionally without further branching.
      if (mp->should_use_locks) {
        ccol_mutex_unlock(mp->lock);
      }
      ccol_assert(false);
    }

    ENTRY_STATUS(mp, idx) = (uint8_t)mp_status_free;
    *(void **)entry = mp->free_inst;
    mp->free_inst = entry;
    ++mp->free_elem_count;
    if (mp->should_use_locks) {
      ccol_mutex_unlock(mp->lock);
    }
    return;
  }

  /* Outside this pool's buffer. The only legitimate way that happens is a
   * dynamic fallback entry this pool handed out, so unless this pool is
   * actually holding some, the address is not ours and nothing is read through
   * it. This gate is what keeps a stray or wrong-pool pointer from being
   * dereferenced at entry - DYNAMIC_ENTRY_PREFIX_SIZE. */
  if (!mp->fallback_to_dynamic_memory ||
      atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                           memory_order_relaxed) == 0) {
    if (mp->should_use_locks) {
      ccol_mutex_unlock(mp->lock);
    }
    ccol_assert(false);
  }

  mp_dynamic_header *dyn = DYNAMIC_ENTRY_HEADER(entry);
  if (dyn->pool_ptr != mp || dyn->elem_status != elem_is_not_a_pool_member) {
    // Not one of this pool's live dynamic entries: a foreign pointer, genuine
    // corruption, or a double free of an entry whose first release rewrote its
    // status to elem_is_freed_dynamic_member precisely so this is caught.
    if (mp->should_use_locks) {
      ccol_mutex_unlock(mp->lock);
    }
    ccol_assert(false);
  }

  atomic_store_explicit(
      &mp->active_dynamic_memory_buffer_count,
      atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                           memory_order_relaxed) -
          1,
      memory_order_release);
  // Marked before the block goes back to the allocator, so that a second,
  // illegitimate free of the same still-unreused block is a controlled assert
  // rather than a silent double free. A double free that races an intervening
  // reallocation of the same block is undetectable without a live registry of
  // outstanding pointers and is out of scope here.
  dyn->elem_status = elem_is_freed_dynamic_member;
  _ccol_mem_free(mp->m_procs, DYNAMIC_ENTRY_RAW_BLOCK(entry));

  if (mp->should_use_locks) {
    ccol_mutex_unlock(mp->lock);
  }
}

/* Public free entry point. Accepts a NULL pointer without asserting (matching
 * the behaviour of standard free). Verifies the caller named a real pool via
 * the ccol_mempool_mark sentinel, then calls __ccol_mempool_free_entry to
 * perform the actual release. */
void _ccol_mempool_free_entry(ccol_mempool *mp, void *entry) {
  if (!entry) {
    // Let's resemble the dynamic memory allocation approach here.
    // Releasing a NULL pointer is acceptable.
    return;
  }

  // A NULL pool is a caller error rather than a tolerated no-op: unlike a NULL
  // entry, it has no free()-like reading, and continuing would mean guessing
  // which pool was meant.
  if (!mp) {
    ccol_assert(false);
  }

  if (mp->ccol_mempool_mark != _ccol_mempool_mark) {
    ccol_assert(false);
  }

  // Ownership is established inside, from the address, without reading
  // anything through the caller's pointer first.
  __ccol_mempool_free_entry(mp, entry);
}

/* Returns the total number of elements the pool was sized for (free + in-use).
 * Acquires the read lock when the pool is in multi-threaded mode. */
size_t ccol_mempool_total_capacity(ccol_mempool *mp) {
  if (!mp) {
    ccol_assert(false);
  }

  size_t result = 0;

  if (mp->should_use_locks) {
    ccol_mutex_lock(mp->lock);
  }

  /* The count the caller asked for, not the physical slot count. A pool with a
   * thread cache holds a reserve beyond this, but never hands out more than
   * this many entries at once, so reporting the physical count would promise
   * capacity that can never be used. */
  result = mp->advertised_elem_count;

  if (mp->should_use_locks) {
    ccol_mutex_unlock(mp->lock);
  }

  return result;
}

/* Returns the number of pool-owned elements currently in use (total - free).
 * Does not include dynamic fallback allocations. */
size_t ccol_mempool_used_count(ccol_mempool *mp) {
  if (!mp) {
    ccol_assert(false);
  }

  size_t result = 0;

  if (mp->should_use_locks) {
    ccol_mutex_lock(mp->lock);
  }

  /* Entries sitting in a thread cache have been pulled off the shared free
   * list but not handed to anyone, so they are neither free nor in use.
   * Subtracting them is what keeps this reporting entries a caller actually
   * holds: without it, a single allocation would report a whole refill batch
   * as used, which for a large pool is off by more than an order of magnitude.
   *
   * Each count is read with a relaxed atomic load. The value can be stale if
   * its owning thread is allocating concurrently, which is inherent to asking
   * for a count while the thing being counted is moving; it is never torn, and
   * it is exact whenever the pool is quiescent. */
  result = mempool_live_count_locked(mp);

  if (mp->should_use_locks) {
    ccol_mutex_unlock(mp->lock);
  }

  return result;
}

/* Returns the number of dynamic fallback allocations currently outstanding.
 * Non-zero means pool entries were exhausted at some point. */
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
// The highest power of two a size_t can hold (common.h's own
// ccol_max_power_of_two_size_t; 2^63 on a 64-bit size_t, 2^31 on a 32-bit one),
// not SIZE_MAX / 2: the latter is one less than that power of two (e.g.
// 2^63 - 1, not 2^63, on a 64-bit platform, since SIZE_MAX itself is odd),
// which would silently reject the single largest power-of-two size this
// module's own documented "must be <= 2^63 bytes" contract promises.
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
  // Kept alongside smallest_size (its own linear value, 1 << this) so
  // ccol_r_mempool_pool_index_for_size can turn the "/smallest_size" division
  // every size-to-tier lookup needs into an exact right-shift instead.
  uint8_t smallest_size_power_of_two;
};

/* Destroys the ranged memory pool. If the fallback policy is
 * ccol_fallback_at_last_exhaustion and outstanding dynamic entries exist,
 * asserts to expose the leak (same philosophy as _ccol_mempool_destroy). Each
 * internal sub-pool is destroyed individually before the sub-pool array is
 * freed. */
void _ccol_r_mempool_destroy(ccol_r_mempool *rmp) {
  if (rmp) {
    if (rmp->fb_policy == ccol_fallback_at_last_exhaustion) {
      if (ccol_mempool_dynamic_allocs_count(&rmp->pseudo_pool) > 0) {
        // We have dynamic pointers that have not been freed yet
        // That's a potential leak, let's make it noticed.
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

/* Validates the power-of-two size parameters for a ccol_r_mempool and derives
 * the number of internal sub-pools. The constraint
 * smallest_elem_count_power_of_two >= (largest -smallest) ensures that each
 * successively larger sub-pool can have at least one element (dividing the
 * count by 2 for each doubling of size). Populates rmp fields on success. */
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

/* Initialises the pseudo_pool embedded in rmp, which acts as a sentinel/tracker
 * for global dynamic fallback allocations under the
 * ccol_fallback_at_last_exhaustion policy. In that mode the pseudo_pool's
 * active_dynamic_memory_buffer_count tracks all dynamic entries across all
 * sub-pools. */
static bool init_r_mempool_pseudo_pool(ccol_r_mempool *rmp) {
  ccol_mem_zero(&rmp->pseudo_pool, sizeof(ccol_mempool));
  rmp->pseudo_pool.m_procs = rmp->m_procs;
  rmp->pseudo_pool.owner_rmp = rmp;
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

/* Creates all sub-pools ranging from smallest_size to largest_size, each with
 * half as many elements as the previous (compensating for twice the element
 * size). The fallback policy per sub-pool is ccol_fallback_at_first_exhaustion
 * iff the ccol_r_mempool's overall policy is also first-exhaustion. */
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
    // The cleanup will be performed by the caller.
    if (err) {
      *err = CCOL_ERR_STR("failed to allocate mem_pools array");
    }
    return false;
  }
  ccol_mem_zero(rmp->mem_pools,
                rmp->number_of_mempools * sizeof(ccol_mempool *));

  size_t first_size = rmp->smallest_size;
  size_t first_count = rmp->smallest_elem_count;

  for (size_t esize = first_size, ecount = first_count, index = 0;
       index < rmp->number_of_mempools; esize *= 2, ecount /= 2, ++index) {
    rmp->mem_pools[index] = ccol_mempool_create(
        ecount, esize, rmp->fb_policy == ccol_fallback_at_first_exhaustion,
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
 * A precomputed reverse lookup array answering the same question would be
 * sized largest_size/smallest_size entries, which is exponential in the
 * number of tiers (2^(largest_size_power_of_two - smallest_size_power_of_
 * two)), not linear: a caller picking a modest-looking size range (e.g. 16
 * bytes to 1 GiB, 26 tiers) would need a ~512 MiB lookup table just to
 * answer it. This formula needs no table and no allocation, so it fails only
 * in exactly the cases ccol_r_mempool_alloc_entry/etc. already reject up front
 * (size == 0 or size > largest_size). */
static inline size_t ccol_r_mempool_pool_index_for_size(ccol_r_mempool *rmp,
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

/* Carves the preallocated_buffer into contiguous sub-buffer segments, one per
 * sub-pool, using the same size/count progression as
 * init_r_mempool_internal_pools. Asserts that cumulative_size equals
 * preallocated_buffer_size to ensure the caller has provided a buffer of
 * exactly the right size. */
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
    /* One stride per entry plus one status byte per entry, the status region
       rounded up so the next sub-pool's own first entry stays aligned. The
       rounding is slack this sub-pool never reads: its element count is derived
       from the stride and the status byte alone, so the extra bytes cannot make
       it claim an entry the segment does not hold. */
    size_t sub_buffer_size =
        ecount * mp_stride_for(esize) + _ccol_mempool_align_up(ecount);
    rmp->mem_pools[index] = ccol_mempool_create_from_preallocated_buffer(
        sub_buffer, sub_buffer_size, esize,
        rmp->fb_policy == ccol_fallback_at_first_exhaustion,
        !rmp->should_use_locks, rmp->m_procs, err);
    if (!rmp->mem_pools[index]) {
      // The cleanup will be performed by the caller.
      return false;
    }
    rmp->mem_pools[index]->owner_rmp = rmp;
    cumulative_size += sub_buffer_size;
  }

  return true;
}

/* Like ccol_r_mempool_create but uses an externally supplied buffer for all
 * sub-pool element storage. The buffer is not freed by the ccol_r_mempool; the
 * caller is responsible for its lifetime. */
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

  // Checked once, up front, for the same reason
  // ccol_mempool_create_from_preallocated_buffer checks it: every entry a
  // sub-pool hands out must meet _ccol_mempool_entry_align. Each sub-pool's own
  // creation call would eventually re-derive this (its own segment's offset
  // from the buffer start is always a multiple of that alignment, so
  // misalignment can only ever originate from the buffer's own starting
  // address), but checking it here gives a single, immediate,
  // clearly-attributed error instead of a failure surfacing from deep inside
  // sub-pool construction.
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

/* Allocates a dynamic entry tagged as elem_is_not_a_pool_member through the
 * pseudo_pool, which acts as a tracker for global fallback allocations under
 * the ccol_fallback_at_last_exhaustion policy. The pseudo_pool itself has no
 * object buffer; it only maintains the active_dynamic_memory_buffer_count
 * counter. Since the pseudo_pool has no fixed element size of its own (unlike a
 * real sub-pool, whose entries can always recover their size from
 * pool_ptr->extended_elem_size), every entry records its own user-visible size
 * in the header that sits ahead of it, so a later ccol_r_mempool_realloc_entry
 * call can recover exactly how many bytes are safe to copy out of it, instead
 * of guessing or discarding the data entirely.
 *
 * Unlike ccol_mempool_create/ccol_mempool_create_from_preallocated_buffer,
 * elem_size is deliberately NOT rounded up to sizeof(addr_t) here. That
 * rounding exists only so a POOL-OWNED entry is always large enough to double
 * as a free-list node (the link to the next free entry is written into its own
 * first bytes while it sits free); a pseudo_pool entry is a one-off heap
 * allocation that is
 * never linked into any free list; it is simply handed to _ccol_mem_free once
 * released, so it has no such minimum-size requirement. Rounding here would
 * also corrupt the entry's own size prefix relative to what the caller actually
 * asked for: ccol_r_mempool_realloc_entry's "same size, no move needed" fast
 * path for a pseudo_pool entry compares the caller's newly requested size
 * directly against that recorded size, so recording a size
 * other than exactly what was requested would make that comparison fail for a
 * request under sizeof(addr_t) bytes even when nothing about the request
 * actually changed, defeating the fast path silently on every such call. */
static void *ccol_mempool_pseudo_alloc_entry(ccol_mempool *mp,
                                             size_t elem_size) {
  void *result = NULL;

  // Checked before the allocation size is formed. Adding the prefix to an
  // elem_size within DYNAMIC_ENTRY_PREFIX_SIZE of SIZE_MAX wraps to a small
  // request that succeeds, while the size prefix a few lines below still
  // records the original, huge elem_size; a dangerously undersized block whose
  // recorded size lies about its real capacity to any later
  // ccol_r_mempool_realloc_entry call.
  if (elem_size > SIZE_MAX - DYNAMIC_ENTRY_PREFIX_SIZE) {
    return NULL;
  }

  if (mp->should_use_locks) {
    ccol_mutex_lock(mp->lock);
  }

  void *raw_block =
      _ccol_mem_alloc(mp->m_procs, DYNAMIC_ENTRY_PREFIX_SIZE + elem_size);
  if (raw_block) {
    result = (uint8_t *)raw_block + DYNAMIC_ENTRY_PREFIX_SIZE;
    mp_dynamic_header *dyn = DYNAMIC_ENTRY_HEADER(result);
    dyn->user_size = elem_size;
    dyn->elem_status = elem_is_not_a_pool_member;
    dyn->pool_ptr = mp;
    atomic_store_explicit(
        &mp->active_dynamic_memory_buffer_count,
        atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                             memory_order_relaxed) +
            1,
        memory_order_release);
  }

  if (mp->should_use_locks) {
    ccol_mutex_unlock(mp->lock);
  }

  return result;
}

/* Allocates a buffer of at least size bytes. ccol_r_mempool_pool_index_for_size
 * is used to find the smallest sub-pool whose element size fits size; if that
 * sub-pool is exhausted the next larger one is tried (escalation). Only if all
 * sub-pools are exhausted does the ccol_fallback_at_last_exhaustion path kick
 * in. */
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

/* Allocates and zeroes a buffer of at least size bytes from the ccol_r_mempool.
 */
void *ccol_r_mempool_calloc_entry(ccol_r_mempool *rmp, size_t size) {
  void *result = ccol_r_mempool_alloc_entry(rmp, size);

  if (result) {
    ccol_mem_zero(result, size);
  }

  return result;
}

/* An entry inside the pool's own buffer is exactly one stride wide; the
 * rounding is the pool's, but the bytes belong to whoever holds the entry.
 * Anything else reaching here is a dynamic fallback entry, whose size varies
 * per allocation and is recorded in its own header. */
static inline size_t entry_user_size(ccol_mempool *mp, void *entry) {
  if (valid_mempool_addr(mp, (uintptr_t)entry)) {
    return mp->extended_elem_size;
  }
  return DYNAMIC_ENTRY_USER_SIZE(entry);
}

/* Returns true iff sub is genuinely one of rmp's own sub-pools or rmp's own
 * pseudo_pool, i.e. a real member of THIS specific
 * ccol_r_mempool rather than merely any ccol_mempool anywhere in the process
 * that happens to share the same global ccol_mempool_mark sentinel (every
 * ccol_mempool does, by construction). Used by ccol_r_mempool_realloc_entry to
 * reject a pointer obtained from a different ccol_r_mempool (or from a bare
 * ccol_mempool_create() pool) instead of silently accepting it.
 *
 * O(1): every sub-pool (and the pseudo_pool) has its owner_rmp field set once,
 * at construction time, by init_r_mempool_internal_pools/init_
 * preallocated_r_mempool_internal_pools/init_r_mempool_pseudo_pool, so this
 * reduces to a single pointer comparison instead of a linear scan over
 * rmp->mem_pools on every single realloc call. A bare ccol_mempool_create()
 * pool (never wrapped by any ccol_r_mempool) has owner_rmp == NULL, which can
 * never equal a non-NULL rmp, so it is correctly rejected
 * too. */
static bool entry_belongs_to_rmp(ccol_r_mempool *rmp, ccol_mempool *sub) {
  return sub->owner_rmp == (void *)rmp;
}

/* Does this ranged pool hold any dynamic fallback entries at all? Both of its
 * policies are covered: ccol_fallback_at_first_exhaustion keeps them per
 * sub-pool, ccol_fallback_at_last_exhaustion keeps them on the pseudo_pool.
 * Used to decide whether an address that matched no tier could legitimately be
 * one of ours, before anything is read through it. */
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

/* Resolves an address to the sub-pool of rmp that owns it.
 *
 * A tier is identified from the address alone, so nothing is read through the
 * caller's pointer to find it. An address matching no tier can still be a
 * dynamic fallback entry, but only if this ranged pool is holding some; when it
 * is not, the answer is "not ours" and the entry's header is never touched.
 * *is_dynamic reports which of the two kinds was found. */
static ccol_mempool *rmp_owner_of(ccol_r_mempool *rmp, void *entry,
                                  bool *is_dynamic) {
  *is_dynamic = false;
  for (size_t i = 0; i < rmp->number_of_mempools; ++i) {
    ccol_mempool *sub = rmp->mem_pools[i];
    if (sub && valid_mempool_addr(sub, (uintptr_t)entry)) return sub;
  }

  if (!rmp_holds_dynamic_entries(rmp)) return NULL;

  mp_dynamic_header *dyn = DYNAMIC_ENTRY_HEADER(entry);
  ccol_mempool *owner = dyn->pool_ptr;
  if (!owner || owner->ccol_mempool_mark != _ccol_mempool_mark ||
      !entry_belongs_to_rmp(rmp, owner) ||
      dyn->elem_status != elem_is_not_a_pool_member) {
    return NULL;
  }
  *is_dynamic = true;
  return owner;
}

#ifdef RUNNING_UNIT_TESTS
/* The ranged counterpart of _ccol_mempool_corrupt_entry_status_for_tests. The
 * corruption tests hold a ccol_r_mempool and one of its entries; which sub-pool
 * owns that entry is resolved here, from the address, exactly as the free and
 * realloc paths do. */
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

  bool is_dynamic = false;
  ccol_mempool *owner = rmp_owner_of(rmp, entry, &is_dynamic);
  if (!owner) {
    ccol_assert(false);
  }

  __ccol_mempool_free_entry(owner, entry);
}

/* Reallocates addr to a buffer of at least size bytes. If the requested size
 * maps to the same extended element size as the current allocation, the
 * original pointer is returned unchanged (no copy). Otherwise a new entry is
 * allocated, the smaller of old/new user sizes is copied, and the old entry is
 * freed. */
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

  if (addr) {
    /* Everything is validated before the fast "same tier, return addr
     * unchanged" path below, so that path can never hand back an entry that is
     * already free or was never ours. Ownership comes from the address, and a
     * pool-owned entry's status is then checked in its pool's status array; a
     * dynamic entry's status was already checked while resolving it. */
    owner = rmp_owner_of(rmp, addr, &owner_is_dynamic);
    if (!owner) {
      ccol_assert(false);
    }
    if (!owner_is_dynamic &&
        ENTRY_STATUS(owner, ENTRY_INDEX(owner, (uintptr_t)addr)) !=
            mp_status_taken) {
      ccol_assert(false);
    }

    if (owner_is_dynamic) {
      /* A dynamic entry occupies a one-size-wide tier of its own: it was
       * allocated to exactly the size asked for, so "no move needed" means the
       * request is for exactly the bytes already held. A different size still
       * moves, mirroring a real tier's entry that shrinks across a boundary. */
      if (size == entry_user_size(owner, addr)) {
        return addr;
      }
    } else {
      size_t new_ext_size =
          rmp->mem_pools[ccol_r_mempool_pool_index_for_size(rmp, size)]
              ->extended_elem_size;

      if (new_ext_size == owner->extended_elem_size) {
        // The requested size matches the current
        // size, return the original pointer.
        return addr;
      }
    }
  }

  void *new_entry = ccol_r_mempool_alloc_entry(rmp, size);
  if (!new_entry && addr && !owner_is_dynamic &&
      size <= owner->extended_elem_size) {
    /* The move could not be made, but it was never needed: this entry already
       has room for the requested size where it sits. The check above compares
       the IDEAL tier for the new size against this entry's actual owner, so any
       entry whose owner is a larger tier than the new size would choose arrives
       here: an ordinary shrink across a tier boundary does, and so does an
       entry that was escalated into a larger tier because its own was full.
       An escalated entry whose new size happens to name the tier it sits in
       matches the check above instead and never reaches this point.
       Returning the entry unmoved is what realloc(3) does for a request that
       fits, and it keeps a shrink, or a resize to the size already held, from
       failing for want of memory it does not need. The entry keeps its current
       tier's capacity, which is what entry_user_size already reports for it. */
    return addr;
  }
  if (new_entry && addr) {
    // Both sizes are read back from the entries actually involved (never
    // inferred from the *ideal* target tier for `size`), so this is correct
    // regardless of whether either entry ended up pool-owned, escalated to a
    // larger tier, or (under ccol_fallback_at_last_exhaustion) routed through
    // the pseudo_pool, which can legitimately hold far fewer bytes than the
    // ideal tier its own size would otherwise suggest.
    bool new_is_dynamic = false;
    ccol_mempool *new_owner = rmp_owner_of(rmp, new_entry, &new_is_dynamic);
    size_t old_user_size = entry_user_size(owner, addr);
    size_t new_user_size =
        new_owner ? entry_user_size(new_owner, new_entry) : 0;
    size_t copy_size =
        old_user_size < new_user_size ? old_user_size : new_user_size;

    ccol_mem_cpy(new_entry, addr, copy_size);
    /* Released through the sub-pool that owns it rather than through rmp: this
       is an internal release of an entry this function has already resolved, so
       it needs no second ownership check. */
    ccol_mempool_free_entry(owner, addr);
  }

  return new_entry;
}

/* Returns the used element count of the sub-pool that handles allocations of
 * the given size. Returns 0 for sizes outside the pool's range. */
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

/* Returns the total element capacity of the sub-pool that handles allocations
 * of the given size. Returns 0 for sizes outside the pool's range. */
/* The entry block's size in bytes: every slot's stride plus every slot's status
 * byte. total_elem_count rather than advertised_elem_count deliberately, since
 * the point of this call is the memory actually taken, reserve included, which
 * is the one number ccol_mempool_total_capacity() cannot report without
 * promising capacity that will never be handed out. */
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

/* The sum over every tier. The pseudo_pool carries no entry block of its own,
 * so it contributes nothing. */
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
