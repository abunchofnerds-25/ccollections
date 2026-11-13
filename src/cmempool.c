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

// The fixed-size pool (cmempool) and the ranged pool (crmempool).

#include <cmempool.h>
#include <internal/ctlsmodel.h>
#include <internal/test_helpers/cmempool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// The library half of the link-time layout check in cmempool.h: Only the tag of
// this build's layout exists. It's a scalar and not an array, so its debug
// info is the same under GCC and Clang and the ABI corpus doesn't differ.
#if CMEMPOOL_COMPACT_LAYOUT
const char cmempool__built_with_compact_layout = 0;
#else
const char cmempool__built_with_fast_layout = 0;
#endif

// Every live pool points cmempool_mark here, so a sanity check can spot a bad
// handle.
const char *cmempool_mark_value = "cmempool";

// A free entry seen as its free-list link.
typedef uintptr_t *mp_addr;

struct ccol_mp_magazine;

// A pool entry has no header: Its state is one byte of the status array at
// upper_addr_limit, and a free entry keeps the free-list link in its first
// bytes. The bytes of the whole stride belong to the holder of the entry.
// MP_HAS_CACHE(mp) evaluates to true when mp has a thread cache, the test every
// allocation and free starts with. MP_ENTRY_INDEX(mp, c_entry) evaluates to
// the index of the entry at address c_entry, which the status lookups need: A
// shift in the default layout, a multiply by a reciprocal in the compact one,
// whose product needs twice the address width (__uint128_t on 64-bit
// targets).
#if CMEMPOOL_COMPACT_LAYOUT
#define MP_HAS_CACHE(mp) ((mp)->has_cache)
#if UINTPTR_MAX > 0xFFFFFFFFu
typedef __uint128_t mp_wide;
#define MP_WIDE_BITS 64
#else
typedef uint64_t mp_wide;
#define MP_WIDE_BITS 32
#endif
#define MP_ENTRY_INDEX(mp, c_entry)                                     \
  (size_t)((uintptr_t)(((mp_wide)((c_entry) - (mp)->lower_addr_limit) * \
                        (mp)->entry_index_magic) >>                     \
                       MP_WIDE_BITS) >>                                 \
           (mp)->entry_index_shift)
#else
#define MP_HAS_CACHE(mp) ((mp)->max_magazines != 0)
#define MP_ENTRY_INDEX(mp, c_entry) \
  (size_t)(((c_entry) - (mp)->lower_addr_limit) >> (mp)->entry_shift)
#endif

// Evaluates to the status byte of entry idx of mp, as an lvalue, so the
// alloc and free paths can read and mark an entry.
#define MP_ENTRY_STATUS(mp, idx) (((uint8_t *)(mp)->upper_addr_limit)[(idx)])

// The header of a dynamic fallback entry, right before the address the caller
// gets. Such an entry is outside the pool's buffer, so it can't use the status
// array. We read it only for an address the pool's registry of live dynamic
// entries holds; any other address may have no allocation in front of it.
typedef struct {
  size_t user_size;
  size_t elem_status;
} mp_dynamic_header;

// The bytes in front of a dynamic entry, rounded to the entry alignment so the
// caller's address stays aligned (as long as the allocator gives memory
// aligned for any type, as malloc() does).
#define DYNAMIC_ENTRY_PREFIX_SIZE cmempool__align_up(sizeof(mp_dynamic_header))

// Evaluate to the header, the raw allocated block and the recorded user size
// of a dynamic entry, from the address the caller holds; the dynamic alloc,
// free and realloc paths need them.
#define DYNAMIC_ENTRY_HEADER(entry) \
  ((mp_dynamic_header *)((uint8_t *)(entry) - DYNAMIC_ENTRY_PREFIX_SIZE))

#define DYNAMIC_ENTRY_RAW_BLOCK(entry) \
  ((void *)((uint8_t *)(entry) - DYNAMIC_ENTRY_PREFIX_SIZE))

static inline bool valid_mempool_addr(cmempool *mp, uintptr_t c_entry);

static inline __attribute__((always_inline)) void *mempool_serve_locked(
    cmempool *mp);

// Evaluates to the user size recorded in a dynamic entry's header (see the
// group above).
#define DYNAMIC_ENTRY_USER_SIZE(entry) (DYNAMIC_ENTRY_HEADER(entry)->user_size)

// A fixed-size pool. The fields up to entry_shift (and has_cache in the
// compact layout) are the hot line that every allocation and free reads; on
// LP64 they are exactly 64 bytes. free_inst and free_elem_count belong there,
// because a pool without a thread cache reaches them on every call.
struct cmempool {
  // Points at cmempool_mark_value while the pool is alive.
  const char *cmempool_mark;
#if CMEMPOOL_COMPACT_LAYOUT
  // Turns an entry's byte offset into its index in the compact layout, where
  // the stride isn't a power of two.
  uintptr_t entry_index_magic;
#else
  size_t max_magazines;
#endif
  size_t extended_elem_size;
  // The entries span [lower, upper); the status array starts at upper.
  uintptr_t lower_addr_limit;
  uintptr_t upper_addr_limit;
  void *free_inst;
  size_t free_elem_count;
  bool should_use_locks;
  bool fallback_to_dynamic_memory;
  bool is_preallocated;
  // log2 of the stride in the default layout (0 in the compact one). A uint8_t
  // among the flags, so it fits into padding of the hot line.
  uint8_t entry_shift;
#if CMEMPOOL_COMPACT_LAYOUT
  bool has_cache;
  uint8_t entry_index_shift;
#endif

#if CMEMPOOL_COMPACT_LAYOUT
  size_t max_magazines;
#endif
  // Cold fields: Construction, teardown and introspection.
  size_t magazine_capacity;
  // What the caller asked for. total_elem_count adds the thread-cache reserve,
  // which keeps the advertised count available to every thread; concurrent
  // callers can briefly hold a few more than advertised, never more than the
  // total. Every public count is in terms of the advertised one.
  size_t advertised_elem_count;
  size_t total_elem_count;
  // Live dynamic entries. Changed under the pool lock with release; atomic
  // because a ranged pool reads it in every tier without their locks.
  _Atomic size_t active_dynamic_memory_buffer_count;
  // Magazines handed out, against max_magazines. An unlink must decrement it,
  // or exited threads fill the budget and the pool locks on every call. A plain
  // size_t read under the lock, because an _Atomic qualifier here slows the
  // cached allocate-and-free path although that path never reads it.
  size_t live_magazines;
  struct ccol_mp_magazine *magazines;
  ccol_memmgmt_procs *m_procs;
  void *objects;
  // The registry of live dynamic entries, NULL until the first one; the pool
  // lock guards it.
  uintptr_t *dynamic_entries;
  ccol__mutex lock;
};

// The two states of a pool entry, as one byte in the status array.
typedef enum { mp_status_free = 0, mp_status_taken = 1 } mp_entry_status;

// Word-wide markers in the header of a dynamic entry, picked to stand out in a
// memory dump. Static, so they can't collide with an application symbol at
// static-link time.
#if UINTPTR_MAX == UINT32_MAX
static const size_t elem_is_not_a_pool_member = 0xfadeface;
static const size_t elem_is_freed_dynamic_member = 0xdeadc0de;
#elif UINTPTR_MAX == UINT64_MAX
static const size_t elem_is_not_a_pool_member = 0xfadefacefadeface;
static const size_t elem_is_freed_dynamic_member = 0xdeadc0dedeadc0de;
#else
#error "Unexpected pointer size"
#endif

// The registry of live dynamic fallback entries. A free of an address outside
// the pool's buffer is either one of its dynamic entries or a caller error,
// and the registry answers from the address alone, before the header in front
// of it is read. It's an open-addressing set with linear probing and
// backward-shift deletion (no tombstones), kept at most half full. The slot
// index is the high bits of a multiplicative hash, because the low bits of an
// aligned address are constant. The table stores its shift in the word before
// slot 0, so the pool needs one pointer. MP_DYNAMIC_SET_MIN_CAPACITY is the
// smallest table, MP_ADDR_BITS the address width and MP_ADDR_HASH_MUL the
// Fibonacci multiplier of the hash.
#define MP_DYNAMIC_SET_MIN_CAPACITY 16u
#define MP_ADDR_BITS (sizeof(uintptr_t) * CHAR_BIT)
#if UINTPTR_MAX == UINT32_MAX
#define MP_ADDR_HASH_MUL ((uintptr_t)0x9E3779B9u)
#else
#define MP_ADDR_HASH_MUL ((uintptr_t)0x9E3779B97F4A7C15ull)
#endif

// Gives the shift that reduces a hash to a slot index of mp's registry; the
// table has 1 << (address bits - shift) slots. The registry helpers need it to
// size and index the table, and it's stored in the word before slot 0.
static inline unsigned mp_dynamic_set_shift(const cmempool *mp) {
  return (unsigned)mp->dynamic_entries[-1];
}

// Gives the home slot of key in mp's registry, from the high bits of a
// multiplicative hash. Every probe of the registry starts there.
static inline size_t mp_dynamic_set_home(const cmempool *mp, uintptr_t key) {
  return (size_t)((key * MP_ADDR_HASH_MUL) >> mp_dynamic_set_shift(mp));
}

// Gives the slot count of mp's registry, or 0 when it has none yet. The
// registry helpers use it as their probe mask and their "not found" answer.
static inline size_t mp_dynamic_set_capacity(const cmempool *mp) {
  if (!mp->dynamic_entries) return 0;
  return (size_t)1 << (MP_ADDR_BITS - mp_dynamic_set_shift(mp));
}

// Writes key into the first empty slot of its probe sequence. Insert and
// growth use it; the table must have room and must not hold key yet.
static void mp_dynamic_set_place(cmempool *mp, uintptr_t key) {
  size_t mask = mp_dynamic_set_capacity(mp) - 1;
  size_t i = mp_dynamic_set_home(mp, key);
  while (mp->dynamic_entries[i] != 0) i = (i + 1) & mask;
  mp->dynamic_entries[i] = key;
}

// Looks key up in mp's registry, for the free path's ownership check and for
// removal. Gives the slot that holds key, or the capacity when it's absent.
// Needs mp->lock.
static size_t mp_dynamic_set_find_locked(const cmempool *mp, uintptr_t key) {
  size_t cap = mp_dynamic_set_capacity(mp);
  if (cap == 0) return 0;
  size_t mask = cap - 1;
  for (size_t i = mp_dynamic_set_home(mp, key);; i = (i + 1) & mask) {
    uintptr_t k = mp->dynamic_entries[i];
    if (k == key) return i;
    if (k == 0) return cap;
  }
}

// Tells whether key is a live dynamic entry of mp. A ranged pool asks this to
// find which tier owns an address outside every buffer. Needs mp->lock.
static bool mp_dynamic_set_contains_locked(const cmempool *mp, uintptr_t key) {
  return mp_dynamic_set_find_locked(mp, key) != mp_dynamic_set_capacity(mp);
}

