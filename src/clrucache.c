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

  int waiters; /* threads currently blocked on cond; protected by cache->mutex
                */

  struct clru_entry *prev; /* LRU list links (NULL when not in LRU) */
  struct clru_entry *next;
  bool in_lru;
} clru_entry;

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
};

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
  cond_var_init(e->cond);
  return e;
}

/* Free key, value, condvar, and the entry itself.
 * Precondition: entry->evicted == true && entry->waiters == 0, so no other
 * thread holds a reference.  The cache mutex may or may not be held by the
 * caller; correctness depends solely on exclusive access guaranteed by the
 * two preconditions above. */
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
 * chmap_entry is __attribute__((packed)), so val_storage.inline_data is not
 * 8-byte aligned. A direct *(clru_entry **)found->ptr cast is UB at -O3.
 * Use memcpy to perform an alignment-safe load (same fix as in cjson/cyaml). */
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
    return NULL;
  }

  if (!ccol_verify_memmgmt_procs(mprocs, err)) return NULL;

  clrucache *cache = (clrucache *)(mprocs ? mprocs->calloc(1, sizeof(*cache))
                                          : calloc(1, sizeof(*cache)));
  if (!cache) {
    if (err) *err = CCOL_ERR_STR("failed to allocate cache struct");
    return NULL;
  }

  if (mprocs) {
    cache->m_procs = (ccol_memmgmt_procs_t *)mprocs->malloc(sizeof(*mprocs));
    if (!cache->m_procs) {
      mprocs->free(cache);
      if (err) *err = CCOL_ERR_STR("failed to allocate m_procs copy");
      return NULL;
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
    return NULL;
  }

  /* Initialise LRU sentinel ring (sentinels have no condvar — never waited on)
   */
  cache->lru_head.next = &cache->lru_tail;
  cache->lru_head.prev = NULL;
  cache->lru_tail.prev = &cache->lru_head;
  cache->lru_tail.next = NULL;

  mutex_init(cache->mutex);

  cache->capacity = capacity;
  cache->remote_getter = getter;
  cache->remote_setter = setter;
  cache->eviction_cb = eviction_cb;

  return cache;
}

/* ========================================================================== */
/*                         __clrucache_destroy                                */
/* ========================================================================== */

void __clrucache_destroy(clru_cache cache) {
  if (!cache) return;

  /* Evict all remaining live entries (calls eviction callback for each) */
  mutex_lock(cache->mutex);
  while (cache->lru_tail.prev != &cache->lru_head) {
    evict_lru(cache);
  }
  mutex_unlock(cache->mutex);

  __chmap_destroy(cache->map);
  mutex_destroy(cache->mutex);

  cache_free_self(cache);
}

/* ========================================================================== */
/*                         clrucache_get_full                                 */
/* ========================================================================== */

