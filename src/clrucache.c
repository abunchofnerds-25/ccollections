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

#include <chashmap.h>
#include <clrucache.h>
#include <cthreadcomm.h>
#include <cvector.h>
#include <internal/chashkey.h>
#include <internal/cpintable.h>
#include <internal/test_helpers/chashmap.h>
#include <internal/test_helpers/clrucache.h>
#include <stdatomic.h>
#include <string.h>
#ifdef RUNNING_UNIT_TESTS
#include <unistd.h>
#endif

// One entry of a segment. It sits in the segment's map (key -> entry pointer)
// and, while it has a value, in the segment's LRU list. A placeholder has no
// value and isn't in the list (a fetch or set is in flight), a live entry has a
// value and can be evicted (also while a remote setter runs for it: eviction
// turns it back into a placeholder that the set fills or removes), and a dead
// entry has left the map and the list (evicted) and lives only while waiters
// hold it.
//
// The segment's mutex guards every field, and cond shares it. Remote getters
// and setters run outside the mutex, so a thread that unlocks while it holds an
// entry pointer increments waiters first and decrements it after locking again;
// whoever brings waiters of a dead entry to 0 frees it. fetch_failed and
// fetch_size_mismatch are set only right before a failed fetch tears the entry
// down, so a waiter that wakes to no value reports the same result as the
// fetching thread.
typedef struct ccache_entry {
  void *key;
  size_t key_size;

  void *value;
  size_t value_size;

  ccol__cond_var cond;

  bool fetch_in_progress;
  bool set_in_progress;
  bool evicted;

  bool fetch_size_mismatch;

  int waiters;

  struct ccache_entry *prev;
  struct ccache_entry *next;
  bool in_lru;

  bool fetch_failed;
} ccache_entry;

// One slot of the handle table. A ccache is (slot index << 32) | generation;
// the generation goes up on every reuse, so a stale handle never names a later
// cache and clrucache__destroy can report a double destroy. ptr is NULL while
// the slot is free.
typedef struct {
  struct clrucache *ptr;
  uint32_t generation;
  bool in_use;
} clrucache_slot;

// The handle table. Only create, destroy and test helpers take its rwlock; a
// get or set resolves through the lock-free pin table. slots only grows;
// free_indices is a LIFO of released indices. lost_indices counts released
// slots whose index couldn't go back on the free list, which an acquire then
// finds by a scan. release_deferred is set when the exit destructor finds a
// cache still live, so the destroy that frees the last slot frees the table.
// Those two fields are used only under the write lock. The module registers no
// fork handlers, so the table has no fork-time state.
static struct {
  ccol__rw_lock rwlock;
  ccol__once_flag once;
  cvec slots;
  cvec free_indices;
  size_t lost_indices;
  bool release_deferred;
} clrucache_slot_table = {0};

static bool clrucache_any_slot_live_locked(void);
static void clrucache_release_slot_table_locked(void);
static void clrucache_release_slot_table_if_deferred_locked(void);

// Creates the handle table; the once of the table runs it on first use, from a
// create, a destroy or a test helper.
static void clrucache_slot_table_init_globals(void) {
  if (ccol__rw_lock_init(clrucache_slot_table.rwlock) != 0)
    ccol__fatal_err("ccache slot table: failed to initialize rwlock");
  clrucache_slot_table.slots = cvector_create(sizeof(clrucache_slot), NULL);
  if (!clrucache_slot_table.slots)
    ccol__fatal_err("ccache slot table: failed to allocate slots vector");
  clrucache_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!clrucache_slot_table.free_indices)
    ccol__fatal_err("ccache slot table: failed to allocate free-index vector");
}

// One segment of a cache, with its own map (key -> entry pointer), LRU list and
// lock, so operations on keys of different segments run in parallel. The
// sentinels lru_head (newest end) and lru_tail (oldest end) live in the struct.
// capacity is the segment's share and size counts its live entries. Each
// segment keeps its own copy of the callbacks; the allocator is borrowed from
// the router, which owns the one copy of the caller's procs.
typedef struct ccache_shard {
  chmap map;

  ccache_entry lru_head;
  ccache_entry lru_tail;

  size_t capacity;
  size_t size;

  ccol__mutex mutex;

  ccache_remote_getter_fn remote_getter;
  ccache_remote_setter_fn remote_setter;
  ccache_eviction_fn eviction_cb;

  ccol_memmgmt_procs *m_procs;
} ccache_shard;

// The cache itself: the router that picks a segment for each key. key_type and
// key_fixed_width keep the segment choice in agreement with the maps about
// which keys are equal, and let a wrongly sized hand-built key be refused
// before it's read. capacity is the sum of the segment capacities. self_handle
// is written once before the publish. m_procs points at m_procs_copy when the
// caller gave an allocator, so the caller's struct may go away after the
// create; the copy comes last, after every field a lookup reads.
struct clrucache {
  ccache_shard **shards;
  size_t shard_count;

  ccol_data_type key_type;
  size_t key_fixed_width;

  size_t capacity;

  ccol_memmgmt_procs *m_procs;

  ccache self_handle;

  ccol_memmgmt_procs m_procs_copy;
};

// The hot half of the handle table: handle to pointer, plus the pin that keeps
// a cache alive during a call. A resolve writes nothing that another thread
// reads, so cache operations from many threads don't contend on it.
static ccol_pintable clrucache_pintable;

// Resolves h to its cache and pins it, so a concurrent destroy waits until the
// caller is done with it. Every public call on a cache starts here. Gives NULL
// for 0, garbage, a free slot or a reused one. On success the caller must call
// clrucache_resolve_unpin() exactly once. It takes no lock and makes no shared
// write.
static struct clrucache *clrucache_resolve(ccache h) {
  return (struct clrucache *)ccol_pintable_pin(&clrucache_pintable, h);
}

// Drops the pin that clrucache_resolve took, when a public call is done with
// the cache. It touches nothing else: the cache can be freed right after, so
// nothing may read raw once the pin is gone.
static void clrucache_resolve_unpin(struct clrucache *raw) {
  ccol_pintable_unpin(&clrucache_pintable, raw->self_handle);
}

#ifdef RUNNING_UNIT_TESTS
// Test only: makes the next free list push fail like an allocation.
static _Atomic bool g_ccache_fail_next_free_index_push = false;

// Arms g_ccache_fail_next_free_index_push, so a test reaches the recovery of a
// lost slot index.
void clrucache__force_next_free_index_push_failure_for_tests(void) {
  atomic_store(&g_ccache_fail_next_free_index_push, true);
}
#endif

