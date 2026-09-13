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
#include <stdatomic.h>
#ifdef RUNNING_UNIT_TESTS
#include <unistd.h> /* usleep(); not a pthread/sem primitive, needs no
                        common.h wrapper */
#endif

/* ========================================================================== */
/*                         INTERNAL STRUCTURES                                */
/* ========================================================================== */

/*
 * A single cache entry. Entries live in two data structures simultaneously:
 *
 *   1. The hash map (cache->map): key -> clru_entry*
 *      The map stores a COPY of the pointer (8 bytes). Following the pointer
 *      gives us the mutable entry, including its mutex and value.
 *
 *   2. The LRU doubly-linked list (only when the entry is LIVE, i.e. has a
 *      value). The list is bounded by two sentinel nodes embedded in the cache
 *      struct (lru_head = MRU end, lru_tail = LRU end).
 *
 * Entry lifecycle:
 *   PLACEHOLDER  in map, NOT in LRU, value==NULL, fetch_in_progress or
 *                set_in_progress set. Never evicted by the LRU mechanism.
 *   LIVE         in map, IN LRU, value!=NULL. Evictable.
 *   DEAD         NOT in map, NOT in LRU, evicted==true. Kept alive only by
 *                threads that hold a waiter reference (waiters > 0). Freed
 *                by whoever decrements waiters to 0 (while holding the
 *                cache mutex).
 *
 * Locking discipline:
 *   A single cache->mutex protects ALL fields of the cache struct AND all
 *   fields of every entry. Per-entry condition variables share this same mutex
 *   (valid for cond_wait). This eliminates lock-ordering issues.
 *
 *   Slow operations (remote getter/setter) always happen OUTSIDE the mutex.
 *   Any thread releasing the mutex while holding a pointer to an entry MUST
 *   increment entry->waiters before releasing, and decrement on re-acquire.
 *   This prevents the entry from being freed under its feet.
 */
typedef struct clru_entry {
  void *key;
  size_t key_size;

  void *value; /* NULL while fetch/set is in progress */
  size_t value_size;

  cond_var_t cond; /* shared with cache->mutex */

  bool fetch_in_progress; /* a remote getter is running for this key */
  bool set_in_progress;   /* a remote/async setter is running for this key */
  bool evicted;           /* removed from map+LRU; kept alive by waiters */

  /* Set only by __clrucache_get_into()'s own miss-path when a remote
   * getter's fetched value size does not match the requesting caller's
   * buf_size, right before that entry is torn down as a failed fetch
   * (never left true on a live entry). A coalesced waiter that wakes to
   * find entry->value still NULL consults this to report the same
   * ccol_unexpected_failure the fetching thread itself received, instead
   * of the generic ccol_key_not_found every other fetch-failure reason
   * produces; without it, only the one thread that happened to actually
   * run the remote getter learned that the real problem was a mis-sized
   * value, contradicting this module's own "all others ... receive the
   * same result" coalescing contract (see clrucache.h's file-level doc
   * comment). */
  bool fetch_size_mismatch;

  int waiters; /* threads currently blocked on cond; protected by cache->mutex
                */

  struct clru_entry *prev; /* LRU list links (NULL when not in LRU) */
  struct clru_entry *next;
  bool in_lru;
} clru_entry;

/* clru_cache is an opaque value handle (top 32 bits = slot index, bottom 32
 * bits = generation; see include/clrucache.h's own doc comment on the
 * typedef), resolved through this table before the underlying struct
 * clrucache* is ever touched. This is what lets __clrucache_destroy detect
 * BOTH a concurrent double-destroy (racing another destroy on the same
 * still-live handle) AND a sequential one (a stale handle, from an earlier,
 * already-completed destroy) as a fatal_err rather than a use-after-free/
 * double-free: a slot is marked not-in-use the instant it is released, and
 * its generation is bumped on every reuse, so a stale handle can never
 * alias a later, unrelated cache occupying the same slot index. Mirrors
 * chttpcli_slot_table/chttpsvr_slot_table/event_loop_slot_table exactly;
 * see src/chttpclient.c's own copy of this comment for the full design
 * rationale.
 *
 * The table's own lock is a read-write lock, not a plain mutex: _clrucache_
 * resolve (read-only: bounds-check idx, compare generation, read slot->ptr)
 * runs on every single clru_cache get/set/delete call; _clrucache_handle_
 * slot_acquire/__clrucache_destroy (the only mutators) each run once per
 * cache's entire lifetime, not once per operation. Mirrors cthreadcomm.c's/
 * cthreadpool.c's/chttpclient.c's own identical slot-table rwlock
 * conversions. Like chttpclient.c's chttpcli_slot_table, this table has no
 * pthread_atfork() protection of its own at all (clrucache.c registers
 * none), so this conversion carries none of cthreadcomm.c's/cthreadpool.c's
 * own TID-tracked-write-lock reinit-in-child subtlety: there being nothing
 * to preserve doesn't change what fork() safety this table already did or
 * didn't have. */
typedef struct {
  struct clrucache *ptr; /* NULL when slot is free */
  uint32_t generation;   /* minted fresh on every acquire; monotonic per
                             slot index, starts at 0 (pre-first-use),
                             becomes 1 on first acquire */
  bool in_use;
} clrucache_slot_t;

static struct {
  rw_lock_t rwlock;
  once_flag_t once;
  cvec slots;        /* cvec of clrucache_slot_t; grows via push_back only,
                         indices permanent once allocated */
  cvec free_indices; /* cvec of uint32_t; LIFO free list, O(1) reuse */
} clrucache_slot_table = {0};

static void _clrucache_slot_table_init_globals(void) {
  if (rw_lock_init(clrucache_slot_table.rwlock) != 0)
    fatal_err("clru_cache slot table: failed to initialize rwlock");
  clrucache_slot_table.slots = cvector_create(sizeof(clrucache_slot_t), NULL);
  if (!clrucache_slot_table.slots)
    fatal_err("clru_cache slot table: failed to allocate slots vector");
  clrucache_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!clrucache_slot_table.free_indices)
    fatal_err("clru_cache slot table: failed to allocate free-index vector");
}

struct clrucache {
  chmap map; /* key -> clru_entry* (always separate-chaining) */

  /* Sentinel nodes for the doubly-linked LRU list.
   * lru_head.next == MRU entry; lru_tail.prev == LRU entry.
   * The sentinels are embedded (not heap-allocated) and their cond/key/value
   * fields are never used. */
  clru_entry lru_head;
  clru_entry lru_tail;

  size_t capacity;
  size_t size; /* number of LIVE entries */

  mutex_t mutex; /* protects everything */

  clru_remote_getter_t remote_getter;
  clru_remote_setter_t remote_setter;
  clru_eviction_cb_t eviction_cb;

  ccol_memmgmt_procs_t *m_procs;

