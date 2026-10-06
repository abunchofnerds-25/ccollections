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
#include <stdatomic.h>
#include <string.h>
#ifdef RUNNING_UNIT_TESTS
#include <unistd.h> /* usleep(); not a pthread/sem primitive, needs no
                        common.h wrapper */
#endif

/* ========================================================================== */
/*                         INTERNAL STRUCTURES                                */
/* ========================================================================== */

/*
 * A single cache entry. A key belongs to exactly one segment of the cache.
 * The entry of that key is in two data structures of that segment at the same
 * time:
 *
 *   1. The hash map (the map of the segment): key -> clru_entry*
 *      The map stores a COPY of the pointer (8 bytes). The pointer leads to
 *      the mutable entry. That entry holds the condition variable and the
 *      value.
 *
 *   2. The LRU doubly-linked list. The entry is in this list only when it is
 *      LIVE, that is, when it has a value. Two sentinel nodes bound the list.
 *      The segment embeds both of them (lru_head is the MRU end, lru_tail is
 *      the LRU end). Each segment therefore has its own eviction order.
 *
 * Entry lifecycle:
 *   PLACEHOLDER  in map, NOT in LRU, value==NULL, fetch_in_progress or
 *                set_in_progress is true. The LRU mechanism never evicts it.
 *   LIVE         in map, IN LRU, value!=NULL. The cache can evict it. This
 *                holds while a remote setter runs for the key as well: the
 *                entry keeps its place in the eviction order and in the size
 *                of the segment for the whole call, exactly as if no set were
 *                in flight. evict_lru() treats such an entry as a victim like
 *                any other. It reports the old value to the eviction callback
 *                and turns the entry back into a PLACEHOLDER, which the set
 *                then either fills (success) or removes (failure).
 *   DEAD         NOT in map, NOT in LRU, evicted==true. Only the threads that
 *                hold a waiter reference keep it alive (waiters > 0). The
 *                thread that decrements waiters to 0 frees it, and that
 *                thread holds the mutex of the owning segment.
 *
 * Locking discipline:
 *   The mutex of a segment protects ALL the fields of that segment. It also
 *   protects all the fields of every entry in that segment. The condition
 *   variable of each entry shares that same mutex, which is what cond_wait
 *   needs. A key maps to one segment. Every step of an operation on that key
 *   stays inside that segment. This is why no operation holds two of these
 *   mutexes, and why there is no lock order to get wrong.
 *
 *   The slow operations are the remote getter and the remote setter. They
 *   always run OUTSIDE the mutex. A thread that unlocks the mutex while it
 *   holds a pointer to an entry MUST increment entry->waiters first. It must
 *   decrement entry->waiters when it locks the mutex again. Without this,
 *   another thread frees the entry while the first thread still uses it.
 *
 *   That rule has no exception, and the three remote-call windows of this
 *   file all obey it: the two miss paths that run a remote getter, and the
 *   set path that runs a remote setter. A PLACEHOLDER happens to be
 *   unreachable by eviction today, because evict_lru() picks its victim from
 *   the LRU list and a placeholder is not in that list. That is a property of
 *   the eviction policy and not of the reference rule. A window that leans on
 *   it holds a bare pointer whose safety no counter records, and it becomes a
 *   use-after-free the moment some future change lets a placeholder be
 *   reclaimed. The reference costs one increment and one decrement under a
 *   mutex that the thread already holds.
 */
typedef struct clru_entry {
  void *key;
  size_t key_size;

  void *value; /* NULL while fetch/set is in progress */
  size_t value_size;

  ccol_cond_var_t cond; /* shared with the owning segment's mutex */

  bool fetch_in_progress; /* a remote getter is running for this key */
  bool set_in_progress;   /* a remote/async setter is running for this key */
  bool evicted;           /* removed from map+LRU; kept alive by waiters */

  /* Only the miss path of __clrucache_get_into() sets this. It sets this when
   * the size of the value that a remote getter fetched does not match the
   * buf_size of the caller that asked. It sets it immediately before it tears
   * that entry down as a failed fetch. A live entry never has this true. A
   * coalesced waiter that wakes up and finds entry->value still NULL reads
   * this field. It then reports the same ccol_unexpected_failure that the
   * thread which ran the fetch got. Without this field, that waiter reports
   * the generic ccol_key_not_found that every other fetch failure produces.
   * Without this field, only the one thread that runs the remote getter
   * learns that the real problem is a value of the wrong size. That
   * contradicts the coalescing contract of this module, which says that "all
   * others ... receive the same result" (see the file-level doc comment of
   * clrucache.h). */
  bool fetch_size_mismatch;

  /* How many threads hold a reference to this entry and are not the one that
   * owns its lifecycle. Two kinds of thread are counted. The first is a
   * thread blocked on cond. The second is a thread that released the mutex of
   * the segment to run a remote getter or a remote setter for this entry, and
   * that will touch the entry again when it takes that mutex back.
   *
   * The count is what keeps entry_free() away from an entry that another
   * thread can still reach. A thread that drops the count to zero on an entry
   * whose evicted flag is set is the thread that frees it. The mutex of the
   * owning segment protects this field. */
  int waiters;

  struct clru_entry *prev; /* LRU list links (NULL when not in LRU) */
  struct clru_entry *next;
  bool in_lru;

  /* Only the two miss paths set this, immediately before they tear down an
   * entry whose remote getter failed (a size mismatch included). A live
   * entry never has this true. It is what separates the two ways a coalesced
   * waiter can wake up and find no value. When the operation it waited on
   * was a FETCH, the remote getter already answered for the key, and every
   * waiter reports that same answer. When the operation was a SET that
   * failed, nobody asked the remote getter anything. That waiter then drops
   * its reference and runs its lookup again, so an absent key takes the
   * ordinary miss path, which runs the remote getter. A get that ends in
   * "not found" without a getter call would match no serial order of the
   * get and the failed set.
   *
   * The field sits beside in_lru, in the tail padding of the struct, so it
   * adds no size and moves no other field. The segment embeds two sentinel
   * entries, so a larger entry would also move the fields of the segment
   * that the hit path reads. */
  bool fetch_failed;
} clru_entry;

/* clru_cache is an opaque value handle. The top 32 bits are the slot index
 * and the bottom 32 bits are the generation. See the doc comment on the
 * typedef in include/clrucache.h. The library resolves the handle through
 * this table before it touches the struct clrucache* behind it. This is what
 * lets __clrucache_destroy report a second destroy as a ccol_fatal_err
 * instead of a use-after-free or a double-free. It detects a concurrent
 * second destroy, which races another destroy on the same still-live handle.
 * It also detects a sequential one, where the handle is stale after an
 * earlier destroy already completed. The table marks a slot not-in-use at the
 * moment it releases that slot. It also increments the generation of the slot
 * on every reuse. A stale handle can therefore never alias a later, unrelated
 * cache that holds the same slot index. This table mirrors
 * chttpcli_slot_table, chttpsvr_slot_table and ccol_event_loop_slot_table
 * exactly. See the copy of this comment in src/chttpclient.c for the full
 * design rationale.
 *
 * The lock of the table is a read-write lock, not a plain mutex.
 * _clrucache_resolve only reads. It bounds-checks idx, compares the
 * generation and reads slot->ptr. It runs on every single get, set and delete
 * call on a clru_cache. _clrucache_handle_slot_acquire and
 * __clrucache_destroy are the only writers. Each of them runs one time in the
 * whole life of a cache, and not one time per operation. This mirrors the
 * identical slot-table rwlocks in cthreadcomm.c, cthreadpool.c and
 * chttpclient.c. Like chttpcli_slot_table in chttpclient.c, this table has no
 * pthread_atfork() protection of its own, because clrucache.c registers none.
 * cthreadcomm.c and cthreadpool.c have a subtle write lock that a tracked
 * thread ID drives, so that they can initialize it again in the child. That
 * subtlety does not apply here. This table has no fork()-time lock state to
 * keep. */
typedef struct {
  struct clrucache *ptr; /* NULL when slot is free */
  uint32_t generation;   /* A fresh value on every acquire. It is monotonic
                             for each slot index. It starts at 0 before the
                             first use, and becomes 1 at the first acquire */
  bool in_use;
} clrucache_slot_t;

static struct {
  ccol_rw_lock_t rwlock;
  ccol_once_flag_t once;
  cvec slots;        /* A cvec of clrucache_slot_t. It grows only by
                         push_back. An index is permanent after its
                         allocation */
  cvec free_indices; /* A cvec of uint32_t. It is a LIFO free list that gives
                         O(1) reuse */
  /* The number of slot indices that a push onto free_indices could not take,
     because that push could not allocate. Such a slot is fully released. Its
     ptr is NULL and its in_use is false, but nothing names it. Without this
     counter, that slot stays unreachable for the rest of the life of the
     process, and every later create grows the table by one more slot. An
     acquire that finds free_indices empty and this counter above zero
     recovers one slot with a scan of slots instead. The scan is unambiguous
     because it runs only when the free list is empty. A released slot is
     either on that list or lost, and never both. The code reads and writes
     this field only under the write lock. */
  size_t lost_indices;
  /* True when the process-exit destructor finds a cache that is still live
     and leaves this table alone. The destroy that then releases the last slot
     does the release that the destructor could not do. Without this field, a
     cache that a destructor linked earlier than this one destroys leaves the
     table and the pin index allocated for the rest of the process. A leak
     checker that treats still-reachable memory as an error reports that. The
     code reads and writes this field only under the write lock. */
  bool release_deferred;
} clrucache_slot_table = {0};

/* The definitions are with the process-exit teardown below. The declarations
   are here, because the final locked section of __clrucache_destroy does the
   release that the destructor defers. */
static bool _clrucache_any_slot_live_locked(void);
static void _clrucache_release_slot_table_locked(void);
static void _clrucache_release_slot_table_if_deferred_locked(void);

static void _clrucache_slot_table_init_globals(void) {
  if (ccol_rw_lock_init(clrucache_slot_table.rwlock) != 0)
    ccol_fatal_err("clru_cache slot table: failed to initialize rwlock");
  clrucache_slot_table.slots = cvector_create(sizeof(clrucache_slot_t), NULL);
  if (!clrucache_slot_table.slots)
    ccol_fatal_err("clru_cache slot table: failed to allocate slots vector");
  clrucache_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!clrucache_slot_table.free_indices)
    ccol_fatal_err(
        "clru_cache slot table: failed to allocate free-index vector");
}