// Puts idx back on the free list for the next create to reuse; the destroy and
// a failed acquire call it with the write lock held. A failed push is counted
// in lost_indices instead of stranding the slot for good.
static void clrucache_free_index_release_locked(uint32_t idx) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_ccache_fail_next_free_index_push, false)) {
    clrucache_slot_table.lost_indices++;
    return;
  }
#endif
  if (cvector_push_back(clrucache_slot_table.free_indices, &idx) !=
      ccol_success)
    clrucache_slot_table.lost_indices++;
}

// Finds a released slot that no free list entry names (ptr NULL and not in_use)
// and puts its index in *out_idx, so a slot lost to a failed push gets reused
// instead of the table growing. The acquire calls it with the write lock held
// after finding the free list empty, which makes that state mean lost rather
// than free; a destroy in progress still has ptr set. A scan that finds nothing
// clears lost_indices.
static __attribute__((noinline)) bool clrucache_reclaim_lost_index_locked(
    uint32_t *out_idx) {
  size_t slot_count = cvector_elem_count(clrucache_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    clrucache_slot *slot =
        (clrucache_slot *)cvector_at(clrucache_slot_table.slots, i);
    if (slot->ptr == NULL && !slot->in_use) {
      *out_idx = (uint32_t)i;
      clrucache_slot_table.lost_indices--;
      return true;
    }
  }
  clrucache_slot_table.lost_indices = 0;
  return false;
}

// Gives cache a slot (a free one, a lost one or a new one), publishes it in the
// pin table and gives the handle; it's the last step of clrucache_create_full.
// Gives 0 when the table is full or can't grow, and also when the module
// destructor already released the table, which *released reports. Generation 0
// is skipped because handle 0 is CCACHE_NULL. self_handle is written before the
// publish so a resolver finds it, and cleared again when the publish fails and
// the slot goes back. Takes the write lock.
static ccache clrucache_handle_slot_acquire(struct clrucache *cache,
                                            bool *released) {
  ccol__call_once(clrucache_slot_table.once, clrucache_slot_table_init_globals);
  ccol__rw_lock_wrlock(clrucache_slot_table.rwlock);
  *released = clrucache_slot_table.slots == NULL;
  if (__builtin_expect(*released, 0)) {
    ccol__rw_lock_unlock(clrucache_slot_table.rwlock);
    return 0;
  }
  uint32_t idx;
  clrucache_slot *slot;
  if (cvector_elem_count(clrucache_slot_table.free_indices) > 0) {
    cvector_pop_back(clrucache_slot_table.free_indices, &idx);
    slot = (clrucache_slot *)cvector_at(clrucache_slot_table.slots, idx);
  } else if (clrucache_slot_table.lost_indices > 0 &&
             clrucache_reclaim_lost_index_locked(&idx)) {
    slot = (clrucache_slot *)cvector_at(clrucache_slot_table.slots, idx);
  } else {
    if (cvector_elem_count(clrucache_slot_table.slots) >= CCOL_PIN_MAX_SLOTS) {
      ccol__rw_lock_unlock(clrucache_slot_table.rwlock);
      return 0;
    }
    clrucache_slot fresh = {0};
    if (cvector_push_back(clrucache_slot_table.slots, &fresh) != ccol_success) {
      ccol__rw_lock_unlock(clrucache_slot_table.rwlock);
      return 0;
    }
    idx = (uint32_t)cvector_elem_count(clrucache_slot_table.slots) - 1;
    slot = (clrucache_slot *)cvector_at(clrucache_slot_table.slots, idx);
  }
  slot->generation++;
  if (slot->generation == 0) slot->generation++;
  ccache h = ((ccache)idx << 32) | (ccache)slot->generation;

  cache->self_handle = h;

  if (!ccol_pintable_publish(&clrucache_pintable, idx, slot->generation,
                             cache)) {
    cache->self_handle = 0;
    clrucache_free_index_release_locked(idx);
    ccol__rw_lock_unlock(clrucache_slot_table.rwlock);
    return 0;
  }

  slot->ptr = cache;
  slot->in_use = true;
  ccol__rw_lock_unlock(clrucache_slot_table.rwlock);
  return h;
}

// Links e in as the newest entry of the segment, when it gets a value or is
// used.
static void lru_add_to_front(ccache_shard *cache, ccache_entry *e) {
  e->next = cache->lru_head.next;
  e->prev = &cache->lru_head;
  cache->lru_head.next->prev = e;
  cache->lru_head.next = e;
  e->in_lru = true;
}

// Unlinks e from the LRU list, when it's evicted or turned back into a
// placeholder; no-op when it isn't in the list.
static void lru_remove(ccache_entry *e) {
  if (!e->in_lru) return;
  e->prev->next = e->next;
  e->next->prev = e->prev;
  e->prev = NULL;
  e->next = NULL;
  e->in_lru = false;
}

// Makes e the newest entry of the segment, which a get or set does to the key
// it uses.
static void lru_move_to_front(ccache_shard *cache, ccache_entry *e) {
  lru_remove(e);
  lru_add_to_front(cache, e);
}

// Allocates a zeroed entry with an initialized condition variable, or gives
// NULL. A failed init frees the block directly, since entry_free would destroy
// a condition variable that was never initialized.
static ccache_entry *entry_alloc(ccache_shard *cache) {
  ccache_entry *e =
      (ccache_entry *)ccol__mem_calloc(cache->m_procs, 1, sizeof(ccache_entry));
  if (!e) return NULL;
  if (ccol__cond_var_init(e->cond) != 0) {
    ccol__mem_free(cache->m_procs, e);
    return NULL;
  }
  return e;
}

// Frees an entry with its key, value and condition variable, when nobody can
// reach it any more: it was never published, or it is evicted and has no
// waiters. The segment's mutex isn't needed for either case.
static void entry_free(ccache_shard *cache, ccache_entry *e) {
  ccol__mem_free(cache->m_procs, e->key);
  ccol__mem_free(cache->m_procs, e->value);
  ccol__cond_var_destroy(e->cond);
  ccol__mem_free(cache->m_procs, e);
}

// Evicts the oldest live entry of the segment to make room, after calling the
// eviction callback with its key and value; the caller holds the mutex. Gives
// false when the list is empty. The victim is the real oldest entry even when a
// remote setter runs for it, since that set hasn't taken effect yet; such an
// entry stays in the map as a placeholder for the set to fill or remove.
// Otherwise the entry leaves the map, its waiters are woken, and it's freed
// when none hold it.
static bool evict_lru(ccache_shard *cache) {
  ccache_entry *victim = cache->lru_tail.prev;
  if (victim == &cache->lru_head) return false;

  if (cache->eviction_cb && victim->value) {
    cmap_pair kp = {.ptr = victim->key, .size = victim->key_size};
    cmap_pair vp = {.ptr = victim->value, .size = victim->value_size};
    cache->eviction_cb(&kp, &vp);
  }

  lru_remove(victim);
  cache->size--;

  if (victim->set_in_progress) {
    ccol__mem_free(cache->m_procs, victim->value);
    victim->value = NULL;
    victim->value_size = 0;
    return true;
  }

  cmap_pair kp = {.ptr = victim->key, .size = victim->key_size};
  chashmap_delete_elem(cache->map, &kp);

  victim->evicted = true;
  ccol__cond_var_broadcast(victim->cond);

  if (victim->waiters == 0) {
    entry_free(cache, victim);
  }
  return true;
}