// Records key, a dynamic entry about to be handed out, in mp's registry, so a
// later free can prove it's ours before reading its header. It grows the table
// (through mp's allocator) to keep it at most half full. Gives false when the
// growth can't be allocated; the caller then gives the entry back and fails
// the allocation. Needs mp->lock; the live count doesn't include key yet.
static __attribute__((noinline)) bool mp_dynamic_set_insert_locked(
    cmempool *mp, uintptr_t key) {
  size_t live = atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                                     memory_order_relaxed);
  size_t old_cap = mp_dynamic_set_capacity(mp);
  if ((live + 1) * 2 > old_cap) {
    if (old_cap > SIZE_MAX / 4 / sizeof(uintptr_t)) return false;
    size_t new_cap = old_cap ? old_cap * 2 : MP_DYNAMIC_SET_MIN_CAPACITY;
    uintptr_t new_shift =
        old_cap ? mp->dynamic_entries[-1] - 1 : MP_ADDR_BITS - 4u;
    uintptr_t *fresh = (uintptr_t *)ccol__mem_calloc(mp->m_procs, new_cap + 1,
                                                     sizeof(uintptr_t));
    if (!fresh) return false;
    fresh[0] = new_shift;
    uintptr_t *old = mp->dynamic_entries;
    mp->dynamic_entries = fresh + 1;
    for (size_t i = 0; i < old_cap; ++i) {
      if (old[i] != 0) mp_dynamic_set_place(mp, old[i]);
    }
    if (old) ccol__mem_free(mp->m_procs, old - 1);
  }
  mp_dynamic_set_place(mp, key);
  return true;
}

// Removes key from mp's registry when a dynamic entry is freed; the removal is
// also the ownership check, so a second free of the same entry fails. Later
// members of the cluster shift back into the hole when it lies on their probe
// path, so the table never holds tombstones. Gives false when key isn't a live
// dynamic entry of mp. Needs mp->lock.
static __attribute__((noinline)) bool mp_dynamic_set_remove_locked(
    cmempool *mp, uintptr_t key) {
  size_t cap = mp_dynamic_set_capacity(mp);
  size_t i = mp_dynamic_set_find_locked(mp, key);
  if (i == cap) return false;
  size_t mask = cap - 1;
  for (size_t j = (i + 1) & mask;; j = (j + 1) & mask) {
    uintptr_t k = mp->dynamic_entries[j];
    if (k == 0) break;
    size_t home = mp_dynamic_set_home(mp, k);
    if (((j - home) & mask) >= ((j - i) & mask)) {
      mp->dynamic_entries[i] = k;
      i = j;
    }
  }
  mp->dynamic_entries[i] = 0;
  return true;
}

// Frees mp's registry table. Only a destroy calls it, once no dynamic entry
// is live.
static void mp_dynamic_set_release(cmempool *mp) {
  if (mp->dynamic_entries) ccol__mem_free(mp->m_procs, mp->dynamic_entries - 1);
  mp->dynamic_entries = NULL;
}