/* One segment of a cache. Each segment has its own lock.
 *
 * The library divides a cache into several of these segments. A key belongs
 * to exactly one segment, and a hash of the key selects that segment (see
 * shard_for). Each segment carries its own map, its own eviction order and
 * its own lock. Operations on keys that land in different segments therefore
 * run at the same time. They do not queue behind one another.
 *
 * Every segment holds a copy of the configured callbacks. A segment does not
 * reach them through a back-pointer. This keeps the operations of each
 * segment inside memory that the segment already owns. The allocator is the
 * one thing that a segment borrows. The router holds the single copy of the
 * allocator of the caller. A copy in each segment needs a free by the same
 * allocator that the copy names. */
typedef struct clru_shard {
  /* key -> clru_entry*. The value type is a pointer. The map therefore
     selects its own backend from the key type. It uses open addressing for an
     integral key or a pointer key. It uses separate chaining for a string, a
     struct, or a long double. */
  chmap map;

  /* Sentinel nodes for the doubly-linked LRU list.
   * lru_head.next is the MRU entry. lru_tail.prev is the LRU entry.
   * The struct embeds the sentinels. They are not on the heap. Nothing uses
   * their cond, key and value fields. */
  clru_entry lru_head;
  clru_entry lru_tail;

  size_t capacity; /* this segment's share of the configured capacity */
  size_t size;     /* number of LIVE entries in this segment */

  ccol_mutex_t mutex; /* protects everything in this segment */

  clru_remote_getter_t remote_getter;
  clru_remote_setter_t remote_setter;
  clru_eviction_cb_t eviction_cb;

  ccol_memmgmt_procs_t *m_procs;
} clru_shard;

struct clrucache {
  clru_shard **shards;
  size_t shard_count;

  /* The declared key type, and its width when the type has a fixed width. The
   * width is 0 when the type has no fixed width. These two fields keep the
   * choice of segment in agreement with the maps inside the segments about
   * which keys are the same key. They also let the library reject a
   * hand-built cmap_pair whose size does not agree with the type. The library
   * rejects it before anything reads through it. */
  ccol_data_type key_type;
  size_t key_fixed_width;

  /* The capacity that the caller asked for. It is exactly the sum of the own
   * capacities of the segments. */
  size_t capacity;

  ccol_memmgmt_procs_t *m_procs;

  /* The handle of this cache. An unpin uses it to find the slot that
   * holds its pin, so the caller does not carry a handle. The code writes
   * this field one time, before it publishes the handle, and never again. */
  clru_cache self_handle;

  /* The copy of the allocator of the caller. m_procs points here when the
   * caller gave an allocator, and is NULL otherwise. The cache keeps no
   * pointer to the struct of the caller, so that struct may go out of scope
   * as soon as clrucache_create_full returns. It sits after every field
   * that a lookup reads, so that it moves none of them. */
  ccol_memmgmt_procs_t m_procs_copy;
};

/* ========================================================================== */
/*                    CLRU_CACHE HANDLE RESOLVE / UNPIN                       */
/* ========================================================================== */

/* Resolves h and pins the result against a concurrent destroy. It returns
 * NULL when h is 0 or garbage. It also returns NULL when h names a slot that
 * is free now, or a slot that the table already reused, which gives the wrong
 * generation. On success the caller MUST call
 * _clrucache_resolve_unpin(result) exactly one time. It must call it as soon
 * as it finishes with the resolved struct clrucache*. */
/* The hot half of the slot table. It maps a handle to a pointer. It also
 * holds the pin that keeps a cache alive for the length of a call. It is
 * separate from the table itself, because a resolve runs on every public
 * call. A resolve must not write anything that another thread reads. The
 * recycle of a slot is cold, and it stays under the rwlock. */
static ccol_pintable clrucache_pintable;

static struct clrucache *_clrucache_resolve(clru_cache h) {
  /* There is no lock here and no shared write. Every public entry point of
   * this module runs this function. A write here to memory that another
   * thread reads therefore costs time on every single cache operation. */
  return (struct clrucache *)ccol_pintable_pin(&clrucache_pintable, h);
}

static void _clrucache_resolve_unpin(struct clrucache *raw) {
  /* This function releases the pin and touches nothing else. A lock of the
   * own mutex of this cache here makes every operation lock that mutex twice.
   * The second lock comes immediately after the unlock. That is how a convoy
   * keeps itself alive under concurrent callers. There is also no wakeup to
   * deliver. __clrucache_destroy polls the pin count. It does not sleep on a
   * condition variable and wait for this function.
   *
   * The read of raw->self_handle before the release is safe, because the pin
   * is still held at that point. After the release, another thread can free
   * the object at any instant. Nothing may touch raw after this call. */
  ccol_pintable_unpin(&clrucache_pintable, raw->self_handle);
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only. This makes the very next push onto the free list behave exactly
 * as an allocation failure does. It then disarms itself. Without it, the
 * recovery of a lost index below needs a real out-of-memory condition at one
 * specific cvector_push_back. An ordinary test run cannot reach that
 * condition. */
static _Atomic bool g_clrucache_fail_next_free_index_push = false;

void _clrucache_force_next_free_index_push_failure_for_tests(void) {
  atomic_store(&g_clrucache_fail_next_free_index_push, true);
}
#endif

/* Hands idx back for reuse. The caller holds the write lock.
 *
 * This function records a failed push. It does not drop it in silence. The
 * slot that the index names is already fully released. A silent drop strands
 * that index for the life of the process, and it makes every later create
 * grow the table again. See clrucache_slot_table.lost_indices. */
static void _clrucache_free_index_release_locked(uint32_t idx) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_clrucache_fail_next_free_index_push, false)) {
    clrucache_slot_table.lost_indices++;
    return;
  }
#endif
  if (cvector_push_back(clrucache_slot_table.free_indices, &idx) !=
      ccol_success)
    clrucache_slot_table.lost_indices++;
}

/* Recovers one slot index that a failed push onto the free list stranded (see
 * clrucache_slot_table.lost_indices). It scans for a slot that is released,
 * but that nothing names. The caller holds the write lock, and the caller
 * already found free_indices empty. That is what makes "ptr is NULL and
 * in_use is false" mean lost, and not only free. A slot that a release put on
 * that list successfully is on the list. A slot whose destroy is still in
 * progress keeps a ptr that is not NULL until the very end of that destroy.
 * The destroy clears in_use as its first step, and it clears ptr only in its
 * final locked step. The two tests together therefore skip exactly that
 * window. This function only returns an index that the table already holds.
 * It can never take the table past CCOL_PIN_MAX_SLOTS.
 *
 * This function is out of line. The code reaches it only when the counter is
 * above zero, so an ordinary acquire pays one comparison for it. The function
 * sets the counter to zero when the scan finds nothing. Without that, a
 * counter that outlives its slot makes every future acquire scan the
 * table. */
static __attribute__((noinline)) bool _clrucache_reclaim_lost_index_locked(
    uint32_t *out_idx) {
  size_t slot_count = cvector_elem_count(clrucache_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    clrucache_slot_t *slot =
        (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, i);
    if (slot->ptr == NULL && !slot->in_use) {
      *out_idx = (uint32_t)i;
      clrucache_slot_table.lost_indices--;
      return true;
    }
  }
  clrucache_slot_table.lost_indices = 0;
  return false;
}

/* Allocates a fresh slot for cache, or reuses a slot that a release freed. It
 * returns the resulting handle, or 0 when it cannot allocate memory.
 * clrucache_create_full calls it one time, after the object is fully
 * constructed in every other respect. */