// Evicts until the segment has room for one more live entry; every entry that
// size counts is in the LRU list, so the loop always gets there. The caller
// holds the mutex.
static void make_room(ccache_shard *cache) {
  while (cache->size >= cache->capacity) {
    if (!evict_lru(cache)) break;
  }
}

// Gives the entry stored for key_pair in the segment's map, or NULL; the caller
// holds the mutex. The pointer is read with memcpy, as in the other modules
// that store pointers in a chmap.
static ccache_entry *map_lookup(ccache_shard *cache,
                                const cmap_pair *key_pair) {
  cmap_pair found;
  ccol_retval r = chashmap_get_elem_ref(cache->map, key_pair, &found);
  if (r != ccol_success) return NULL;
  ccache_entry *e;
  memcpy(&e, found.ptr, sizeof(e));
  return e;
}

// Stores entry under key_pair in the segment's map (insert or replace); the
// caller holds the mutex. Gives the map's result.
static ccol_retval map_upsert(ccache_shard *cache, const cmap_pair *key_pair,
                              ccache_entry *entry) {
  cmap_pair vp = {.ptr = &entry, .size = sizeof(entry)};
  return chashmap_insert_elem(cache->map, key_pair, &vp);
}

// The most segments a cache gets. More segments mean less contention but a
// coarser eviction order, and no realistic thread count needs more.
#define CCACHE_MAX_SHARDS 16u

// The size a segment must have before a cache is split. Keys spread unevenly,
// so a full split cache evicts from its fuller segments early; with segments of
// this size that loss stays at a few percent, and smaller caches keep one exact
// eviction order.
#define CCACHE_MIN_ENTRIES_PER_SHARD 64u

// Gives the number of segments for a cache of capacity entries, at least 1 and
// at most CCACHE_MAX_SHARDS; the create sizes the cache with it.
static size_t shard_count_for(size_t capacity) {
  size_t n = capacity / CCACHE_MIN_ENTRIES_PER_SHARD;
  if (n > CCACHE_MAX_SHARDS) n = CCACHE_MAX_SHARDS;
  if (n == 0) n = 1;
  return n;
}

// Reduces a key hash h to a segment index below shard_count. The modulo reads
// only low bits, and the map's hash is meant to be read from its top bits, so a
// key set with constant low bits (aligned pointers, scaled IDs) would all land
// on one segment. A 64-bit avalanche first makes every bit depend on every
// input bit.
static inline size_t ccache_reduce(uint64_t h, size_t shard_count) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return (size_t)(h % (uint64_t)shard_count);
}

// Gives the segment that key_pair belongs to. It hashes through the map's own
// key hash, so keys the map treats as equal (-0.0 and 0.0, a long double by
// value) always land on the same segment and no padding byte is read. Gives
// NULL when the size of a fixed-width key is wrong, before anything reads the
// key, so a malformed key gives ccol_invalid_args whatever the segment count.
static ccache_shard *shard_for(struct clrucache *router,
                               const cmap_pair *key_pair) {
  if (router->key_fixed_width != 0 &&
      key_pair->size != router->key_fixed_width) {
    return NULL;
  }
  if (router->shard_count == 1) return router->shards[0];
  uint64_t h =
      ccol_chmap_hash_key(key_pair->ptr, key_pair->size, router->key_type);
  return router->shards[ccache_reduce(h, router->shard_count)];
}

// Frees the router and its segment array; the free function is read from the
// router's own procs copy before the router goes away. The last step of a
// destroy or a failed create.
static void router_free_self(struct clrucache *router) {
  ccol_memmgmt_procs *mp = router->m_procs;
  if (router->shards) ccol__mem_free(mp, router->shards);
  if (mp) {
    ccol_free f = mp->free;
    f(router);
  } else {
    free(router);
  }
}

// Builds one segment with its share of the capacity: its map, its LRU ring and
// its mutex, plus copies of the callbacks; mprocs is the router's procs, which
// the segment borrows. Gives NULL with *err set on failure. A failed mutex init
// is checked, since a lock on it would be undefined.
static ccache_shard *shard_create(size_t capacity, ccol_data_type key_type,
                                  ccache_remote_getter_fn getter,
                                  ccache_remote_setter_fn setter,
                                  ccache_eviction_fn eviction_cb,
                                  ccol_memmgmt_procs *mprocs, char **err) {
  ccache_shard *cache =
      (ccache_shard *)ccol__mem_calloc(mprocs, 1, sizeof(*cache));
  if (!cache) {
    if (err) *err = CCOL__ERR_STR("failed to allocate cache segment");
    return NULL;
  }
  cache->m_procs = mprocs;

  char *map_err = NULL;
  cache->map =
      chashmap_create_full(CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, key_type,
                           ccol_pointer, cache->m_procs, NULL, NULL, &map_err);
  if (!cache->map) {
    if (err)
      *err = map_err ? map_err : CCOL__ERR_STR("failed to create internal map");
    ccol__mem_free(mprocs, cache);
    return NULL;
  }

  cache->lru_head.next = &cache->lru_tail;
  cache->lru_head.prev = NULL;
  cache->lru_tail.prev = &cache->lru_head;
  cache->lru_tail.next = NULL;

  if (ccol__mutex_init(cache->mutex) != 0) {
    if (err) *err = CCOL__ERR_STR("failed to initialize cache mutex");
    chashmap__destroy(cache->map);
    ccol__mem_free(mprocs, cache);
    return NULL;
  }

  cache->capacity = capacity;
  cache->remote_getter = getter;
  cache->remote_setter = setter;
  cache->eviction_cb = eviction_cb;
  return cache;
}

// Evicts everything a segment holds, through the eviction callback and under
// its lock as on every other path, and frees the segment. No set can be in
// flight here, since the destroy waited for every pin first.
static void shard_destroy(ccache_shard *cache) {
  if (!cache) return;
  ccol__mutex_lock(cache->mutex);
  while (evict_lru(cache)) {
  }
  ccol__mutex_unlock(cache->mutex);

  chashmap__destroy(cache->map);
  ccol__mutex_destroy(cache->mutex);
  ccol__mem_free(cache->m_procs, cache);
}