ccol_retval_t clrucache_get_full(clru_cache cache, const cmap_pair *key_pair,
                                 cmap_pair *val_out) {
  if (!cache || !key_pair || !key_pair->ptr || !key_pair->size || !val_out)
    return ccol_invalid_args;

  mutex_lock(cache->mutex);

  clru_entry *entry = map_lookup(cache, key_pair);

  if (!entry) {
    /* ---- Cache miss ---- */
    if (!cache->remote_getter) {
      mutex_unlock(cache->mutex);
      return ccol_key_not_found;
    }

    /* Create a placeholder so other threads waiting for the same key can
     * find it and coalesce onto the single remote fetch we're about to do. */
    entry = entry_alloc(cache);
    if (!entry) {
      mutex_unlock(cache->mutex);
      return ccol_not_enough_memory;
    }

    entry->key = _mem_alloc(cache->m_procs, key_pair->size);
    if (!entry->key) {
      entry_free(cache, entry);
      mutex_unlock(cache->mutex);
      return ccol_not_enough_memory;
    }
    mem_cpy(entry->key, key_pair->ptr, key_pair->size);
    entry->key_size = key_pair->size;
    entry->fetch_in_progress = true;

    ccol_retval_t ins = map_upsert(cache, key_pair, entry);
    if (ins != ccol_success) {
      entry_free(cache, entry);
      mutex_unlock(cache->mutex);
      return ins;
    }

    /* ---- We are the fetcher: release mutex, call remote getter ---- */
    mutex_unlock(cache->mutex);

    cmap_pair fetched = {};
    bool fetch_ok = cache->remote_getter(key_pair, &fetched);

    mutex_lock(cache->mutex);
    entry->fetch_in_progress = false;

    if (fetch_ok && fetched.ptr && fetched.size > 0) {
      make_room(cache);
      entry->value = fetched.ptr;
      entry->value_size = fetched.size;
      lru_add_to_front(cache, entry);
      cache->size++;

      void *copy = _mem_alloc(cache->m_procs, fetched.size);
      if (!copy) {
        /* Entry is cached; caller just can't get a copy this time. */
        cond_var_broadcast(entry->cond);
        mutex_unlock(cache->mutex);
        return ccol_not_enough_memory;
      }
      mem_cpy(copy, fetched.ptr, fetched.size);
      val_out->ptr = copy;
      val_out->size = fetched.size;

      cond_var_broadcast(entry->cond);
      mutex_unlock(cache->mutex);
      return ccol_success;
    } else {
      /* Remote getter failed: remove placeholder, notify waiters */
      if (fetched.ptr)
        _mem_free(cache->m_procs, fetched.ptr); /* size==0 edge case */
      cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
      chmap_delete_elem(cache->map, &kp);
      entry->evicted = true;
      cond_var_broadcast(entry->cond);
      bool should_free = (entry->waiters == 0);
      mutex_unlock(cache->mutex);
      if (should_free) entry_free(cache, entry);
      return ccol_key_not_found;
    }
  }

  /* ---- Entry exists: wait for any in-progress operation to settle ---- */
  if (entry->fetch_in_progress || entry->set_in_progress) {
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      cond_var_wait(entry->cond, cache->mutex);
    }
    entry->waiters--;

    if (entry->evicted || !entry->value) {
      bool should_free = (entry->evicted && entry->waiters == 0);
      mutex_unlock(cache->mutex);
      if (should_free) entry_free(cache, entry);
      return ccol_key_not_found;
    }
  }

  /* ---- Entry is LIVE: copy value out and refresh LRU position ---- */
  /* Invariant: if map_lookup returned a non-NULL entry and no in-progress
   * operation was pending, the entry must be LIVE (in LRU, with a value).
   * Eviction removes the entry from the map before setting evicted=true, so
   * a non-NULL map_lookup result is always a valid LIVE entry here. */
  assert(!entry->evicted && entry->value != NULL && entry->in_lru);
  lru_move_to_front(cache, entry);

  void *copy = _mem_alloc(cache->m_procs, entry->value_size);
  if (!copy) {
    mutex_unlock(cache->mutex);
    return ccol_not_enough_memory;
  }
  mem_cpy(copy, entry->value, entry->value_size);
  val_out->ptr = copy;
  val_out->size = entry->value_size;

  mutex_unlock(cache->mutex);
  return ccol_success;
}

/* ========================================================================== */
/*                         clrucache_set_full                                 */
/* ========================================================================== */

/*
 * Helper: allocate a new placeholder entry for the given key and insert it
 * into the chmap. Returns the entry on success, NULL on OOM.
 * Caller must hold cache->mutex.
 */