  /* Pinned by _clrucache_resolve (lock-free atomic increment) for as long
   * as some caller holds a just-resolved struct clrucache* it hasn't yet
   * released via _clrucache_resolve_unpin. __clrucache_destroy blocks
   * until this reaches 0 (via pin_cv, under mutex) before freeing the
   * object, closing a real resolve-then-use race a naive "look up, unlock,
   * return the pointer" resolve step would otherwise leave open. Reuses
   * this cache's own single mutex for the unpin side's decrement+broadcast
   * (unlike event_loop's fully lock-free pin, this module is already a
   * single-global-mutex design, so this introduces no new contention
   * beyond what every entry point already pays today). */
  _Atomic size_t pending_resolve_count;
  cond_var_t pin_cv; /* wakes __clrucache_destroy's wait; shares mutex */
};

/* ========================================================================== */
/*                    CLRU_CACHE HANDLE RESOLVE / UNPIN                       */
/* ========================================================================== */

/* Resolves h and pins the result against concurrent destroy, or returns NULL
 * if h is 0, garbage, or references a currently-free or already-reused
 * (wrong-generation) slot. On success, the caller MUST call
 * _clrucache_resolve_unpin(result) exactly once, as soon as it is done
 * touching the resolved struct clrucache*. */
static struct clrucache *_clrucache_resolve(clru_cache h) {
  call_once(clrucache_slot_table.once, _clrucache_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  rw_lock_rdlock(clrucache_slot_table.rwlock);
  struct clrucache *raw = NULL;
  if (idx < cvector_elem_count(clrucache_slot_table.slots)) {
    clrucache_slot_t *slot =
        (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  /* No raw->mutex acquisition here at all, so nothing can ever block while
   * clrucache_slot_table.rwlock is held; matches chttpcli's own
   * _chttpcli_resolve reasoning exactly. Safe because raw is guaranteed
   * still-allocated here regardless: the only thing that could make it
   * unsafe to touch, __clrucache_destroy's slot-release step, also
   * requires clrucache_slot_table.rwlock's write side, which cannot run
   * concurrently with this read side regardless. */
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  rw_lock_unlock(clrucache_slot_table.rwlock);
  return raw;
}

static void _clrucache_resolve_unpin(struct clrucache *raw) {
  /* The decrement itself MUST happen under raw->mutex, not as a bare atomic
   * op outside it: see _chttpcli_resolve_unpin's own comment in
   * src/chttpclient.c for the full account of the lost-wakeup use-after-free
   * an earlier draft of that exact function had, which this mirrors
   * exactly. */
  mutex_lock(raw->mutex);
  atomic_fetch_sub(&raw->pending_resolve_count, 1);
  cond_var_broadcast(raw->pin_cv); /* wake a destroy waiting on this */
  mutex_unlock(raw->mutex);
}

/* Allocates a fresh slot (or reuses a freed one) for cache and returns the
 * resulting handle, or 0 on OOM. Called once, from clrucache_create_full,
 * after the object is otherwise fully constructed. */
static clru_cache _clrucache_handle_slot_acquire(struct clrucache *cache) {
  call_once(clrucache_slot_table.once, _clrucache_slot_table_init_globals);
  rw_lock_wrlock(clrucache_slot_table.rwlock);
  uint32_t idx;
  clrucache_slot_t *slot;
  if (cvector_elem_count(clrucache_slot_table.free_indices) > 0) {
    cvector_pop_back(clrucache_slot_table.free_indices, &idx);
    slot = (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
  } else {
    clrucache_slot_t fresh = {0};
    if (cvector_push_back(clrucache_slot_table.slots, &fresh) != ccol_success) {
      rw_lock_unlock(clrucache_slot_table.rwlock);
      return 0; /* ordinary, non-fatal OOM */
    }
    idx = (uint32_t)cvector_elem_count(clrucache_slot_table.slots) - 1;
    slot = (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
  }
  slot->generation++;
  /* Skip the one generation value that would collide with the reserved
   * "invalid handle" sentinel (0) after ~2^32 reuses of this exact slot
   * index; see chttpcli_handle_slot_acquire's identical guard for the full
   * rationale. */
  if (slot->generation == 0) slot->generation++;
  slot->ptr = cache;
  slot->in_use = true;
  clru_cache h = ((clru_cache)idx << 32) | (clru_cache)slot->generation;
  rw_lock_unlock(clrucache_slot_table.rwlock);
  return h;
}

/* ========================================================================== */
/*                         LRU LIST HELPERS                                   */
/* ========================================================================== */

static void lru_add_to_front(clrucache *cache, clru_entry *e) {
  e->next = cache->lru_head.next;
  e->prev = &cache->lru_head;
  cache->lru_head.next->prev = e;
  cache->lru_head.next = e;
  e->in_lru = true;
}

static void lru_remove(clru_entry *e) {
  if (!e->in_lru) return;
  e->prev->next = e->next;
  e->next->prev = e->prev;
  e->prev = NULL;
  e->next = NULL;
  e->in_lru = false;
}

static void lru_move_to_front(clrucache *cache, clru_entry *e) {
  lru_remove(e);
  lru_add_to_front(cache, e);
}

/* ========================================================================== */
/*                         CACHE SELF-FREE HELPER                             */
/* ========================================================================== */

/* Free the cache struct and its privately-owned m_procs copy.
 * Must be called LAST in any teardown path, after all other resources are
 * freed. Captures the free function pointer before releasing procs so it
 * remains valid after the procs struct itself is freed (same pattern as
 * __chmap_destroy). */
static void cache_free_self(clrucache *cache) {
  ccol_memmgmt_procs_t *mp = cache->m_procs;
  if (mp) {
    ccol_free_t free_fn = mp->free;
    free_fn(mp);
    free_fn(cache);
  } else {
    free(cache);
  }
}

/* ========================================================================== */
/*                         ENTRY HELPERS                                      */
/* ========================================================================== */

static clru_entry *entry_alloc(clrucache *cache) {
  clru_entry *e =
      (clru_entry *)_mem_calloc(cache->m_procs, 1, sizeof(clru_entry));
  if (!e) return NULL;
  if (cond_var_init(e->cond) != 0) {
    /* Do NOT route this through entry_free(): that would call
     * cond_var_destroy() on a condvar that was never successfully
     * initialized, which is undefined behavior. e->key/e->value are still
     * NULL (fresh calloc), so a plain free is all that is needed. */
    _mem_free(cache->m_procs, e);
    return NULL;
  }
  return e;
}

/* Free key, value, condvar, and the entry itself.
 * Precondition: no other thread can reach or is currently referencing e.
 * This holds in exactly two situations: (1) e was never published into
 * cache->map (an allocation/insert failed before or during
 * map_upsert/chmap_insert_elem), so no other thread could ever have found
 * it in the first place; or (2) e->evicted == true && e->waiters == 0, i.e.
 * it was published, has since been removed from both the map and the LRU
 * list, and every thread that was ever waiting on it has already woken up
 * and released its reference. The cache mutex may or may not be held by
 * the caller; correctness depends solely on one of the two situations
 * above, not on the mutex itself. */
static void entry_free(clrucache *cache, clru_entry *e) {
  _mem_free(cache->m_procs, e->key);
  _mem_free(cache->m_procs, e->value);
  cond_var_destroy(e->cond);
  _mem_free(cache->m_procs, e);
}

/* ========================================================================== */
/*                         EVICTION (called under mutex)                      */
/* ========================================================================== */

/* Evict the least-recently-used LIVE entry. Calls the eviction callback
 * (while holding the mutex) before removing the entry. */
static void evict_lru(clrucache *cache) {
  clru_entry *victim = cache->lru_tail.prev;
  if (victim == &cache->lru_head) return; /* empty LRU list */

  if (cache->eviction_cb && victim->value) {
    cmap_pair kp = {.ptr = victim->key, .size = victim->key_size};
    cmap_pair vp = {.ptr = victim->value, .size = victim->value_size};
    cache->eviction_cb(&kp, &vp);
  }

  lru_remove(victim);

  cmap_pair kp = {.ptr = victim->key, .size = victim->key_size};
  chmap_delete_elem(cache->map, &kp);

  victim->evicted = true;
  cond_var_broadcast(victim->cond);
  cache->size--;

  if (victim->waiters == 0) {
    entry_free(cache, victim);
  }
}

/* Ensure cache->size < cache->capacity by evicting LRU entries. */
static void make_room(clrucache *cache) {
  while (cache->size >= cache->capacity &&
         cache->lru_tail.prev != &cache->lru_head) {
    evict_lru(cache);
  }
}

/* ========================================================================== */
/*                         CHMAP LOOKUP HELPER                                */
/* ========================================================================== */

/* Look up the entry pointer stored in the internal chmap.
 * Returns NULL if key not found. Caller must hold cache->mutex.
 *
 * chmap_entry's SSO storage is naturally aligned, so this memcpy is
 * defense-in-depth rather than a live alignment requirement; kept for
 * consistency with cjson/cyaml/cthreadcomm/chttpclient, which use the same
 * pattern for their own chmap-backed pointer storage. */
static clru_entry *map_lookup(clrucache *cache, const cmap_pair *key_pair) {
  cmap_pair *found = NULL;
  ccol_retval_t r = chmap_get_elem_ref(cache->map, key_pair, &found);
  if (r != ccol_success) return NULL;
  clru_entry *e;
  memcpy(&e, found->ptr, sizeof(e));
  return e;
}

/* Insert or update the entry pointer in the internal chmap. */
static ccol_retval_t map_upsert(clrucache *cache, const cmap_pair *key_pair,
                                clru_entry *entry) {
  cmap_pair vp = {.ptr = &entry, .size = sizeof(entry)};
  return chmap_insert_elem(cache->map, key_pair, &vp);
}

/* ========================================================================== */
/*                         clrucache_create_full                              */
/* ========================================================================== */

clru_cache clrucache_create_full(size_t capacity, ccol_data_type key_type,
                                 ccol_data_type val_type,
                                 clru_remote_getter_t getter,
                                 clru_remote_setter_t setter,
                                 clru_eviction_cb_t eviction_cb,
                                 ccol_memmgmt_procs_t *mprocs, char **err) {
  (void)val_type; /* val_type not needed: internal map always stores pointers */

  if (capacity == 0) {
    if (err) *err = CCOL_ERR_STR("capacity must be > 0");
    return CLRU_CACHE_INVALID;
  }

  if (!ccol_verify_memmgmt_procs(mprocs, err)) return CLRU_CACHE_INVALID;

  clrucache *cache = (clrucache *)(mprocs ? mprocs->calloc(1, sizeof(*cache))
                                          : calloc(1, sizeof(*cache)));
  if (!cache) {
    if (err) *err = CCOL_ERR_STR("failed to allocate cache struct");
    return CLRU_CACHE_INVALID;
  }

  if (mprocs) {
    cache->m_procs = (ccol_memmgmt_procs_t *)mprocs->malloc(sizeof(*mprocs));
    if (!cache->m_procs) {
      mprocs->free(cache);
      if (err) *err = CCOL_ERR_STR("failed to allocate m_procs copy");
      return CLRU_CACHE_INVALID;
    }
    mem_cpy(cache->m_procs, mprocs, sizeof(*mprocs));
  }

  /* Internal map: key type as supplied; value type is always ccol_pointer */
  char *map_err = NULL;
  cache->map = chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, key_type,
                                 ccol_pointer, cache->m_procs, NULL, &map_err);
  if (!cache->map) {
    if (err)
      *err = map_err ? map_err : CCOL_ERR_STR("failed to create internal map");
    cache_free_self(cache);
    return CLRU_CACHE_INVALID;
  }

  /* Initialise LRU sentinel ring (sentinels have no condvar; never waited on)
   */
  cache->lru_head.next = &cache->lru_tail;
  cache->lru_head.prev = NULL;
  cache->lru_tail.prev = &cache->lru_head;
  cache->lru_tail.next = NULL;

  /* Both return values are checked, unlike a bare fire-and-forget call:
   * pthread_mutex_init/pthread_cond_init can legitimately fail (e.g. ENOMEM)
   * per POSIX, and returning a handle backed by a not-fully-initialized
   * mutex/condvar would make every later lock/wait/broadcast on it (in
   * particular _clrucache_resolve_unpin and __clrucache_destroy) undefined
   * behavior; mirrors entry_alloc's own cond_var_init check a few dozen
   * lines above in this same file. */
  if (mutex_init(cache->mutex) != 0) {
    if (err) *err = CCOL_ERR_STR("failed to initialize cache mutex");
    __chmap_destroy(cache->map);
    cache_free_self(cache);
    return CLRU_CACHE_INVALID;
  }
  if (cond_var_init(cache->pin_cv) != 0) {
    if (err)
      *err = CCOL_ERR_STR("failed to initialize cache pin condition variable");
    mutex_destroy(cache->mutex);
    __chmap_destroy(cache->map);
    cache_free_self(cache);
    return CLRU_CACHE_INVALID;
  }
  atomic_init(&cache->pending_resolve_count, (size_t)0);

  cache->capacity = capacity;
  cache->remote_getter = getter;
  cache->remote_setter = setter;
  cache->eviction_cb = eviction_cb;

  /* Slot acquisition is the LITERAL LAST step, after the cache is otherwise
   * fully constructed: mirroring chttpcli/chttpsvr/event_loop's own
   * constructors exactly, so that no handle is ever exposed to any caller
   * until this function is already about to return success. Unlike
   * event_loop/ctpool, this module spawns no threads of its own, so a
   * failure here needs no thread-stopping, just releasing what was already
   * successfully constructed. */
  clru_cache h = _clrucache_handle_slot_acquire(cache);
  if (h == 0) {
    if (err) *err = CCOL_ERR_STR("failed to allocate clru_cache handle slot");
    __chmap_destroy(cache->map);
    mutex_destroy(cache->mutex);
    cond_var_destroy(cache->pin_cv);
    cache_free_self(cache);
    return CLRU_CACHE_INVALID;
  }

  return h;
}

/* ========================================================================== */
/*                         __clrucache_destroy                                */
/* ========================================================================== */

void __clrucache_destroy(clru_cache cache) {
  if (!cache) return;

  /* Resolve cache through the slot table, marking the slot not-in-use in
   * the same critical section as the lookup: this is what makes a second,
   * concurrent (or later, sequential) destroy call on the same handle
   * value see a resolve failure rather than racing this call's own
   * teardown; see the slot table's own file-level comment and
   * _clrucache_resolve's comment for the full design. A stale or
   * already-destroyed handle reaching here is exactly the misuse this
   * redesign exists to catch: it is fatal, not a silent use-after-free/
   * double-free. */
  call_once(clrucache_slot_table.once, _clrucache_slot_table_init_globals);
  uint32_t idx = (uint32_t)(cache >> 32);
  uint32_t gen = (uint32_t)(cache & 0xFFFFFFFFu);
  rw_lock_wrlock(clrucache_slot_table.rwlock);
  clrucache_slot_t *slot = NULL;
  struct clrucache *raw = NULL;
  if (idx < cvector_elem_count(clrucache_slot_table.slots)) {
    clrucache_slot_t *s =
        (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
    if (s->in_use && s->generation == gen) {
      slot = s;
      raw = s->ptr;
    }
  }
  if (!raw) {
    rw_lock_unlock(clrucache_slot_table.rwlock);
    fatal_err(
        "clrucache_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a clru_cache handle)");
  }
  slot->in_use = false; /* blocks ALL future resolves for this handle from
                            this instant, including a second concurrent
                            destroy attempt */
  rw_lock_unlock(clrucache_slot_table.rwlock);

  /* Wait for pending_resolve_count to reach 0 BEFORE running any teardown
   * logic at all (not just before freeing memory); mirrors chttpcli's own
   * ordering exactly. Safe waiting first (unlike ctpool): the module's own
   * blocking waits (the coalescing-getter/setter cond_var_wait loops) are
   * released by whichever other application thread's remote_getter/
   * remote_setter call completes for that key, a mechanism entirely
   * independent of anything destroy does, so no pinned/blocked caller here
   * ever depends on destroy-side logic to release its own pin. */
  mutex_lock(raw->mutex);
  while (atomic_load(&raw->pending_resolve_count) > 0) {
    cond_var_wait(raw->pin_cv, raw->mutex);
  }
  mutex_unlock(raw->mutex);

  /* Evict all remaining live entries (calls eviction callback for each) */
  mutex_lock(raw->mutex);
  while (raw->lru_tail.prev != &raw->lru_head) {
    evict_lru(raw);
  }
  mutex_unlock(raw->mutex);

  __chmap_destroy(raw->map);
  mutex_destroy(raw->mutex);
  cond_var_destroy(raw->pin_cv);

  cache_free_self(raw);

  /* Release the slot last, only after raw is fully torn down and freed:
   * this is what makes the slot's generation bump (and the free-index
   * push-back) mark the handle as reusable, not any earlier step. Re-fetch
   * by idx rather than reusing `slot`: a concurrent
   * clrucache_create_full's own _clrucache_handle_slot_acquire call in
   * between may have reallocated slots' backing array via
   * cvector_push_back, invalidating any pointer into it taken before this
   * second lock acquisition; idx itself is stable. */
  rw_lock_wrlock(clrucache_slot_table.rwlock);
  clrucache_slot_t *slot2 =
      (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* bumps this slot's generation past whatever value
      the just-freed cache's handle carried, so that stale handle can never
      again match a FUTURE acquire's generation for this same index */
  cvector_push_back(clrucache_slot_table.free_indices, &idx);
  rw_lock_unlock(clrucache_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS
/* Resolves h to its underlying struct clrucache* WITHOUT pinning it (does
 * not touch pending_resolve_count at all): a bare slot-table lookup, safe
 * for tests specifically because test code calling this runs synchronously,
 * single-threaded, with no concurrent destroy to race in the first place;
 * unlike _clrucache_resolve, there is no matching _unpin call a test needs
 * to remember, which would otherwise be an easy gap to leave (a forgotten
 * unpin would leave pending_resolve_count permanently nonzero on that
 * cache, silently hanging every future clru_destroy call against it).
 * Returns NULL under the exact same conditions _clrucache_resolve does. */
struct clrucache *_clrucache_resolve_for_tests(clru_cache h) {
  call_once(clrucache_slot_table.once, _clrucache_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  rw_lock_rdlock(clrucache_slot_table.rwlock);
  struct clrucache *raw = NULL;
  if (idx < cvector_elem_count(clrucache_slot_table.slots)) {
    clrucache_slot_t *slot =
        (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  rw_lock_unlock(clrucache_slot_table.rwlock);
  return raw;
}

/* Reads how many slots the clru_cache handle table currently holds (grown
 * ones plus freed-but-not-yet-reused ones): lets a test assert that a
 * create/destroy churn loop reuses freed slots rather than growing the
 * table without bound. */
size_t _clrucache_slot_table_capacity_for_tests(void) {
  call_once(clrucache_slot_table.once, _clrucache_slot_table_init_globals);
  rw_lock_rdlock(clrucache_slot_table.rwlock);
  size_t n = cvector_elem_count(clrucache_slot_table.slots);
  rw_lock_unlock(clrucache_slot_table.rwlock);
  return n;
}

/*
 * Lets a test bias, without guaranteeing outright, which of two threads
 * wins the race for raw->mutex once a successful fetch/set finishes
 * publishing an entry: called while the publishing thread STILL HOLDS
 * raw->mutex, immediately before it calls cond_var_broadcast() (which is
 * what first makes a coalesced waiter blocked on that same entry start
 * contending for the mutex) and mutex_unlock(). Holding the mutex for
 * longer here gives a concurrent, unrelated thread trying to lock the same
 * mutex (e.g. to run an eviction that will remove the entry just
 * published) more time to already be queued up waiting for it BEFORE the
 * coalesced waiter is even woken, which in practice (though POSIX makes no
 * FIFO guarantee) tends to let the already-queued thread be serviced
 * first once the mutex is finally released. This is what lets a test
 * reliably exercise the coalesced-waiter-survives-a-racing-eviction path
 * instead of depending on rare, hard-to-reproduce scheduling luck. Does
 * not auto-disarm: a test needs this armed for exactly the one call under
 * test, then must reset it to 0 itself once done so later, unrelated
 * tests in the same process are unaffected. */
static _Atomic unsigned int _clru_test_post_publish_delay_us = 0;
static _Atomic bool _clru_test_post_publish_delay_entered = false;

void clru_test_set_post_publish_delay_us(unsigned int delay_us) {
  atomic_store(&_clru_test_post_publish_delay_entered, false);
  atomic_store(&_clru_test_post_publish_delay_us, delay_us);
}

/* Lets a test poll for "the publishing thread has just entered its delay
 * and is therefore, right now, still holding raw->mutex" WITHOUT itself
 * needing raw->mutex (which it cannot safely take here anyway; taking it
 * would just make the polling thread block until the delay is already
 * over, defeating the point). This is what lets a test's own evictor
 * thread attempt to lock the mutex at (as close as userspace polling
 * allows to) the earliest possible moment, rather than guessing at a
 * fixed sleep duration that might fire too early (before the publishing
 * thread has even re-locked the mutex, in which case there is nothing yet
 * to queue up behind) or leave less margin than intended. */
bool clru_test_post_publish_delay_entered(void) {
  return atomic_load(&_clru_test_post_publish_delay_entered);
}

static void _clru_test_maybe_delay_post_publish(void) {
  unsigned int us = atomic_load(&_clru_test_post_publish_delay_us);
  if (!us) return;
  atomic_store(&_clru_test_post_publish_delay_entered, true);
  usleep(us);
}
#endif

/* Frees the slot table's own bookkeeping arrays at process exit, so
 * make memtest's leak-kind reporting does not flag them as still-
 * reachable; mirrors event_loop's own _cleanup_event_loop_slot_table
 * exactly (see that function's own comment for the full rationale,
 * including why this is sound only given every clru_cache the
 * application created was itself destroyed before process exit; the
 * same precondition this test suite already satisfies for a clean
 * make memtest). MUST call_once here: __attribute__((destructor))
 * functions run unconditionally for the whole shared object regardless
 * of which parts of it were actually used, so a process that links this
 * library but never creates a single clru_cache would otherwise lock a
 * never-pthread_mutex_init'd mutex here. */
__attribute__((destructor)) static void _cleanup_clrucache_slot_table(void) {
  call_once(clrucache_slot_table.once, _clrucache_slot_table_init_globals);
  rw_lock_wrlock(clrucache_slot_table.rwlock);
  __cvector_destroy(clrucache_slot_table.slots);
  __cvector_destroy(clrucache_slot_table.free_indices);
  rw_lock_unlock(clrucache_slot_table.rwlock);
}

/* ========================================================================== */
/*                         clrucache_get_full                                 */
/* ========================================================================== */

ccol_retval_t clrucache_get_full(clru_cache cache, const cmap_pair *key_pair,
                                 cmap_pair *val_out) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return ccol_invalid_args;
  if (!key_pair || !key_pair->ptr || !key_pair->size || !val_out) {
    _clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  mutex_lock(raw->mutex);

  clru_entry *entry = map_lookup(raw, key_pair);

  if (!entry) {
    /* Cache miss */
    if (!raw->remote_getter) {
      mutex_unlock(raw->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }

    /* Create a placeholder so other threads waiting for the same key can
     * find it and coalesce onto the single remote fetch we're about to do. */
    entry = entry_alloc(raw);
    if (!entry) {
      mutex_unlock(raw->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }

    entry->key = _mem_alloc(raw->m_procs, key_pair->size);
    if (!entry->key) {
      /* Release the mutex before freeing, matching every other entry_free
       * call site in this file that frees an entry never published into
       * cache->map: the single global cache mutex must not be held across
       * the freed entry's own _mem_free/cond_var_destroy calls, which would
       * otherwise needlessly block every other concurrent getter/setter on
       * this cache for their duration. */
      mutex_unlock(raw->mutex);
      entry_free(raw, entry);
      _clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }
    mem_cpy(entry->key, key_pair->ptr, key_pair->size);
    entry->key_size = key_pair->size;
    entry->fetch_in_progress = true;

    /* ccol_key_already_present is a documented SUCCESS outcome of
     * chmap_insert_elem (the key's stored value, i.e. our own entry
     * pointer, was written in place) and must be treated the same as
     * ccol_success here, matching every other chmap_insert_elem call site
     * in this codebase (see cjson.c/clogger.c). This should be structurally
     * unreachable given map_lookup() just confirmed the key absent under
     * this same, uninterrupted mutex hold, but treating it as a hard
     * failure would be actively dangerous, not merely redundant: the map
     * already points at `entry` by the time chmap_insert_elem returns, so
     * freeing `entry` here (as a plain OOM-style failure would) would leave
     * a dangling pointer behind for the very next lookup of this key. */
    ccol_retval_t ins = map_upsert(raw, key_pair, entry);
    if (ins != ccol_success && ins != ccol_key_already_present) {
      mutex_unlock(raw->mutex);
      entry_free(raw, entry);
      _clrucache_resolve_unpin(raw);
      return ins;
    }

    /* We are the fetcher: release mutex, call remote getter */
    mutex_unlock(raw->mutex);

    cmap_pair fetched = {};
    bool fetch_ok = raw->remote_getter(key_pair, &fetched);

    mutex_lock(raw->mutex);
    entry->fetch_in_progress = false;

    if (fetch_ok && fetched.ptr && fetched.size > 0) {
      make_room(raw);
      entry->value = fetched.ptr;
      entry->value_size = fetched.size;
      lru_add_to_front(raw, entry);
      raw->size++;

      void *copy = _mem_alloc(raw->m_procs, fetched.size);
      if (!copy) {
        /* Entry is cached; caller just can't get a copy this time. */
        cond_var_broadcast(entry->cond);
        mutex_unlock(raw->mutex);
        _clrucache_resolve_unpin(raw);
        return ccol_not_enough_memory;
      }
      mem_cpy(copy, fetched.ptr, fetched.size);
      val_out->ptr = copy;
      val_out->size = fetched.size;

#ifdef RUNNING_UNIT_TESTS
      /* Widens the window a concurrent, unrelated eviction has to reach
       * and lock raw->mutex before the broadcast below wakes any
       * coalesced waiter; see the hook's own doc comment. */
      _clru_test_maybe_delay_post_publish();
#endif
      cond_var_broadcast(entry->cond);
      mutex_unlock(raw->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_success;
    } else {
      /* Remote getter failed: remove placeholder, notify waiters */
      if (fetched.ptr)
        _mem_free(raw->m_procs, fetched.ptr); /* size==0 edge case */
      cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
      chmap_delete_elem(raw->map, &kp);
      entry->evicted = true;
      cond_var_broadcast(entry->cond);
      bool should_free = (entry->waiters == 0);
      mutex_unlock(raw->mutex);
      if (should_free) entry_free(raw, entry);
      _clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }
  }

  /* Entry exists: wait for any in-progress operation to settle */
  if (entry->fetch_in_progress || entry->set_in_progress) {
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      cond_var_wait(entry->cond, raw->mutex);
    }
    entry->waiters--;

    if (!entry->value) {
      /* The fetch/set we coalesced onto genuinely failed (or never
       * populated a value): nothing to return. A size-mismatched fetch
       * (see fetch_size_mismatch's own comment) is reported the same way
       * to every coalesced waiter as it was to the thread that actually
       * ran the remote getter, rather than downgrading to the generic
       * "not found" just because this thread didn't run the fetch itself. */
      ccol_retval_t fail_r = entry->fetch_size_mismatch
                                 ? ccol_unexpected_failure
                                 : ccol_key_not_found;
      bool should_free = (entry->evicted && entry->waiters == 0);
      mutex_unlock(raw->mutex);
      if (should_free) entry_free(raw, entry);
      _clrucache_resolve_unpin(raw);
      return fail_r;
    }
    /* entry->value is non-NULL: the operation we coalesced onto
     * succeeded. Fall through to the shared "return the value" path
     * below even if entry->evicted is now true; an unrelated,
     * concurrent cache operation may have evicted this entry from the
     * map/LRU in the window between the fetcher/setter's broadcast and
     * our own wakeup, but entry itself (and its value) is still safe to
     * read here: we still hold a waiter reference on it, which is
     * exactly what keeps it from being freed until we are done. Coalesced
     * waiters must receive the same result the fetcher/setter did, not
     * silently discard a value that already exists right in front of
     * them just because it was evicted moments later. */
  }

  /* Copy the value out and, if the entry is still live, refresh its
   * LRU position */
  /* Invariant: entry->value != NULL here always implies either the entry
   * is still LIVE (in_lru) or it was evicted after successfully being
   * populated (in which case it is no longer part of the LRU list and
   * must not be touched as one). Eviction removes the entry from the map
   * before setting evicted=true, so a non-NULL map_lookup result that
   * required no wait is always a valid LIVE entry. */
  ccol_assert(entry->value != NULL);
  ccol_assert(entry->evicted || entry->in_lru);

  /* Allocate the caller's copy BEFORE promoting the entry's LRU position:
   * a call that ends up failing (OOM here) must not still have the side
   * effect of moving the entry to the front of the eviction order. */
  void *copy = _mem_alloc(raw->m_procs, entry->value_size);
  if (!copy) {
    bool should_free = (entry->evicted && entry->waiters == 0);
    mutex_unlock(raw->mutex);
    if (should_free) entry_free(raw, entry);
    _clrucache_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }
  if (!entry->evicted) lru_move_to_front(raw, entry);
  mem_cpy(copy, entry->value, entry->value_size);
  val_out->ptr = copy;
  val_out->size = entry->value_size;

  bool should_free = (entry->evicted && entry->waiters == 0);
  mutex_unlock(raw->mutex);
  if (should_free) entry_free(raw, entry);
  _clrucache_resolve_unpin(raw);
  return ccol_success;
}

/* ========================================================================== */
/*                         clrucache_set_full                                 */
/* ========================================================================== */

/*
 * Helper: allocate a new placeholder entry for the given key and insert it
 * into the chmap. Returns the entry on success, NULL on failure with the
 * real ccol_retval_t written to *err_out (matching clrucache_get_full's/
 * __clrucache_get_into's own miss-path handling of map_upsert's return
 * value: a non-OOM failure, e.g. ccol_container_full, must be reported as
 * itself, not folded into a generic ccol_not_enough_memory). Caller must
 * hold cache->mutex; this helper always returns with cache->mutex held
 * (both on success and on failure), briefly releasing it only around its
 * own entry_free() calls (see those call sites' own comment).
 */
static clru_entry *create_and_insert_placeholder(clrucache *cache,
                                                 const cmap_pair *key_pair,
                                                 ccol_retval_t *err_out) {
  clru_entry *e = entry_alloc(cache);
  if (!e) {
    *err_out = ccol_not_enough_memory;
    return NULL;
  }

  e->key = _mem_alloc(cache->m_procs, key_pair->size);
  if (!e->key) {
    /* Release the mutex before freeing, then re-acquire it before
     * returning, so this helper's own "returns with cache->mutex held"
     * contract holds regardless of outcome: the single global cache mutex
     * must not be held across the freed entry's own _mem_free/
     * cond_var_destroy calls, which would otherwise needlessly block every
     * other concurrent getter/setter on this cache for their duration. */
    mutex_unlock(cache->mutex);
    entry_free(cache, e);
    mutex_lock(cache->mutex);
    *err_out = ccol_not_enough_memory;
    return NULL;
  }
  mem_cpy(e->key, key_pair->ptr, key_pair->size);
  e->key_size = key_pair->size;

  /* ccol_key_already_present is a documented success outcome of
   * chmap_insert_elem, not a failure; see the identical guard and comment
   * in clrucache_get_full's own miss path for the full reasoning. */
  ccol_retval_t ins = map_upsert(cache, key_pair, e);
  if (ins != ccol_success && ins != ccol_key_already_present) {
    mutex_unlock(cache->mutex);
    entry_free(cache, e);
    mutex_lock(cache->mutex);
    *err_out = ins;
    return NULL;
  }
  return e;
}

/*
 * Helper: write value into an entry and make it LIVE.
 * Evicts if necessary. Caller must hold cache->mutex.
 */
static ccol_retval_t entry_store_value(clrucache *cache, clru_entry *entry,
                                       const cmap_pair *val_pair) {
  void *new_val = _mem_alloc(cache->m_procs, val_pair->size);
  if (!new_val) return ccol_not_enough_memory;
  mem_cpy(new_val, val_pair->ptr, val_pair->size);

  _mem_free(cache->m_procs, entry->value);
  entry->value = new_val;
  entry->value_size = val_pair->size;

  if (!entry->in_lru) {
    make_room(cache);
    lru_add_to_front(cache, entry);
    cache->size++;
  } else {
    lru_move_to_front(cache, entry);
  }
  return ccol_success;
}

ccol_retval_t clrucache_set_full(clru_cache cache, const cmap_pair *key_pair,
                                 const cmap_pair *val_pair) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return ccol_invalid_args;
  if (!key_pair || !key_pair->ptr || !key_pair->size || !val_pair ||
      !val_pair->ptr || val_pair->size == 0) {
    _clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  mutex_lock(raw->mutex);

  bool created_new = false;
  clru_entry *entry;

  /*
   * Loop until we own the set slot for this key.  A single map_lookup +
   * create-or-wait is not enough because multiple setter threads that were
   * all blocked on the same in-progress operation can wake up simultaneously
   * after that operation completes (or fails).  Without the loop, the second
   * thread to run would call create_and_insert_placeholder for a key that the
   * first thread already re-inserted, getting ccol_key_already_present from
   * chmap_insert_elem and incorrectly returning ccol_not_enough_memory to the
   * caller.  By looping back to map_lookup we find the first thread's
   * placeholder and wait for it, serialising the setters correctly.
   */
  for (;;) {
    entry = map_lookup(raw, key_pair);
    if (!entry) {
      ccol_retval_t create_err = ccol_not_enough_memory;
      entry = create_and_insert_placeholder(raw, key_pair, &create_err);
      if (!entry) {
        mutex_unlock(raw->mutex);
        _clrucache_resolve_unpin(raw);
        return create_err;
      }
      created_new = true;
      break;
    }
    if (!entry->fetch_in_progress && !entry->set_in_progress) {
      break; /* Entry is live; we own it */
    }
    /* A concurrent fetch or set is in progress: wait for it to complete */
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      cond_var_wait(entry->cond, raw->mutex);
    }
    entry->waiters--;
    if (!entry->evicted) {
      break; /* Entry survived; we own it */
    }
    /* Entry was cleaned up while we waited: loop back to re-check the map
     * before creating a new placeholder, since another waiter may have
     * already done so. */
    bool should_free = (entry->waiters == 0);
    if (should_free) {
      /* Release the mutex before freeing, matching every other entry_free
       * call site in this file: the single global cache mutex must not be
       * held across the freed entry's own _mem_free/cond_var_destroy calls,
       * which would otherwise needlessly block every other concurrent
       * getter/setter on this cache for their duration. Safe to re-lock and
       * continue the loop afterward: `entry` is not touched again except by
       * being reassigned from the next map_lookup() call, and this
       * function's own pin (acquired at entry via _clrucache_resolve) keeps
       * `raw` itself alive regardless of whether the mutex is held. */
      mutex_unlock(raw->mutex);
      entry_free(raw, entry);
      mutex_lock(raw->mutex);
    }
  }

  /* We now own the set slot */
  entry->set_in_progress = true;

  /* If there is no remote setter, this whole operation is a plain, in-memory
   * update: never release the mutex at all (nothing to block on), and never
   * touch the LRU list ahead of entry_store_value(), which already handles
   * both the !in_lru (new/placeholder) and in_lru (existing, move-to-front)
   * cases correctly on its own. This avoids paying for a needless
   * unlock/relock pair and an LRU-remove/make_room/re-add cycle on every
   * clru_set() call for the common case of a cache with no remote source. */
  bool removed_from_lru = false;
  bool remote_ok = true;

  if (raw->remote_setter) {
    /* If the entry is currently LIVE (in the LRU list), remove it before
     * releasing the mutex.  evict_lru() only selects from the LRU list, so an
     * entry that is not in the list cannot be chosen as a victim.  Without
     * this removal a concurrent make_room() could evict the entry while the
     * remote setter is executing; any getter blocked on set_in_progress
     * would then wake to find entry->evicted==true and return
     * ccol_key_not_found even though the setter succeeds and re-inserts the
     * value.  entry_store_value() handles the !in_lru path (make_room +
     * lru_add_to_front) correctly on success; on failure or OOM we restore
     * the entry to the LRU manually below. */
    removed_from_lru = (!created_new && entry->in_lru);
    if (removed_from_lru) {
      lru_remove(entry);
      raw->size--;
    }

    /* Hold a waiter reference so the entry is not freed if evicted while we
     * are blocked in the remote call. */
    entry->waiters++;
    mutex_unlock(raw->mutex);

    remote_ok = raw->remote_setter(key_pair, val_pair);

    mutex_lock(raw->mutex);
    entry->waiters--;
  }

  ccol_retval_t retval = remote_ok ? ccol_success : ccol_unexpected_failure;

  /* entry->evicted is always false here: brand-new placeholders are never in
   * the LRU list and cannot be selected by evict_lru; existing LIVE entries
   * were either explicitly removed from the LRU before the mutex was
   * released (remote setter configured), or the mutex was never released at
   * all (no remote setter), so they also cannot be evicted mid-call. */
  if (remote_ok) {
    ccol_retval_t store_r = entry_store_value(raw, entry, val_pair);
    if (store_r != ccol_success) {
      /* OOM: if we created the entry, remove it from the map */
      if (created_new) {
        cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
        chmap_delete_elem(raw->map, &kp);
        entry->evicted = true;
      } else if (removed_from_lru && entry->value) {
        /* Restore old-valued entry to LRU so it remains accessible.
         * make_room first: concurrent inserts may have filled the cache
         * while the mutex was released for the remote call. */
        make_room(raw);
        lru_add_to_front(raw, entry);
        raw->size++;
      }
      entry->set_in_progress = false;
      cond_var_broadcast(entry->cond);
      bool should_free = (entry->evicted && entry->waiters == 0);
      mutex_unlock(raw->mutex);
      if (should_free) entry_free(raw, entry);
      _clrucache_resolve_unpin(raw);
      return store_r;
    }
  } else {
    /* Remote setter failed. This branch is only reachable when
     * raw->remote_setter is non-NULL (remote_ok can only be false when the
     * remote call actually ran), so removed_from_lru's real formula
     * (!created_new && entry->in_lru) applies here, not its default-false
     * value. A non-created_new entry reaching this point is always LIVE
     * (fetch_in_progress/set_in_progress both false and evicted false, per
     * this file's own entry-lifecycle invariant, implies in_lru), which
     * this loop's remote_setter block has already unconditionally removed
     * from the LRU before releasing the mutex for the call; so
     * removed_from_lru is always true whenever created_new is false in
     * this specific branch. Asserted explicitly, rather than left as an
     * unenforced assumption, so a future change to that removal logic
     * would fail loudly here instead of silently leaving an existing
     * entry un-restored (removed from the LRU/size accounting but still
     * reachable via the map) after a failed set. */
    if (created_new) {
      /* Placeholder we created has no value: clean it up */
      cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
      chmap_delete_elem(raw->map, &kp);
      entry->evicted = true;
    } else {
      ccol_assert(removed_from_lru);
      /* Existing LIVE entry was removed from LRU: restore it with the old
       * value so the cache remains consistent and the entry stays
       * accessible. make_room first: concurrent inserts may have filled
       * the cache while the mutex was released for the remote call. */
      make_room(raw);
      lru_add_to_front(raw, entry);
      raw->size++;
    }
  }

  entry->set_in_progress = false;
#ifdef RUNNING_UNIT_TESTS
  _clru_test_maybe_delay_post_publish();
#endif
  cond_var_broadcast(entry->cond);

  bool should_free = (entry->evicted && entry->waiters == 0);
  mutex_unlock(raw->mutex);
  if (should_free) entry_free(raw, entry);

  _clrucache_resolve_unpin(raw);
  return retval;
}

/* ========================================================================== */
/*                         __clrucache_get_into                               */
/* ========================================================================== */

ccol_retval_t __clrucache_get_into(clru_cache cache, const cmap_pair *key_pair,
                                   void *buf, size_t buf_size) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return ccol_invalid_args;
  if (!key_pair || !key_pair->ptr || !key_pair->size || !buf || buf_size == 0) {
    _clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  mutex_lock(raw->mutex);

  clru_entry *entry = map_lookup(raw, key_pair);

  if (!entry) {
    if (!raw->remote_getter) {
      mutex_unlock(raw->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }

    entry = entry_alloc(raw);
    if (!entry) {
      mutex_unlock(raw->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }

    entry->key = _mem_alloc(raw->m_procs, key_pair->size);
    if (!entry->key) {
      /* Release the mutex before freeing; see the identical comment in
       * clrucache_get_full's own miss path for the full reasoning. */
      mutex_unlock(raw->mutex);
      entry_free(raw, entry);
      _clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }
    mem_cpy(entry->key, key_pair->ptr, key_pair->size);
    entry->key_size = key_pair->size;
    entry->fetch_in_progress = true;

    /* ccol_key_already_present is a documented success outcome of
     * chmap_insert_elem, not a failure; see the identical guard and comment
     * in clrucache_get_full's own miss path for the full reasoning. */
    ccol_retval_t ins = map_upsert(raw, key_pair, entry);
    if (ins != ccol_success && ins != ccol_key_already_present) {
      mutex_unlock(raw->mutex);
      entry_free(raw, entry);
      _clrucache_resolve_unpin(raw);
      return ins;
    }

    mutex_unlock(raw->mutex);

    cmap_pair fetched = {};
    bool fetch_ok = raw->remote_getter(key_pair, &fetched);

    mutex_lock(raw->mutex);
    entry->fetch_in_progress = false;

    if (fetch_ok && fetched.ptr && fetched.size > 0) {
      /* Reject a value whose size does not exactly match the destination
       * buffer. A larger value would overflow buf; a smaller one would leave
       * the tail of buf untouched (uninitialized/stale from the caller's own
       * stack/heap) while still reporting ccol_success, silently handing the
       * type-safe clru_get() macro's caller a partially-populated ValT.
       * Caching a mis-sized value would also make every future
       * __clrucache_get_into call for this key hit the same problem, so
       * treat any size mismatch as a fetch failure entirely rather than
       * caching it. */
      if (fetched.size != buf_size) {
        _mem_free(raw->m_procs, fetched.ptr);
        cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
        chmap_delete_elem(raw->map, &kp);
        entry->evicted = true;
        entry->fetch_size_mismatch = true;
        cond_var_broadcast(entry->cond);
        bool should_free = (entry->waiters == 0);
        mutex_unlock(raw->mutex);
        if (should_free) entry_free(raw, entry);
        _clrucache_resolve_unpin(raw);
        return ccol_unexpected_failure;
      }

      make_room(raw);
      entry->value = fetched.ptr;
      entry->value_size = fetched.size;
      lru_add_to_front(raw, entry);
      raw->size++;

      mem_cpy(buf, fetched.ptr, fetched.size);

#ifdef RUNNING_UNIT_TESTS
      _clru_test_maybe_delay_post_publish();
#endif
      cond_var_broadcast(entry->cond);
      mutex_unlock(raw->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_success;
    } else {
      if (fetched.ptr) _mem_free(raw->m_procs, fetched.ptr);
      cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
      chmap_delete_elem(raw->map, &kp);
      entry->evicted = true;
      cond_var_broadcast(entry->cond);
      bool should_free = (entry->waiters == 0);
      mutex_unlock(raw->mutex);
      if (should_free) entry_free(raw, entry);
      _clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }
  }

  if (entry->fetch_in_progress || entry->set_in_progress) {
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      cond_var_wait(entry->cond, raw->mutex);
    }
    entry->waiters--;

    if (!entry->value) {
      /* The fetch/set we coalesced onto genuinely failed (or never
       * populated a value): nothing to return. A size-mismatched fetch
       * (see fetch_size_mismatch's own comment) is reported the same way
       * to every coalesced waiter as it was to the thread that actually
       * ran the remote getter, rather than downgrading to the generic
       * "not found" just because this thread didn't run the fetch itself. */
      ccol_retval_t fail_r = entry->fetch_size_mismatch
                                 ? ccol_unexpected_failure
                                 : ccol_key_not_found;
      bool should_free = (entry->evicted && entry->waiters == 0);
      mutex_unlock(raw->mutex);
      if (should_free) entry_free(raw, entry);
      _clrucache_resolve_unpin(raw);
      return fail_r;
    }
    /* entry->value is non-NULL: the operation we coalesced onto
     * succeeded. Fall through to the shared "return the value" path
     * below even if entry->evicted is now true; see the identical
     * reasoning in clrucache_get_full's own wait branch. */
  }

  /* Invariant: entry->value != NULL here always implies either the entry
   * is still LIVE (in_lru) or it was evicted after successfully being
   * populated (in which case it is no longer part of the LRU list and
   * must not be touched as one). */
  ccol_assert(entry->value != NULL);
  ccol_assert(entry->evicted || entry->in_lru);

  /* Reject a size mismatch in either direction BEFORE promoting the
   * entry's LRU position: see the identical-size requirement and
   * reasoning in this function's own fetch-path check above. Only an
   * exact match may be copied into buf, and a call that is going to be
   * rejected must not still have the side effect of moving the entry to
   * the front of the eviction order. */
  if (entry->value_size != buf_size) {
    bool should_free = (entry->evicted && entry->waiters == 0);
    mutex_unlock(raw->mutex);
    if (should_free) entry_free(raw, entry);
    _clrucache_resolve_unpin(raw);
    return ccol_unexpected_failure;
  }
  if (!entry->evicted) lru_move_to_front(raw, entry);
  mem_cpy(buf, entry->value, entry->value_size);

  bool should_free = (entry->evicted && entry->waiters == 0);
  mutex_unlock(raw->mutex);
  if (should_free) entry_free(raw, entry);
  _clrucache_resolve_unpin(raw);
  return ccol_success;
}

/* ========================================================================== */
/*                         SIZE / CAPACITY QUERIES                            */
/* ========================================================================== */

size_t clrucache_size(clru_cache cache) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return 0;
  mutex_lock(raw->mutex);
  size_t s = raw->size;
  mutex_unlock(raw->mutex);
  _clrucache_resolve_unpin(raw);
  return s;
}

size_t clrucache_capacity(clru_cache cache) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return 0;
  mutex_lock(raw->mutex);
  size_t c = raw->capacity;
  mutex_unlock(raw->mutex);
  _clrucache_resolve_unpin(raw);
  return c;
}