// Destroys every segment of the router, for a destroy or a failed create.
static void router_destroy_shards(struct clrucache *router) {
  if (!router->shards) return;
  for (size_t i = 0; i < router->shard_count; i++) {
    shard_destroy(router->shards[i]);
    router->shards[i] = NULL;
  }
}

// Creates a cache of capacity entries split into shard_count_for(capacity)
// segments, and gives its handle or CCACHE_NULL with *err set. The segment
// capacities add up to exactly capacity (the first capacity % shard_count
// segments get one more). The procs are copied into the router. The slot is
// acquired last, once everything else is built; the module starts no threads,
// so a failure there only frees what was built. val_type is unused, since the
// maps store pointers.
ccache clrucache_create_full(size_t capacity, ccol_data_type key_type,
                             ccol_data_type val_type,
                             ccache_remote_getter_fn getter,
                             ccache_remote_setter_fn setter,
                             ccache_eviction_fn eviction_cb,
                             ccol_memmgmt_procs *mprocs, char **err) {
  (void)val_type;

  if (capacity == 0) {
    if (err) *err = CCOL__ERR_STR("capacity must be > 0");
    return CCACHE_NULL;
  }

  if (!ccol__verify_memmgmt_procs(mprocs, err)) return CCACHE_NULL;

  struct clrucache *router =
      (struct clrucache *)(mprocs ? mprocs->calloc(1, sizeof(*router))
                                  : calloc(1, sizeof(*router)));
  if (!router) {
    if (err) *err = CCOL__ERR_STR("failed to allocate cache struct");
    return CCACHE_NULL;
  }

  if (mprocs) {
    memcpy(&router->m_procs_copy, mprocs, sizeof(router->m_procs_copy));
    router->m_procs = &router->m_procs_copy;
  }

  router->capacity = capacity;
  router->key_type = key_type;
  router->key_fixed_width = ccol_fixed_width_data_type_size(key_type);
  router->shard_count = shard_count_for(capacity);
  router->shards = (ccache_shard **)ccol__mem_calloc(
      router->m_procs, router->shard_count, sizeof(*router->shards));
  if (!router->shards) {
    if (err) *err = CCOL__ERR_STR("failed to allocate cache segments");
    router_free_self(router);
    return CCACHE_NULL;
  }

  size_t base = capacity / router->shard_count;
  size_t extra = capacity % router->shard_count;
  for (size_t i = 0; i < router->shard_count; i++) {
    size_t cap_i = base + (i < extra ? 1 : 0);
    router->shards[i] = shard_create(cap_i, key_type, getter, setter,
                                     eviction_cb, router->m_procs, err);
    if (!router->shards[i]) {
      router_destroy_shards(router);
      router_free_self(router);
      return CCACHE_NULL;
    }
  }

  bool table_released = false;
  ccache h = clrucache_handle_slot_acquire(router, &table_released);
  if (h == 0) {
    if (err)
      *err = table_released
                 ? CCOL__ERR_STR(
                       "the ccache handle table is released at the end of "
                       "the process")
                 : CCOL__ERR_STR("failed to allocate ccache handle slot");
    router_destroy_shards(router);
    router_free_self(router);
    return CCACHE_NULL;
  }

  return h;
}