// Hands out one dynamic fallback entry of user_size bytes owned by mp. A pool
// that ran out uses it when its fallback is on, and so does the pseudo pool of
// a ranged pool. It allocates the header and the entry through mp's allocator,
// records the entry in the registry and bumps the live count (release). Gives
// NULL when the allocator refuses or the size can't carry the header. Needs
// mp->lock when the pool locks.
static __attribute__((noinline)) void *mempool_alloc_dynamic_locked(
    cmempool *mp, size_t user_size) {
  if (user_size > SIZE_MAX - DYNAMIC_ENTRY_PREFIX_SIZE) return NULL;
  void *raw_block =
      ccol__mem_alloc(mp->m_procs, DYNAMIC_ENTRY_PREFIX_SIZE + user_size);
  if (!raw_block) return NULL;
  void *result = (uint8_t *)raw_block + DYNAMIC_ENTRY_PREFIX_SIZE;
  if (!mp_dynamic_set_insert_locked(mp, (uintptr_t)result)) {
    ccol__mem_free(mp->m_procs, raw_block);
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

// The thread cache. Each thread keeps a magazine of free entries per pool, so
// the common allocation and free take no lock and no atomic read-modify-write;
// only refilling an empty magazine and flushing a full one touch the shared
// list under the pool lock. A pool asked for N entries allocates N + R slots,
// R being the most all caches can hold, so N stay available to any thread. R
// is at most N/2 and shrinks as a proportion for large pools.
//
// The most magazines one pool hands out, the most entries one magazine holds,
// the smallest pool that gets a cache, and the reserve as N divided by this
// divisor (deeper magazines hit their refill and flush boundaries less
// often).
#define CCOL_MP_MAGAZINE_LIMIT 16u
#define CCOL_MP_CAPACITY_LIMIT 128u
#define CCOL_MP_MIN_CACHED_POOL 8u
#define CCOL_MP_RESERVE_DIVISOR 2u
// A thread's direct-mapped cache of magazines, so a ranged pool working across
// tiers doesn't thrash one slot. A power of two, so mempool_tls_slot() can
// mask.
#define CCOL_MP_TLS_SLOTS 8u
_Static_assert(CCOL_MP_TLS_SLOTS > 0u &&
                   (CCOL_MP_TLS_SLOTS & (CCOL_MP_TLS_SLOTS - 1u)) == 0,
               "mempool_tls_slot() reduces with a mask, which only covers the "
               "whole slot array while this is a power of two; zero satisfies "
               "the power-of-two test on its own and must be excluded, since "
               "it would make the mask every bit");

// One thread's magazine for one pool. It's bigger than a cache line, so two
// magazines never share one.
typedef struct ccol_mp_magazine {
  // Cleared (release) by whoever unlinks it from its pool; the owning thread
  // frees an orphaned magazine on seeing NULL (acquire), so the unlink must be
  // visible first.
  _Atomic(cmempool *) pool;
  // Written only by the owning thread, read relaxed by the introspection
  // functions, so cmempool_used_count() is exact at no cost to the fast path.
  _Atomic size_t count;
  size_t capacity;
  struct ccol_mp_magazine *next_in_pool;
  struct ccol_mp_magazine *prev_in_pool;
  struct ccol_mp_magazine *next_in_thread;
  // Kept here because an orphaned magazine outlives its pool's m_procs; NULL
  // for the default allocator.
  ccol_free free_proc;
  // Sized for this pool's magazine capacity.
  void *slots[];
} ccol_mp_magazine;

// A thread's whole cache state in one object, so a call resolves one
// thread-local and not several. A key destructor only runs for a thread whose
// key value is set, so armed records that the thread set it; under
// CCOL__EMULATE_DARWIN_TLS the state is the key's value instead.
typedef struct {
  ccol_mp_magazine *mags;
  // The reap clears every slot that points at a magazine before freeing it,
  // so a magazine's own pool field is the whole hit test.
  ccol_mp_magazine *cache[CCOL_MP_TLS_SLOTS];
  bool armed;
} mp_tls_state;

// The thread's cache state, with the TLS model of ctlsmodel.h. initial-exec
// keeps a __tls_get_addr call off every allocation and free; build with
// -DCMEMPOOL_DYNAMIC_TLS=1 when a dlopen() into a full static TLS block must
// work. Under CCOL__EMULATE_DARWIN_TLS there's no __thread object.
#if defined(CCOL__EMULATE_DARWIN_TLS)
#elif defined(CMEMPOOL_DYNAMIC_TLS) && CMEMPOOL_DYNAMIC_TLS
static __thread mp_tls_state mp_tls;
#else
static __thread mp_tls_state mp_tls __attribute__((tls_model("initial-exec")));
#endif

// The key whose destructor drains a thread's magazines at exit.
static ccol__thread_ls_key mp_tls_key;
// Serializes a thread's exit drain against a pool destroy, so the drain never
// locks a freed pool. Taken before any pool lock.
static ccol__mutex mp_registry_lock;
// Serializes arming a thread against the deletion of the key at unload. A leaf
// that may be taken with a pool lock held. It can't be the registry lock: A
// pool's allocator may be another pool that arms the thread, and arming under
// the registry lock would deadlock with an exit drain.
static ccol__mutex mp_arm_lock;
// True while the key exists and a thread may arm it; it goes false under the
// arm lock at unload.
static atomic_bool mp_cache_live;
// Creates the locks and the key once.
static ccol__once_flag mp_cache_once = CCOL__ONCE_INIT;

static void mp_tls_drain(void *arg);

// Gives the calling thread's cache state, which every cache path starts from.
// Under CCOL__EMULATE_DARWIN_TLS it's the key's heap block: NULL when the
// thread has none or the cache is gone (mp_cache_live false). Elsewhere it's
// the __thread object and the callers' NULL tests fold away.
#if defined(CCOL__EMULATE_DARWIN_TLS)
static inline __attribute__((always_inline)) mp_tls_state *mp_tls_cur(void) {
  if (!atomic_load_explicit(&mp_cache_live, memory_order_acquire)) return NULL;
  return (mp_tls_state *)ccol__thread_ls_get(mp_tls_key);
}
#else
static inline __attribute__((always_inline)) mp_tls_state *mp_tls_cur(void) {
  return &mp_tls;
}
#endif

// Creates the registry lock, the arm lock and the key, then sets mp_cache_live.
// mp_cache_available() runs it once; any failure leaves the cache off.
static void mp_cache_init(void) {
  if (ccol__mutex_init(mp_registry_lock) != 0) return;
  if (ccol__mutex_init(mp_arm_lock) != 0) return;
  if (ccol__thread_ls_key_create(mp_tls_key, mp_tls_drain) != 0) return;
  atomic_store(&mp_cache_live, true);
}

// Tells whether a new pool may get a thread cache: true from the moment the key
// exists until the module unloads. A pool created while it's false (for
// example after PTHREAD_KEYS_MAX) runs without a cache instead of failing.
static bool mp_cache_available(void) {
  ccol__call_once(mp_cache_once, mp_cache_init);
  return atomic_load(&mp_cache_live);
}

// Works out the magazine count, the magazine capacity and the reserve for a
// pool of elem_count entries, which the constructor needs to size the block.
// Pools below CCOL_MP_MIN_CACHED_POOL get none; that gate is separate from the
// clamps, which would otherwise force a magazine onto a one-element pool and a
// reserve of N. A reserve that would overflow leaves all three at 0.
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
  if (cap > CCOL_MP_CAPACITY_LIMIT) cap = CCOL_MP_CAPACITY_LIMIT;
  if (cap == 0) cap = 1;

  if (mags > SIZE_MAX / cap) return;
  size_t r = mags * cap;
  if (r > SIZE_MAX - elem_count) return;

  *max_magazines = mags;
  *capacity = cap;
  *reserve = r;
}

// Gives the entries of mp handed out to callers right now: What left the
// shared free list minus what sits in the magazines. The allocation limit and
// cmempool_used_count() need it. Needs mp->lock; the magazine counts are read
// relaxed.
static size_t mempool_live_count_locked(cmempool *mp) {
  size_t live = mp->total_elem_count - mp->free_elem_count;
  for (ccol_mp_magazine *mag = mp->magazines; mag; mag = mag->next_in_pool) {
    size_t cached = atomic_load_explicit(&mag->count, memory_order_relaxed);
    live = (live > cached) ? (live - cached) : 0;
  }
  return live;
}

// Moves entries from mp's free list into mag until it's full, so the next
// allocations of that thread take no lock. List corruption is reported through
// *corrupt so the caller can unlock before asserting. Needs mp->lock, and
// changes the free list, free_elem_count and mag's count.
// The budget is free_elem_count minus the reserve: An entry in another
// thread's magazine counts as handed out, because nothing checks the budget
// again before it's popped. Charging only this magazine would let several
// magazines each overshoot together. The bound can't be exact, because the
// locked path must keep serving while callers hold fewer than advertised; this
// keeps the overshoot to a few entries.
static void mempool_magazine_refill_locked(cmempool *mp, ccol_mp_magazine *mag,
                                           bool *corrupt) {
  size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
  size_t reserve = mp->total_elem_count - mp->advertised_elem_count;
  if (mp->free_elem_count <= reserve) return;
  size_t budget = mp->free_elem_count - reserve;

  while (budget > 0 && count < mag->capacity && mp->free_inst) {
    void *entry = mp->free_inst;
    if (!valid_mempool_addr(mp, (uintptr_t)entry) ||
        MP_ENTRY_STATUS(mp, MP_ENTRY_INDEX(mp, (uintptr_t)entry)) !=
            mp_status_free) {
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

// Gives how_many of mag's entries back to mp's free list, when a magazine is
// full or its thread exits. Callers flush half of a full magazine, so a caller
// at the full mark doesn't lock on every second call. Cached entries are
// already marked free, so only the links change. Needs mp->lock.
static void mempool_magazine_flush_locked(cmempool *mp, ccol_mp_magazine *mag,
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

// Frees a magazine through the free_proc it stored, because an orphaned
// magazine can outlive its pool's procs.
static void mempool_magazine_free(ccol_mp_magazine *mag) {
  if (mag->free_proc)
    mag->free_proc(mag);
  else
    ccol__default_mem_free(mag);
}

// Drops an orphaned magazine (its pool was destroyed) from this thread's list
// at *link, clears the direct-map slots that point at it and frees it, so
// dead magazines don't pile up. No lock: An orphan is unlinked from its pool
// before its pool pointer is cleared, so this thread is its only owner, and
// the destroy has already taken it off live_magazines.
static void mempool_tls_reap(mp_tls_state *tls, ccol_mp_magazine **link,
                             ccol_mp_magazine *mag) {
  *link = mag->next_in_thread;
  for (size_t i = 0; i < CCOL_MP_TLS_SLOTS; ++i) {
    if (tls->cache[i] == mag) tls->cache[i] = NULL;
  }
  mempool_magazine_free(mag);
}

// Gives the direct-map slot of a pool, from address bits 8 to 10. The tiers of
// a ranged pool are separate allocations a fixed distance apart, and the low
// bits above the alignment alias for common strides; these bits spread the
// tiers best over measured layouts, at the cost of one shift and one mask.
static inline __attribute__((always_inline)) size_t
mempool_tls_slot(const cmempool *mp) {
  return (size_t)(((uintptr_t)mp >> 8) & (uintptr_t)(CCOL_MP_TLS_SLOTS - 1));
}

// Gives this thread's magazine for mp when the direct map holds it, else
// NULL. It's the hit every call after a thread's first touch of a pool takes,
// inlined into the fast paths; the miss path stays out of line. Reads the
// thread's cache state and the magazine's pool pointer (acquire).
static inline __attribute__((always_inline)) ccol_mp_magazine *
mempool_tls_magazine_hit(cmempool *mp) {
  mp_tls_state *tls = mp_tls_cur();
  if (!tls) return NULL;
  size_t slot = mempool_tls_slot(mp);
  ccol_mp_magazine *mag = tls->cache[slot];
  if (__builtin_expect(
          mag && atomic_load_explicit(&mag->pool, memory_order_acquire) == mp,
          1))
    return mag;
  return NULL;
}

// Finds this thread's magazine for mp after a direct-map miss, by walking the
// thread's list, caches it in the direct map and gives it, or NULL. Magazines
// of destroyed pools are reaped on the way. No lock. A reap calls an allocator
// that may be another cached pool and reap inside this walk, so the walk
// restarts from the head after each reap.
static ccol_mp_magazine *mempool_tls_magazine_find(cmempool *mp) {
  mp_tls_state *tls = mp_tls_cur();
  if (!tls) return NULL;
  ccol_mp_magazine *mag;

  ccol_mp_magazine **link = &tls->mags;
  while ((mag = *link) != NULL) {
    cmempool *owner = atomic_load_explicit(&mag->pool, memory_order_acquire);
    if (!owner) {
      mempool_tls_reap(tls, link, mag);
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

// Builds a magazine for this thread and claims one of mp's magazines, the
// first time a thread uses a cached pool. Gives it, or NULL for "use the
// locked path" (no key value, no memory, or no magazine left).
// The thread first arms the key under mp_arm_lock while mp_cache_live holds,
// because a magazine held without a key value is never drained at exit; the
// arm lock orders that against the key's deletion at unload. The magazine is
// complete before it's linked, because the teardown walk clears the pool
// pointer of every magazine it reaches. The allocation runs outside the pool
// lock, and the claim checks live_magazines again under it, linking the
// magazine into mp's list and the thread's list.
static __attribute__((noinline)) ccol_mp_magazine *mempool_tls_magazine_create(
    cmempool *mp) {
  mp_tls_state *tls = mp_tls_cur();

#if defined(CCOL__EMULATE_DARWIN_TLS)
  if (!tls) {
    tls = (mp_tls_state *)calloc(1, sizeof(*tls));
    if (!tls) return NULL;
    bool set = false;
    ccol__mutex_lock(mp_arm_lock);
    if (atomic_load_explicit(&mp_cache_live, memory_order_relaxed) &&
        ccol__thread_ls_set(mp_tls_key, tls) == 0)
      set = true;
    ccol__mutex_unlock(mp_arm_lock);
    if (!set) {
      free(tls);
      return NULL;
    }
  }
#else
  if (!tls->armed) {
    ccol__mutex_lock(mp_arm_lock);
    if (atomic_load_explicit(&mp_cache_live, memory_order_relaxed) &&
        ccol__thread_ls_set(mp_tls_key, (void *)1) == 0)
      tls->armed = true;
    ccol__mutex_unlock(mp_arm_lock);
    if (!tls->armed) return NULL;
  }
#endif

  ccol_mp_magazine *mag = ccol__mem_calloc(
      mp->m_procs, 1, sizeof(*mag) + mp->magazine_capacity * sizeof(void *));
  if (!mag) return NULL;

  mag->capacity = mp->magazine_capacity;
  mag->free_proc = mp->m_procs ? mp->m_procs->free : NULL;
  atomic_store_explicit(&mag->pool, mp, memory_order_release);

  bool claimed = false;
  ccol__mutex_lock(mp->lock);
  if (mp->live_magazines < mp->max_magazines) {
    ++mp->live_magazines;
    mag->next_in_pool = mp->magazines;
    if (mp->magazines) mp->magazines->prev_in_pool = mag;
    mp->magazines = mag;
    claimed = true;
  }
  ccol__mutex_unlock(mp->lock);

  if (!claimed) {
    mempool_magazine_free(mag);
    return NULL;
  }

  mag->next_in_thread = tls->mags;
  tls->mags = mag;
  tls->cache[mempool_tls_slot(mp)] = mag;
  return mag;
}

// Unlinks mag from mp's list and gives its slot back to live_magazines, when a
// thread exits or the pool is destroyed. Needs mp->lock.
static void mempool_magazine_unlink_locked(cmempool *mp,
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

// Drains this thread's magazines at its exit; it's the key's destructor, so
// cached entries aren't lost to their pools and the magazines don't leak. It
// flushes and unlinks each magazine under mp_registry_lock and its pool's lock
// (so a destroy can't free the pool between the pointer read and the lock),
// then frees it. Freeing a magazine may call another cached pool's allocator,
// which builds a new magazine on this thread, so it loops until the list stays
// empty. Under CCOL__EMULATE_DARWIN_TLS arg is the state block, and it frees
// it; elsewhere it clears armed.
static void mp_tls_drain(void *arg) {
#if defined(CCOL__EMULATE_DARWIN_TLS)
  mp_tls_state *tls = (mp_tls_state *)arg;
  if (!tls) return;
#else
  mp_tls_state *tls = &mp_tls;
  (void)arg;
#endif
  ccol_mp_magazine *mag;
  while ((mag = tls->mags) != NULL) {
    tls->mags = NULL;
    for (size_t i = 0; i < CCOL_MP_TLS_SLOTS; ++i) tls->cache[i] = NULL;
    while (mag) {
      ccol_mp_magazine *next = mag->next_in_thread;
      ccol__mutex_lock(mp_registry_lock);
      cmempool *mp = atomic_load_explicit(&mag->pool, memory_order_acquire);
      if (mp) {
        ccol__mutex_lock(mp->lock);
        mempool_magazine_flush_locked(
            mp, mag, atomic_load_explicit(&mag->count, memory_order_relaxed));
        mempool_magazine_unlink_locked(mp, mag);
        ccol__mutex_unlock(mp->lock);
      }
      ccol__mutex_unlock(mp_registry_lock);
      mempool_magazine_free(mag);
      mag = next;
    }
  }
#if defined(CCOL__EMULATE_DARWIN_TLS)
  free(tls);
#else
  tls->armed = false;
#endif
}

// Shuts the thread cache down at unload (dlclose() and exit), because the key
// holds a destructor address that would be unmapped. It drains the calling
// thread, then, under mp_arm_lock, clears mp_cache_live and deletes the key,
// so a thread that outlives a dlclose() never runs unmapped drain code; its
// magazines are leaked. Existing pools keep working through their locked paths
// and new pools get no cache. A second run returns at once.
__attribute__((destructor)) static void mp_cache_fini(void) {
  if (!atomic_load(&mp_cache_live)) return;
#if defined(CCOL__EMULATE_DARWIN_TLS)
  mp_tls_state *tls = mp_tls_cur();
  if (tls) {
    (void)ccol__thread_ls_set(mp_tls_key, NULL);
    mp_tls_drain(tls);
  }
#else
  mp_tls_drain(NULL);
#endif
  ccol__mutex_lock(mp_arm_lock);
  atomic_store(&mp_cache_live, false);
  ccol__thread_ls_key_delete(mp_tls_key);
  ccol__mutex_unlock(mp_arm_lock);
}

static void *mempool_alloc_locked(cmempool *mp);
static void mempool_free_locked(cmempool *mp, void *entry);

// Allocates an entry for a thread whose inlined pop failed: No magazine in the
// direct map, or an empty one (mag, possibly NULL). The pool has a cache and
// no lock is held. Gives the entry, or NULL for an exhausted pool. Apart from
// creating a magazine once, it costs at most one critical section: A thread
// the pool refuses a magazine is served under the same lock, and an empty
// magazine whose refill yields nothing is served under the refill's lock. That
// answer is final. Kept out of line, because inlining it slows pools without a
// cache.
static __attribute__((noinline)) void *mempool_alloc_from_cache_slow(
    cmempool *mp, ccol_mp_magazine *mag) {
  void *served;
  if (!mag) {
    mag = mempool_tls_magazine_find(mp);
    if (!mag) {
      ccol__mutex_lock(mp->lock);
      bool budget_available = mp->live_magazines < mp->max_magazines;
      served = budget_available ? NULL : mempool_serve_locked(mp);
      ccol__mutex_unlock(mp->lock);
      if (!budget_available) return served;
      mag = mempool_tls_magazine_create(mp);
      if (!mag) return mempool_alloc_locked(mp);
    }
  }

  if (atomic_load_explicit(&mag->count, memory_order_relaxed) == 0) {
    bool corrupt = false;
    served = NULL;
    ccol__mutex_lock(mp->lock);
    mempool_magazine_refill_locked(mp, mag, &corrupt);
    bool refilled =
        atomic_load_explicit(&mag->count, memory_order_relaxed) != 0;
    if (!corrupt && !refilled) served = mempool_serve_locked(mp);
    ccol__mutex_unlock(mp->lock);
    if (corrupt) {
      ccol__assert(false);
    }
    if (!refilled) return served;
  }

  size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
  void *entry = mag->slots[--count];
  atomic_store_explicit(&mag->count, count, memory_order_relaxed);
  MP_ENTRY_STATUS(mp, MP_ENTRY_INDEX(mp, (uintptr_t)entry)) =
      (uint8_t)mp_status_taken;
  return entry;
}

// Allocates an entry for cmempool_alloc_entry() on a cached pool by popping
// this thread's magazine, or through the slow path. The __builtin_expect hints
// keep the pop right after the lookup at the 64-byte aligned start of the entry
// point; placed elsewhere it can split across two cache lines. Every other
// case tail-jumps out, so there's no frame.
static inline __attribute__((always_inline)) void *mempool_alloc_from_cache(
    cmempool *mp) {
  ccol_mp_magazine *mag = mempool_tls_magazine_hit(mp);
  if (__builtin_expect(mag != NULL, 1)) {
    size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
    if (__builtin_expect(count != 0, 1)) {
      void *entry = mag->slots[--count];
      atomic_store_explicit(&mag->count, count, memory_order_relaxed);
      MP_ENTRY_STATUS(mp, MP_ENTRY_INDEX(mp, (uintptr_t)entry)) =
          (uint8_t)mp_status_taken;
      return entry;
    }
  }
  return mempool_alloc_from_cache_slow(mp, mag);
}

// Frees an entry (index idx) that can't just be pushed onto this thread's
// magazine. A thread with no magazine that the pool refuses gives the entry
// back to the shared list under one lock, rereading the status there; a full
// magazine flushes half under the lock first. What can't be parked goes to
// the locked path. mag is the direct-map result, possibly NULL.
static __attribute__((noinline)) void mempool_free_to_cache_slow(
    cmempool *mp, void *entry, size_t idx, ccol_mp_magazine *mag) {
  if (!mag) {
    mag = mempool_tls_magazine_find(mp);
    if (!mag) {
      bool corrupt = false;
      ccol__mutex_lock(mp->lock);
      bool budget_available = mp->live_magazines < mp->max_magazines;
      if (!budget_available) {
        if (MP_ENTRY_STATUS(mp, idx) != mp_status_taken) {
          corrupt = true;
        } else {
          MP_ENTRY_STATUS(mp, idx) = (uint8_t)mp_status_free;
          *(void **)entry = mp->free_inst;
          mp->free_inst = entry;
          ++mp->free_elem_count;
        }
      }
      ccol__mutex_unlock(mp->lock);
      if (corrupt) {
        ccol__assert(false);
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
    ccol__mutex_lock(mp->lock);
    mempool_magazine_flush_locked(mp, mag, half);
    ccol__mutex_unlock(mp->lock);
    count = atomic_load_explicit(&mag->count, memory_order_relaxed);
  }
  if (count >= mag->capacity) {
    mempool_free_locked(mp, entry);
    return;
  }

  MP_ENTRY_STATUS(mp, idx) = (uint8_t)mp_status_free;
  mag->slots[count++] = entry;
  atomic_store_explicit(&mag->count, count, memory_order_relaxed);
}

// Frees an entry for cmempool__free_entry() on a cached pool by pushing it onto
// this thread's magazine, or through the slow or locked path. The tests run in
// the order of the locked path: The bounds check first (it also sends dynamic
// entries to the locked path), then the status byte. The bounds are immutable,
// but the status byte is read with no lock, so two threads freeing one entry at
// the same instant can slip past; that's a data race in the caller anyway.
static inline __attribute__((always_inline)) void mempool_free_to_cache(
    cmempool *mp, void *entry, uintptr_t c_entry) {
  if (__builtin_expect(!valid_mempool_addr(mp, c_entry), 0)) {
    mempool_free_locked(mp, entry);
    return;
  }

  size_t idx = MP_ENTRY_INDEX(mp, c_entry);
  if (__builtin_expect(MP_ENTRY_STATUS(mp, idx) != mp_status_taken, 0)) {
    ccol__assert(false);
  }

  ccol_mp_magazine *mag = mempool_tls_magazine_hit(mp);
  if (__builtin_expect(mag != NULL, 1)) {
    size_t count = atomic_load_explicit(&mag->count, memory_order_relaxed);
    if (__builtin_expect(count < mag->capacity, 1)) {
      MP_ENTRY_STATUS(mp, idx) = (uint8_t)mp_status_free;
      mag->slots[count++] = entry;
      atomic_store_explicit(&mag->count, count, memory_order_relaxed);
      return;
    }
  }
  mempool_free_to_cache_slow(mp, entry, idx, mag);
}

#ifdef RUNNING_UNIT_TESTS

// Test hook: Overwrites the status byte of entry in mp, so a corruption test
// can prove the pool catches a state no correct call sequence produces.
void cmempool__corrupt_entry_status_for_tests(cmempool *mp, void *entry,
                                              unsigned char value) {
  if (!mp || !entry) return;
  if (!valid_mempool_addr(mp, (uintptr_t)entry)) return;
  MP_ENTRY_STATUS(mp, MP_ENTRY_INDEX(mp, (uintptr_t)entry)) = value;
}

// Test hook: Gives the magazines mp has handed out (under its lock), so a test
// can see that a thread's exit gives its slot back.
size_t cmempool__live_magazines_for_tests(cmempool *mp) {
  if (!mp) return 0;
  size_t n;
  if (mp->should_use_locks) ccol__mutex_lock(mp->lock);
  n = mp->live_magazines;
  if (mp->should_use_locks) ccol__mutex_unlock(mp->lock);
  return n;
}

// Test hook: Gives the magazines the calling thread holds, so a test can see
// that orphans get reaped.
size_t cmempool__thread_magazines_for_tests(void) {
  size_t n = 0;
  mp_tls_state *tls = mp_tls_cur();
  for (ccol_mp_magazine *mag = tls ? tls->mags : NULL; mag;
       mag = mag->next_in_thread)
    n++;
  return n;
}

// Test hook: Runs the unload handler while the code stays mapped.
void cmempool__unload_cache_for_tests(void) { mp_cache_fini(); }

// Test hook: Gives the slots above mp's advertised count; 0 without a cache.
size_t cmempool__reserve_for_tests(cmempool *mp) {
  if (!mp) return 0;
  return mp->total_elem_count - mp->advertised_elem_count;
}
#endif

// Destroys mp, the body of cmempool_destroy(); NULL does nothing. It cuts
// loose every magazine first, under mp_registry_lock and then mp->lock, so an
// exit drain can't lock a destroyed mutex. Each magazine is unlinked before its
// pool pointer is cleared, because its owner frees it on seeing NULL; its
// entries aren't drained, since the owner writes them unsynchronized. It
// asserts when dynamic entries are still live, then frees the registry, the
// block (unless preallocated) and the pool through its allocator.
void cmempool__destroy(cmempool *mp) {
  if (mp) {
    if (mp->max_magazines != 0) {
      ccol__mutex_lock(mp_registry_lock);
      ccol__mutex_lock(mp->lock);
      ccol_mp_magazine *mag = mp->magazines;
      while (mag) {
        ccol_mp_magazine *next = mag->next_in_pool;
        mempool_magazine_unlink_locked(mp, mag);
        atomic_store_explicit(&mag->pool, NULL, memory_order_release);
        mag = next;
      }
      ccol__mutex_unlock(mp->lock);
      ccol__mutex_unlock(mp_registry_lock);
    }

    if (mp->fallback_to_dynamic_memory) {
      if (cmempool_dynamic_allocs_count(mp) > 0) {
        ccol__assert(false);
      }
    }

    mp_dynamic_set_release(mp);

    if (mp->should_use_locks) {
      ccol__mutex_destroy(mp->lock);
    }

    if (!mp->is_preallocated && mp->objects) {
      ccol__mem_free(mp->m_procs, mp->objects);
    }

    if (mp->m_procs) {
      ccol_free free_func = mp->m_procs->free;
      free_func(mp->m_procs);
      free_func(mp);
    } else {
      ccol__default_mem_free(mp);
    }
  }
}

// Gives the stride for elem_size at run time, the same value as
// cmempool__stride() (which exists separately because an array size must be a
// constant expression): Raised to sizeof(mp_addr), aligned and, in the default
// layout, rounded to a power of two. Gives 0 when no stride fits, which
// callers treat as a bad elem_size.
static size_t mp_stride_for(size_t elem_size) {
  if (elem_size < sizeof(mp_addr)) elem_size = sizeof(mp_addr);
  if (!cmempool__align_up_fits(elem_size)) return 0;
  size_t base = cmempool__align_up(elem_size);
#if CMEMPOOL_COMPACT_LAYOUT
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

// Computes the bytes of a block of count slots (strides plus status bytes)
// into *out. The allocator gets one pre-multiplied size and can't catch a wrap
// itself, so this check must. Gives false on overflow; it matches
// cmempool__buffer_params_fit().
static bool mp_block_bytes(size_t count, size_t stride, size_t *out) {
  if (count == 0 || stride > (SIZE_MAX - count) / count) return false;
  *out = count * stride + count;
  return true;
}

#if CMEMPOOL_COMPACT_LAYOUT
// Finds a multiplier and shift (into *magic and *shift) that turn a division
// by a compact stride into a multiply and a shift, so the compact layout can
// index entries without dividing. A slightly wrong reciprocal gives a
// neighbouring index and corrupts another entry's status, so the result is
// proved exhaustively: Both functions never go down, so agreeing at every
// multiple of the stride and the byte below it means agreeing everywhere, which
// also makes the bounds test reject an address between entries. Gives false
// when no shift works, and the pool isn't built.
static bool mp_build_reciprocal(size_t stride, size_t count, uintptr_t *magic,
                                uint8_t *shift) {
  if (stride == 0 || count == 0) return false;
  for (uint8_t s = 0; s < MP_WIDE_BITS; ++s) {
    mp_wide candidate = (((mp_wide)1 << (MP_WIDE_BITS + s)) / stride) + 1;
    if ((candidate >> MP_WIDE_BITS) != 0) continue;
    uintptr_t m = (uintptr_t)candidate;

    bool ok = true;
    for (size_t i = 0; i <= count && ok; ++i) {
      uintptr_t at = (uintptr_t)i * (uintptr_t)stride;
      uintptr_t high_at = (uintptr_t)(((mp_wide)at * m) >> MP_WIDE_BITS);
      if ((size_t)(high_at >> s) != i) {
        ok = false;
      } else if (i > 0) {
        uintptr_t high_below =
            (uintptr_t)(((mp_wide)(at - 1) * m) >> MP_WIDE_BITS);
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

#if !CMEMPOOL_COMPACT_LAYOUT
// Gives log2 of a power of two; the default layout stores it as the index
// shift.
static uint8_t mp_log2_exact(size_t v) {
  uint8_t r = 0;
  while ((size_t)1 << r < v) ++r;
  return r;
}
#endif

// Lays out mp->objects as elem_count entries of extended_elem_size bytes plus
// their status bytes, marks them free, threads them onto the free list and
// fills the pool's scalar fields. Both constructors finish with it. Gives
// false only in the compact layout, when no reciprocal can be proved.
static bool cmempool_init_internal_scalars(cmempool *mp, size_t elem_count,
                                           size_t extended_elem_size,
                                           bool fallback_to_dynamic_memory) {
  uintptr_t base = (uintptr_t)mp->objects;

  mp->cmempool_mark = cmempool_mark_value;
  mp->extended_elem_size = extended_elem_size;
#if CMEMPOOL_COMPACT_LAYOUT
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

// Creates a heap-backed pool of elem_count entries of elem_size bytes; it's
// the constructor behind cmempool_create() and the tiers of a ranged pool.
// Gives the pool, or NULL with *err set. The block holds the entries and their
// status bytes and is sized before the cache geometry, so an impossible size
// is reported as a bad request. A pool that locks gets a thread cache and its
// reserve when mp_cache_available() says so. The reserve is our addition, so
// when the block with it overflows or can't be allocated, the pool is built
// without a cache at exactly the requested size.
cmempool *cmempool_create_mp(size_t elem_count, size_t elem_size,
                             bool fallback_to_dynamic_memory,
                             bool single_threaded,
                             ccol_memmgmt_procs *mmgmt_procs, char **err) {
  if (err) {
    *err = NULL;
  }

  if (elem_count == 0 || elem_size == 0) {
    if (err) {
      *err = CCOL__ERR_STR("elem_count or elem_size is zero");
    }
    return NULL;
  } else if (mp_stride_for(elem_size) == 0) {
    if (err) {
      *err = CCOL__ERR_STR(
          "elem_size is too large: the stride rounding would "
          "overflow size_t");
    }
    return NULL;
  } else if (elem_size < sizeof(mp_addr)) {
    elem_size = sizeof(mp_addr);
  }

  if (!ccol__verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  cmempool *mp = (cmempool *)ccol__mem_calloc(mmgmt_procs, 1, sizeof(cmempool));
  if (!mp) {
    if (err) {
      *err = CCOL__ERR_STR("failed to allocate memory pool struct");
    }
    return NULL;
  }

  if (!ccol__populate_mem_mgmt_procs(mp, mmgmt_procs, err)) {
    ccol__mem_free(mmgmt_procs, mp);
    return NULL;
  }

  size_t extended_elem_size = mp_stride_for(elem_size);
  if (extended_elem_size == 0) {
    if (err) {
      *err = CCOL__ERR_STR("elem_size is too large for a pool entry");
    }
    cmempool_destroy(mp);
    return NULL;
  }

  size_t asked_bytes = 0;
  if (!mp_block_bytes(elem_count, extended_elem_size, &asked_bytes)) {
    if (err) {
      *err = CCOL__ERR_STR(
          "elem_count and elem_size are too large: the entries and their "
          "status bytes would overflow size_t");
    }
    cmempool_destroy(mp);
    return NULL;
  }

  size_t max_magazines = 0, magazine_capacity = 0, reserve = 0;
  if (!single_threaded && mp_cache_available())
    mempool_cache_geometry(elem_count, &max_magazines, &magazine_capacity,
                           &reserve);

  size_t slot_count = elem_count + reserve;
  size_t slot_bytes = asked_bytes;
  if (reserve > 0 &&
      !mp_block_bytes(slot_count, extended_elem_size, &slot_bytes)) {
    max_magazines = 0;
    magazine_capacity = 0;
    reserve = 0;
    slot_count = elem_count;
    slot_bytes = asked_bytes;
  }

  mp->objects = ccol__mem_calloc(mp->m_procs, 1, slot_bytes);
  if (!mp->objects && reserve > 0) {
    max_magazines = 0;
    magazine_capacity = 0;
    reserve = 0;
    slot_count = elem_count;
    slot_bytes = asked_bytes;
    mp->objects = ccol__mem_calloc(mp->m_procs, 1, slot_bytes);
  }
  if (!mp->objects) {
    if (err) {
      *err = CCOL__ERR_STR("failed to allocate memory pool data area");
    }
    cmempool_destroy(mp);
    return NULL;
  }

  mp->should_use_locks = !single_threaded;

  if (mp->should_use_locks) {
    if (ccol__mutex_init(mp->lock) != 0) {
      if (err) {
        *err = CCOL__ERR_STR("failed to initialize the mutex");
      }
      mp->should_use_locks = false;
      cmempool_destroy(mp);
      return NULL;
    }
  }

  mp->max_magazines = max_magazines;
#if CMEMPOOL_COMPACT_LAYOUT
  mp->has_cache = (max_magazines != 0);
#endif
  mp->magazine_capacity = magazine_capacity;
  mp->advertised_elem_count = elem_count;

  if (!cmempool_init_internal_scalars(mp, slot_count, extended_elem_size,
                                      fallback_to_dynamic_memory)) {
    if (err) {
      *err = CCOL__ERR_STR(
          "no verified reciprocal exists for this element size and count");
    }
    cmempool_destroy(mp);
    return NULL;
  }

  return mp;
}

// Creates a pool on the caller's buffer, which it never frees; embedded code
// and the tiers of a preallocated ranged pool use it. Gives the pool, or NULL
// with *err set. The buffer must be aligned, because entry 0 sits at its start
// and the stride only carries that alignment on. The elem_size checks run in
// the same order as in cmempool_create_mp(). Each element costs its stride
// plus a status byte, which is what the buffer macro sizes for. Such a pool
// has no cache.
cmempool *cmempool_create_from_preallocated_buffer_mp(
    void *buffer, size_t buf_size, size_t elem_size,
    bool fallback_to_dynamic_memory, bool single_threaded,
    ccol_memmgmt_procs *mmgmt_procs, char **err) {
  if (err) {
    *err = NULL;
  }

  if (!buffer) {
    if (err) {
      *err = CCOL__ERR_STR("buffer is not acceptable");
    }
    return NULL;
  }

  if ((uintptr_t)buffer % cmempool__entry_align != 0) {
    if (err) {
      *err = CCOL__ERR_STR("buffer must be aligned to at least 16 bytes");
    }
    return NULL;
  }

  if (elem_size == 0) {
    if (err) {
      *err = CCOL__ERR_STR("elem_size is zero");
    }
    return NULL;
  } else if (mp_stride_for(elem_size) == 0) {
    if (err) {
      *err = CCOL__ERR_STR(
          "elem_size is too large: the stride rounding would "
          "overflow size_t");
    }
    return NULL;
  } else if (elem_size < sizeof(mp_addr)) {
    elem_size = sizeof(mp_addr);
  }

  size_t extended_elem_size = mp_stride_for(elem_size);
  size_t elem_count = buf_size / (extended_elem_size + 1);
  if (elem_count == 0) {
    if (err) {
      *err = CCOL__ERR_STR("calculated elem_count is zero");
    }
    return NULL;
  }

  if (!ccol__verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  cmempool *mp = (cmempool *)ccol__mem_calloc(mmgmt_procs, 1, sizeof(cmempool));
  if (!mp) {
    if (err) {
      *err = CCOL__ERR_STR("failed to allocate cmempool struct");
    }
    return NULL;
  }

  if (!ccol__populate_mem_mgmt_procs(mp, mmgmt_procs, err)) {
    ccol__mem_free(mmgmt_procs, mp);
    return NULL;
  }

  mp->is_preallocated = true;
  mp->objects = buffer;

  mp->should_use_locks = !single_threaded;

  if (mp->should_use_locks) {
    if (ccol__mutex_init(mp->lock) != 0) {
      if (err) {
        *err = CCOL__ERR_STR("failed to init the rw lock");
      }
      mp->should_use_locks = false;
      cmempool_destroy(mp);
      return NULL;
    }
  }

  mp->advertised_elem_count = elem_count;

  if (!cmempool_init_internal_scalars(mp, elem_count, extended_elem_size,
                                      fallback_to_dynamic_memory)) {
    if (err) {
      *err = CCOL__ERR_STR(
          "no verified reciprocal exists for this element size and count");
    }
    cmempool_destroy(mp);
    return NULL;
  }

  return mp;
}

// Allocates from mp's shared state: Pops the head of the free list, or makes a
// dynamic fallback entry when the pool is empty and the fallback is on. Every
// allocation path that misses the thread cache ends here. locked tells whether
// the caller holds mp->lock (a constant at every call), so a corruption assert
// can unlock first. Gives the entry or NULL. A cached pool treats a free list
// it can only serve from its reserve as empty, so it never hands out more than
// advertised; a pool without a cache skips that check.
static inline __attribute__((always_inline)) void *mempool_serve(cmempool *mp,
                                                                 bool locked) {
  void *result = NULL;

  bool can_serve = mp->free_inst != NULL;
  if (can_serve && MP_HAS_CACHE(mp)) {
    can_serve = mempool_live_count_locked(mp) < mp->advertised_elem_count;
  }

  if (__builtin_expect(can_serve, 1)) {
    void *entry = mp->free_inst;

    if (__builtin_expect(
            !valid_mempool_addr(mp, (uintptr_t)entry) ||
                MP_ENTRY_STATUS(mp, MP_ENTRY_INDEX(mp, (uintptr_t)entry)) !=
                    mp_status_free,
            0)) {
      if (locked) {
        ccol__mutex_unlock(mp->lock);
      }
      ccol__assert(false);
    }

    mp->free_inst = *(void **)entry;
    MP_ENTRY_STATUS(mp, MP_ENTRY_INDEX(mp, (uintptr_t)entry)) =
        (uint8_t)mp_status_taken;
    result = entry;
    --mp->free_elem_count;
  } else if (mp->fallback_to_dynamic_memory) {
    result = mempool_alloc_dynamic_locked(mp, mp->extended_elem_size);
  }

  return result;
}

// Allocates from the free list inside a critical section the caller holds;
// the thread-cache paths serve refused or empty-magazine requests with it.
static inline __attribute__((always_inline)) void *mempool_serve_locked(
    cmempool *mp) {
  return mempool_serve(mp, true);
}

// Allocates from a pool without locks, for cmempool_alloc_entry(). This path
// and the locked one below each have their own function on a 64-byte
// boundary, so their speed doesn't depend on where the surrounding code ends;
// the entry point tail-jumps here and the constant lock argument drops every
// test of should_use_locks. Check the layout with objdump and callgrind after
// any change.
static __attribute__((noinline, aligned(64))) void *mempool_alloc_unlocked(
    cmempool *mp) {
  return mempool_serve(mp, false);
}

// Allocates under mp->lock, for a locking pool without a cache and as the
// fallback of the cache paths.
static __attribute__((noinline, aligned(64))) void *mempool_alloc_locked(
    cmempool *mp) {
  ccol__mutex_lock(mp->lock);
  void *result = mempool_serve(mp, true);
  ccol__mutex_unlock(mp->lock);
  return result;
}

// Allocates an entry from mp: From the thread cache, the free list or the
// dynamic fallback. Gives NULL when nothing can serve it; asserts on a NULL mp.
// It starts on a 64-byte boundary with the thread-cache hit as its
// fall-through. The cache test sits inside the lock test, since a
// single-threaded pool never has a cache.
__attribute__((aligned(64))) void *cmempool_alloc_entry(cmempool *mp) {
  if (!mp) {
    ccol__assert(false);
  }

  if (!mp->should_use_locks) {
    return mempool_alloc_unlocked(mp);
  }

  if (MP_HAS_CACHE(mp)) {
    return mempool_alloc_from_cache(mp);
  }
  return mempool_alloc_locked(mp);
}

// Allocates an entry and zeroes its whole stride, which all belongs to the
// caller; for callers that need zeroed memory.
void *cmempool_calloc_entry(cmempool *mp) {
  void *result = cmempool_alloc_entry(mp);

  if (result) {
    memset(result, 0, mp->extended_elem_size);
  }

  return result;
}

// Tells whether c_entry is the start of one of mp's entries. Every free and
// the corruption checks use it to tell a pool entry from anything else. An
// address partway into an entry is rejected too, since its index names a
// neighbour: The offset must be a multiple of the stride (one mask in the
// default layout, a multiply back in the compact one). Both bounds are compared
// explicitly; the pseudo pool has an empty range.
static inline bool valid_mempool_addr(cmempool *mp, uintptr_t c_entry) {
  if (c_entry < mp->lower_addr_limit || c_entry >= mp->upper_addr_limit) {
    return false;
  }
  uintptr_t off = c_entry - mp->lower_addr_limit;
#if CMEMPOOL_COMPACT_LAYOUT
  return (uintptr_t)MP_ENTRY_INDEX(mp, c_entry) *
             (uintptr_t)mp->extended_elem_size ==
         off;
#else
  return (off & (mp->extended_elem_size - 1)) == 0;
#endif
}

// Releases a dynamic entry that the free path has already removed from mp's
// registry (the removal is the ownership check, so a second free finds it
// gone). It drops the live count (release), marks the header freed so a
// memory dump shows it, and frees the block. Needs mp->lock when the pool
// locks.
static inline __attribute__((always_inline)) void
mempool_release_dynamic_locked(cmempool *mp, void *entry) {
  mp_dynamic_header *dyn = DYNAMIC_ENTRY_HEADER(entry);

  atomic_store_explicit(
      &mp->active_dynamic_memory_buffer_count,
      atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                           memory_order_relaxed) -
          1,
      memory_order_release);
  dyn->elem_status = elem_is_freed_dynamic_member;
  ccol__mem_free(mp->m_procs, DYNAMIC_ENTRY_RAW_BLOCK(entry));
}

// Frees entry when it's a live dynamic entry of mp. A ranged pool asks each
// tier in turn with it. Gives false, touching nothing, otherwise. Needs
// mp->lock when the pool locks.
static __attribute__((noinline)) bool mempool_free_dynamic_locked(cmempool *mp,
                                                                  void *entry) {
  if (!mp_dynamic_set_remove_locked(mp, (uintptr_t)entry)) return false;
  mempool_release_dynamic_locked(mp, entry);
  return true;
}

// Frees entry to mp's shared state; every free that misses the thread cache
// ends here. Ownership comes from the address: Inside the buffer it's a pool
// entry (a status other than taken is a double free or corruption and
// asserts); outside it can only be a live dynamic entry, which the registry
// decides before anything is read through the address. locked means the caller
// holds mp->lock, and this function releases it (also before any assert).
static inline __attribute__((always_inline)) void mempool_free_body(
    cmempool *mp, void *entry, bool locked) {
  uintptr_t c_entry = (uintptr_t)entry;

  if (__builtin_expect(valid_mempool_addr(mp, c_entry), 1)) {
    size_t idx = MP_ENTRY_INDEX(mp, c_entry);
    if (__builtin_expect(MP_ENTRY_STATUS(mp, idx) != mp_status_taken, 0)) {
      if (locked) {
        ccol__mutex_unlock(mp->lock);
      }
      ccol__assert(false);
    }

    MP_ENTRY_STATUS(mp, idx) = (uint8_t)mp_status_free;
    *(void **)entry = mp->free_inst;
    mp->free_inst = entry;
    ++mp->free_elem_count;
    if (locked) {
      ccol__mutex_unlock(mp->lock);
    }
    return;
  }

  if (!mp->fallback_to_dynamic_memory ||
      atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                           memory_order_relaxed) == 0) {
    if (locked) {
      ccol__mutex_unlock(mp->lock);
    }
    ccol__assert(false);
  }

  if (!mp_dynamic_set_remove_locked(mp, c_entry)) {
    if (locked) {
      ccol__mutex_unlock(mp->lock);
    }
    ccol__assert(false);
  }

  mempool_release_dynamic_locked(mp, entry);

  if (locked) {
    ccol__mutex_unlock(mp->lock);
  }
}

// Frees on a pool without locks. Like the allocation paths, it and the locked
// path below sit on 64-byte boundaries (see mempool_alloc_unlocked()).
static __attribute__((noinline, aligned(64))) void mempool_free_unlocked(
    cmempool *mp, void *entry) {
  mempool_free_body(mp, entry, false);
}

// Frees under mp->lock, for a locking pool without a cache and as the fallback
// of the cache paths; mempool_free_body() releases the lock.
static __attribute__((noinline, aligned(64))) void mempool_free_locked(
    cmempool *mp, void *entry) {
  ccol__mutex_lock(mp->lock);
  mempool_free_body(mp, entry, true);
}

// Frees entry to mp through the right path; cmempool__free_entry() and the
// ranged free share it. Asserts on a NULL mp. The cache test sits inside the
// lock test, as in cmempool_alloc_entry().
static inline __attribute__((always_inline)) void cmempool_free_entry_body(
    cmempool *mp, void *entry) {
  if (!mp) {
    ccol__assert(false);
  }

  if (!mp->should_use_locks) {
    mempool_free_unlocked(mp, entry);
    return;
  }

  if (MP_HAS_CACHE(mp)) {
    mempool_free_to_cache(mp, entry, (uintptr_t)entry);
    return;
  }
  mempool_free_locked(mp, entry);
}

// Frees entry back to mp, the body of cmempool_free_entry(). A NULL entry does
// nothing, like free(); a NULL pool or a bad mark asserts. It starts on a
// 64-byte boundary with the cache push as its fall-through.
__attribute__((aligned(64))) void cmempool__free_entry(cmempool *mp,
                                                       void *entry) {
  if (!entry) {
    return;
  }

  if (!mp) {
    ccol__assert(false);
  }

  if (mp->cmempool_mark != cmempool_mark_value) {
    ccol__assert(false);
  }

  cmempool_free_entry_body(mp, entry);
}

// Gives the advertised count of mp, read under its lock. The slot count isn't
// reported, since the reserve can't be handed out on its own.
size_t cmempool_total_capacity(cmempool *mp) {
  if (!mp) {
    ccol__assert(false);
  }

  size_t result = 0;

  if (mp->should_use_locks) {
    ccol__mutex_lock(mp->lock);
  }

  result = mp->advertised_elem_count;

  if (mp->should_use_locks) {
    ccol__mutex_unlock(mp->lock);
  }

  return result;
}

// Gives the entries of mp handed out to callers, read under the lock. Entries
// in a thread cache are subtracted (read relaxed, so possibly stale while
// their owner allocates, but never torn).
size_t cmempool_used_count(cmempool *mp) {
  if (!mp) {
    ccol__assert(false);
  }

  size_t result = 0;

  if (mp->should_use_locks) {
    ccol__mutex_lock(mp->lock);
  }

  result = mempool_live_count_locked(mp);

  if (mp->should_use_locks) {
    ccol__mutex_unlock(mp->lock);
  }

  return result;
}

// Gives the live dynamic entries of mp, read under its lock; the destroy uses
// it for its leak assert.
size_t cmempool_dynamic_allocs_count(cmempool *mp) {
  if (!mp) {
    ccol__assert(false);
  }

  size_t result = 0;

  if (mp->should_use_locks) {
    ccol__mutex_lock(mp->lock);
  }

  result = atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                                memory_order_relaxed);

  if (mp->should_use_locks) {
    ccol__mutex_unlock(mp->lock);
  }

  return result;
}

// The size limits of a ranged pool. The largest is the highest power of two a
// size_t holds, not SIZE_MAX / 2, which is one less than it.
static const size_t min_allowed_smallest_size = 16;
static const size_t max_allowed_largest_size = ccol__max_power_of_two_size;

// A ranged pool: One cmempool per tier, plus a pseudo pool with an empty
// range that owns the shared fallback of
// crmempool_fallback_at_last_exhaustion.
struct crmempool {
  cmempool **mem_pools;
  cmempool pseudo_pool;
  crmempool_fallback_policy fb_policy;
  bool should_use_locks;
  ccol_memmgmt_procs *m_procs;
  size_t number_of_mempools;
  size_t smallest_size;
  size_t largest_size;
  size_t smallest_elem_count;
  // log2 of smallest_size, so the size-to-tier lookup shifts instead of
  // dividing.
  uint8_t smallest_size_power_of_two;
};

// Destroys rmp, the body of crmempool_destroy(); NULL does nothing. It
// asserts when the shared fallback still holds live entries, then destroys
// each tier (which asserts the same way) and frees the rest through rmp's
// allocator.
void crmempool__destroy(crmempool *rmp) {
  if (rmp) {
    if (rmp->fb_policy == crmempool_fallback_at_last_exhaustion) {
      if (cmempool_dynamic_allocs_count(&rmp->pseudo_pool) > 0) {
        ccol__assert(false);
      }
    }

    if (rmp->mem_pools) {
      for (size_t i = 0; i < rmp->number_of_mempools; ++i) {
        if (rmp->mem_pools[i]) {
          cmempool_destroy(rmp->mem_pools[i]);
        }
      }
      ccol__mem_free(rmp->m_procs, rmp->mem_pools);
    }

    if (rmp->fb_policy == crmempool_fallback_at_last_exhaustion) {
      mp_dynamic_set_release(&rmp->pseudo_pool);
      if (rmp->pseudo_pool.should_use_locks) {
        ccol__mutex_destroy(rmp->pseudo_pool.lock);
      }
    }

    if (rmp->m_procs) {
      ccol_free free_func = rmp->m_procs->free;
      free_func(rmp->m_procs);
      free_func(rmp);
    } else {
      ccol__default_mem_free(rmp);
    }
  }
}

// Validates the size parameters of a ranged pool for both constructors and,
// on success, fills rmp's sizes, lock flag and tier count. It refuses zeros,
// exponents as wide as size_t, largest <= smallest, a count power below
// largest - smallest (every tier needs at least one element), an unknown
// policy and sizes beyond the limits; *err says which. Gives true on
// success.
static bool assess_r_mempool_create_inputs(
    crmempool *rmp, uint8_t smallest_size_power_of_two,
    uint8_t largest_size_power_of_two, uint8_t smallest_elem_count_power_of_two,
    crmempool_fallback_policy fb_policy, bool single_threaded, char **err) {
  if (smallest_size_power_of_two == 0 || largest_size_power_of_two == 0 ||
      smallest_elem_count_power_of_two == 0) {
    if (err) {
      *err = CCOL__ERR_STR("zero sizes are not acceptable");
    }
    return false;
  }

  if (largest_size_power_of_two >= sizeof(size_t) * CHAR_BIT ||
      smallest_size_power_of_two >= sizeof(size_t) * CHAR_BIT ||
      smallest_elem_count_power_of_two >= sizeof(size_t) * CHAR_BIT) {
    if (err) {
      *err = CCOL__ERR_STR("power of two exceeds size_t width");
    }
    return false;
  }

  if (largest_size_power_of_two <= smallest_size_power_of_two ||
      (smallest_elem_count_power_of_two <
       (largest_size_power_of_two - smallest_size_power_of_two))) {
    if (err) {
      *err = CCOL__ERR_STR("inconsistent sizes are not acceptable");
    }
    return false;
  }

  if (fb_policy < 0 || fb_policy >= crmempool__fallback_end_place_holder) {
    if (err) {
      *err = CCOL__ERR_STR("unknown fallback policy");
    }
    return false;
  }

  size_t smallest_size = (size_t)1 << smallest_size_power_of_two;
  size_t largest_size = (size_t)1 << largest_size_power_of_two;
  size_t smallest_elem_count = (size_t)1 << smallest_elem_count_power_of_two;

  if (largest_size > max_allowed_largest_size ||
      smallest_size < min_allowed_smallest_size) {
    if (err) {
      *err = CCOL__ERR_STR("sizes beyond limits are not acceptable");
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

// Sets up rmp's pseudo pool for both constructors. Under
// crmempool_fallback_at_last_exhaustion it becomes the owner of the shared
// fallback (with a lock when rmp locks), and its live count covers every
// dynamic entry of rmp. Gives false when the mutex can't be created.
static bool init_r_mempool_pseudo_pool(crmempool *rmp) {
  memset(&rmp->pseudo_pool, 0, sizeof(cmempool));
  rmp->pseudo_pool.m_procs = rmp->m_procs;
  if (rmp->fb_policy == crmempool_fallback_at_last_exhaustion) {
    if (rmp->should_use_locks) {
      if (ccol__mutex_init(rmp->pseudo_pool.lock) != 0) {
        return false;
      }
      rmp->pseudo_pool.should_use_locks = true;
    }
    rmp->pseudo_pool.fallback_to_dynamic_memory = true;
    rmp->pseudo_pool.cmempool_mark = cmempool_mark_value;
  }

  return true;
}

// Builds the tiers of a heap-backed ranged pool: The pseudo pool and one
// cmempool per tier, the size doubling and the count halving each step. A tier
// falls back on its own only under crmempool_fallback_at_first_exhaustion.
// Gives false with *err set; the caller cleans up.
static bool init_r_mempool_internal_pools(crmempool *rmp, char **err) {
  if (!init_r_mempool_pseudo_pool(rmp)) {
    if (err) {
      *err = CCOL__ERR_STR("failed to initialize the pseudo_pool");
    }
    return false;
  }

  rmp->mem_pools = (cmempool **)ccol__mem_alloc(
      rmp->m_procs, rmp->number_of_mempools * sizeof(cmempool *));
  if (!rmp->mem_pools) {
    if (err) {
      *err = CCOL__ERR_STR("failed to allocate mem_pools array");
    }
    return false;
  }
  memset(rmp->mem_pools, 0, rmp->number_of_mempools * sizeof(cmempool *));

  size_t first_size = rmp->smallest_size;
  size_t first_count = rmp->smallest_elem_count;

  for (size_t esize = first_size, ecount = first_count, index = 0;
       index < rmp->number_of_mempools; esize *= 2, ecount /= 2, ++index) {
    rmp->mem_pools[index] = cmempool_create_mp(
        ecount, esize, rmp->fb_policy == crmempool_fallback_at_first_exhaustion,
        !rmp->should_use_locks, rmp->m_procs, err);
    if (!rmp->mem_pools[index]) {
      return false;
    }
  }

  return true;
}

// Gives the index of the smallest tier that fits size (1 <= size <= largest);
// every ranged call that takes a size uses it. One shift and one bit scan do
// it, where a lookup table would need largest / smallest entries.
static inline size_t crmempool_pool_index_for_size(crmempool *rmp,
                                                   size_t size) {
  size_t t = (size - 1) >> rmp->smallest_size_power_of_two;
  if (t == 0) {
    return 0;
  }
  return (size_t)(sizeof(unsigned long long) * CHAR_BIT) -
         (size_t)__builtin_clzll((unsigned long long)t);
}

// Creates a heap-backed ranged pool, the constructor behind
// crmempool_create(): It validates the parameters, then builds the tiers.
// Gives the pool, or NULL with *err set; a partial pool is destroyed.
crmempool *crmempool_create_mp(uint8_t smallest_size_power_of_two,
                               uint8_t largest_size_power_of_two,
                               uint8_t smallest_elem_count_power_of_two,
                               crmempool_fallback_policy fb_policy,
                               bool single_threaded,
                               ccol_memmgmt_procs *mmgmt_procs, char **err) {
  if (err) {
    *err = NULL;
  }

  if (!ccol__verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  crmempool *rmp =
      (crmempool *)ccol__mem_calloc(mmgmt_procs, 1, sizeof(crmempool));
  if (!rmp) {
    if (err) {
      *err = CCOL__ERR_STR("failed to allocate crmempool struct");
    }
    return NULL;
  }

  if (!ccol__populate_mem_mgmt_procs(rmp, mmgmt_procs, err)) {
    ccol__mem_free(mmgmt_procs, rmp);
    return NULL;
  }

  if (!assess_r_mempool_create_inputs(
          rmp, smallest_size_power_of_two, largest_size_power_of_two,
          smallest_elem_count_power_of_two, fb_policy, single_threaded, err)) {
    crmempool_destroy(rmp);
    return NULL;
  }
  rmp->fb_policy = fb_policy;

  if (!init_r_mempool_internal_pools(rmp, err)) {
    crmempool_destroy(rmp);
    return NULL;
  }

  return rmp;
}

// Builds the tiers of a ranged pool on the caller's buffer: Sets up the pseudo
// pool and carves one segment per tier, with the progression of
// init_r_mempool_internal_pools(). The whole expected size is computed first,
// each term and partial sum checked for overflow, and must equal
// preallocated_buffer_size, so a wrong buffer is refused before anything is
// written. Each segment's status region is rounded up so the next tier stays
// aligned. Gives false with *err set; the caller cleans up.
static bool init_preallocated_r_mempool_internal_pools(
    crmempool *rmp, void *preallocated_buffer, size_t preallocated_buffer_size,
    char **err) {
  if (!init_r_mempool_pseudo_pool(rmp)) {
    if (err) {
      *err = CCOL__ERR_STR("failed to initialize the pseudo_pool");
    }
    return false;
  }

  size_t first_size = rmp->smallest_size;
  size_t first_count = rmp->smallest_elem_count;

  size_t expected_size = 0;
  for (size_t esize = first_size, ecount = first_count, index = 0;
       index < rmp->number_of_mempools; esize *= 2, ecount /= 2, ++index) {
    if (mp_stride_for(esize) == 0) {
      if (err) {
        *err = CCOL__ERR_STR(
            "requested configuration's buffer size overflows size_t");
      }
      return false;
    }
    size_t elem_extended_size = mp_stride_for(esize);
    if (!cmempool__align_up_fits(ecount)) {
      if (err) {
        *err = CCOL__ERR_STR(
            "requested configuration's buffer size overflows size_t");
      }
      return false;
    }
    size_t status_bytes = cmempool__align_up(ecount);
    if (ecount != 0 &&
        elem_extended_size > (SIZE_MAX - status_bytes) / ecount) {
      if (err) {
        *err = CCOL__ERR_STR(
            "requested configuration's buffer size overflows size_t");
      }
      return false;
    }
    size_t term = ecount * elem_extended_size + status_bytes;
    if (expected_size > SIZE_MAX - term) {
      if (err) {
        *err = CCOL__ERR_STR(
            "requested configuration's buffer size overflows size_t");
      }
      return false;
    }
    expected_size += term;
  }
  if (expected_size != preallocated_buffer_size) {
    if (err) {
      *err = CCOL__ERR_STR("buffer sizes differ");
    }
    return false;
  }

  rmp->mem_pools = (cmempool **)ccol__mem_calloc(
      rmp->m_procs, rmp->number_of_mempools, sizeof(cmempool *));
  if (!rmp->mem_pools) {
    if (err) {
      *err = CCOL__ERR_STR("failed to allocate mem_pools array");
    }
    return false;
  }

  size_t cumulative_size = 0;

  for (size_t esize = first_size, ecount = first_count, index = 0;
       index < rmp->number_of_mempools; esize *= 2, ecount /= 2, ++index) {
    uint8_t *sub_buffer = (uint8_t *)preallocated_buffer + cumulative_size;
    size_t sub_buffer_size =
        ecount * mp_stride_for(esize) + cmempool__align_up(ecount);
    rmp->mem_pools[index] = cmempool_create_from_preallocated_buffer_mp(
        sub_buffer, sub_buffer_size, esize,
        rmp->fb_policy == crmempool_fallback_at_first_exhaustion,
        !rmp->should_use_locks, rmp->m_procs, err);
    if (!rmp->mem_pools[index]) {
      return false;
    }
    cumulative_size += sub_buffer_size;
  }

  return true;
}

// Creates a ranged pool on the caller's buffer, which it never frees, for code
// that avoids the heap. The alignment is checked once up front, so a bad
// buffer gets one clear error instead of a failure deep inside a tier. Gives
// the pool, or NULL with *err set.
crmempool *crmempool_create_from_preallocated_buffer_mp(
    void *buffer, size_t buf_size, uint8_t smallest_size_power_of_two,
    uint8_t largest_size_power_of_two, uint8_t smallest_elem_count_power_of_two,
    crmempool_fallback_policy fb_policy, bool single_threaded,
    ccol_memmgmt_procs *mmgmt_procs, char **err) {
  if (err) {
    *err = NULL;
  }

  if (!buffer) {
    if (err) {
      *err = CCOL__ERR_STR("buffer is NULL");
    }
    return NULL;
  }

  if ((uintptr_t)buffer % cmempool__entry_align != 0) {
    if (err) {
      *err = CCOL__ERR_STR("buffer must be aligned to at least 16 bytes");
    }
    return NULL;
  }

  if (!ccol__verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  crmempool *rmp =
      (crmempool *)ccol__mem_calloc(mmgmt_procs, 1, sizeof(crmempool));
  if (!rmp) {
    if (err) {
      *err = CCOL__ERR_STR("failed to allocate crmempool struct");
    }
    return NULL;
  }

  if (!ccol__populate_mem_mgmt_procs(rmp, mmgmt_procs, err)) {
    ccol__mem_free(mmgmt_procs, rmp);
    return NULL;
  }

  if (!assess_r_mempool_create_inputs(
          rmp, smallest_size_power_of_two, largest_size_power_of_two,
          smallest_elem_count_power_of_two, fb_policy, single_threaded, err)) {
    crmempool_destroy(rmp);
    return NULL;
  }
  rmp->fb_policy = fb_policy;

  if (!init_preallocated_r_mempool_internal_pools(rmp, buffer, buf_size, err)) {
    crmempool_destroy(rmp);
    return NULL;
  }

  return rmp;
}

// Allocates a shared fallback entry of exactly elem_size bytes through the
// pseudo pool mp, once every tier is empty under
// crmempool_fallback_at_last_exhaustion. Takes mp's lock when it has one.
// The size isn't raised to sizeof(mp_addr): The entry is never on a free list,
// and the exact recorded size keeps the same-size shortcut of
// crmempool_realloc_entry() working. Gives NULL when the prefix would overflow
// or the allocator refuses.
static void *cmempool_pseudo_alloc_entry(cmempool *mp, size_t elem_size) {
  void *result = NULL;

  if (elem_size > SIZE_MAX - DYNAMIC_ENTRY_PREFIX_SIZE) {
    return NULL;
  }

  if (mp->should_use_locks) {
    ccol__mutex_lock(mp->lock);
  }

  result = mempool_alloc_dynamic_locked(mp, elem_size);

  if (mp->should_use_locks) {
    ccol__mutex_unlock(mp->lock);
  }

  return result;
}

// Allocates at least size bytes from rmp: From the smallest tier that fits,
// then from larger tiers, and last from the shared fallback under
// crmempool_fallback_at_last_exhaustion. Gives NULL for an invalid size or when
// nothing serves it. Asserts on a NULL rmp.
void *crmempool_alloc_entry(crmempool *rmp, size_t size) {
  if (!rmp) {
    ccol__assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return NULL;
  }

  size_t pool_index = crmempool_pool_index_for_size(rmp, size);

  void *result = NULL;

  for (; pool_index < rmp->number_of_mempools; ++pool_index) {
    result = cmempool_alloc_entry(rmp->mem_pools[pool_index]);
    if (result) {
      break;
    }
  }

  if (!result && rmp->fb_policy == crmempool_fallback_at_last_exhaustion) {
    result = cmempool_pseudo_alloc_entry(&rmp->pseudo_pool, size);
  }

  return result;
}

// Allocates at least size bytes and zeroes the first size of them, for
// callers that need zeroed memory.
void *crmempool_calloc_entry(crmempool *rmp, size_t size) {
  void *result = crmempool_alloc_entry(rmp, size);

  if (result) {
    memset(result, 0, size);
  }

  return result;
}

// Gives the bytes an entry of mp holds: The whole stride for a pool entry,
// the recorded size for a dynamic one. Realloc needs it to know how much to
// copy.
static inline size_t entry_user_size(cmempool *mp, void *entry) {
  if (valid_mempool_addr(mp, (uintptr_t)entry)) {
    return mp->extended_elem_size;
  }
  return DYNAMIC_ENTRY_USER_SIZE(entry);
}

// Tells whether any tier or the pseudo pool of rmp holds a live dynamic entry,
// so the owner lookup can call an address outside every tier foreign without
// reading through it. It reads the atomic counts (acquire) without the tiers'
// locks.
static bool rmp_holds_dynamic_entries(crmempool *rmp) {
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

// Gives the pool of rmp whose registry holds c_entry as a live dynamic entry,
// or NULL; realloc uses it to resolve an entry. Each registry is read under its
// own lock, one pool at a time. The answer can go stale after the unlock, so a
// free asks again under the lock.
static cmempool *rmp_dynamic_owner_of(crmempool *rmp, uintptr_t c_entry) {
  for (size_t i = 0; i <= rmp->number_of_mempools; ++i) {
    cmempool *mp =
        i < rmp->number_of_mempools ? rmp->mem_pools[i] : &rmp->pseudo_pool;
    if (!mp || !mp->fallback_to_dynamic_memory ||
        atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                             memory_order_acquire) == 0) {
      continue;
    }
    if (mp->should_use_locks) ccol__mutex_lock(mp->lock);
    bool found = mp_dynamic_set_contains_locked(mp, c_entry);
    if (mp->should_use_locks) ccol__mutex_unlock(mp->lock);
    if (found) return mp;
  }
  return NULL;
}

// Frees entry when one of rmp's pools holds it as a live dynamic entry; the
// ranged free uses it for an address outside every tier. Each candidate is
// locked once and the free happens in that critical section. Gives false,
// having read nothing through entry, when no pool holds it.
static __attribute__((noinline)) bool rmp_free_dynamic(crmempool *rmp,
                                                       void *entry) {
  for (size_t i = 0; i <= rmp->number_of_mempools; ++i) {
    cmempool *mp =
        i < rmp->number_of_mempools ? rmp->mem_pools[i] : &rmp->pseudo_pool;
    if (!mp || !mp->fallback_to_dynamic_memory ||
        atomic_load_explicit(&mp->active_dynamic_memory_buffer_count,
                             memory_order_acquire) == 0) {
      continue;
    }
    if (mp->should_use_locks) ccol__mutex_lock(mp->lock);
    bool freed = mempool_free_dynamic_locked(mp, entry);
    if (mp->should_use_locks) ccol__mutex_unlock(mp->lock);
    if (freed) return true;
  }
  return false;
}

// Gives the pool of rmp that owns entry, or NULL, and sets *is_dynamic to say
// whether it's a dynamic entry; realloc needs both. A tier is found from the
// address alone; an address outside every tier is looked up in the registries,
// and only when rmp holds dynamic entries at all.
static cmempool *rmp_owner_of(crmempool *rmp, void *entry, bool *is_dynamic) {
  *is_dynamic = false;
  for (size_t i = 0; i < rmp->number_of_mempools; ++i) {
    cmempool *sub = rmp->mem_pools[i];
    if (sub && valid_mempool_addr(sub, (uintptr_t)entry)) return sub;
  }

  if (!rmp_holds_dynamic_entries(rmp)) return NULL;

  cmempool *owner = rmp_dynamic_owner_of(rmp, (uintptr_t)entry);
  if (!owner) return NULL;
  *is_dynamic = true;
  return owner;
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: Overwrites the status byte of entry in the tier that owns it, the
// ranged form of cmempool__corrupt_entry_status_for_tests().
void crmempool__corrupt_entry_status_for_tests(crmempool *rmp, void *entry,
                                               unsigned char value) {
  if (!rmp || !entry) return;
  for (size_t i = 0; i < rmp->number_of_mempools; ++i) {
    cmempool *sub = rmp->mem_pools[i];
    if (sub && valid_mempool_addr(sub, (uintptr_t)entry)) {
      MP_ENTRY_STATUS(sub, MP_ENTRY_INDEX(sub, (uintptr_t)entry)) = value;
      return;
    }
  }
}
#endif

// Frees entry back to rmp, the body of crmempool_free_entry(): To the tier
// that owns it, or as a live dynamic entry of one of rmp's pools. NULL does
// nothing; a NULL rmp or a foreign address asserts.
void crmempool__free_entry(crmempool *rmp, void *entry) {
  if (!entry) {
    return;
  }

  if (!rmp) {
    ccol__assert(false);
  }

  for (size_t i = 0; i < rmp->number_of_mempools; ++i) {
    cmempool *sub = rmp->mem_pools[i];
    if (sub && valid_mempool_addr(sub, (uintptr_t)entry)) {
      cmempool_free_entry_body(sub, entry);
      return;
    }
  }

  if (!rmp_free_dynamic(rmp, entry)) {
    ccol__assert(false);
  }
}

// Resizes addr to at least size bytes (NULL allocates). Ownership and status
// are checked first, so the no-move answer never returns a free or foreign
// entry. When the entry already holds what the tier for size gives, or exactly
// size bytes, it comes back unmoved. Otherwise a new entry gets the smaller of
// the two real sizes (read from both entries, never from the ideal tier) and
// the old one is freed. When the allocation fails but size fits in what addr
// holds, addr comes back unmoved, as realloc(3) does. An invalid size or a
// failure gives NULL and leaves addr alone.
void *crmempool_realloc_entry(crmempool *rmp, void *addr, size_t size) {
  if (!rmp) {
    ccol__assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return NULL;
  }

  cmempool *owner = NULL;
  bool owner_is_dynamic = false;
  size_t held = 0;

  if (addr) {
    owner = rmp_owner_of(rmp, addr, &owner_is_dynamic);
    if (!owner) {
      ccol__assert(false);
    }
    if (!owner_is_dynamic &&
        MP_ENTRY_STATUS(owner, MP_ENTRY_INDEX(owner, (uintptr_t)addr)) !=
            mp_status_taken) {
      ccol__assert(false);
    }

    held = owner_is_dynamic ? DYNAMIC_ENTRY_USER_SIZE(addr)
                            : owner->extended_elem_size;

    size_t new_ext_size =
        rmp->mem_pools[crmempool_pool_index_for_size(rmp, size)]
            ->extended_elem_size;

    if (held == new_ext_size || held == size) {
      return addr;
    }
  }

  void *new_entry = crmempool_alloc_entry(rmp, size);
  if (!new_entry && addr && size <= held) {
    return addr;
  }
  if (new_entry && addr) {
    bool new_is_dynamic = false;
    cmempool *new_owner = rmp_owner_of(rmp, new_entry, &new_is_dynamic);
    size_t old_user_size = held;
    size_t new_user_size =
        new_owner ? entry_user_size(new_owner, new_entry) : 0;
    size_t copy_size =
        old_user_size < new_user_size ? old_user_size : new_user_size;

    memcpy(new_entry, addr, copy_size);
    cmempool_free_entry(owner, addr);
  }

  return new_entry;
}

// Gives the used count of the tier that serves size, or 0 for an invalid
// size.
size_t crmempool_used_count(crmempool *rmp, size_t size) {
  if (!rmp) {
    ccol__assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return 0;
  }

  return cmempool_used_count(
      rmp->mem_pools[crmempool_pool_index_for_size(rmp, size)]);
}

// Gives the bytes of mp's entry block, every slot's stride and status byte
// with the reserve included, read under the lock. It's what a pool costs,
// which cmempool_total_capacity() deliberately doesn't report.
size_t cmempool_allocated_bytes(cmempool *mp) {
  if (!mp) {
    ccol__assert(false);
  }

  size_t result = 0;

  if (mp->should_use_locks) {
    ccol__mutex_lock(mp->lock);
  }

  result = mp->total_elem_count * mp->extended_elem_size + mp->total_elem_count;

  if (mp->should_use_locks) {
    ccol__mutex_unlock(mp->lock);
  }

  return result;
}

// Gives the sum of every tier's entry block, what a ranged pool costs; the
// pseudo pool has no block.
size_t crmempool_allocated_bytes(crmempool *rmp) {
  if (!rmp) {
    ccol__assert(false);
  }

  size_t result = 0;
  for (size_t i = 0; i < rmp->number_of_mempools; ++i) {
    if (rmp->mem_pools[i]) {
      result += cmempool_allocated_bytes(rmp->mem_pools[i]);
    }
  }
  return result;
}

// Gives the capacity of the tier that serves size, or 0 for an invalid
// size.
size_t crmempool_total_capacity(crmempool *rmp, size_t size) {
  if (!rmp) {
    ccol__assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return 0;
  }

  return cmempool_total_capacity(
      rmp->mem_pools[crmempool_pool_index_for_size(rmp, size)]);
}

// Gives the live fallback entries for size: The tier's count under
// crmempool_fallback_at_first_exhaustion, the pseudo pool's count under
// crmempool_fallback_at_last_exhaustion, and 0 when the fallback is off or the
// size is invalid.
size_t crmempool_dynamic_allocs_count(crmempool *rmp, size_t size) {
  if (!rmp) {
    ccol__assert(false);
  }

  if (size == 0 || size > rmp->largest_size) {
    return 0;
  }

  if (rmp->fb_policy == crmempool_fallback_disabled) {
    return 0;
  }

  if (rmp->fb_policy == crmempool_fallback_at_first_exhaustion) {
    return cmempool_dynamic_allocs_count(
        rmp->mem_pools[crmempool_pool_index_for_size(rmp, size)]);
  }

  return cmempool_dynamic_allocs_count(&rmp->pseudo_pool);
}