static clru_entry *create_and_insert_placeholder(clrucache *cache,
                                                 const cmap_pair *key_pair) {
  clru_entry *e = entry_alloc(cache);
  if (!e) return NULL;

  e->key = _mem_alloc(cache->m_procs, key_pair->size);
  if (!e->key) {
    entry_free(cache, e);
    return NULL;
  }
  mem_cpy(e->key, key_pair->ptr, key_pair->size);
  e->key_size = key_pair->size;

  if (map_upsert(cache, key_pair, e) != ccol_success) {
    entry_free(cache, e);
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
  if (!cache || !key_pair || !key_pair->ptr || !key_pair->size || !val_pair ||
      !val_pair->ptr || val_pair->size == 0)
    return ccol_invalid_args;

  mutex_lock(cache->mutex);

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
    entry = map_lookup(cache, key_pair);
    if (!entry) {
      entry = create_and_insert_placeholder(cache, key_pair);
      if (!entry) {
        mutex_unlock(cache->mutex);
        return ccol_not_enough_memory;
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
      cond_var_wait(entry->cond, cache->mutex);
    }
    entry->waiters--;
    if (!entry->evicted) {
      break; /* Entry survived; we own it */
    }
    /* Entry was cleaned up while we waited: loop back to re-check the map
     * before creating a new placeholder, since another waiter may have
     * already done so. */
    bool should_free = (entry->waiters == 0);
    if (should_free) entry_free(cache, entry);
  }

  /* We now own the set slot */
  entry->set_in_progress = true;

  /* If the entry is currently LIVE (in the LRU list), remove it before
   * releasing the mutex.  evict_lru() only selects from the LRU list, so an
   * entry that is not in the list cannot be chosen as a victim.  Without this
   * removal a concurrent make_room() could evict the entry while the remote
   * setter is executing; any getter blocked on set_in_progress would then wake
   * to find entry->evicted==true and return ccol_key_not_found even though the
   * setter succeeds and re-inserts the value.  entry_store_value() handles the
   * !in_lru path (make_room + lru_add_to_front) correctly on success; on
   * failure or OOM we restore the entry to the LRU manually below. */
  bool removed_from_lru = (!created_new && entry->in_lru);
  if (removed_from_lru) {
    lru_remove(entry);
    cache->size--;
  }

  /* Hold a waiter reference so the entry is not freed if evicted while we
   * are blocked in the remote call. */
  entry->waiters++;
  mutex_unlock(cache->mutex);

  bool remote_ok = true;
  if (cache->remote_setter) {
    remote_ok = cache->remote_setter(key_pair, val_pair);
  }

  mutex_lock(cache->mutex);
  entry->waiters--;
  ccol_retval_t retval = remote_ok ? ccol_success : ccol_unexpected_failure;

  /* entry->evicted is always false here: brand-new placeholders are never in
   * the LRU list and cannot be selected by evict_lru; existing LIVE entries
   * were explicitly removed from the LRU before the mutex was released, so
   * they also cannot be evicted while the remote call executes. */
  if (remote_ok) {
    ccol_retval_t store_r = entry_store_value(cache, entry, val_pair);
    if (store_r != ccol_success) {
      /* OOM: if we created the entry, remove it from the map */
      if (created_new) {
        cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
        chmap_delete_elem(cache->map, &kp);
        entry->evicted = true;
      } else if (removed_from_lru && entry->value) {
        /* Restore old-valued entry to LRU so it remains accessible.
         * make_room first: concurrent inserts may have filled the cache
         * while the mutex was released for the remote call. */
        make_room(cache);
        lru_add_to_front(cache, entry);
        cache->size++;
      }
      entry->set_in_progress = false;
      cond_var_broadcast(entry->cond);
      bool should_free = (entry->evicted && entry->waiters == 0);
      mutex_unlock(cache->mutex);
      if (should_free) entry_free(cache, entry);
      return store_r;
    }
  } else {
    /* Remote setter failed */
    if (created_new) {
      /* Placeholder we created has no value: clean it up */
      cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
      chmap_delete_elem(cache->map, &kp);
      entry->evicted = true;
    } else if (removed_from_lru) {
      /* Existing LIVE entry was removed from LRU: restore it with the old value
       * so the cache remains consistent and the entry stays accessible.
       * make_room first: concurrent inserts may have filled the cache
       * while the mutex was released for the remote call. */
      make_room(cache);
      lru_add_to_front(cache, entry);
      cache->size++;
    }
    /* For an existing entry already in LRU (not removed), leave it intact */
  }

  entry->set_in_progress = false;
  cond_var_broadcast(entry->cond);

  bool should_free = (entry->evicted && entry->waiters == 0);
  mutex_unlock(cache->mutex);
  if (should_free) entry_free(cache, entry);

  return retval;
}

/* ========================================================================== */
/*                         __clrucache_get_into                               */
/* ========================================================================== */

ccol_retval_t __clrucache_get_into(clru_cache cache, const cmap_pair *key_pair,
                                   void *buf, size_t buf_size) {
  if (!cache || !key_pair || !key_pair->ptr || !key_pair->size || !buf ||
      buf_size == 0)
    return ccol_invalid_args;

  mutex_lock(cache->mutex);

  clru_entry *entry = map_lookup(cache, key_pair);

  if (!entry) {
    if (!cache->remote_getter) {
      mutex_unlock(cache->mutex);
      return ccol_key_not_found;
    }

    entry = entry_alloc(cache);
    if (!entry) {
      mutex_unlock(cache->mutex);
      return ccol_not_enough_memory;
    }

    entry->key = _mem_alloc(cache->m_procs, key_pair->size);
    if (!entry->key) {
      entry_free(cache, entry);
      mutex_unlock(cache->mutex);
      return ccol_not_enough_memory;
    }
    mem_cpy(entry->key, key_pair->ptr, key_pair->size);
    entry->key_size = key_pair->size;
    entry->fetch_in_progress = true;

    ccol_retval_t ins = map_upsert(cache, key_pair, entry);
    if (ins != ccol_success) {
      entry_free(cache, entry);
      mutex_unlock(cache->mutex);
      return ins;
    }

    mutex_unlock(cache->mutex);

    cmap_pair fetched = {};
    bool fetch_ok = cache->remote_getter(key_pair, &fetched);

    mutex_lock(cache->mutex);
    entry->fetch_in_progress = false;

    if (fetch_ok && fetched.ptr && fetched.size > 0) {
      /* Reject a value that is larger than the destination buffer; caching it
       * would make every future __clrucache_get_into call for this key return
       * an error as well, so treat this as a fetch failure entirely. */
      if (fetched.size > buf_size) {
        _mem_free(cache->m_procs, fetched.ptr);
        cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
        chmap_delete_elem(cache->map, &kp);
        entry->evicted = true;
        cond_var_broadcast(entry->cond);
        bool should_free = (entry->waiters == 0);
        mutex_unlock(cache->mutex);
        if (should_free) entry_free(cache, entry);
        return ccol_unexpected_failure;
      }

      make_room(cache);
      entry->value = fetched.ptr;
      entry->value_size = fetched.size;
      lru_add_to_front(cache, entry);
      cache->size++;

      mem_cpy(buf, fetched.ptr, fetched.size);

      cond_var_broadcast(entry->cond);
      mutex_unlock(cache->mutex);
      return ccol_success;
    } else {
      if (fetched.ptr) _mem_free(cache->m_procs, fetched.ptr);
      cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
      chmap_delete_elem(cache->map, &kp);
      entry->evicted = true;
      cond_var_broadcast(entry->cond);
      bool should_free = (entry->waiters == 0);
      mutex_unlock(cache->mutex);
      if (should_free) entry_free(cache, entry);
      return ccol_key_not_found;
    }
  }

  if (entry->fetch_in_progress || entry->set_in_progress) {
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      cond_var_wait(entry->cond, cache->mutex);
    }
    entry->waiters--;

    if (entry->evicted || !entry->value) {
      bool should_free = (entry->evicted && entry->waiters == 0);
      mutex_unlock(cache->mutex);
      if (should_free) entry_free(cache, entry);
      return ccol_key_not_found;
    }
  }

  assert(!entry->evicted && entry->value != NULL && entry->in_lru);
  lru_move_to_front(cache, entry);

  if (entry->value_size > buf_size) {
    mutex_unlock(cache->mutex);
    return ccol_unexpected_failure;
  }
  mem_cpy(buf, entry->value, entry->value_size);

  mutex_unlock(cache->mutex);
  return ccol_success;
}

/* ========================================================================== */
/*                         SIZE / CAPACITY QUERIES                            */
/* ========================================================================== */

size_t clrucache_size(clru_cache cache) {
  if (!cache) return 0;
  mutex_lock(cache->mutex);
  size_t s = cache->size;
  mutex_unlock(cache->mutex);
  return s;
}

size_t clrucache_capacity(clru_cache cache) {
  return cache ? cache->capacity : 0;
}