// Destroys the cache behind a live handle and releases its slot; CCACHE_NULL is
// a no-op and a stale handle is fatal. It looks the handle up and retires it in
// one write lock section, so a second destroy fails the lookup. Then it waits
// for the pins to drain before any teardown: no blocked caller here depends on
// the destroy to wake it (they wait for remote calls of other threads). It
// evicts every entry through the callback, frees the segments and the router,
// and releases the slot last, fetched again by index because a concurrent
// create can move the slots vector, with its generation raised. The destroy
// that frees the last slot also does a release the exit destructor deferred.
void clrucache__destroy(ccache cache) {
  if (!cache) return;

  ccol__call_once(clrucache_slot_table.once, clrucache_slot_table_init_globals);
  uint32_t idx = (uint32_t)(cache >> 32);
  uint32_t gen = (uint32_t)(cache & 0xFFFFFFFFu);
  ccol__rw_lock_wrlock(clrucache_slot_table.rwlock);
  clrucache_slot *slot = NULL;
  struct clrucache *raw = NULL;
  if (clrucache_slot_table.slots != NULL &&
      idx < cvector_elem_count(clrucache_slot_table.slots)) {
    clrucache_slot *s =
        (clrucache_slot *)cvector_at(clrucache_slot_table.slots, idx);
    if (s->in_use && s->generation == gen) {
      slot = s;
      raw = s->ptr;
    }
  }
  if (!raw) {
    ccol__rw_lock_unlock(clrucache_slot_table.rwlock);
    ccol__fatal_err(
        "clrucache_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a ccache handle)");
  }
  slot->in_use = false;
  ccol_pintable_retire(&clrucache_pintable, idx);
  ccol__rw_lock_unlock(clrucache_slot_table.rwlock);

  {
    long delay_ns = 1000;
    while (ccol_pintable_pins(&clrucache_pintable, idx) > 0) {
      struct timespec ts = {.tv_sec = 0, .tv_nsec = delay_ns};
      nanosleep(&ts, NULL);
      if (delay_ns < 1000000L) delay_ns *= 2;
    }
  }

  router_destroy_shards(raw);
  router_free_self(raw);

  ccol__rw_lock_wrlock(clrucache_slot_table.rwlock);
  clrucache_slot *slot2 =
      (clrucache_slot *)cvector_at(clrucache_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++;
  clrucache_free_index_release_locked(idx);
  clrucache_release_slot_table_if_deferred_locked();
  ccol__rw_lock_unlock(clrucache_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS
// Gives the cache behind h, looked up under the read lock with no pin, so a
// test has no unpin to forget.
struct clrucache *clrucache__resolve_for_tests(ccache h) {
  ccol__call_once(clrucache_slot_table.once, clrucache_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol__rw_lock_rdlock(clrucache_slot_table.rwlock);
  struct clrucache *raw = NULL;
  if (clrucache_slot_table.slots != NULL &&
      idx < cvector_elem_count(clrucache_slot_table.slots)) {
    clrucache_slot *slot =
        (clrucache_slot *)cvector_at(clrucache_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  ccol__rw_lock_unlock(clrucache_slot_table.rwlock);
  return raw;
}

// Gives the sum of the segment capacities and the segment count, so a test can
// pin how a capacity is split; nothing public shows the remainder.
size_t clrucache__segment_capacity_sum_for_tests(ccache cache,
                                                 size_t *out_segments) {
  struct clrucache *raw = clrucache_resolve(cache);
  if (!raw) {
    if (out_segments) *out_segments = 0;
    return 0;
  }
  size_t total = 0;
  for (size_t i = 0; i < raw->shard_count; i++)
    total += raw->shards[i]->capacity;
  if (out_segments) *out_segments = raw->shard_count;
  clrucache_resolve_unpin(raw);
  return total;
}

// Counts the segments whose map hashes in the keyed mode, each read under its
// mutex, so a test can see a flood switch the maps.
size_t clrucache__keyed_segment_maps_for_tests(ccache cache,
                                               size_t *out_segments) {
  struct clrucache *raw = clrucache_resolve(cache);
  if (!raw) {
    if (out_segments) *out_segments = 0;
    return 0;
  }
  size_t keyed = 0;
  for (size_t i = 0; i < raw->shard_count; i++) {
    ccol__mutex_lock(raw->shards[i]->mutex);
    if (chashmap_is_keyed_for_tests(raw->shards[i]->map)) keyed++;
    ccol__mutex_unlock(raw->shards[i]->mutex);
  }
  if (out_segments) *out_segments = raw->shard_count;
  clrucache_resolve_unpin(raw);
  return keyed;
}

// Gives the number of indices on the free list, so a test can see that no slot
// is lost.
size_t clrucache__free_index_count_for_tests(void) {
  ccol__call_once(clrucache_slot_table.once, clrucache_slot_table_init_globals);
  ccol__rw_lock_rdlock(clrucache_slot_table.rwlock);
  size_t n = clrucache_slot_table.free_indices
                 ? cvector_elem_count(clrucache_slot_table.free_indices)
                 : 0;
  ccol__rw_lock_unlock(clrucache_slot_table.rwlock);
  return n;
}

// Gives the waiter references of key_pair's entry (blocked threads and a thread
// running a remote call), or -1 when the key is absent, so a test knows that a
// getter really waits instead of guessing with a sleep.
int clrucache__key_waiters_for_tests(ccache cache, const cmap_pair *key_pair) {
  struct clrucache *raw = clrucache_resolve(cache);
  if (!raw) return -1;
  int n = -1;
  ccache_shard *shard = shard_for(raw, key_pair);
  if (shard) {
    ccol__mutex_lock(shard->mutex);
    ccache_entry *entry = map_lookup(shard, key_pair);
    if (entry) n = entry->waiters;
    ccol__mutex_unlock(shard->mutex);
  }
  clrucache_resolve_unpin(raw);
  return n;
}

// Gives the number of slots in the table, free ones included, so a test can see
// that a create and destroy loop reuses slots.
size_t clrucache__slot_table_capacity_for_tests(void) {
  ccol__call_once(clrucache_slot_table.once, clrucache_slot_table_init_globals);
  ccol__rw_lock_rdlock(clrucache_slot_table.rwlock);
  size_t n = clrucache_slot_table.slots
                 ? cvector_elem_count(clrucache_slot_table.slots)
                 : 0;
  ccol__rw_lock_unlock(clrucache_slot_table.rwlock);
  return n;
}

// Test hook: a publisher of a fetched or set value holds its segment's mutex
// this much longer before it wakes the waiters, so an unrelated thread (an
// evictor) can queue for the mutex first and the test reaches the path where a
// waiter survives a racing eviction. The flag tells that a publisher is in the
// delay. The test resets the delay to 0 itself.
static _Atomic unsigned int ccache_test_post_publish_delay_us = 0;
static _Atomic bool ccache_test_post_publish_delay_entered_flag = false;

// Sets the publish delay and clears the entered flag, for a test that races an
// eviction against a waiter.
void ccache_test_set_post_publish_delay_us(unsigned int delay_us) {
  atomic_store(&ccache_test_post_publish_delay_entered_flag, false);
  atomic_store(&ccache_test_post_publish_delay_us, delay_us);
}

// Tells whether a publisher is in its delay and so holds the mutex right now,
// so the test's evictor can lock it at the earliest moment.
bool ccache_test_post_publish_delay_entered(void) {
  return atomic_load(&ccache_test_post_publish_delay_entered_flag);
}

// Sleeps for the armed publish delay and sets the entered flag; publishers call
// it with the segment's mutex held, right before they wake the waiters.
static void ccache_test_maybe_delay_post_publish(void) {
  unsigned int us = atomic_load(&ccache_test_post_publish_delay_us);
  if (!us) return;
  atomic_store(&ccache_test_post_publish_delay_entered_flag, true);
  usleep(us);
}
#endif

// Tells whether any slot names a cache, so the table is never freed while a
// cache or its destroy may still use it; the caller holds the write lock. It
// reads ptr, not in_use: a destroy clears in_use first but still uses the table
// until its last step.
static bool clrucache_any_slot_live_locked(void) {
  size_t slot_count = cvector_elem_count(clrucache_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    clrucache_slot *slot =
        (clrucache_slot *)cvector_at(clrucache_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

// Frees the table when the exit destructor deferred that and no slot is live
// any more; the caller holds the write lock. Out of line so the destroy path
// keeps its shape.
static __attribute__((noinline)) void
clrucache_release_slot_table_if_deferred_locked(void) {
  if (clrucache_slot_table.release_deferred &&
      !clrucache_any_slot_live_locked()) {
    clrucache_release_slot_table_locked();
  }
}

// Frees the table vectors and the pin table; the caller holds the write lock
// and found no live slot. Each vector becomes NULL, so a later call sees the
// release. The rwlock isn't destroyed, since the caller holds it.
static void clrucache_release_slot_table_locked(void) {
  cvector_destroy(clrucache_slot_table.slots);
  cvector_destroy(clrucache_slot_table.free_indices);
  ccol_pintable_dispose(&clrucache_pintable);
  clrucache_slot_table.release_deferred = false;
}

// Frees the table at process exit or unload so a leak checker doesn't report
// it, or sets release_deferred when a cache is still live, so the destroy that
// frees the last slot does it. The call_once is needed because a process may
// never have created a cache. The static rwlock is never destroyed.
__attribute__((destructor)) static void cleanup_ccache_slot_table(void) {
  ccol__call_once(clrucache_slot_table.once, clrucache_slot_table_init_globals);
  if (!clrucache_slot_table.slots) return;
  ccol__rw_lock_wrlock(clrucache_slot_table.rwlock);
  if (clrucache_any_slot_live_locked()) {
    clrucache_slot_table.release_deferred = true;
    ccol__rw_lock_unlock(clrucache_slot_table.rwlock);
    return;
  }
  clrucache_release_slot_table_locked();
  ccol__rw_lock_unlock(clrucache_slot_table.rwlock);
}

static ccol_retval clrucache_get_full_retry(struct clrucache *raw,
                                            ccache_shard *shard,
                                            const cmap_pair *key_pair,
                                            cmap_pair *val_out);

// The body of clrucache_get_full(), entered with the segment's mutex held;
// every path that returns releases the mutex and the pin. On a miss it inserts
// a placeholder, runs the remote getter outside the mutex with a waiter
// reference held (so other getters of the key join this fetch) and stores the
// value; a failed fetch removes the placeholder with fetch_failed set. On a hit
// it waits for a fetch or set in flight. A waiter that finds a failed set looks
// the key up again, since nobody asked the getter: with again == NULL (the hot
// instantiation) through clrucache_get_full_retry, otherwise by setting *again.
// A waiter that finds a value keeps it even when an eviction removed the entry
// meanwhile; its waiter reference keeps the entry alive. Gives a heap copy in
// *val_out.
static inline __attribute__((always_inline)) ccol_retval
clrucache_get_full_locked(struct clrucache *raw, ccache_shard *shard,
                          const cmap_pair *key_pair, cmap_pair *val_out,
                          bool *again) {
  ccache_entry *entry = map_lookup(shard, key_pair);

  if (!entry) {
    if (!shard->remote_getter) {
      ccol__mutex_unlock(shard->mutex);
      clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }

    entry = entry_alloc(shard);
    if (!entry) {
      ccol__mutex_unlock(shard->mutex);
      clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }

    entry->key = ccol__mem_alloc(shard->m_procs, key_pair->size);
    if (!entry->key) {
      ccol__mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }
    memcpy(entry->key, key_pair->ptr, key_pair->size);
    entry->key_size = key_pair->size;
    entry->fetch_in_progress = true;

    ccol_retval ins = map_upsert(shard, key_pair, entry);
    if (ins != ccol_success && ins != ccol_key_already_present) {
      ccol__mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      clrucache_resolve_unpin(raw);
      return ins;
    }

    entry->waiters++;
    ccol__mutex_unlock(shard->mutex);

    cmap_pair fetched = {};
    bool fetch_ok = shard->remote_getter(key_pair, &fetched);

    ccol__mutex_lock(shard->mutex);
    entry->waiters--;
    entry->fetch_in_progress = false;

    if (fetch_ok && fetched.ptr && fetched.size > 0) {
      make_room(shard);
      entry->value = fetched.ptr;
      entry->value_size = fetched.size;
      lru_add_to_front(shard, entry);
      shard->size++;

      void *copy = ccol__mem_alloc(shard->m_procs, fetched.size);
      if (!copy) {
        ccol__cond_var_broadcast(entry->cond);
        ccol__mutex_unlock(shard->mutex);
        clrucache_resolve_unpin(raw);
        return ccol_not_enough_memory;
      }
      memcpy(copy, fetched.ptr, fetched.size);
      val_out->ptr = copy;
      val_out->size = fetched.size;

#ifdef RUNNING_UNIT_TESTS
      ccache_test_maybe_delay_post_publish();
#endif
      ccol__cond_var_broadcast(entry->cond);
      ccol__mutex_unlock(shard->mutex);
      clrucache_resolve_unpin(raw);
      return ccol_success;
    } else {
      if (fetched.ptr) ccol__mem_free(shard->m_procs, fetched.ptr);
      cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
      chashmap_delete_elem(shard->map, &kp);
      entry->evicted = true;
      entry->fetch_failed = true;
      ccol__cond_var_broadcast(entry->cond);
      bool should_free = (entry->waiters == 0);
      ccol__mutex_unlock(shard->mutex);
      if (should_free) entry_free(shard, entry);
      clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }
  }

  if (__builtin_expect(entry->fetch_in_progress || entry->set_in_progress, 0)) {
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      ccol__cond_var_wait(entry->cond, shard->mutex);
    }
    entry->waiters--;

    if (!entry->value) {
      if (!entry->fetch_failed) {
        if (entry->evicted && entry->waiters == 0) {
          ccol__mutex_unlock(shard->mutex);
          entry_free(shard, entry);
          ccol__mutex_lock(shard->mutex);
        }
        if (again) {
          *again = true;
          return ccol_success;
        }
        return clrucache_get_full_retry(raw, shard, key_pair, val_out);
      }
      ccol_retval fail_r = entry->fetch_size_mismatch ? ccol_unexpected_failure
                                                      : ccol_key_not_found;
      bool should_free = (entry->evicted && entry->waiters == 0);
      ccol__mutex_unlock(shard->mutex);
      if (should_free) entry_free(shard, entry);
      clrucache_resolve_unpin(raw);
      return fail_r;
    }
  }

  ccol__assert(entry->value != NULL);
  ccol__assert(entry->evicted || entry->in_lru);

  void *copy = ccol__mem_alloc(shard->m_procs, entry->value_size);
  if (!copy) {
    bool should_free = (entry->evicted && entry->waiters == 0);
    ccol__mutex_unlock(shard->mutex);
    if (should_free) entry_free(shard, entry);
    clrucache_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }
  if (!entry->evicted) lru_move_to_front(shard, entry);
  memcpy(copy, entry->value, entry->value_size);
  val_out->ptr = copy;
  val_out->size = entry->value_size;

  bool should_free = (entry->evicted && entry->waiters == 0);
  ccol__mutex_unlock(shard->mutex);
  if (should_free) entry_free(shard, entry);
  clrucache_resolve_unpin(raw);
  return ccol_success;
}

// The cold instantiation of clrucache_get_full_locked(): repeats the lookup as
// long as a waiter keeps finding a failed set. Keeping the loop here spares the
// hit path a register spill.
static __attribute__((noinline)) ccol_retval
clrucache_get_full_retry(struct clrucache *raw, ccache_shard *shard,
                         const cmap_pair *key_pair, cmap_pair *val_out) {
  for (;;) {
    bool retry = false;
    ccol_retval r =
        clrucache_get_full_locked(raw, shard, key_pair, val_out, &retry);
    if (!retry) return r;
  }
}

// Looks key_pair up and gives a heap copy of its value; the raw get. It
// resolves the handle, checks the arguments, picks the key's segment (NULL
// means a wrongly sized fixed-width key) and runs clrucache_get_full_locked
// under that segment's mutex.
ccol_retval clrucache_get_full(ccache cache, const cmap_pair *key_pair,
                               cmap_pair *val_out) {
  struct clrucache *raw = clrucache_resolve(cache);
  if (!raw) return ccol_invalid_args;
  if (!key_pair || !key_pair->ptr || !key_pair->size || !val_out) {
    clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccache_shard *shard = shard_for(raw, key_pair);
  if (!shard) {
    clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccol__mutex_lock(shard->mutex);
  return clrucache_get_full_locked(raw, shard, key_pair, val_out, NULL);
}

// Allocates a placeholder entry for key_pair and inserts it into the segment's
// map, for a set of an absent key. Gives the entry, or NULL with the real
// failure code in *err_out. Called and returns with the mutex held; it unlocks
// only around its own entry_free calls, so the free doesn't block other
// callers.
static ccache_entry *create_and_insert_placeholder(ccache_shard *cache,
                                                   const cmap_pair *key_pair,
                                                   ccol_retval *err_out) {
  ccache_entry *e = entry_alloc(cache);
  if (!e) {
    *err_out = ccol_not_enough_memory;
    return NULL;
  }

  e->key = ccol__mem_alloc(cache->m_procs, key_pair->size);
  if (!e->key) {
    ccol__mutex_unlock(cache->mutex);
    entry_free(cache, e);
    ccol__mutex_lock(cache->mutex);
    *err_out = ccol_not_enough_memory;
    return NULL;
  }
  memcpy(e->key, key_pair->ptr, key_pair->size);
  e->key_size = key_pair->size;

  ccol_retval ins = map_upsert(cache, key_pair, e);
  if (ins != ccol_success && ins != ccol_key_already_present) {
    ccol__mutex_unlock(cache->mutex);
    entry_free(cache, e);
    ccol__mutex_lock(cache->mutex);
    *err_out = ins;
    return NULL;
  }
  return e;
}

// Installs a value copy that the set already allocated and makes the entry live
// and newest, evicting first when the entry is new to the list. It allocates
// nothing and can't fail, which is what lets a set never fail after its remote
// setter succeeded. Gives the old value, which the caller frees after
// unlocking. The caller holds the mutex.
static void *entry_store_value(ccache_shard *cache, ccache_entry *entry,
                               void *new_val, size_t new_size) {
  void *old_val = entry->value;
  entry->value = new_val;
  entry->value_size = new_size;

  if (!entry->in_lru) {
    make_room(cache);
    lru_add_to_front(cache, entry);
    cache->size++;
  } else {
    lru_move_to_front(cache, entry);
  }
  return old_val;
}

// Stores val_pair under key_pair after the remote setter succeeded; the raw
// set. The value copy is made first, outside the mutex, so every allocation
// fails before any side effect. It loops until this thread owns the key's set
// slot, waiting on any fetch or set in flight, because several setters woken
// together must find each other's placeholder. Without a remote setter the
// update runs under the mutex in one go. With one, the setter runs outside the
// mutex with a waiter reference held while the key stays an ordinary entry; a
// failed setter frees the copy and removes a placeholder that an eviction left,
// a successful one installs the copy. Waiters are woken and freed memory is
// released after the unlock.
ccol_retval clrucache_set_full(ccache cache, const cmap_pair *key_pair,
                               const cmap_pair *val_pair) {
  struct clrucache *raw = clrucache_resolve(cache);
  if (!raw) return ccol_invalid_args;
  if (!key_pair || !key_pair->ptr || !key_pair->size || !val_pair ||
      !val_pair->ptr || val_pair->size == 0) {
    clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccache_shard *shard = shard_for(raw, key_pair);
  if (!shard) {
    clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  void *new_val = ccol__mem_alloc(shard->m_procs, val_pair->size);
  if (!new_val) {
    clrucache_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }
  memcpy(new_val, val_pair->ptr, val_pair->size);

  ccol__mutex_lock(shard->mutex);

  ccache_entry *entry;

  for (;;) {
    entry = map_lookup(shard, key_pair);
    if (!entry) {
      ccol_retval create_err = ccol_not_enough_memory;
      entry = create_and_insert_placeholder(shard, key_pair, &create_err);
      if (!entry) {
        ccol__mutex_unlock(shard->mutex);
        ccol__mem_free(shard->m_procs, new_val);
        clrucache_resolve_unpin(raw);
        return create_err;
      }
      break;
    }
    if (!entry->fetch_in_progress && !entry->set_in_progress) {
      break;
    }
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      ccol__cond_var_wait(entry->cond, shard->mutex);
    }
    entry->waiters--;
    if (!entry->evicted) {
      break;
    }
    bool should_free = (entry->waiters == 0);
    if (should_free) {
      ccol__mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      ccol__mutex_lock(shard->mutex);
    }
  }

  entry->set_in_progress = true;

  bool remote_ok = true;

  if (shard->remote_setter) {
    entry->waiters++;
    ccol__mutex_unlock(shard->mutex);

    remote_ok = shard->remote_setter(key_pair, val_pair);

    ccol__mutex_lock(shard->mutex);
    entry->waiters--;
  }

  ccol_retval retval = remote_ok ? ccol_success : ccol_unexpected_failure;

  void *val_to_free;
  if (remote_ok) {
    val_to_free = entry_store_value(shard, entry, new_val, val_pair->size);
  } else {
    val_to_free = new_val;
  }
  if (!remote_ok && !entry->in_lru) {
    cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
    chashmap_delete_elem(shard->map, &kp);
    entry->evicted = true;
  }

  entry->set_in_progress = false;
#ifdef RUNNING_UNIT_TESTS
  ccache_test_maybe_delay_post_publish();
#endif
  ccol__cond_var_broadcast(entry->cond);

  bool should_free = (entry->evicted && entry->waiters == 0);
  ccol__mutex_unlock(shard->mutex);
  if (val_to_free) ccol__mem_free(shard->m_procs, val_to_free);
  if (should_free) entry_free(shard, entry);

  clrucache_resolve_unpin(raw);
  return retval;
}

static ccol_retval clrucache_get_into_retry(struct clrucache *raw,
                                            ccache_shard *shard,
                                            const cmap_pair *key_pair,
                                            void *buf, size_t buf_size);

// The body of clrucache__get_into(), entered with the segment's mutex held;
// every path that returns releases the mutex and the pin. It follows
// clrucache_get_full_locked, but copies into buf. A fetched or stored value
// whose size isn't exactly buf_size gives ccol_unexpected_failure: a larger one
// would overflow buf and a smaller one leave part of it stale. A fetched value
// of the wrong size is a failed fetch (fetch_size_mismatch, nothing cached) so
// every waiter gets the same answer, and a stored one is refused before the
// entry moves in the eviction order.
static inline __attribute__((always_inline)) ccol_retval
clrucache_get_into_locked(struct clrucache *raw, ccache_shard *shard,
                          const cmap_pair *key_pair, void *buf, size_t buf_size,
                          bool *again) {
  ccache_entry *entry = map_lookup(shard, key_pair);

  if (!entry) {
    if (!shard->remote_getter) {
      ccol__mutex_unlock(shard->mutex);
      clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }

    entry = entry_alloc(shard);
    if (!entry) {
      ccol__mutex_unlock(shard->mutex);
      clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }

    entry->key = ccol__mem_alloc(shard->m_procs, key_pair->size);
    if (!entry->key) {
      ccol__mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }
    memcpy(entry->key, key_pair->ptr, key_pair->size);
    entry->key_size = key_pair->size;
    entry->fetch_in_progress = true;

    ccol_retval ins = map_upsert(shard, key_pair, entry);
    if (ins != ccol_success && ins != ccol_key_already_present) {
      ccol__mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      clrucache_resolve_unpin(raw);
      return ins;
    }

    entry->waiters++;
    ccol__mutex_unlock(shard->mutex);

    cmap_pair fetched = {};
    bool fetch_ok = shard->remote_getter(key_pair, &fetched);

    ccol__mutex_lock(shard->mutex);
    entry->waiters--;
    entry->fetch_in_progress = false;

    if (fetch_ok && fetched.ptr && fetched.size > 0) {
      if (fetched.size != buf_size) {
        ccol__mem_free(shard->m_procs, fetched.ptr);
        cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
        chashmap_delete_elem(shard->map, &kp);
        entry->evicted = true;
        entry->fetch_size_mismatch = true;
        entry->fetch_failed = true;
        ccol__cond_var_broadcast(entry->cond);
        bool should_free = (entry->waiters == 0);
        ccol__mutex_unlock(shard->mutex);
        if (should_free) entry_free(shard, entry);
        clrucache_resolve_unpin(raw);
        return ccol_unexpected_failure;
      }

      make_room(shard);
      entry->value = fetched.ptr;
      entry->value_size = fetched.size;
      lru_add_to_front(shard, entry);
      shard->size++;

      memcpy(buf, fetched.ptr, fetched.size);

#ifdef RUNNING_UNIT_TESTS
      ccache_test_maybe_delay_post_publish();
#endif
      ccol__cond_var_broadcast(entry->cond);
      ccol__mutex_unlock(shard->mutex);
      clrucache_resolve_unpin(raw);
      return ccol_success;
    } else {
      if (fetched.ptr) ccol__mem_free(shard->m_procs, fetched.ptr);
      cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
      chashmap_delete_elem(shard->map, &kp);
      entry->evicted = true;
      entry->fetch_failed = true;
      ccol__cond_var_broadcast(entry->cond);
      bool should_free = (entry->waiters == 0);
      ccol__mutex_unlock(shard->mutex);
      if (should_free) entry_free(shard, entry);
      clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }
  }

  if (entry->fetch_in_progress || entry->set_in_progress) {
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      ccol__cond_var_wait(entry->cond, shard->mutex);
    }
    entry->waiters--;

    if (!entry->value) {
      if (!entry->fetch_failed) {
        if (entry->evicted && entry->waiters == 0) {
          ccol__mutex_unlock(shard->mutex);
          entry_free(shard, entry);
          ccol__mutex_lock(shard->mutex);
        }
        if (again) {
          *again = true;
          return ccol_success;
        }
        return clrucache_get_into_retry(raw, shard, key_pair, buf, buf_size);
      }
      ccol_retval fail_r = entry->fetch_size_mismatch ? ccol_unexpected_failure
                                                      : ccol_key_not_found;
      bool should_free = (entry->evicted && entry->waiters == 0);
      ccol__mutex_unlock(shard->mutex);
      if (should_free) entry_free(shard, entry);
      clrucache_resolve_unpin(raw);
      return fail_r;
    }
  }

  ccol__assert(entry->value != NULL);
  ccol__assert(entry->evicted || entry->in_lru);

  if (entry->value_size != buf_size) {
    bool should_free = (entry->evicted && entry->waiters == 0);
    ccol__mutex_unlock(shard->mutex);
    if (should_free) entry_free(shard, entry);
    clrucache_resolve_unpin(raw);
    return ccol_unexpected_failure;
  }
  if (!entry->evicted) lru_move_to_front(shard, entry);
  memcpy(buf, entry->value, entry->value_size);

  bool should_free = (entry->evicted && entry->waiters == 0);
  ccol__mutex_unlock(shard->mutex);
  if (should_free) entry_free(shard, entry);
  clrucache_resolve_unpin(raw);
  return ccol_success;
}

// The cold instantiation of clrucache_get_into_locked(): repeats the lookup as
// long as a waiter keeps finding a failed set.
static __attribute__((noinline)) ccol_retval clrucache_get_into_retry(
    struct clrucache *raw, ccache_shard *shard, const cmap_pair *key_pair,
    void *buf, size_t buf_size) {
  for (;;) {
    bool retry = false;
    ccol_retval r =
        clrucache_get_into_locked(raw, shard, key_pair, buf, buf_size, &retry);
    if (!retry) return r;
  }
}

// Copies the value of key_pair into buf; the get behind ccache_get() for every
// value type but char *. It resolves the handle, checks the arguments, picks
// the key's segment and runs clrucache_get_into_locked under its mutex.
ccol_retval clrucache__get_into(ccache cache, const cmap_pair *key_pair,
                                void *buf, size_t buf_size) {
  struct clrucache *raw = clrucache_resolve(cache);
  if (!raw) return ccol_invalid_args;
  if (!key_pair || !key_pair->ptr || !key_pair->size || !buf || buf_size == 0) {
    clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccache_shard *shard = shard_for(raw, key_pair);
  if (!shard) {
    clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccol__mutex_lock(shard->mutex);
  return clrucache_get_into_locked(raw, shard, key_pair, buf, buf_size, NULL);
}

// Gives the number of live entries, for monitoring. It adds up the segment
// sizes, each under its own lock, so the total is a snapshot.
size_t clrucache_size(ccache cache) {
  struct clrucache *raw = clrucache_resolve(cache);
  if (!raw) return 0;
  size_t total = 0;
  for (size_t i = 0; i < raw->shard_count; i++) {
    ccache_shard *sh = raw->shards[i];
    ccol__mutex_lock(sh->mutex);
    total += sh->size;
    ccol__mutex_unlock(sh->mutex);
  }
  clrucache_resolve_unpin(raw);
  return total;
}

// Gives the capacity the cache was created with. It never changes after the
// create, so no lock is needed.
size_t clrucache_capacity(ccache cache) {
  struct clrucache *raw = clrucache_resolve(cache);
  if (!raw) return 0;
  size_t c = raw->capacity;
  clrucache_resolve_unpin(raw);
  return c;
}