static clru_cache _clrucache_handle_slot_acquire(struct clrucache *cache) {
  ccol_call_once(clrucache_slot_table.once, _clrucache_slot_table_init_globals);
  ccol_rw_lock_wrlock(clrucache_slot_table.rwlock);
  uint32_t idx;
  clrucache_slot_t *slot;
  if (cvector_elem_count(clrucache_slot_table.free_indices) > 0) {
    cvector_pop_back(clrucache_slot_table.free_indices, &idx);
    slot = (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
  } else if (clrucache_slot_table.lost_indices > 0 &&
             _clrucache_reclaim_lost_index_locked(&idx)) {
    slot = (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
  } else {
    /* The pin table cannot hold a slot whose index is past its own limit.
     * Nothing can publish such a slot. This code therefore refuses it here.
     * It does not claim the slot and then roll the claim back. A roll-back
     * puts an index that no later publish can use onto the free list, and
     * every acquire pops from that list. This is an ordinary failure, which
     * is how a caller must already treat a table that cannot grow. */
    if (cvector_elem_count(clrucache_slot_table.slots) >= CCOL_PIN_MAX_SLOTS) {
      ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
      return 0;
    }
    clrucache_slot_t fresh = {0};
    if (cvector_push_back(clrucache_slot_table.slots, &fresh) != ccol_success) {
      ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
      return 0; /* an ordinary failure to allocate memory; not fatal */
    }
    idx = (uint32_t)cvector_elem_count(clrucache_slot_table.slots) - 1;
    slot = (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
  }
  slot->generation++;
  /* Skip the one generation value that collides with the reserved sentinel
   * for an "invalid handle" (0). That value comes up after about 2^32 reuses
   * of this exact slot index. See the identical guard in
   * chttpcli_handle_slot_acquire for the full rationale. */
  if (slot->generation == 0) slot->generation++;
  clru_cache h = ((clru_cache)idx << 32) | (clru_cache)slot->generation;

  /* The code writes this before it publishes the handle. A resolver that
   * finds this cache therefore also finds the handle that its own unpin
   * needs. */
  cache->self_handle = h;

  /* A publish can allocate memory. A failure leaves a handle that no call can
   * resolve. The slot therefore goes back on the free list. */
  if (!ccol_pintable_publish(&clrucache_pintable, idx, slot->generation,
                             cache)) {
    /* The code clears this as the index goes back on the free list. This
     * struct therefore never carries a handle that names a slot which now
     * belongs to another object. An unpin with such a handle charges the pin
     * count of that other object for a pin that nobody took. The caller frees
     * this object and does not resolve it again, so no path reads this field
     * after this point. The clear is what keeps that fact local to this
     * function. Without it, every future user of the failure path must know
     * the rule. */
    cache->self_handle = 0;
    _clrucache_free_index_release_locked(idx);
    ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
    return 0;
  }

  slot->ptr = cache;
  slot->in_use = true;
  ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
  return h;
}

/* ========================================================================== */
/*                         LRU LIST HELPERS                                   */
/* ========================================================================== */

static void lru_add_to_front(clru_shard *cache, clru_entry *e) {
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

static void lru_move_to_front(clru_shard *cache, clru_entry *e) {
  lru_remove(e);
  lru_add_to_front(cache, e);
}

/* ========================================================================== */
/*                         ENTRY HELPERS                                      */
/* ========================================================================== */

static clru_entry *entry_alloc(clru_shard *cache) {
  clru_entry *e =
      (clru_entry *)_ccol_mem_calloc(cache->m_procs, 1, sizeof(clru_entry));
  if (!e) return NULL;
  if (ccol_cond_var_init(e->cond) != 0) {
    /* Do NOT route this through entry_free(). entry_free() calls
     * ccol_cond_var_destroy() on a condition variable that no call
     * initialized successfully, and that is undefined behavior. e->key and
     * e->value are still NULL after the fresh calloc. A plain free is all
     * that this path needs. */
    _ccol_mem_free(cache->m_procs, e);
    return NULL;
  }
  return e;
}

/* Free the key, the value, the condition variable, and the entry itself.
 * Precondition: no other thread can reach e, and no other thread references e
 * now. Exactly two situations give that. (1) No call published e into
 * cache->map, because an allocation or an insert failed before or during
 * map_upsert or chmap_insert_elem. No other thread can find e at all.
 * (2) e->evicted == true and e->waiters == 0. A call published e. A later
 * call removed it from both the map and the LRU list, and every thread that
 * waited
 * on it woke up and released its reference. The caller can hold the mutex of
 * the segment, or not hold it. Correctness depends only on one of the two
 * situations above. It does not depend on the mutex. */
static void entry_free(clru_shard *cache, clru_entry *e) {
  _ccol_mem_free(cache->m_procs, e->key);
  _ccol_mem_free(cache->m_procs, e->value);
  ccol_cond_var_destroy(e->cond);
  _ccol_mem_free(cache->m_procs, e);
}

/* ========================================================================== */
/*                         EVICTION (called under mutex)                      */
/* ========================================================================== */

/* Evict the least recently used LIVE entry. This function calls the eviction
 * callback before it removes the entry, and it holds the mutex during that
 * call. It returns true when it evicts an entry, and false when the list is
 * empty.
 *
 * The victim is always the true least recently used entry. That includes an
 * entry whose key has a remote setter in flight. Such a set has not happened
 * yet: it takes effect when its remote call returns, and only if that call
 * succeeds. Until then the key holds its old value in its old place, and an
 * insert that needs room evicts it exactly as it evicts any other key. To
 * spare it would be right only if the set later succeeds. If the set fails,
 * sparing it has cost an innocent key its place, and no later step can
 * give that key back without undoing an eviction that a caller may already
 * have observed.
 *
 * An evicted entry with a set in flight stays in the map. It becomes a
 * placeholder again: no value, out of the eviction order and out of the size
 * of the segment. The set that owns it then finishes the job. On success it
 * stores the new value and inserts the key at the most recently used end,
 * which is exactly what a set of an absent key does. On failure it removes
 * the placeholder, and the key stays evicted. Every thread that waits on the
 * key waits on that set, so none of them can observe the placeholder. */
static bool evict_lru(clru_shard *cache) {
  clru_entry *victim = cache->lru_tail.prev;
  if (victim == &cache->lru_head) return false; /* nothing to evict */

  if (cache->eviction_cb && victim->value) {
    cmap_pair kp = {.ptr = victim->key, .size = victim->key_size};
    cmap_pair vp = {.ptr = victim->value, .size = victim->value_size};
    cache->eviction_cb(&kp, &vp);
  }

  lru_remove(victim);
  cache->size--;

  if (victim->set_in_progress) {
    _ccol_mem_free(cache->m_procs, victim->value);
    victim->value = NULL;
    victim->value_size = 0;
    return true;
  }

  cmap_pair kp = {.ptr = victim->key, .size = victim->key_size};
  chmap_delete_elem(cache->map, &kp);

  victim->evicted = true;
  ccol_cond_var_broadcast(victim->cond);

  if (victim->waiters == 0) {
    entry_free(cache, victim);
  }
  return true;
}

/* Make sure that cache->size < cache->capacity. This function evicts LRU
 * entries to get there. Every entry that cache->size counts is in the LRU
 * list and can be evicted, so the loop always reaches that bound. */
static void make_room(clru_shard *cache) {
  while (cache->size >= cache->capacity) {
    if (!evict_lru(cache)) break;
  }
}

/* ========================================================================== */
/*                         CHMAP LOOKUP HELPER                                */
/* ========================================================================== */

/* Look up the entry pointer that the internal chmap stores.
 * It returns NULL when the map does not hold the key. The caller must hold
 * cache->mutex.
 *
 * The SSO storage of a chmap_entry is aligned naturally. This memcpy is
 * therefore defense-in-depth, and not a live requirement for alignment. It
 * stays here for consistency with cjson, cyaml, cthreadcomm and chttpclient.
 * Those modules use the same pattern for their own pointer storage inside a
 * chmap. */
static clru_entry *map_lookup(clru_shard *cache, const cmap_pair *key_pair) {
  const cmap_pair *found = NULL;
  ccol_retval_t r = chmap_get_elem_ref(cache->map, key_pair, &found);
  if (r != ccol_success) return NULL;
  clru_entry *e;
  memcpy(&e, found->ptr, sizeof(e));
  return e;
}

/* Insert or update the entry pointer in the internal chmap. */
static ccol_retval_t map_upsert(clru_shard *cache, const cmap_pair *key_pair,
                                clru_entry *entry) {
  cmap_pair vp = {.ptr = &entry, .size = sizeof(entry)};
  return chmap_insert_elem(cache->map, key_pair, &vp);
}

/* ========================================================================== */
/*                         clrucache_create_full                              */
/* ========================================================================== */

/* The number of segments that a cache of this capacity gets. Each segment has
 * its own lock.
 *
 * More segments give less contention. But they also give a coarser picture of
 * the eviction order, because each segment evicts from its own order. The
 * capacity bounds the count, so that no segment starts with nothing to hold.
 * A ceiling also bounds the count. This is why a very large cache does not
 * pay for segments that no realistic number of threads uses at the same
 * time. */
#define CLRU_MAX_SHARDS 16u

/* The number of entries that a segment must be worth before the library
 * divides the cache at all.
 *
 * A division costs real capacity, and not only capacity in principle. A hash
 * spreads the keys. At any instant some segments therefore hold more than
 * their share and other segments hold less. A cache that is full to exactly
 * its capacity then evicts from the full segments while the other segments
 * still have room. The smaller each segment is, the worse that unevenness
 * gets, because the spread is relative to the size of the segment.
 * Measurements with randomly distributed keys show that segments of four
 * entries keep about three quarters of a full cache. Segments of this size
 * keep the loss down to the few percent that the public documentation states
 * as the cost of a division.
 *
 * This value is here for two reasons. A cache that is small enough for the
 * loss to matter stays undivided, and it keeps one exact, global eviction
 * order. A cache that is large enough for lock contention to be the real
 * problem gets exactly as many segments as it gets without this value. */
#define CLRU_MIN_ENTRIES_PER_SHARD 64u

static size_t shard_count_for(size_t capacity) {
  size_t n = capacity / CLRU_MIN_ENTRIES_PER_SHARD;
  if (n > CLRU_MAX_SHARDS) n = CLRU_MAX_SHARDS;
  if (n == 0) n = 1;
  return n;
}

/* Spreads the whole value of a hash over its low bits, before the code
 * reduces that hash to a segment index.
 *
 * The reduction is a modulo by the segment count, and a modulo reads only the
 * low bits of what it gets. The hash of the map is built to be read from its
 * top bits, which is where the map takes an index from, and the map gives no
 * promise about the spread of its low bits. A modulo that reads a hash whose
 * low bits depend only on the low bits of the key reads only the low bits of
 * the key, and a key set that holds those bits constant is ordinary, and not
 * exotic. Every pointer from malloc is aligned to at least 16 bytes. An
 * identifier that a block size or a page size scales has as many trailing
 * zeroes as that size. Such a set would land entirely on one segment. That
 * serialises the whole cache on the lock of that segment. It also caps the
 * cache at the share of the capacity that the segment holds. The cache then
 * evicts everything past that share while the other segments stay empty.
 *
 * The finalizer below is what rules that out whatever the hash of the map
 * is. It stays a concern of the cache, and not of the map, because the map
 * reads the same hash from its own high bits. A poor spread costs a hash
 * table some probes, but a segment count partitions a fixed capacity. The
 * finalizer is the standard 64-bit avalanche, which makes every output bit
 * depend on every input bit. It sits ahead of a mutex lock and a map lookup,
 * so its few instructions are not the cost that matters here. */
static inline size_t clru_reduce(uint64_t h, size_t shard_count) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return (size_t)(h % (uint64_t)shard_count);
}

/* Gives the segment that a key belongs to.
 *
 * Each segment has its own map. Two keys that a map treats as one key must
 * therefore land on the same segment. Without that, the cache holds two
 * entries for one key, and a later get cannot find what a set stored. Byte
 * identity is not the notion of key identity that the map uses for every type
 * it accepts. -0.0 and 0.0 are one key for float and for double. The map
 * hashes and compares a long double by value. It does this because the type
 * carries padding on some ABIs. Those padding bytes are often uninitialised,
 * and any read of them is a hazard. A route through the key hash of the
 * map is therefore what keeps a cache with segments equivalent to a cache
 * without segments. It also keeps this path from a read of bytes that the
 * caller never wrote.
 *
 * This function returns NULL when key_pair->size does not agree with the own
 * width of a fixed-width key type. That is what stops a read past the end of
 * a cmap_pair of the wrong size that a caller built. This check runs ahead of
 * the segment count. One malformed key therefore reports ccol_invalid_args
 * whether or not the cache is large enough to have segments. */
static clru_shard *shard_for(struct clrucache *router,
                             const cmap_pair *key_pair) {
  if (router->key_fixed_width != 0 &&
      key_pair->size != router->key_fixed_width) {
    return NULL;
  }
  if (router->shard_count == 1) return router->shards[0];
  uint64_t h =
      ccol_chmap_hash_key(key_pair->ptr, key_pair->size, router->key_type);
  return router->shards[clru_reduce(h, router->shard_count)];
}

/* Frees the allocation of the router. The copy of the allocator of the
 * caller lives inside the router, so the function reads the free procedure
 * out of it before the router goes away. */
static void router_free_self(struct clrucache *router) {
  ccol_memmgmt_procs_t *mp = router->m_procs;
  if (router->shards) _ccol_mem_free(mp, router->shards);
  if (mp) {
    ccol_free_t f = mp->free;
    f(router);
  } else {
    free(router);
  }
}

/* Builds one segment. Everything below the router belongs to a single
 * segment. This function is therefore the whole construction of a segment:
 * its own map, its own eviction order and its own lock. */
static clru_shard *shard_create(size_t capacity, ccol_data_type key_type,
                                clru_remote_getter_t getter,
                                clru_remote_setter_t setter,
                                clru_eviction_cb_t eviction_cb,
                                ccol_memmgmt_procs_t *mprocs, char **err) {
  clru_shard *cache = (clru_shard *)_ccol_mem_calloc(mprocs, 1, sizeof(*cache));
  if (!cache) {
    if (err) *err = CCOL_ERR_STR("failed to allocate cache segment");
    return NULL;
  }
  /* The segment borrows this. It does not copy it. The router owns the one
   * copy of the allocator of the caller, and the router lives longer than
   * every segment that it creates. */
  cache->m_procs = mprocs;

  char *map_err = NULL;
  cache->map =
      chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, key_type,
                        ccol_pointer, cache->m_procs, NULL, NULL, &map_err);
  if (!cache->map) {
    if (err)
      *err = map_err ? map_err : CCOL_ERR_STR("failed to create internal map");
    _ccol_mem_free(mprocs, cache);
    return NULL;
  }

  /* Initialise the ring of LRU sentinels. A sentinel has no condition
   * variable, and nothing ever waits on one. */
  cache->lru_head.next = &cache->lru_tail;
  cache->lru_head.prev = NULL;
  cache->lru_tail.prev = &cache->lru_head;
  cache->lru_tail.next = NULL;

  /* This code checks the return value. POSIX lets pthread_mutex_init fail for
   * a real reason, for example ENOMEM. A handle that carries a mutex which is
   * not fully initialized makes every later lock on that mutex undefined
   * behavior. This mirrors the ccol_cond_var_init check in entry_alloc, in
   * this same file. */
  if (ccol_mutex_init(cache->mutex) != 0) {
    if (err) *err = CCOL_ERR_STR("failed to initialize cache mutex");
    __chmap_destroy(cache->map);
    _ccol_mem_free(mprocs, cache);
    return NULL;
  }

  cache->capacity = capacity;
  cache->remote_getter = getter;
  cache->remote_setter = setter;
  cache->eviction_cb = eviction_cb;
  return cache;
}

/* Evicts everything that a segment still holds, and then frees the segment
 * itself. The eviction runs under the lock of the segment. The caller
 * supplies the eviction callback, and every other path that calls it holds
 * that lock too. */
static void shard_destroy(clru_shard *cache) {
  if (!cache) return;
  ccol_mutex_lock(cache->mutex);
  /* evict_lru() returns false exactly when the list is empty. No set is in
   * flight when a destroy reaches this point, because the pin count of the
   * handle drains every in-flight call first. Every entry that this loop
   * evicts therefore leaves the map as well. */
  while (evict_lru(cache)) {
  }
  ccol_mutex_unlock(cache->mutex);

  __chmap_destroy(cache->map);
  ccol_mutex_destroy(cache->mutex);
  _ccol_mem_free(cache->m_procs, cache);
}

static void router_destroy_shards(struct clrucache *router) {
  if (!router->shards) return;
  for (size_t i = 0; i < router->shard_count; i++) {
    shard_destroy(router->shards[i]);
    router->shards[i] = NULL;
  }
}

clru_cache clrucache_create_full(size_t capacity, ccol_data_type key_type,
                                 ccol_data_type val_type,
                                 clru_remote_getter_t getter,
                                 clru_remote_setter_t setter,
                                 clru_eviction_cb_t eviction_cb,
                                 ccol_memmgmt_procs_t *mprocs, char **err) {
  (void)val_type; /* val_type is not needed. The map stores pointers */

  if (capacity == 0) {
    if (err) *err = CCOL_ERR_STR("capacity must be > 0");
    return CLRU_CACHE_INVALID;
  }

  if (!ccol_verify_memmgmt_procs(mprocs, err)) return CLRU_CACHE_INVALID;

  struct clrucache *router =
      (struct clrucache *)(mprocs ? mprocs->calloc(1, sizeof(*router))
                                  : calloc(1, sizeof(*router)));
  if (!router) {
    if (err) *err = CCOL_ERR_STR("failed to allocate cache struct");
    return CLRU_CACHE_INVALID;
  }

  if (mprocs) {
    memcpy(&router->m_procs_copy, mprocs, sizeof(router->m_procs_copy));
    router->m_procs = &router->m_procs_copy;
  }

  router->capacity = capacity;
  router->key_type = key_type;
  router->key_fixed_width = ccol_fixed_width_data_type_size(key_type);
  router->shard_count = shard_count_for(capacity);
  router->shards = (clru_shard **)_ccol_mem_calloc(
      router->m_procs, router->shard_count, sizeof(*router->shards));
  if (!router->shards) {
    if (err) *err = CCOL_ERR_STR("failed to allocate cache segments");
    router_free_self(router);
    return CLRU_CACHE_INVALID;
  }

  /* The capacities of the segments add up to exactly the total that the
   * caller requested. The first `capacity % shard_count` segments carry one
   * extra entry each. Nothing is lost to rounding, and the caller still gets
   * the capacity that it asked for. */
  size_t base = capacity / router->shard_count;
  size_t extra = capacity % router->shard_count;
  for (size_t i = 0; i < router->shard_count; i++) {
    size_t cap_i = base + (i < extra ? 1 : 0);
    router->shards[i] = shard_create(cap_i, key_type, getter, setter,
                                     eviction_cb, router->m_procs, err);
    if (!router->shards[i]) {
      router_destroy_shards(router);
      router_free_self(router);
      return CLRU_CACHE_INVALID;
    }
  }

  /* The acquisition of a slot is the LITERAL LAST step. It runs after the
   * cache is fully constructed in every other respect. This mirrors the
   * constructors of chttpcli, chttpsvr and ccol_event_loop exactly. No caller
   * ever sees a handle until this function is about to return success.
   * ccol_event_loop and ctpool start threads of their own. This module does
   * not. A failure here therefore stops no thread. It only frees what the
   * function already constructed successfully. */
  clru_cache h = _clrucache_handle_slot_acquire(router);
  if (h == 0) {
    if (err) *err = CCOL_ERR_STR("failed to allocate clru_cache handle slot");
    router_destroy_shards(router);
    router_free_self(router);
    return CLRU_CACHE_INVALID;
  }

  return h;
}

/* ========================================================================== */
/*                         __clrucache_destroy                                */
/* ========================================================================== */

void __clrucache_destroy(clru_cache cache) {
  if (!cache) return;

  /* Resolve cache through the slot table. Mark the slot not-in-use in the
   * same critical section as the lookup. This is what makes a second destroy
   * call on the same handle value see a resolve failure. That second call can
   * be concurrent, or later and sequential. It never races the teardown of
   * this call. See the file-level comment of the slot table and the comment
   * on _clrucache_resolve for the full design. A stale handle that reaches
   * here, or one that a call already destroyed, is exactly the misuse that
   * this generation-tagged handle design catches. It is fatal. It is not a
   * silent use-after-free or double-free. */
  ccol_call_once(clrucache_slot_table.once, _clrucache_slot_table_init_globals);
  uint32_t idx = (uint32_t)(cache >> 32);
  uint32_t gen = (uint32_t)(cache & 0xFFFFFFFFu);
  ccol_rw_lock_wrlock(clrucache_slot_table.rwlock);
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
    ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
    ccol_fatal_err(
        "clrucache_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a clru_cache handle)");
  }
  slot->in_use = false; /* This blocks ALL future resolves for this handle
                            from this instant. It also blocks a second,
                            concurrent destroy attempt */
  /* Same step, same lock. From here the table grants no new pin. That is what
   * lets the count below reach zero and stay there. */
  ccol_pintable_retire(&clrucache_pintable, idx);
  ccol_rw_lock_unlock(clrucache_slot_table.rwlock);

  /* Wait for the pin count to reach 0 BEFORE any teardown logic runs at all.
   * Do not wait only before the free of memory. This mirrors the order in
   * chttpcli exactly. A wait first is safe here. It is not safe in ctpool.
   * The blocking waits of this module are the ccol_cond_var_wait loops that
   * coalesce a getter or a setter. The remote_getter or remote_setter call of
   * some other application thread completes for that key and releases them.
   * That mechanism is fully independent of what destroy does. No pinned or
   * blocked caller here depends on destroy-side logic to release its own
   * pin. */
  {
    long delay_ns = 1000;
    while (ccol_pintable_pins(&clrucache_pintable, idx) > 0) {
      struct timespec ts = {.tv_sec = 0, .tv_nsec = delay_ns};
      nanosleep(&ts, NULL);
      if (delay_ns < 1000000L) delay_ns *= 2;
    }
  }

  /* Evict every live entry that remains, and call the eviction callback for
   * each one. Then free every segment, and then the router itself. */
  router_destroy_shards(raw);
  router_free_self(raw);

  /* Release the slot last. Do it only after the teardown and the free of raw
   * are complete. This is what marks the handle as reusable: the increment of
   * the generation of the slot, and the push of the index back onto the free
   * list. No earlier step marks it. Fetch the slot again by idx. Do not reuse
   * `slot`. A concurrent clrucache_create_full can run its own
   * _clrucache_handle_slot_acquire in between. That call can allocate the
   * backing array of slots again through cvector_push_back. Any pointer into
   * that array from before this second lock is then invalid. idx itself is
   * stable. */
  ccol_rw_lock_wrlock(clrucache_slot_table.rwlock);
  clrucache_slot_t *slot2 =
      (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* This takes the generation of this slot past the
      value that the handle of the cache which this call freed carried. That
      stale handle can therefore never match the generation of a FUTURE
      acquire for this same index */
  _clrucache_free_index_release_locked(idx);
  /* The process-exit destructor already ran, and it found this cache live.
     The release that it could not do belongs to the call that frees the last
     slot. This call can be that one. */
  _clrucache_release_slot_table_if_deferred_locked();
  ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS
/* Resolves h to the struct clrucache* behind it, and does NOT pin it. This is
 * a bare lookup in the slot table. It is safe for tests, because the test
 * code that calls it runs synchronously on one thread. There is no concurrent
 * destroy to race. _clrucache_resolve needs a matching _unpin call, and this
 * function needs none, so a test has nothing to remember. A forgotten unpin
 * leaves a pin outstanding on that cache, and that hangs every future
 * clru_destroy call against it in silence.
 * It returns NULL under exactly the same conditions as _clrucache_resolve. */
struct clrucache *_clrucache_resolve_for_tests(clru_cache h) {
  ccol_call_once(clrucache_slot_table.once, _clrucache_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol_rw_lock_rdlock(clrucache_slot_table.rwlock);
  struct clrucache *raw = NULL;
  if (idx < cvector_elem_count(clrucache_slot_table.slots)) {
    clrucache_slot_t *slot =
        (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
  return raw;
}

/* Reads how many slots the handle table for a clru_cache holds now. This
 * count includes the slots that the table grew, and the slots that a release
 * freed but no acquire reused yet. It lets a test assert that a loop of
 * creates and destroys reuses freed slots. Without that, the table grows
 * without bound. */
/* The sum of the capacities of the segments, and the number of segments.
 * The division hands the first `capacity % shard_count` segments one extra
 * entry each. A caller can observe nothing that separates a division which
 * keeps that remainder from one which drops it. clrucache_capacity() reports
 * the number that the caller asked for in both cases. The eviction slack in a
 * full cache is far wider than the one or two entries at stake. A direct read
 * of the segments is what lets a test pin this. */
size_t _clrucache_segment_capacity_sum_for_tests(clru_cache cache,
                                                 size_t *out_segments) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) {
    if (out_segments) *out_segments = 0;
    return 0;
  }
  size_t total = 0;
  for (size_t i = 0; i < raw->shard_count; i++)
    total += raw->shards[i]->capacity;
  if (out_segments) *out_segments = raw->shard_count;
  _clrucache_resolve_unpin(raw);
  return total;
}

/* The number of segments whose map hashes with the keyed mode of chashmap,
 * and in *out_segments the number of segments. A test uses it to see that a
 * flood switched the maps of the segments. */
extern bool chashmap_is_keyed_for_tests(chmap chm);
size_t _clrucache_keyed_segment_maps_for_tests(clru_cache cache,
                                               size_t *out_segments) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) {
    if (out_segments) *out_segments = 0;
    return 0;
  }
  size_t keyed = 0;
  for (size_t i = 0; i < raw->shard_count; i++) {
    ccol_mutex_lock(raw->shards[i]->mutex);
    if (chashmap_is_keyed_for_tests(raw->shards[i]->map)) keyed++;
    ccol_mutex_unlock(raw->shards[i]->mutex);
  }
  if (out_segments) *out_segments = raw->shard_count;
  _clrucache_resolve_unpin(raw);
  return keyed;
}

/* The indices that sit on the free list now. Read this together with the
 * capacity above. A roll-back that loses a slot shows up as a table that
 * grew, but only while the free list is empty. A test that measures the
 * growth alone therefore passes or fails according to how many handles the
 * earlier tests held at one time. The two values together describe the table
 * independently of that. */
size_t _clrucache_free_index_count_for_tests(void) {
  ccol_call_once(clrucache_slot_table.once, _clrucache_slot_table_init_globals);
  ccol_rw_lock_rdlock(clrucache_slot_table.rwlock);
  size_t n = cvector_elem_count(clrucache_slot_table.free_indices);
  ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
  return n;
}

/* The number of waiter references that the entry of key_pair holds now, or
 * -1 when the cache does not hold the key. A thread that blocks on the entry
 * holds one, and so does a thread that runs a remote getter or a remote
 * setter for it. A test polls this to learn that a getter really waits on an
 * operation in flight, instead of guessing with a sleep. */
int _clrucache_key_waiters_for_tests(clru_cache cache,
                                     const cmap_pair *key_pair) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return -1;
  int n = -1;
  clru_shard *shard = shard_for(raw, key_pair);
  if (shard) {
    ccol_mutex_lock(shard->mutex);
    clru_entry *entry = map_lookup(shard, key_pair);
    if (entry) n = entry->waiters;
    ccol_mutex_unlock(shard->mutex);
  }
  _clrucache_resolve_unpin(raw);
  return n;
}

size_t _clrucache_slot_table_capacity_for_tests(void) {
  ccol_call_once(clrucache_slot_table.once, _clrucache_slot_table_init_globals);
  ccol_rw_lock_rdlock(clrucache_slot_table.rwlock);
  size_t n = cvector_elem_count(clrucache_slot_table.slots);
  ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
  return n;
}

/*
 * Lets a test bias which of two threads wins the race for the mutex of a
 * segment. It does not guarantee the winner. The race starts after a
 * successful fetch or set publishes an entry. The thread that publishes the
 * entry STILL HOLDS that mutex when it calls this function. The call comes
 * immediately before ccol_cond_var_broadcast() and ccol_mutex_unlock(). The
 * broadcast is what first makes a coalesced waiter on that same entry start
 * to contend for the mutex. A longer hold of the mutex here gives more time
 * to a concurrent, unrelated thread that wants the same mutex. One example of
 * such a thread is a thread that runs an eviction which removes the entry
 * that the publish added. That thread can then queue up for the mutex BEFORE
 * anything wakes the coalesced waiter. POSIX guarantees no FIFO order, but in
 * practice the thread that is already in the queue gets the mutex first after
 * the release. This is what lets a test exercise the path where a coalesced
 * waiter survives a racing eviction. Without it, the test depends on rare
 * scheduling luck that is hard to reproduce. This flag does not disarm
 * itself. A test arms it for exactly the one call under test. The test must
 * then set it back to 0 itself, so that later, unrelated tests in the same
 * process are unaffected. */
static _Atomic unsigned int _clru_test_post_publish_delay_us = 0;
static _Atomic bool _clru_test_post_publish_delay_entered = false;

void clru_test_set_post_publish_delay_us(unsigned int delay_us) {
  atomic_store(&_clru_test_post_publish_delay_entered, false);
  atomic_store(&_clru_test_post_publish_delay_us, delay_us);
}

/* Lets a test poll for one fact: the thread that publishes an entry entered
 * its delay, and therefore still holds the mutex of that segment right now.
 * The test does not need that mutex for this. It also cannot take the mutex
 * here safely. A lock of the mutex makes the thread that polls block until
 * the delay is over, which defeats the point. This is what lets the own
 * evictor thread of a test try to lock the mutex at the earliest possible
 * moment. A poll from userspace cannot get closer to it. Without this, the
 * test guesses at a fixed sleep duration. Such a sleep can fire too early,
 * before the thread that publishes locks the mutex again, and there is then
 * nothing to queue up behind. It can also leave less margin than the test
 * intends. */
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

/* Frees the bookkeeping arrays of the slot table at process exit. The
 * leak-kind report of make memtest then does not flag them as still
 * reachable. This mirrors _cleanup_event_loop_slot_table of ccol_event_loop
 * exactly. See the comment on that function for the full rationale. That
 * comment also says why this is sound only when the application destroys
 * every clru_cache that it creates before process exit. This test suite
 * already satisfies that precondition for a clean make memtest. This function
 * MUST use ccol_call_once. An __attribute__((destructor)) function runs for
 * the whole shared object, whichever parts of it the process used. Without
 * ccol_call_once, a process that links this library and creates no clru_cache
 * locks a mutex here that no call to pthread_mutex_init prepared. */
/* Tells whether any slot still names a cache. The caller holds the write
 * lock.
 *
 * This scan reads slot->ptr, and not slot->in_use. A destroy clears in_use as
 * its first step, so that it rejects a second destroy or a new resolve as
 * early as possible. The rest of the teardown runs after that step. That rest
 * drains the pins and does the final locked release of the index. A scan that
 * trusts in_use alone frees this table out from under a destroy that is still
 * in that window. The last step of that destroy then indexes the table.
 * The code writes ptr only after a slot is fully acquired, and it clears ptr
 * only in that final locked step. ptr is therefore true for exactly as long
 * as the table must not be released. */
static bool _clrucache_any_slot_live_locked(void) {
  size_t slot_count = cvector_elem_count(clrucache_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    clrucache_slot_t *slot =
        (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

/* Releases the bookkeeping of the table and the pin index. The caller
 * holds the write lock, and the caller already established that no slot is
 * live.
 *
 * The slot storage of the pin index is deliberately never released while the
 * process runs, because a resolve indexes it with no lock held. A leak
 * checker that treats still-reachable memory as an error therefore reports it
 * at exit, unless this function releases it here. The code sets each vector
 * to NULL as it destroys it. That is what makes a later call answer "already
 * released" instead of an index into a freed vector. This function does not
 * destroy the rwlock. It can run from an ordinary destroy that still holds
 * that lock. */
/* One out-of-line call holds both the check and the release. The destroy path
 * that must make that call therefore keeps the code shape that it has without
 * any of this. Cold code in a hot object file is not free. Inline here, the
 * same few instructions measurably slow the push path of an unrelated
 * container, because they shift what the linker lays out around it. The
 * instruction count does not change. */
static __attribute__((noinline)) void
_clrucache_release_slot_table_if_deferred_locked(void) {
  if (clrucache_slot_table.release_deferred &&
      !_clrucache_any_slot_live_locked()) {
    _clrucache_release_slot_table_locked();
  }
}

static void _clrucache_release_slot_table_locked(void) {
  cvector_destroy(clrucache_slot_table.slots);
  cvector_destroy(clrucache_slot_table.free_indices);
  ccol_pintable_dispose(&clrucache_pintable);
  clrucache_slot_table.release_deferred = false;
}

__attribute__((destructor)) static void _cleanup_clrucache_slot_table(void) {
  ccol_call_once(clrucache_slot_table.once, _clrucache_slot_table_init_globals);
  /* There is nothing to do, and nothing that is safe to touch. The code sets
     each vector to NULL as it destroys it. A second run of this function
     therefore answers here, instead of an index into a freed vector. */
  if (!clrucache_slot_table.slots) return;
  ccol_rw_lock_wrlock(clrucache_slot_table.rwlock);
  /* Release only after nothing remains that can still resolve a handle. The
   * order of the destructor of one translation unit against the destructor of
   * another is not for this library to decide. Without this check, a later
   * destructor that still holds a live handle finds the table and the pin
   * index freed under it. In that case this code hands the release to the
   * destroy that frees the last slot. It does not skip the release. An
   * application that destroys its caches therefore leaves nothing behind, in
   * whatever order the destructors run. */
  if (_clrucache_any_slot_live_locked()) {
    clrucache_slot_table.release_deferred = true;
    ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
    return;
  }
  _clrucache_release_slot_table_locked();
  ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
  /* This code deliberately does not destroy the rwlock. The rwlock has static
     storage duration, so it holds nothing that a leak checker reports. The
     destructor cannot own its lifetime in any case. The deferred branch above
     returns while the table is still live. The release that happens later
     runs while it holds this very lock. There is therefore no path where
     every user is provably finished with the lock. A destroy of the lock here
     leaves the entry points of the other branch with a read lock on a
     destroyed object. That turns a stale-handle call, which is documented to
     fail cleanly through ccol_fatal_err, into undefined behaviour. The order
     of destructors across translation units is also not for this library to
     decide. */
}

/* ========================================================================== */
/*                         clrucache_get_full                                 */
/* ========================================================================== */

static ccol_retval_t clrucache_get_full_retry(struct clrucache *raw,
                                              clru_shard *shard,
                                              const cmap_pair *key_pair,
                                              cmap_pair *val_out);

/* The body of clrucache_get_full(), entered with the mutex of the segment held.
 * Every path that returns a result releases that mutex and the pin of the
 * handle first.
 *
 * The one case that must look the key up again is a waiter that finds that
 * a set failed. The entry point instantiates this body with again == NULL,
 * and there that case leaves through a call to clrucache_get_full_retry(). A
 * loop back to the lookup inside the entry point would keep more values live
 * across the whole function, and that costs the hit path a spill. The cold
 * function instantiates the body with again != NULL. There the body reports
 * the retry through *again, with the mutex and the pin still held, and the
 * cold function loops. */
static inline __attribute__((always_inline)) ccol_retval_t
clrucache_get_full_locked(struct clrucache *raw, clru_shard *shard,
                          const cmap_pair *key_pair, cmap_pair *val_out,
                          bool *again) {
  clru_entry *entry = map_lookup(shard, key_pair);

  if (!entry) {
    /* Cache miss */
    if (!shard->remote_getter) {
      ccol_mutex_unlock(shard->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }

    /* Create a placeholder. Other threads that wait for the same key can then
     * find it. They coalesce onto the single remote fetch that follows. */
    entry = entry_alloc(shard);
    if (!entry) {
      ccol_mutex_unlock(shard->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }

    entry->key = _ccol_mem_alloc(shard->m_procs, key_pair->size);
    if (!entry->key) {
      /* Unlock the mutex before the free. This matches every other entry_free
       * call site in this file that frees an entry which no call published
       * into cache->map. The lock of a segment must not stay held across the
       * _ccol_mem_free and ccol_cond_var_destroy calls for the freed entry.
       * Without this unlock, those calls block every other concurrent getter
       * and setter on this cache for their whole duration. */
      ccol_mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }
    memcpy(entry->key, key_pair->ptr, key_pair->size);
    entry->key_size = key_pair->size;
    entry->fetch_in_progress = true;

    /* ccol_key_already_present is a documented SUCCESS outcome of
     * chmap_insert_elem. It means that the map wrote the stored value of the
     * key, which is the entry pointer here, in place. This code must treat it
     * the same as ccol_success. Every other chmap_insert_elem call site in
     * this codebase does the same (see cjson.c and clogger.c). This outcome
     * is structurally unreachable here, because map_lookup() confirmed that
     * the key is absent under this same, uninterrupted hold of the mutex. But
     * a hard failure here is dangerous, and not only redundant. The map
     * already points at `entry` when chmap_insert_elem returns. A free of
     * `entry` here, which a plain out-of-memory failure does, leaves a
     * dangling pointer behind for the very next lookup of this key. */
    ccol_retval_t ins = map_upsert(shard, key_pair, entry);
    if (ins != ccol_success && ins != ccol_key_already_present) {
      ccol_mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return ins;
    }

    /* This thread is the fetcher. Hold a reference across the window where
     * the mutex is not held, exactly as the remote-setter window of
     * clrucache_set_full does. See the locking discipline at the top of this
     * file: a thread that unlocks while it holds a pointer to an entry takes
     * a reference first, with no exception for a placeholder. */
    entry->waiters++;
    ccol_mutex_unlock(shard->mutex);

    cmap_pair fetched = {};
    bool fetch_ok = shard->remote_getter(key_pair, &fetched);

    ccol_mutex_lock(shard->mutex);
    entry->waiters--;
    entry->fetch_in_progress = false;

    if (fetch_ok && fetched.ptr && fetched.size > 0) {
      make_room(shard);
      entry->value = fetched.ptr;
      entry->value_size = fetched.size;
      lru_add_to_front(shard, entry);
      shard->size++;

      void *copy = _ccol_mem_alloc(shard->m_procs, fetched.size);
      if (!copy) {
        /* The cache holds the entry. The caller gets no copy this time. */
        ccol_cond_var_broadcast(entry->cond);
        ccol_mutex_unlock(shard->mutex);
        _clrucache_resolve_unpin(raw);
        return ccol_not_enough_memory;
      }
      memcpy(copy, fetched.ptr, fetched.size);
      val_out->ptr = copy;
      val_out->size = fetched.size;

#ifdef RUNNING_UNIT_TESTS
      /* This widens the window that a concurrent, unrelated eviction has to
       * reach and lock shard->mutex. It widens it before the broadcast below
       * wakes any coalesced waiter. See the doc comment on the hook. */
      _clru_test_maybe_delay_post_publish();
#endif
      ccol_cond_var_broadcast(entry->cond);
      ccol_mutex_unlock(shard->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_success;
    } else {
      /* The remote getter failed. Remove the placeholder, notify the
       * waiters */
      if (fetched.ptr)
        _ccol_mem_free(shard->m_procs, fetched.ptr); /* size==0 edge case */
      cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
      chmap_delete_elem(shard->map, &kp);
      entry->evicted = true;
      entry->fetch_failed = true;
      ccol_cond_var_broadcast(entry->cond);
      bool should_free = (entry->waiters == 0);
      ccol_mutex_unlock(shard->mutex);
      if (should_free) entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }
  }

  /* The entry exists. Wait for any operation in progress to settle. A hit
   * on a settled entry is the common case, and the hint keeps its path
   * straight. */
  if (__builtin_expect(entry->fetch_in_progress || entry->set_in_progress, 0)) {
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      ccol_cond_var_wait(entry->cond, shard->mutex);
    }
    entry->waiters--;

    if (!entry->value) {
      if (!entry->fetch_failed) {
        /* A set that this call waited on failed, and the key is not in the
         * cache: the set created the entry, or an eviction took the key
         * while the remote setter ran. Nobody asked the remote getter for
         * this key, so "not found" is not an answer that this call may give.
         * Drop the reference and look the key up again. An absent key then
         * takes the miss path below, exactly as a get that ran after the
         * failed set does; see the comment on fetch_failed. */
        if (entry->evicted && entry->waiters == 0) {
          /* Unlock around the free, as every other entry_free call site in
           * this file does. The pin of the handle keeps the segment alive
           * while the mutex is not held. */
          ccol_mutex_unlock(shard->mutex);
          entry_free(shard, entry);
          ccol_mutex_lock(shard->mutex);
        }
        if (again) {
          *again = true;
          return ccol_success;
        }
        return clrucache_get_full_retry(raw, shard, key_pair, val_out);
      }
      /* The fetch that this call coalesced onto failed. There is nothing to
       * give back. A fetch can also have a value of the wrong size; see the
       * comment on fetch_size_mismatch. Every coalesced waiter then gets the
       * same report as the thread that ran the remote getter. This code does
       * not downgrade that report to the generic "not found" because this
       * thread did not run the fetch. */
      ccol_retval_t fail_r = entry->fetch_size_mismatch
                                 ? ccol_unexpected_failure
                                 : ccol_key_not_found;
      bool should_free = (entry->evicted && entry->waiters == 0);
      ccol_mutex_unlock(shard->mutex);
      if (should_free) entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return fail_r;
    }
    /* entry->value is not NULL. The operation that this call coalesced onto
     * succeeded. Fall through to the shared path below that gives the value
     * back, even when entry->evicted is now true. An unrelated, concurrent
     * cache operation can evict this entry from the map and the LRU list. It
     * can do that in the window between the broadcast of the fetcher or the
     * setter and the wakeup of this thread. entry itself, and its value, are
     * still safe to read here. This thread still holds a waiter reference on
     * the entry, and that is exactly what keeps the entry alive until this
     * thread finishes. A coalesced waiter must get the same result as the
     * fetcher or the setter. It must not discard a value that is in front of
     * it, only because an eviction removed the entry moments later. */
  }

  /* Copy the value out. Refresh the LRU position of the entry when the entry
   * is still live. */
  /* Invariant: entry->value != NULL here always means one of two things. The
   * entry is still LIVE, which is in_lru. Or an eviction removed the entry
   * after a call stored a value in it successfully. In that second case the
   * entry is not part of the LRU list, and nothing may treat it as one. An
   * eviction removes the entry from the map before it sets evicted=true. A
   * map_lookup result that is not NULL and that needed no wait is therefore
   * always a valid LIVE entry. */
  ccol_assert(entry->value != NULL);
  ccol_assert(entry->evicted || entry->in_lru);

  /* Allocate the copy for the caller BEFORE this code promotes the LRU
   * position of the entry. A call that fails here, because it cannot
   * allocate memory, must not also move the entry to the front of the
   * eviction order. */
  void *copy = _ccol_mem_alloc(shard->m_procs, entry->value_size);
  if (!copy) {
    bool should_free = (entry->evicted && entry->waiters == 0);
    ccol_mutex_unlock(shard->mutex);
    if (should_free) entry_free(shard, entry);
    _clrucache_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }
  if (!entry->evicted) lru_move_to_front(shard, entry);
  memcpy(copy, entry->value, entry->value_size);
  val_out->ptr = copy;
  val_out->size = entry->value_size;

  bool should_free = (entry->evicted && entry->waiters == 0);
  ccol_mutex_unlock(shard->mutex);
  if (should_free) entry_free(shard, entry);
  _clrucache_resolve_unpin(raw);
  return ccol_success;
}

/* The cold instantiation of clrucache_get_full_locked(). It runs the lookup
 * again for as long as a waiter finds a failed set; see the comment on
 * fetch_failed. */
static __attribute__((noinline)) ccol_retval_t
clrucache_get_full_retry(struct clrucache *raw, clru_shard *shard,
                         const cmap_pair *key_pair, cmap_pair *val_out) {
  for (;;) {
    bool retry = false;
    ccol_retval_t r =
        clrucache_get_full_locked(raw, shard, key_pair, val_out, &retry);
    if (!retry) return r;
  }
}

ccol_retval_t clrucache_get_full(clru_cache cache, const cmap_pair *key_pair,
                                 cmap_pair *val_out) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return ccol_invalid_args;
  if (!key_pair || !key_pair->ptr || !key_pair->size || !val_out) {
    _clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  /* Everything below works on this one segment. A key belongs to exactly one
   * segment. Its lookup, its eviction order and the coalescence of concurrent
   * misses for it therefore all happen here. NULL means that the size of
   * the key does not agree with the width of the declared key type. The map
   * of the segment rejects such a key in turn. A rejection here keeps the
   * choice of segment from a read past the end of the key. */
  clru_shard *shard = shard_for(raw, key_pair);
  if (!shard) {
    _clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccol_mutex_lock(shard->mutex);
  return clrucache_get_full_locked(raw, shard, key_pair, val_out, NULL);
}

/* ========================================================================== */
/*                         clrucache_set_full                                 */
/* ========================================================================== */

/*
 * Helper. It allocates a new placeholder entry for the given key, and it
 * inserts that entry into the chmap. It returns the entry on success. On
 * failure it returns NULL and writes the real ccol_retval_t to *err_out. This
 * matches how the miss paths of clrucache_get_full and __clrucache_get_into
 * handle the return value of map_upsert. A failure that is not an
 * out-of-memory failure, for example ccol_container_full, must be reported as
 * itself. This code must not fold it into a generic ccol_not_enough_memory.
 * The caller must hold cache->mutex. This helper always returns with
 * cache->mutex held, both on success and on failure. It unlocks that mutex
 * only for a short time around its own entry_free() calls. See the comment at
 * those call sites.
 */
static clru_entry *create_and_insert_placeholder(clru_shard *cache,
                                                 const cmap_pair *key_pair,
                                                 ccol_retval_t *err_out) {
  clru_entry *e = entry_alloc(cache);
  if (!e) {
    *err_out = ccol_not_enough_memory;
    return NULL;
  }

  e->key = _ccol_mem_alloc(cache->m_procs, key_pair->size);
  if (!e->key) {
    /* Unlock the mutex before the free, and lock it again before the return.
     * The "returns with cache->mutex held" contract of this helper then holds
     * whatever the outcome is. The lock of a segment must not stay held
     * across the _ccol_mem_free and ccol_cond_var_destroy calls for the freed
     * entry. Without this unlock, those calls block every other concurrent
     * getter and setter on this cache for their whole duration. */
    ccol_mutex_unlock(cache->mutex);
    entry_free(cache, e);
    ccol_mutex_lock(cache->mutex);
    *err_out = ccol_not_enough_memory;
    return NULL;
  }
  memcpy(e->key, key_pair->ptr, key_pair->size);
  e->key_size = key_pair->size;

  /* ccol_key_already_present is a documented success outcome of
   * chmap_insert_elem, and not a failure. See the identical guard and the
   * comment in the miss path of clrucache_get_full for the full reasoning. */
  ccol_retval_t ins = map_upsert(cache, key_pair, e);
  if (ins != ccol_success && ins != ccol_key_already_present) {
    ccol_mutex_unlock(cache->mutex);
    entry_free(cache, e);
    ccol_mutex_lock(cache->mutex);
    *err_out = ins;
    return NULL;
  }
  return e;
}

/*
 * Helper. It writes a value into an entry and makes that entry LIVE.
 * It evicts when it must. The caller must hold cache->mutex.
 */
static ccol_retval_t entry_store_value(clru_shard *cache, clru_entry *entry,
                                       const cmap_pair *val_pair) {
  void *new_val = _ccol_mem_alloc(cache->m_procs, val_pair->size);
  if (!new_val) return ccol_not_enough_memory;
  memcpy(new_val, val_pair->ptr, val_pair->size);

  _ccol_mem_free(cache->m_procs, entry->value);
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

  /* See the note in clrucache_get_full. The key selects one segment, and
   * every step below stays inside that segment. */
  clru_shard *shard = shard_for(raw, key_pair);
  if (!shard) {
    _clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccol_mutex_lock(shard->mutex);

  clru_entry *entry;

  /*
   * Loop until this thread owns the set slot for this key. One map_lookup
   * plus one create-or-wait is not enough. Many setter threads can block on
   * the same operation in progress. They can all wake up at the same time
   * after that operation completes or fails. Without the loop, the second
   * thread to run calls create_and_insert_placeholder for a key that the
   * first thread already inserted again. It then gets
   * ccol_key_already_present from chmap_insert_elem, and it returns
   * ccol_not_enough_memory to the caller, which is wrong. A loop back to
   * map_lookup finds the placeholder of the first thread and waits for it.
   * That serialises the setters correctly.
   */
  for (;;) {
    entry = map_lookup(shard, key_pair);
    if (!entry) {
      ccol_retval_t create_err = ccol_not_enough_memory;
      entry = create_and_insert_placeholder(shard, key_pair, &create_err);
      if (!entry) {
        ccol_mutex_unlock(shard->mutex);
        _clrucache_resolve_unpin(raw);
        return create_err;
      }
      break;
    }
    if (!entry->fetch_in_progress && !entry->set_in_progress) {
      break; /* The entry is live. This thread owns it */
    }
    /* A concurrent fetch or set is in progress. Wait for it to complete */
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      ccol_cond_var_wait(entry->cond, shard->mutex);
    }
    entry->waiters--;
    if (!entry->evicted) {
      break; /* The entry survived. This thread owns it */
    }
    /* Some call cleaned the entry up while this thread waited. Loop back and
     * check the map again before this thread creates a new placeholder,
     * because another waiter can create one first. */
    bool should_free = (entry->waiters == 0);
    if (should_free) {
      /* Unlock the mutex before the free. This matches every other
       * entry_free call site in this file. The lock of a segment must not
       * stay held across the _ccol_mem_free and ccol_cond_var_destroy calls
       * for the freed entry. Without this unlock, those calls block every
       * other concurrent getter and setter on this cache for their whole
       * duration. A lock again and a continue of the loop are safe. Nothing
       * touches `entry` again, except the next map_lookup() call, which
       * assigns a new value to it. The pin of this function keeps `shard`
       * itself alive, whether or not the mutex is held. This function takes
       * that pin at its start, through _clrucache_resolve. */
      ccol_mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      ccol_mutex_lock(shard->mutex);
    }
  }

  /* This thread now owns the set slot */
  entry->set_in_progress = true;

  /* Without a remote setter, this whole operation is a plain update in
   * memory. This code then never unlocks the mutex, because there is nothing
   * to block on. It also never touches the LRU list ahead of
   * entry_store_value(). That function handles both cases correctly on its
   * own: !in_lru for a new entry or a placeholder, and in_lru for an entry
   * that exists and moves to the front. This is what avoids a needless pair
   * of an unlock and a lock on every clru_set() call. That is the common case
   * for a cache with no remote source. */
  bool remote_ok = true;

  if (shard->remote_setter) {
    /* The set takes effect when the remote call returns, and only if that
     * call succeeds. Until then a LIVE entry stays exactly what it is: in the
     * eviction order at its old place, and in the size of the segment. A
     * concurrent insert that needs room can therefore evict it, as the true
     * least recently used entry, and never evicts some other key in its
     * place. The segment also never holds more entries than its capacity.
     * See evict_lru() for what such an eviction leaves behind.
     *
     * Hold a waiter reference. Nothing then frees the entry while this thread
     * blocks in the remote call. */
    entry->waiters++;
    ccol_mutex_unlock(shard->mutex);

    remote_ok = shard->remote_setter(key_pair, val_pair);

    ccol_mutex_lock(shard->mutex);
    entry->waiters--;
  }

  ccol_retval_t retval = remote_ok ? ccol_success : ccol_unexpected_failure;

  /* entry->evicted is always false here. set_in_progress keeps the entry in
   * the map for the whole call. An eviction during the remote call only
   * turns a LIVE entry back into a placeholder; see evict_lru(). The entry
   * is therefore either LIVE (in_lru) or a placeholder with no value (not
   * in_lru). A placeholder is either new, or a LIVE entry that an eviction
   * took during the remote call. */
  if (remote_ok) {
    ccol_retval_t store_r = entry_store_value(shard, entry, val_pair);
    if (store_r != ccol_success) {
      /* This code cannot allocate memory. entry_store_value() fails before it
       * touches the value of the entry or the LRU list. A LIVE entry
       * therefore keeps its old value and its exact place in the eviction
       * order, and needs nothing at all. A placeholder holds no value, and
       * this call is what created it or what it belongs to. Remove it from
       * the map. */
      if (!entry->in_lru) {
        cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
        chmap_delete_elem(shard->map, &kp);
        entry->evicted = true;
      }
      entry->set_in_progress = false;
      ccol_cond_var_broadcast(entry->cond);
      bool should_free = (entry->evicted && entry->waiters == 0);
      ccol_mutex_unlock(shard->mutex);
      if (should_free) entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return store_r;
    }
  } else if (!entry->in_lru) {
    /* The remote setter failed, and the entry holds no value. Either this
     * call created it, or an eviction took the key during the remote call.
     * In both cases the key is absent from the cache once the set is
     * undone. Remove the placeholder. A getter that waited on this set then
     * looks the key up again and takes the ordinary miss path, which runs
     * the remote getter when the cache has one.
     *
     * A LIVE entry needs nothing at all. The failed set never moved it, never
     * took it out of the size of the segment and never touched its value. The
     * cache is therefore exactly as the concurrent operations alone left it,
     * which is what a set that never happened leaves. */
    cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
    chmap_delete_elem(shard->map, &kp);
    entry->evicted = true;
  }

  entry->set_in_progress = false;
#ifdef RUNNING_UNIT_TESTS
  _clru_test_maybe_delay_post_publish();
#endif
  ccol_cond_var_broadcast(entry->cond);

  bool should_free = (entry->evicted && entry->waiters == 0);
  ccol_mutex_unlock(shard->mutex);
  if (should_free) entry_free(shard, entry);

  _clrucache_resolve_unpin(raw);
  return retval;
}

/* ========================================================================== */
/*                         __clrucache_get_into                               */
/* ========================================================================== */

static ccol_retval_t clrucache_get_into_retry(struct clrucache *raw,
                                              clru_shard *shard,
                                              const cmap_pair *key_pair,
                                              void *buf, size_t buf_size);

/* The body of __clrucache_get_into(), entered with the mutex of the segment
 * held. Every path that returns a result releases that mutex and the pin of
 * the handle first.
 *
 * The one case that must look the key up again is a waiter that finds that
 * a set failed. The entry point instantiates this body with again == NULL,
 * and there that case leaves through a call to clrucache_get_into_retry(). A
 * loop back to the lookup inside the entry point would keep more values live
 * across the whole function, and that costs the hit path a spill. The cold
 * function instantiates the body with again != NULL. There the body reports
 * the retry through *again, with the mutex and the pin still held, and the
 * cold function loops. */
static inline __attribute__((always_inline)) ccol_retval_t
clrucache_get_into_locked(struct clrucache *raw, clru_shard *shard,
                          const cmap_pair *key_pair, void *buf, size_t buf_size,
                          bool *again) {
  clru_entry *entry = map_lookup(shard, key_pair);

  if (!entry) {
    if (!shard->remote_getter) {
      ccol_mutex_unlock(shard->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }

    entry = entry_alloc(shard);
    if (!entry) {
      ccol_mutex_unlock(shard->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }

    entry->key = _ccol_mem_alloc(shard->m_procs, key_pair->size);
    if (!entry->key) {
      /* Unlock the mutex before the free. See the identical comment in the
       * miss path of clrucache_get_full for the full reasoning. */
      ccol_mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }
    memcpy(entry->key, key_pair->ptr, key_pair->size);
    entry->key_size = key_pair->size;
    entry->fetch_in_progress = true;

    /* ccol_key_already_present is a documented success outcome of
     * chmap_insert_elem, and not a failure. See the identical guard and the
     * comment in the miss path of clrucache_get_full for the full
     * reasoning. */
    ccol_retval_t ins = map_upsert(shard, key_pair, entry);
    if (ins != ccol_success && ins != ccol_key_already_present) {
      ccol_mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return ins;
    }

    /* Hold a reference across the window where the mutex is not held. See
     * the identical step in the miss path of clrucache_get_full, and the
     * locking discipline at the top of this file. */
    entry->waiters++;
    ccol_mutex_unlock(shard->mutex);

    cmap_pair fetched = {};
    bool fetch_ok = shard->remote_getter(key_pair, &fetched);

    ccol_mutex_lock(shard->mutex);
    entry->waiters--;
    entry->fetch_in_progress = false;

    if (fetch_ok && fetched.ptr && fetched.size > 0) {
      /* Reject a value whose size does not match the destination buffer
       * exactly. A larger value overflows buf. A smaller value leaves the
       * tail of buf untouched, and that tail is uninitialized or stale data
       * from the stack or heap of the caller. Such a call still reports
       * ccol_success, and it hands the caller of the type-inferred clru_get()
       * macro a ValT that is only partly filled. A cache of a value with the
       * wrong size also makes every future __clrucache_get_into call for this
       * key meet the same problem. This code therefore treats any mismatch of
       * size as a complete failure of the fetch, and it caches nothing. */
      if (fetched.size != buf_size) {
        _ccol_mem_free(shard->m_procs, fetched.ptr);
        cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
        chmap_delete_elem(shard->map, &kp);
        entry->evicted = true;
        entry->fetch_size_mismatch = true;
        entry->fetch_failed = true;
        ccol_cond_var_broadcast(entry->cond);
        bool should_free = (entry->waiters == 0);
        ccol_mutex_unlock(shard->mutex);
        if (should_free) entry_free(shard, entry);
        _clrucache_resolve_unpin(raw);
        return ccol_unexpected_failure;
      }

      make_room(shard);
      entry->value = fetched.ptr;
      entry->value_size = fetched.size;
      lru_add_to_front(shard, entry);
      shard->size++;

      memcpy(buf, fetched.ptr, fetched.size);

#ifdef RUNNING_UNIT_TESTS
      _clru_test_maybe_delay_post_publish();
#endif
      ccol_cond_var_broadcast(entry->cond);
      ccol_mutex_unlock(shard->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_success;
    } else {
      if (fetched.ptr) _ccol_mem_free(shard->m_procs, fetched.ptr);
      cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
      chmap_delete_elem(shard->map, &kp);
      entry->evicted = true;
      entry->fetch_failed = true;
      ccol_cond_var_broadcast(entry->cond);
      bool should_free = (entry->waiters == 0);
      ccol_mutex_unlock(shard->mutex);
      if (should_free) entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }
  }

  if (entry->fetch_in_progress || entry->set_in_progress) {
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      ccol_cond_var_wait(entry->cond, shard->mutex);
    }
    entry->waiters--;

    if (!entry->value) {
      if (!entry->fetch_failed) {
        /* A set that this call waited on failed, and the key is not in the
         * cache: the set created the entry, or an eviction took the key
         * while the remote setter ran. Nobody asked the remote getter for
         * this key, so "not found" is not an answer that this call may give.
         * Drop the reference and look the key up again. An absent key then
         * takes the miss path below, exactly as a get that ran after the
         * failed set does; see the comment on fetch_failed. */
        if (entry->evicted && entry->waiters == 0) {
          /* Unlock around the free, as every other entry_free call site in
           * this file does. The pin of the handle keeps the segment alive
           * while the mutex is not held. */
          ccol_mutex_unlock(shard->mutex);
          entry_free(shard, entry);
          ccol_mutex_lock(shard->mutex);
        }
        if (again) {
          *again = true;
          return ccol_success;
        }
        return clrucache_get_into_retry(raw, shard, key_pair, buf, buf_size);
      }
      /* The fetch that this call coalesced onto failed. There is nothing to
       * give back. A fetch can also have a value of the wrong size; see the
       * comment on fetch_size_mismatch. Every coalesced waiter then gets the
       * same report as the thread that ran the remote getter. This code does
       * not downgrade that report to the generic "not found" because this
       * thread did not run the fetch. */
      ccol_retval_t fail_r = entry->fetch_size_mismatch
                                 ? ccol_unexpected_failure
                                 : ccol_key_not_found;
      bool should_free = (entry->evicted && entry->waiters == 0);
      ccol_mutex_unlock(shard->mutex);
      if (should_free) entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return fail_r;
    }
    /* entry->value is not NULL. The operation that this call coalesced onto
     * succeeded. Fall through to the shared path below that gives the value
     * back, even when entry->evicted is now true. See the identical
     * reasoning in the wait branch of clrucache_get_full. */
  }

  /* Invariant: entry->value != NULL here always means one of two things. The
   * entry is still LIVE, which is in_lru. Or an eviction removed the entry
   * after a call stored a value in it successfully. In that second case the
   * entry is not part of the LRU list, and nothing may treat it as one. */
  ccol_assert(entry->value != NULL);
  ccol_assert(entry->evicted || entry->in_lru);

  /* Reject a mismatch of size in either direction BEFORE this code promotes
   * the LRU position of the entry. See the requirement for an identical size,
   * and its reasoning, in the fetch-path check of this same function above.
   * Only an exact match may go into buf. A call that this code is going to
   * reject must not also move the entry to the front of the eviction
   * order. */
  if (entry->value_size != buf_size) {
    bool should_free = (entry->evicted && entry->waiters == 0);
    ccol_mutex_unlock(shard->mutex);
    if (should_free) entry_free(shard, entry);
    _clrucache_resolve_unpin(raw);
    return ccol_unexpected_failure;
  }
  if (!entry->evicted) lru_move_to_front(shard, entry);
  memcpy(buf, entry->value, entry->value_size);

  bool should_free = (entry->evicted && entry->waiters == 0);
  ccol_mutex_unlock(shard->mutex);
  if (should_free) entry_free(shard, entry);
  _clrucache_resolve_unpin(raw);
  return ccol_success;
}

/* The cold instantiation of clrucache_get_into_locked(). It runs the lookup
 * again for as long as a waiter finds a failed set; see the comment on
 * fetch_failed. */
static __attribute__((noinline)) ccol_retval_t clrucache_get_into_retry(
    struct clrucache *raw, clru_shard *shard, const cmap_pair *key_pair,
    void *buf, size_t buf_size) {
  for (;;) {
    bool retry = false;
    ccol_retval_t r =
        clrucache_get_into_locked(raw, shard, key_pair, buf, buf_size, &retry);
    if (!retry) return r;
  }
}

ccol_retval_t __clrucache_get_into(clru_cache cache, const cmap_pair *key_pair,
                                   void *buf, size_t buf_size) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return ccol_invalid_args;
  if (!key_pair || !key_pair->ptr || !key_pair->size || !buf || buf_size == 0) {
    _clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  /* See the note in clrucache_get_full. The key selects one segment, and
   * every step below stays inside that segment. */
  clru_shard *shard = shard_for(raw, key_pair);
  if (!shard) {
    _clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccol_mutex_lock(shard->mutex);
  return clrucache_get_into_locked(raw, shard, key_pair, buf, buf_size, NULL);
}

/* ========================================================================== */
/*                         SIZE / CAPACITY QUERIES                            */
/* ========================================================================== */

size_t clrucache_size(clru_cache cache) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return 0;
  /* This function adds the sizes of the segments, each one under its own
   * lock. The total is a snapshot, and not a value at one instant. It does
   * not show a concurrent insert into a segment that this loop already
   * counted. A concurrent insert during a read under a single lock is also
   * absent from such a total. */
  size_t total = 0;
  for (size_t i = 0; i < raw->shard_count; i++) {
    clru_shard *sh = raw->shards[i];
    ccol_mutex_lock(sh->mutex);
    total += sh->size;
    ccol_mutex_unlock(sh->mutex);
  }
  _clrucache_resolve_unpin(raw);
  return total;
}

size_t clrucache_capacity(clru_cache cache) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return 0;
  /* The creation fixes this value, and nothing writes it again. It therefore
   * needs no lock. */
  size_t c = raw->capacity;
  _clrucache_resolve_unpin(raw);
  return c;
}
