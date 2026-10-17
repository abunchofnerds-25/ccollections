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
#include <unistd.h> /* usleep(); not a pthread/sem primitive, so it
                        needs no common.h wrapper */
#endif

/* ========================================================================== */
/*                         INTERNAL STRUCTURES                                */
/* ========================================================================== */

/*
 * A single cache entry. Every key belongs to exactly one segment of the
 * cache, and the entry for that key sits in two data structures of that
 * segment at once:
 *
 *   1. The hash map (the segment's map): key -> clru_entry*
 *      The map stores a COPY of the pointer (8 bytes), which leads to the
 *      mutable entry holding the condition variable and the value.
 *
 *   2. The LRU doubly-linked list. The entry is only in this list while it
 *      is LIVE, meaning it has a value. Two sentinel nodes bound the list
 *      and the segment embeds both of them (lru_head is the MRU end,
 *      lru_tail the LRU end), so each segment has its own eviction order.
 *
 * Entry lifecycle:
 *   PLACEHOLDER  in map, NOT in LRU, value==NULL, fetch_in_progress or
 *                set_in_progress is true. The LRU mechanism never evicts it.
 *   LIVE         in map, IN LRU, value!=NULL. The cache can evict it. That
 *                also holds while a remote setter runs for the key: the
 *                entry keeps its place in the eviction order and in the
 *                segment's size for the whole call, exactly as if no set
 *                were in flight. evict_lru() treats such an entry as a
 *                victim like any other, reports the old value to the
 *                eviction callback and turns the entry back into a
 *                PLACEHOLDER, which the set then fills (success) or removes
 *                (failure).
 *   DEAD         NOT in map, NOT in LRU, evicted==true. Only threads holding
 *                a waiter reference keep it alive (waiters > 0). The thread
 *                that brings waiters down to 0 frees it, and that thread
 *                holds the owning segment's mutex.
 *
 * Locking discipline:
 *   A segment's mutex protects ALL the fields of that segment and all the
 *   fields of every entry in it. Each entry's condition variable shares that
 *   same mutex, as cond_wait requires. A key maps to one segment and every
 *   step of an operation on that key stays inside that segment, so no
 *   operation ever holds two of these mutexes and there's no lock order to
 *   get wrong.
 *
 *   The slow operations, the remote getter and the remote setter, always
 *   run OUTSIDE the mutex. A thread that unlocks the mutex while holding a
 *   pointer to an entry MUST increment entry->waiters first, and decrement
 *   it when it locks the mutex again. Otherwise another thread frees the
 *   entry while the first one is still using it.
 *
 *   That rule has no exception, and all three remote-call windows of this
 *   file follow it: the two miss paths that run a remote getter and the set
 *   path that runs a remote setter. As it happens a PLACEHOLDER can't be
 *   reached by eviction, because evict_lru() picks its victim from the LRU
 *   list and a placeholder isn't in that list, but that's a property of the
 *   eviction policy and not of the reference rule. A window that relies on
 *   it holds a bare pointer whose safety no counter records, and it turns
 *   into a use-after-free as soon as some future change lets a placeholder
 *   be reclaimed. The reference costs one increment and one decrement under
 *   a mutex the thread already holds.
 */
typedef struct clru_entry {
  void *key;
  size_t key_size;

  void *value; /* NULL while fetch/set is in progress */
  size_t value_size;

  _ccol_cond_var_t cond; /* shared with the owning segment's mutex */

  bool fetch_in_progress; /* a remote getter is running for this key */
  bool set_in_progress;   /* a remote/async setter is running for this key */
  bool evicted;           /* removed from map+LRU; kept alive by waiters */

  /* Only the miss path of __clrucache_get_into() sets this, when the size of
   * the value a remote getter fetched doesn't match the buf_size of the
   * caller that asked, right before it tears the entry down as a failed
   * fetch. A live entry never has it true. A coalesced waiter that wakes up
   * and finds entry->value still NULL reads this field and reports the same
   * ccol_unexpected_failure the fetching thread got. Without it the waiter
   * would report the generic ccol_key_not_found that every other fetch
   * failure gives, and only the one thread that ran the remote getter would
   * learn that the real problem is a value of the wrong size. That would
   * break this module's coalescing contract, which says that "all others ...
   * receive the same result" (see the file-level doc comment of
   * clrucache.h). */
  bool fetch_size_mismatch;

  /* How many threads hold a reference to this entry without owning its
   * lifecycle. Two kinds of thread count: a thread blocked on cond, and a
   * thread that released the segment's mutex to run a remote getter or
   * setter for this entry and will touch the entry again once it has the
   * mutex back.
   *
   * The count keeps entry_free() away from an entry another thread can still
   * reach. The thread that drops the count to zero on an entry whose evicted
   * flag is set is the one that frees it. The owning segment's mutex
   * protects this field. */
  int waiters;

  struct clru_entry *prev; /* LRU list links (NULL when not in LRU) */
  struct clru_entry *next;
  bool in_lru;

  /* Only the two miss paths set this, right before they tear down an entry
   * whose remote getter failed (a size mismatch included); a live entry
   * never has it true. It tells apart the two ways a coalesced waiter can
   * wake up and find no value. If the operation it waited on was a FETCH,
   * the remote getter has already answered for the key and every waiter
   * reports that answer. If it was a SET that failed, nobody asked the
   * remote getter anything, so the waiter drops its reference and runs its
   * lookup again, and an absent key then takes the ordinary miss path, which
   * runs the remote getter. A get that ended in "not found" without any
   * getter call would match no serial order of the get and the failed set.
   *
   * The field sits beside in_lru in the struct's tail padding, so it adds no
   * size and moves no other field. That matters because the segment embeds
   * two sentinel entries, so a bigger entry would also move the segment
   * fields the hit path reads. */
  bool fetch_failed;
} clru_entry;

/* clru_cache is an opaque value handle: the top 32 bits are the slot index
 * and the bottom 32 bits the generation (see the doc comment on the typedef
 * in include/clrucache.h). The handle is resolved through this table before
 * the struct clrucache* behind it is touched, and that's what lets
 * __clrucache_destroy report a second destroy as a _ccol_fatal_err instead
 * of a use-after-free or a double-free. It catches both a concurrent second
 * destroy, racing another destroy on the same still-live handle, and a
 * sequential one, where the handle went stale after an earlier destroy had
 * completed. A slot is marked not-in-use the moment it is released and its
 * generation goes up on every reuse, so a stale handle can never alias a
 * later, unrelated cache that holds the same slot index. This table mirrors
 * chttpcli_slot_table, chttpsvr_slot_table and ccol_event_loop_slot_table
 * exactly; the copy of this comment in src/chttpclient.c explains the
 * design rationale.
 *
 * The table's lock is a read-write lock rather than a plain mutex. A get or
 * a set never takes it, since _clrucache_resolve pins the handle through the
 * lock-free pin index of cpintable.h. Only _clrucache_handle_slot_acquire
 * and __clrucache_destroy take it, each once in a cache's whole life, and
 * the tests' white-box helpers read the table under its read side. That
 * mirrors the slot-table rwlocks in cthreadcomm.c, cthreadpool.c and
 * chttpclient.c. Like chttpcli_slot_table in chttpclient.c, this table has
 * no pthread_atfork() protection of its own, because clrucache.c registers
 * none. cthreadcomm.c and cthreadpool.c have a subtle write lock driven by a
 * tracked thread ID so they can initialize it again in the child, but none
 * of that applies here, since this table has no fork-time lock state to
 * keep. */
typedef struct {
  struct clrucache *ptr; /* NULL when slot is free */
  uint32_t generation;   /* A fresh value on every acquire, monotonic
                             per slot index. It starts at 0 before the first
                             use and becomes 1 at the first acquire */
  bool in_use;
} clrucache_slot_t;

static struct {
  _ccol_rw_lock_t rwlock;
  _ccol_once_flag_t once;
  cvec slots;        /* A cvec of clrucache_slot_t. It only grows by
                         push_back, and an index is permanent once
                         allocated */
  cvec free_indices; /* A cvec of uint32_t; a LIFO free list that gives
                         O(1) reuse */
  /* How many slot indices a push onto free_indices couldn't take because
     the push couldn't allocate. Such a slot is fully released (ptr NULL,
     in_use false) but nothing names it. Without this counter that slot
     would stay unreachable for the rest of the process, and every later
     create would grow the table by one more slot. An acquire that finds
     free_indices empty while this counter is above zero recovers one slot
     by scanning slots instead. The scan is unambiguous because it only runs
     when the free list is empty, so a released slot is either on that list
     or lost, never both. Only read and written under the write lock. */
  size_t lost_indices;
  /* True when the process-exit destructor finds a cache still live and
     leaves this table alone; the destroy that later releases the last slot
     then does the release the destructor couldn't. Without this, a cache
     destroyed by a destructor that was linked earlier than this one would
     leave the table and the pin index allocated for the rest of the
     process, and a leak checker that counts still-reachable memory as an
     error would report it. Only read and written under the write lock. */
  bool release_deferred;
} clrucache_slot_table = {0};

/* The definitions are down with the process-exit teardown. They're declared
   here because the final locked section of __clrucache_destroy does the
   release that the destructor defers. */
static bool _clrucache_any_slot_live_locked(void);
static void _clrucache_release_slot_table_locked(void);
static void _clrucache_release_slot_table_if_deferred_locked(void);

static void _clrucache_slot_table_init_globals(void) {
  if (_ccol_rw_lock_init(clrucache_slot_table.rwlock) != 0)
    _ccol_fatal_err("clru_cache slot table: failed to initialize rwlock");
  clrucache_slot_table.slots = cvector_create(sizeof(clrucache_slot_t), NULL);
  if (!clrucache_slot_table.slots)
    _ccol_fatal_err("clru_cache slot table: failed to allocate slots vector");
  clrucache_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!clrucache_slot_table.free_indices)
    _ccol_fatal_err(
        "clru_cache slot table: failed to allocate free-index vector");
}

/* One segment of a cache, with its own lock.
 *
 * A cache is split into several of these segments. Every key belongs to
 * exactly one segment, picked by a hash of the key (see shard_for), and each
 * segment has its own map, its own eviction order and its own lock, so
 * operations on keys in different segments run at the same time instead of
 * queuing behind one another.
 *
 * Each segment holds its own copy of the configured callbacks instead of
 * reaching them through a back-pointer, which keeps a segment's operations
 * inside memory the segment already owns. The allocator is the one thing a
 * segment borrows: the router holds the single copy of the caller's
 * allocator, because a copy in each segment would need to be freed by the
 * very allocator that the copy names. */
typedef struct clru_shard {
  /* key -> clru_entry*. The value type is a pointer, so the map picks its
     own backend from the key type: open addressing for an integral or a
     pointer key, separate chaining for a string, a struct or a long
     double. */
  chmap map;

  /* Sentinel nodes of the doubly-linked LRU list. lru_head.next is the MRU
   * entry and lru_tail.prev the LRU entry. The sentinels are embedded in the
   * struct rather than allocated on the heap, and their cond, key and value
   * fields are never used. */
  clru_entry lru_head;
  clru_entry lru_tail;

  size_t capacity; /* this segment's share of the configured capacity */
  size_t size;     /* number of LIVE entries in this segment */

  _ccol_mutex_t mutex; /* protects everything in this segment */

  clru_remote_getter_t remote_getter;
  clru_remote_setter_t remote_setter;
  clru_eviction_cb_t eviction_cb;

  ccol_memmgmt_procs_t *m_procs;
} clru_shard;

struct clrucache {
  clru_shard **shards;
  size_t shard_count;

  /* The declared key type, and its width when it has a fixed one (0 when it
   * doesn't). These two keep the choice of segment in agreement with the
   * maps inside the segments about which keys are the same key, and they
   * let us reject a hand-built cmap_pair whose size doesn't fit the type
   * before anything reads through it. */
  ccol_data_type key_type;
  size_t key_fixed_width;

  /* The capacity the caller asked for, which is exactly the sum of the
   * segments' own capacities. */
  size_t capacity;

  ccol_memmgmt_procs_t *m_procs;

  /* This cache's handle. An unpin uses it to find the slot holding its pin,
   * so the caller doesn't have to carry a handle. Written once, before the
   * handle is published, and never again. */
  clru_cache self_handle;

  /* Our copy of the caller's allocator. m_procs points here when the caller
   * gave an allocator and is NULL otherwise. The cache keeps no pointer to
   * the caller's struct, so that struct may go out of scope as soon as
   * clrucache_create_full returns. It comes after every field a lookup
   * reads, so it moves none of them. */
  ccol_memmgmt_procs_t m_procs_copy;
};

/* ========================================================================== */
/*                    CLRU_CACHE HANDLE RESOLVE / UNPIN                       */
/* ========================================================================== */

/* Resolves h and pins the result against a concurrent destroy. Returns NULL
 * when h is 0 or garbage, and also when h names a slot that is free now or
 * one the table has already reused, which shows as the wrong generation. On
 * success the caller MUST call _clrucache_resolve_unpin(result) exactly once,
 * as soon as it's done with the resolved struct clrucache*. */
/* The hot half of the slot table. It maps a handle to a pointer and holds
 * the pin that keeps a cache alive for the length of a call. It's separate
 * from the table itself because a resolve runs on every public call and must
 * not write anything another thread reads. Recycling a slot is cold and
 * stays under the rwlock. */
static ccol_pintable clrucache_pintable;

static struct clrucache *_clrucache_resolve(clru_cache h) {
  /* No lock and no shared write here. Every public entry point of this
   * module runs this, so a write here to memory another thread reads would
   * cost time on every single cache operation. */
  return (struct clrucache *)ccol_pintable_pin(&clrucache_pintable, h);
}

static void _clrucache_resolve_unpin(struct clrucache *raw) {
  /* Releases the pin and touches nothing else. Locking this cache's own
   * mutex here would make every operation lock it twice, the second time
   * right after the unlock, which is exactly how a convoy keeps itself alive
   * under concurrent callers. There's no wakeup to deliver either, since
   * __clrucache_destroy polls the pin count instead of sleeping on a
   * condition variable and waiting for this.
   *
   * Reading raw->self_handle before the release is safe because the pin is
   * still held then. After the release another thread may free the object
   * at any moment, so nothing may touch raw after this call. */
  ccol_pintable_unpin(&clrucache_pintable, raw->self_handle);
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only. Makes the very next push onto the free list behave exactly like
 * an allocation failure, then disarms itself. Without it, the lost-index
 * recovery below would need a real out-of-memory condition at one specific
 * cvector_push_back, which an ordinary test run can't produce. */
static _Atomic bool g_clrucache_fail_next_free_index_push = false;

void _clrucache_force_next_free_index_push_failure_for_tests(void) {
  atomic_store(&g_clrucache_fail_next_free_index_push, true);
}
#endif

/* Hands idx back for reuse. The caller holds the write lock.
 *
 * A failed push is recorded instead of being dropped silently. The slot the
 * index names is already fully released, so a silent drop would strand that
 * index for the life of the process and make every later create grow the
 * table again. See clrucache_slot_table.lost_indices. */
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

/* Recovers one slot index stranded by a failed free-list push (see
 * clrucache_slot_table.lost_indices) by scanning for a slot that is released
 * but that nothing names. The caller holds the write lock and has already
 * found free_indices empty, and that's what makes "ptr is NULL and in_use is
 * false" mean lost rather than just free: a slot that a release put on that
 * list successfully is on the list. A slot whose destroy is still running
 * keeps a non-NULL ptr until the very end of that destroy, because the
 * destroy clears in_use as its first step and ptr only in its final locked
 * step, so the two tests together skip exactly that window. Only an index
 * the table already holds is returned, so this can never take the table past
 * CCOL_PIN_MAX_SLOTS.
 *
 * It's out of line and only reached while the counter is above zero, so an
 * ordinary acquire pays one comparison for it. When the scan finds nothing
 * the counter is set to zero; otherwise a counter that outlived its slot
 * would make every future acquire scan the table.
 */
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

/* Allocates a fresh slot for cache, or reuses one a release freed, and
 * returns the resulting handle. Returns 0 when memory can't be allocated,
 * and also when the module destructor has already released the table, which
 * *released then reports. clrucache_create_full calls it once, after the
 * object is fully constructed in every other respect. */
static clru_cache _clrucache_handle_slot_acquire(struct clrucache *cache,
                                                 bool *released) {
  _ccol_call_once(clrucache_slot_table.once,
                  _clrucache_slot_table_init_globals);
  _ccol_rw_lock_wrlock(clrucache_slot_table.rwlock);
  /* An application destructor or an exit handler can still create a cache
   * after the module destructor released the table. Such a create is refused
   * like any other failed create, since the table's vectors are gone. */
  *released = clrucache_slot_table.slots == NULL;
  if (__builtin_expect(*released, 0)) {
    _ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
    return 0;
  }
  uint32_t idx;
  clrucache_slot_t *slot;
  if (cvector_elem_count(clrucache_slot_table.free_indices) > 0) {
    cvector_pop_back(clrucache_slot_table.free_indices, &idx);
    slot = (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
  } else if (clrucache_slot_table.lost_indices > 0 &&
             _clrucache_reclaim_lost_index_locked(&idx)) {
    slot = (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
  } else {
    /* The pin table can't hold a slot past its own index limit and nothing can
     * publish one, so we refuse it here instead of claiming the slot and
     * rolling the claim back. A roll-back would put an index that no later
     * publish can use onto the free list that every acquire pops from. This
     * is an ordinary failure, which callers handle anyway for a table
     * that can't grow. */
    if (cvector_elem_count(clrucache_slot_table.slots) >= CCOL_PIN_MAX_SLOTS) {
      _ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
      return 0;
    }
    clrucache_slot_t fresh = {0};
    if (cvector_push_back(clrucache_slot_table.slots, &fresh) != ccol_success) {
      _ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
      return 0; /* an ordinary failure to allocate memory; not fatal */
    }
    idx = (uint32_t)cvector_elem_count(clrucache_slot_table.slots) - 1;
    slot = (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
  }
  slot->generation++;
  /* Skip the one generation value that collides with the reserved "invalid
   * handle" sentinel (0), which comes up after about 2^32 reuses of this
   * exact slot index. The identical guard in chttpcli_handle_slot_acquire
   * explains why. */
  if (slot->generation == 0) slot->generation++;
  clru_cache h = ((clru_cache)idx << 32) | (clru_cache)slot->generation;

  /* Written before the handle is published, so a resolver that finds this
   * cache also finds the handle its own unpin needs. */
  cache->self_handle = h;

  /* A publish can allocate, and if it fails the handle could never be
   * resolved, so the slot goes back on the free list. */
  if (!ccol_pintable_publish(&clrucache_pintable, idx, slot->generation,
                             cache)) {
    /* Cleared as the index goes back on the free list, so this struct never
     * carries a handle naming a slot that belongs to another object; an
     * unpin with such a handle would charge that other object's pin count
     * for a pin nobody took. The caller frees this object without resolving
     * it again, so no path reads the field after this point. Clearing it
     * keeps that fact local to this function, so no future user of the
     * failure path needs to know the rule. */
    cache->self_handle = 0;
    _clrucache_free_index_release_locked(idx);
    _ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
    return 0;
  }

  slot->ptr = cache;
  slot->in_use = true;
  _ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
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
  if (_ccol_cond_var_init(e->cond) != 0) {
    /* Do NOT route this through entry_free(): it calls
     * _ccol_cond_var_destroy() on a condition variable that was never
     * initialized successfully, and that's undefined behavior. e->key and
     * e->value are still NULL after the fresh calloc, so a plain free is all
     * this path needs. */
    _ccol_mem_free(cache->m_procs, e);
    return NULL;
  }
  return e;
}

/* Free the key, the value, the condition variable and the entry itself.
 * Precondition: no other thread can reach e or holds a reference to it.
 * Exactly two situations give that. (1) e was never published into
 * cache->map, because an allocation or an insert failed before or during
 * map_upsert or chmap_insert_elem, so no other thread can find it at all.
 * (2) e->evicted == true and e->waiters == 0: e was published, a later call
 * removed it from both the map and the LRU list, and every thread waiting
 * on it woke up and dropped its reference. The caller may or may not hold
 * the segment's mutex, because correctness only depends on one of these two
 * situations and not on the mutex. */
static void entry_free(clru_shard *cache, clru_entry *e) {
  _ccol_mem_free(cache->m_procs, e->key);
  _ccol_mem_free(cache->m_procs, e->value);
  _ccol_cond_var_destroy(e->cond);
  _ccol_mem_free(cache->m_procs, e);
}

/* ========================================================================== */
/*                         EVICTION (called under mutex)                      */
/* ========================================================================== */

/* Evict the least recently used LIVE entry. The eviction callback runs
 * before the entry is removed, with the mutex held. Returns true when an
 * entry was evicted and false when the list is empty.
 *
 * The victim is always the real least recently used entry, even one whose
 * key has a remote setter in flight. That set hasn't happened yet: it takes
 * effect when its remote call returns, and only if that call succeeds. Until
 * then the key holds its old value in its old place, and an insert that
 * needs room evicts it like any other key. Sparing it would only be right if
 * the set later succeeds. If the set fails, sparing it has cost some innocent
 * key its place, and nothing later can give that key back without undoing an
 * eviction a caller may already have seen.
 *
 * An evicted entry with a set in flight stays in the map and becomes a
 * placeholder again: no value, out of the eviction order and out of the
 * segment's size. The set that owns it then finishes the job. On success it
 * stores the new value and inserts the key at the most recently used end,
 * exactly what a set of an absent key does; on failure it removes the
 * placeholder and the key stays evicted. Every thread waiting on the key is
 * waiting on that set, so none of them can see the placeholder. */
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
  _ccol_cond_var_broadcast(victim->cond);

  if (victim->waiters == 0) {
    entry_free(cache, victim);
  }
  return true;
}

/* Make sure cache->size < cache->capacity by evicting LRU entries. Every
 * entry that cache->size counts is in the LRU list and can be evicted, so
 * the loop always gets there. */
static void make_room(clru_shard *cache) {
  while (cache->size >= cache->capacity) {
    if (!evict_lru(cache)) break;
  }
}

/* ========================================================================== */
/*                         CHMAP LOOKUP HELPER                                */
/* ========================================================================== */

/* Looks up the entry pointer stored in the internal chmap, or NULL when the
 * map doesn't hold the key. The caller must hold cache->mutex.
 *
 * A chmap_entry's SSO storage is naturally aligned, so this memcpy is
 * defense in depth rather than something alignment needs. It stays for
 * consistency with cjson, cyaml, cthreadcomm and chttpclient, which use the
 * same pattern for their own pointer storage inside a chmap. */
static clru_entry *map_lookup(clru_shard *cache, const cmap_pair *key_pair) {
  cmap_pair found;
  ccol_retval_t r = chmap_get_elem_ref(cache->map, key_pair, &found);
  if (r != ccol_success) return NULL;
  clru_entry *e;
  memcpy(&e, found.ptr, sizeof(e));
  return e;
}

/* Inserts or updates the entry pointer in the internal chmap. */
static ccol_retval_t map_upsert(clru_shard *cache, const cmap_pair *key_pair,
                                clru_entry *entry) {
  cmap_pair vp = {.ptr = &entry, .size = sizeof(entry)};
  return chmap_insert_elem(cache->map, key_pair, &vp);
}

/* ========================================================================== */
/*                         clrucache_create_full                              */
/* ========================================================================== */

/* How many segments a cache of this capacity gets, each with its own lock.
 *
 * More segments means less contention but also a coarser eviction order,
 * since each segment evicts from its own order. The count is bounded by the
 * capacity, so no segment starts with nothing to hold, and by a ceiling, so
 * a very large cache doesn't pay for segments that no realistic number of
 * threads would use at once. */
#define CLRU_MAX_SHARDS 16u

/* How many entries a segment has to be worth before a cache is split at
 * all.
 *
 * Splitting costs real capacity, not just capacity in theory. A hash spreads
 * the keys, so at any moment some segments hold more than their share and
 * others less, and a cache that's full to exactly its capacity then evicts
 * from the full segments while the others still have room. The smaller each
 * segment, the worse that unevenness gets, because the spread is relative to
 * the segment's size. Measured with randomly distributed keys, segments of
 * four entries keep about three quarters of a full cache, while segments of
 * this size keep the loss down to the few percent that the public
 * documentation gives as the cost of splitting.
 *
 * So this value does two jobs. A cache small enough for the loss to matter
 * stays unsplit and keeps one exact, global eviction order, and a cache
 * large enough for lock contention to be the real problem gets exactly as
 * many segments as it would without this value. For a small cache an exact
 * eviction order is worth more than a lock that fewer threads share. */
#define CLRU_MIN_ENTRIES_PER_SHARD 64u

static size_t shard_count_for(size_t capacity) {
  size_t n = capacity / CLRU_MIN_ENTRIES_PER_SHARD;
  if (n > CLRU_MAX_SHARDS) n = CLRU_MAX_SHARDS;
  if (n == 0) n = 1;
  return n;
}

/* Spreads the whole of a hash over its low bits before we reduce it to a
 * segment index.
 *
 * The reduction is a modulo by the segment count, and a modulo only reads
 * the low bits of what it gets. The map's hash is designed to be read from
 * its top bits, which is where the map takes its index from, and it makes
 * no promise about how its low bits spread. A modulo over a hash whose low
 * bits depend only on the key's low bits reads only the key's low bits, and
 * a key set that holds those bits constant is ordinary, not exotic: every
 * pointer from malloc is aligned to at least 16 bytes, and an identifier
 * scaled by a block size or a page size has as many trailing zeroes as that
 * size. Such a set would land entirely on one segment. The whole cache would
 * serialise on that segment's lock and be capped at that segment's share of
 * the capacity, evicting everything past it while the other segments stay
 * empty.
 *
 * The finalizer below rules that out whatever the map's hash looks like. It
 * stays the cache's concern and not the map's, because the map reads the
 * same hash from its own high bits; a poor spread costs a hash table some
 * probes, but a segment count partitions a fixed capacity. It's the standard
 * 64-bit avalanche, which makes every output bit depend on every input bit,
 * and it runs ahead of a mutex lock and a map lookup, so its few
 * instructions aren't the cost that matters here. */
static inline size_t clru_reduce(uint64_t h, size_t shard_count) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return (size_t)(h % (uint64_t)shard_count);
}

/* Gives the segment a key belongs to.
 *
 * Each segment has its own map, so two keys that a map treats as one key
 * must land on the same segment. Otherwise the cache would hold two
 * entries for one key and a later get couldn't find what a set stored. Byte
 * identity isn't the notion of key identity the map uses for every type it
 * accepts: -0.0 and 0.0 are one key for float and double, and a long double
 * is hashed and compared by value, because on some ABIs the type carries
 * padding whose bytes are often uninitialised, so reading them at all is a
 * hazard. Going through the map's key hash is what keeps a segmented cache
 * equivalent to an unsegmented one, and it also keeps this path from
 * reading bytes the caller never wrote.
 *
 * Returns NULL when key_pair->size doesn't match the width of a fixed-width
 * key type, which is what stops a read past the end of a wrongly sized
 * cmap_pair that a caller built. The check runs before the segment count is
 * looked at, so a malformed key reports ccol_invalid_args whether or not the
 * cache is big enough to have segments. */
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

/* Frees the router's allocation. Our copy of the caller's allocator lives
 * inside the router, so the free procedure is read out of it before the
 * router goes away. */
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
 * segment, so this is the whole construction of one: its own map, its own
 * eviction order and its own lock. */
static clru_shard *shard_create(size_t capacity, ccol_data_type key_type,
                                clru_remote_getter_t getter,
                                clru_remote_setter_t setter,
                                clru_eviction_cb_t eviction_cb,
                                ccol_memmgmt_procs_t *mprocs, char **err) {
  clru_shard *cache = (clru_shard *)_ccol_mem_calloc(mprocs, 1, sizeof(*cache));
  if (!cache) {
    if (err) *err = _CCOL_ERR_STR("failed to allocate cache segment");
    return NULL;
  }
  /* The segment borrows this instead of copying it. The router owns the one
   * copy of the caller's allocator and outlives every segment it creates. */
  cache->m_procs = mprocs;

  char *map_err = NULL;
  cache->map =
      chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, key_type,
                        ccol_pointer, cache->m_procs, NULL, NULL, &map_err);
  if (!cache->map) {
    if (err)
      *err = map_err ? map_err : _CCOL_ERR_STR("failed to create internal map");
    _ccol_mem_free(mprocs, cache);
    return NULL;
  }

  /* Set up the ring of LRU sentinels. A sentinel has no condition variable,
   * and nothing ever waits on one. */
  cache->lru_head.next = &cache->lru_tail;
  cache->lru_head.prev = NULL;
  cache->lru_tail.prev = &cache->lru_head;
  cache->lru_tail.next = NULL;

  /* We check the return value because POSIX allows pthread_mutex_init to
   * fail for real reasons, ENOMEM for example, and a handle carrying a mutex
   * that isn't fully initialized makes every later lock on it undefined
   * behavior. Same as the _ccol_cond_var_init check in entry_alloc in this
   * file. */
  if (_ccol_mutex_init(cache->mutex) != 0) {
    if (err) *err = _CCOL_ERR_STR("failed to initialize cache mutex");
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

/* Evicts everything a segment still holds and then frees the segment
 * itself. The eviction runs under the segment's lock, because the caller
 * supplies the eviction callback and every other path that calls it holds
 * that lock too. */
static void shard_destroy(clru_shard *cache) {
  if (!cache) return;
  _ccol_mutex_lock(cache->mutex);
  /* evict_lru() returns false exactly when the list is empty. No set can be
   * in flight once a destroy gets here, since the handle's pin count drains
   * every in-flight call first, so every entry this loop evicts leaves the
   * map too. */
  while (evict_lru(cache)) {
  }
  _ccol_mutex_unlock(cache->mutex);

  __chmap_destroy(cache->map);
  _ccol_mutex_destroy(cache->mutex);
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
  (void)val_type; /* val_type isn't needed; the map stores pointers */

  if (capacity == 0) {
    if (err) *err = _CCOL_ERR_STR("capacity must be > 0");
    return CLRU_CACHE_INVALID;
  }

  if (!_ccol_verify_memmgmt_procs(mprocs, err)) return CLRU_CACHE_INVALID;

  struct clrucache *router =
      (struct clrucache *)(mprocs ? mprocs->calloc(1, sizeof(*router))
                                  : calloc(1, sizeof(*router)));
  if (!router) {
    if (err) *err = _CCOL_ERR_STR("failed to allocate cache struct");
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
    if (err) *err = _CCOL_ERR_STR("failed to allocate cache segments");
    router_free_self(router);
    return CLRU_CACHE_INVALID;
  }

  /* The segment capacities add up to exactly the total the caller asked
   * for: the first `capacity % shard_count` segments get one extra entry
   * each, so nothing is lost to rounding. */
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

  /* Acquiring the slot is the LITERAL LAST step and runs once the cache is
   * fully constructed in every other respect. Same as the constructors of
   * chttpcli, chttpsvr and ccol_event_loop: no caller ever sees a handle
   * until this function is about to return success. Unlike ccol_event_loop
   * and ctpool, this module starts no threads of its own, so a failure here
   * has no thread to stop and only frees what was already built. */
  bool table_released = false;
  clru_cache h = _clrucache_handle_slot_acquire(router, &table_released);
  if (h == 0) {
    if (err)
      *err = table_released
                 ? _CCOL_ERR_STR(
                       "the clru_cache handle table is released at the end of "
                       "the process")
                 : _CCOL_ERR_STR("failed to allocate clru_cache handle slot");
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

  /* Resolve cache through the slot table and mark the slot not-in-use in
   * the same critical section as the lookup. That's what makes a second
   * destroy of the same handle value, concurrent or later and sequential,
   * see a resolve failure, so it never races this call's teardown. The
   * file-level comment of the slot table and the comment on
   * _clrucache_resolve describe the design. A stale handle reaching here,
   * or one already destroyed, is exactly the misuse this generation-tagged
   * handle design catches, and it's fatal instead of a silent use-after-free
   * or double-free. */
  _ccol_call_once(clrucache_slot_table.once,
                  _clrucache_slot_table_init_globals);
  uint32_t idx = (uint32_t)(cache >> 32);
  uint32_t gen = (uint32_t)(cache & 0xFFFFFFFFu);
  _ccol_rw_lock_wrlock(clrucache_slot_table.rwlock);
  clrucache_slot_t *slot = NULL;
  struct clrucache *raw = NULL;
  /* Once the table is released no handle names a cache, so a destroy at
   * that point is the stale-handle misuse below. */
  if (clrucache_slot_table.slots != NULL &&
      idx < cvector_elem_count(clrucache_slot_table.slots)) {
    clrucache_slot_t *s =
        (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
    if (s->in_use && s->generation == gen) {
      slot = s;
      raw = s->ptr;
    }
  }
  if (!raw) {
    _ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
    _ccol_fatal_err(
        "clrucache_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a clru_cache handle)");
  }
  slot->in_use = false; /* From this instant on, ALL future resolves for
                            this handle are blocked, and so is a second,
                            concurrent destroy attempt */
  /* Same step, same lock. From here on the table grants no new pin, which
   * is what lets the count below reach zero and stay there. */
  ccol_pintable_retire(&clrucache_pintable, idx);
  _ccol_rw_lock_unlock(clrucache_slot_table.rwlock);

  /* Wait for the pin count to reach 0 BEFORE any teardown logic runs at all,
   * not just before memory is freed, which is the same order as in chttpcli.
   * Waiting first is safe here, even though it isn't in ctpool. This
   * module's blocking waits are the _ccol_cond_var_wait loops that coalesce
   * a getter or a setter, and they're released by the remote_getter or
   * remote_setter call of some other application thread finishing for that
   * key. That mechanism has nothing to do with what destroy does, so no
   * pinned or blocked caller here depends on destroy-side logic to release
   * its own pin. */
  {
    long delay_ns = 1000;
    while (ccol_pintable_pins(&clrucache_pintable, idx) > 0) {
      struct timespec ts = {.tv_sec = 0, .tv_nsec = delay_ns};
      nanosleep(&ts, NULL);
      if (delay_ns < 1000000L) delay_ns *= 2;
    }
  }

  /* Evict every live entry that's left, calling the eviction callback for
   * each, then free every segment, and then the router itself. */
  router_destroy_shards(raw);
  router_free_self(raw);

  /* Release the slot last, only once the teardown and the free of raw are
   * complete. This is the step that marks the handle reusable (bumping the
   * slot's generation and pushing the index back onto the free list), and
   * nothing earlier does. We fetch the slot again by idx instead of reusing
   * `slot`, because a concurrent clrucache_create_full may run its own
   * _clrucache_handle_slot_acquire in between and reallocate the slots
   * array through cvector_push_back, which invalidates any pointer into that
   * array taken before this second lock. idx itself is stable. */
  _ccol_rw_lock_wrlock(clrucache_slot_table.rwlock);
  clrucache_slot_t *slot2 =
      (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* This takes the slot's generation past the value
      carried by the handle of the cache this call freed, so that stale handle
      can never match the generation of a FUTURE acquire for this same
      index */
  _clrucache_free_index_release_locked(idx);
  /* The process-exit destructor has already run and found this cache live.
     The release it couldn't do belongs to whichever call frees the last
     slot, and this call may be that one. */
  _clrucache_release_slot_table_if_deferred_locked();
  _ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS
/* Resolves h to the struct clrucache* behind it WITHOUT pinning it. It's a
 * bare slot-table lookup, safe for tests because the test code calling it
 * runs synchronously on one thread, so there's no concurrent destroy to
 * race. _clrucache_resolve needs a matching _unpin call and this one needs
 * none, so a test has nothing to remember; a forgotten unpin would leave a
 * pin outstanding on that cache and silently hang every future
 * clru_destroy against it. Returns NULL under exactly the same conditions
 * as _clrucache_resolve. */
struct clrucache *_clrucache_resolve_for_tests(clru_cache h) {
  _ccol_call_once(clrucache_slot_table.once,
                  _clrucache_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  _ccol_rw_lock_rdlock(clrucache_slot_table.rwlock);
  struct clrucache *raw = NULL;
  if (clrucache_slot_table.slots != NULL &&
      idx < cvector_elem_count(clrucache_slot_table.slots)) {
    clrucache_slot_t *slot =
        (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  _ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
  return raw;
}

/* Reads how many slots the clru_cache handle table holds right now, counting
 * the slots the table grew and those a release freed that no acquire has
 * reused yet. A test uses it to assert that a create/destroy loop reuses
 * freed slots; without that the table would grow without bound. */
/* The sum of the segment capacities, and the number of segments. Splitting
 * gives the first `capacity % shard_count` segments one extra entry each. A
 * caller can't observe any difference between a split that keeps that
 * remainder and one that drops it: clrucache_capacity() reports the number
 * the caller asked for either way, and the eviction slack of a full cache is
 * much wider than the one or two entries at stake. Reading the segments
 * directly is what lets a test pin it. */
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

/* The number of segments whose map hashes in chashmap's keyed mode, and in
 * *out_segments the number of segments. A test uses it to see that a flood
 * switched the segments' maps. */
size_t _clrucache_keyed_segment_maps_for_tests(clru_cache cache,
                                               size_t *out_segments) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) {
    if (out_segments) *out_segments = 0;
    return 0;
  }
  size_t keyed = 0;
  for (size_t i = 0; i < raw->shard_count; i++) {
    _ccol_mutex_lock(raw->shards[i]->mutex);
    if (chashmap_is_keyed_for_tests(raw->shards[i]->map)) keyed++;
    _ccol_mutex_unlock(raw->shards[i]->mutex);
  }
  if (out_segments) *out_segments = raw->shard_count;
  _clrucache_resolve_unpin(raw);
  return keyed;
}

/* The indices currently on the free list; read it together with the
 * capacity above. A roll-back that loses a slot shows up as a table that
 * grew, but only while the free list is empty, so a test that measures only
 * the growth would pass or fail depending on how many handles earlier tests
 * held at once. The two values together describe the table regardless.
 */
size_t _clrucache_free_index_count_for_tests(void) {
  _ccol_call_once(clrucache_slot_table.once,
                  _clrucache_slot_table_init_globals);
  _ccol_rw_lock_rdlock(clrucache_slot_table.rwlock);
  size_t n = clrucache_slot_table.free_indices
                 ? cvector_elem_count(clrucache_slot_table.free_indices)
                 : 0;
  _ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
  return n;
}

/* How many waiter references key_pair's entry holds right now, or -1 when
 * the cache doesn't hold the key. A thread blocked on the entry holds one,
 * and so does a thread running a remote getter or setter for it. A test
 * polls this to know that a getter really is waiting on an operation in
 * flight, instead of guessing with a sleep. */
int _clrucache_key_waiters_for_tests(clru_cache cache,
                                     const cmap_pair *key_pair) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return -1;
  int n = -1;
  clru_shard *shard = shard_for(raw, key_pair);
  if (shard) {
    _ccol_mutex_lock(shard->mutex);
    clru_entry *entry = map_lookup(shard, key_pair);
    if (entry) n = entry->waiters;
    _ccol_mutex_unlock(shard->mutex);
  }
  _clrucache_resolve_unpin(raw);
  return n;
}

size_t _clrucache_slot_table_capacity_for_tests(void) {
  _ccol_call_once(clrucache_slot_table.once,
                  _clrucache_slot_table_init_globals);
  _ccol_rw_lock_rdlock(clrucache_slot_table.rwlock);
  size_t n = clrucache_slot_table.slots
                 ? cvector_elem_count(clrucache_slot_table.slots)
                 : 0;
  _ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
  return n;
}

/*
 * Lets a test bias, without guaranteeing, which of two threads wins the race
 * for a segment's mutex. The race starts once a successful fetch or set
 * publishes an entry: the publishing thread STILL HOLDS that mutex when it
 * calls this, right before _ccol_cond_var_broadcast() and
 * _ccol_mutex_unlock(), and the broadcast is what first makes a coalesced
 * waiter on that entry start contending for the mutex. Holding the mutex
 * longer here gives an unrelated concurrent thread that wants the same mutex
 * more time, such as one running an eviction that removes the entry the
 * publish just added, so it can queue up for the mutex BEFORE anything wakes
 * the coalesced waiter. POSIX promises no FIFO order, but in practice the
 * thread already queued gets the mutex first after the release. That's how
 * a test exercises the path where a coalesced waiter survives a racing
 * eviction; without it the test would depend on rare scheduling luck that is
 * hard to reproduce. The flag doesn't disarm itself: a test arms it for
 * exactly the one call under test and must set it back to 0 itself, so
 * later unrelated tests in the same process aren't affected. */
static _Atomic unsigned int _clru_test_post_publish_delay_us = 0;
static _Atomic bool _clru_test_post_publish_delay_entered = false;

void clru_test_set_post_publish_delay_us(unsigned int delay_us) {
  atomic_store(&_clru_test_post_publish_delay_entered, false);
  atomic_store(&_clru_test_post_publish_delay_us, delay_us);
}

/* Lets a test poll for one fact: the publishing thread has entered its delay
 * and so still holds that segment's mutex right now. The test doesn't need
 * the mutex for this, and it couldn't safely take it here anyway, since
 * locking it would block the polling thread until the delay is over, which
 * defeats the point. With this, the test's own evictor thread can try to
 * lock the mutex at the earliest possible moment, and no userspace poll can
 * get closer than that. Without it the test would have to guess a fixed
 * sleep, which can fire too early, before the publishing thread has locked
 * the mutex again and with nothing to queue behind, or leave less margin
 * than the test intends. */
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

/* Frees the slot table's bookkeeping arrays at process exit so that the
 * leak-kind report of make memtest doesn't flag them as still reachable.
 * Same as _cleanup_event_loop_slot_table of ccol_event_loop; the comment on
 * that function gives the reasoning, including why this is only sound
 * when the application destroys every clru_cache it creates before process
 * exit. This test suite meets that precondition for a clean make memtest.
 * This function MUST use _ccol_call_once, because an
 * __attribute__((destructor)) function runs for the whole shared object,
 * whichever parts of it the process used: without _ccol_call_once, a
 * process that links this library and never creates a clru_cache would lock
 * a mutex here that no pthread_mutex_init call ever prepared. */
/* Tells whether any slot still names a cache. The caller holds the write
 * lock.
 *
 * The scan reads slot->ptr, not slot->in_use. A destroy clears in_use as its
 * first step so it can reject a second destroy or a new resolve as early as
 * possible, and the rest of the teardown, draining the pins and doing the
 * final locked release of the index, runs after that. A scan that trusted
 * in_use alone would free this table from under a destroy still in that
 * window, and that destroy's last step would then index the table. ptr is
 * only written once a slot is fully acquired and only cleared in that final
 * locked step, so it is true for exactly as long as the table must not be
 * released. */
static bool _clrucache_any_slot_live_locked(void) {
  size_t slot_count = cvector_elem_count(clrucache_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    clrucache_slot_t *slot =
        (clrucache_slot_t *)cvector_at(clrucache_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

/* Releases the table's bookkeeping and the pin index. The caller holds the
 * write lock and has already established that no slot is live.
 *
 * The pin index's slot storage is never released while the process runs, on
 * purpose, because a resolve indexes it without a lock; so a leak checker
 * that counts still-reachable memory as an error reports it at exit unless
 * it's released here. Each vector is set to NULL as it's destroyed, which
 * makes a later call answer "already released" instead of indexing a freed
 * vector. The rwlock isn't destroyed, because this can run from an ordinary
 * destroy that still holds that lock. */
/* The check and the release live in one out-of-line call, so the destroy
 * path that makes that call keeps the code shape it would have
 * without any of this. Cold code in a hot object file isn't free: inlined
 * here, the same few instructions measurably slow down the push path of an
 * unrelated container, because they shift what the linker places around it,
 * even though the instruction count doesn't change. */
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
  _ccol_call_once(clrucache_slot_table.once,
                  _clrucache_slot_table_init_globals);
  /* Nothing to do, and nothing safe to touch. Each vector is set to NULL as
     it's destroyed, so a second run of this function stops here instead of
     indexing a freed vector. */
  if (!clrucache_slot_table.slots) return;
  _ccol_rw_lock_wrlock(clrucache_slot_table.rwlock);
  /* Release only once nothing is left that could still resolve a handle.
   * The order of one translation unit's destructor against another's isn't
   * ours to decide, and without this check a later destructor still holding
   * a live handle would find the table and the pin index freed under it. In
   * that case we hand the release over to the destroy that frees the last
   * slot instead of skipping it, so an application that destroys its caches
   * leaves nothing behind whatever order the destructors run in. */
  if (_clrucache_any_slot_live_locked()) {
    clrucache_slot_table.release_deferred = true;
    _ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
    return;
  }
  _clrucache_release_slot_table_locked();
  _ccol_rw_lock_unlock(clrucache_slot_table.rwlock);
  /* We don't destroy the rwlock, on purpose. It has static storage duration
     so it holds nothing a leak checker would report, and the destructor
     can't own its lifetime anyway: the deferred branch above returns while
     the table is still live, and the release that comes later runs while
     holding this very lock, so there's no path where every user is provably
     done with the lock. Destroying it here would leave the entry points of
     the other branch with a read lock on a destroyed object, turning a
     stale-handle call, which is documented to fail cleanly through
     _ccol_fatal_err, into undefined behaviour. The order of destructors
     across translation units isn't ours to decide either. */
}

/* ========================================================================== */
/*                         clrucache_get_full                                 */
/* ========================================================================== */

static ccol_retval_t clrucache_get_full_retry(struct clrucache *raw,
                                              clru_shard *shard,
                                              const cmap_pair *key_pair,
                                              cmap_pair *val_out);

/* The body of clrucache_get_full(), entered with the segment's mutex held.
 * Every path that returns a result releases that mutex and the handle's pin
 * first.
 *
 * The one case that looks the key up again is a waiter that finds a
 * failed set. The entry point instantiates this body with again == NULL, and
 * there that case leaves through a call to clrucache_get_full_retry(),
 * because looping back to the lookup inside the entry point would keep more
 * values live across the whole function and cost the hit path a spill. The
 * cold function instantiates the body with again != NULL; there the body
 * reports the retry through *again, still holding the mutex and the pin, and
 * the cold function loops. */
static inline __attribute__((always_inline)) ccol_retval_t
clrucache_get_full_locked(struct clrucache *raw, clru_shard *shard,
                          const cmap_pair *key_pair, cmap_pair *val_out,
                          bool *again) {
  clru_entry *entry = map_lookup(shard, key_pair);

  if (!entry) {
    /* Cache miss */
    if (!shard->remote_getter) {
      _ccol_mutex_unlock(shard->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }

    /* Create a placeholder so other threads waiting for the same key can find
     * it and coalesce onto the single remote fetch that follows. */
    entry = entry_alloc(shard);
    if (!entry) {
      _ccol_mutex_unlock(shard->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }

    entry->key = _ccol_mem_alloc(shard->m_procs, key_pair->size);
    if (!entry->key) {
      /* Unlock before the free, like every other entry_free call site in this
       * file that frees an entry never published into cache->map. A
       * segment's lock must not stay held across the _ccol_mem_free and
       * _ccol_cond_var_destroy calls for the freed entry; without this unlock
       * those calls would block every other concurrent getter and setter on
       * this cache for as long as they take. */
      _ccol_mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }
    memcpy(entry->key, key_pair->ptr, key_pair->size);
    entry->key_size = key_pair->size;
    entry->fetch_in_progress = true;

    /* ccol_key_already_present is a documented SUCCESS outcome of
     * chmap_insert_elem: it means the map wrote the key's stored value, the
     * entry pointer here, in place. We treat it like ccol_success, as
     * every other chmap_insert_elem call site in this codebase does (see
     * cjson.c and clogger.c). It can't actually happen here, since
     * map_lookup() confirmed the key is absent under this same uninterrupted
     * hold of the mutex, but failing hard here wouldn't just be redundant, it
     * would be dangerous: the map already points at `entry` when
     * chmap_insert_elem returns, so freeing `entry` here, as a plain
     * out-of-memory failure does, would leave a dangling pointer for the very
     * next lookup of this key. */
    ccol_retval_t ins = map_upsert(shard, key_pair, entry);
    if (ins != ccol_success && ins != ccol_key_already_present) {
      _ccol_mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return ins;
    }

    /* This thread is the fetcher. Hold a reference across the window without
     * the mutex, exactly like the remote-setter window of clrucache_set_full.
     * See the locking discipline at the top of this file: a thread that
     * unlocks while holding a pointer to an entry takes a reference first,
     * and a placeholder is no exception. */
    entry->waiters++;
    _ccol_mutex_unlock(shard->mutex);

    cmap_pair fetched = {};
    bool fetch_ok = shard->remote_getter(key_pair, &fetched);

    _ccol_mutex_lock(shard->mutex);
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
        _ccol_cond_var_broadcast(entry->cond);
        _ccol_mutex_unlock(shard->mutex);
        _clrucache_resolve_unpin(raw);
        return ccol_not_enough_memory;
      }
      memcpy(copy, fetched.ptr, fetched.size);
      val_out->ptr = copy;
      val_out->size = fetched.size;

#ifdef RUNNING_UNIT_TESTS
      /* Widens the window in which an unrelated concurrent eviction can reach
       * and lock shard->mutex before the broadcast below wakes any coalesced
       * waiter. See the doc comment on the hook. */
      _clru_test_maybe_delay_post_publish();
#endif
      _ccol_cond_var_broadcast(entry->cond);
      _ccol_mutex_unlock(shard->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_success;
    } else {
      /* The remote getter failed. Remove the placeholder and notify the
       * waiters */
      if (fetched.ptr)
        _ccol_mem_free(shard->m_procs, fetched.ptr); /* size==0 edge case */
      cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
      chmap_delete_elem(shard->map, &kp);
      entry->evicted = true;
      entry->fetch_failed = true;
      _ccol_cond_var_broadcast(entry->cond);
      bool should_free = (entry->waiters == 0);
      _ccol_mutex_unlock(shard->mutex);
      if (should_free) entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }
  }

  /* The entry exists, so wait for any operation in progress to settle. A hit
   * on a settled entry is the common case, and the hint keeps its path
   * straight.
   */
  if (__builtin_expect(entry->fetch_in_progress || entry->set_in_progress, 0)) {
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      _ccol_cond_var_wait(entry->cond, shard->mutex);
    }
    entry->waiters--;

    if (!entry->value) {
      if (!entry->fetch_failed) {
        /* A set this call waited on failed and the key isn't in the cache:
         * either the set created the entry, or an eviction took the key while
         * the remote setter ran. Nobody asked the remote getter about this
         * key, so "not found" is not an answer this call is allowed to give.
         * Drop the reference and look the key up again. An absent key then
         * takes the miss path below, exactly like a get that ran after the
         * failed set; see the comment on fetch_failed. */
        if (entry->evicted && entry->waiters == 0) {
          /* Unlock around the free, like every other entry_free call site in
           * this file. The handle's pin keeps the segment alive while the
           * mutex isn't held. */
          _ccol_mutex_unlock(shard->mutex);
          entry_free(shard, entry);
          _ccol_mutex_lock(shard->mutex);
        }
        if (again) {
          *again = true;
          return ccol_success;
        }
        return clrucache_get_full_retry(raw, shard, key_pair, val_out);
      }
      /* The fetch this call coalesced onto failed, so there's nothing to give
       * back. The fetch may also have produced a value of the wrong size; see
       * the comment on fetch_size_mismatch. Either way every coalesced waiter
       * gets the same report as the thread that ran the remote getter, and we
       * don't downgrade it to the generic "not found" just because this
       * thread didn't run the fetch. */
      ccol_retval_t fail_r = entry->fetch_size_mismatch
                                 ? ccol_unexpected_failure
                                 : ccol_key_not_found;
      bool should_free = (entry->evicted && entry->waiters == 0);
      _ccol_mutex_unlock(shard->mutex);
      if (should_free) entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return fail_r;
    }
    /* entry->value isn't NULL, so the operation this call coalesced onto
     * succeeded. Fall through to the shared path below that hands the value
     * back, even when entry->evicted is now true. An unrelated concurrent
     * cache operation can evict this entry from the map and the LRU list
     * between the fetcher's or setter's broadcast and this thread's wakeup,
     * but entry itself and its value are still safe to read, because this
     * thread still holds a waiter reference on the entry, which is exactly
     * what keeps it alive until this thread is done. A coalesced waiter must
     * get the same result as the fetcher or the setter rather than throw away
     * a value that's right in front of it just because an eviction removed
     * the entry a moment later. */
  }

  /* Copy the value out, and refresh the entry's LRU position if it's still
   * live. */
  /* Invariant: entry->value != NULL here always means one of two things.
   * Either the entry is still LIVE (in_lru), or an eviction removed it after
   * a value was stored in it successfully, and then it isn't part of the LRU
   * list and nothing may treat it as if it were. An eviction removes the
   * entry from the map before setting evicted=true, so a non-NULL map_lookup
   * result that needed no wait is always a valid LIVE entry. */
  _ccol_assert(entry->value != NULL);
  _ccol_assert(entry->evicted || entry->in_lru);

  /* Allocate the caller's copy BEFORE promoting the entry's LRU position: a
   * call that fails here because it can't allocate must not also move the
   * entry to the front of the eviction order. */
  void *copy = _ccol_mem_alloc(shard->m_procs, entry->value_size);
  if (!copy) {
    bool should_free = (entry->evicted && entry->waiters == 0);
    _ccol_mutex_unlock(shard->mutex);
    if (should_free) entry_free(shard, entry);
    _clrucache_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }
  if (!entry->evicted) lru_move_to_front(shard, entry);
  memcpy(copy, entry->value, entry->value_size);
  val_out->ptr = copy;
  val_out->size = entry->value_size;

  bool should_free = (entry->evicted && entry->waiters == 0);
  _ccol_mutex_unlock(shard->mutex);
  if (should_free) entry_free(shard, entry);
  _clrucache_resolve_unpin(raw);
  return ccol_success;
}

/* The cold instantiation of clrucache_get_full_locked(). It repeats the
 * lookup for as long as a waiter keeps finding a failed set; see the comment
 * on fetch_failed. */
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
   * segment, so its lookup, its eviction order and the coalescing of
   * concurrent misses for it all happen here. NULL means the key's size
   * doesn't match the width of the declared key type, which the segment's
   * map would reject anyway; rejecting it here keeps the choice of segment
   * from reading past the end of the key. */
  clru_shard *shard = shard_for(raw, key_pair);
  if (!shard) {
    _clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  _ccol_mutex_lock(shard->mutex);
  return clrucache_get_full_locked(raw, shard, key_pair, val_out, NULL);
}

/* ========================================================================== */
/*                         clrucache_set_full                                 */
/* ========================================================================== */

/*
 * Helper. Allocates a new placeholder entry for the given key and inserts it
 * into the chmap. Returns the entry on success; on failure it returns NULL
 * and writes the real ccol_retval_t to *err_out, the same way the miss paths
 * of clrucache_get_full and __clrucache_get_into handle map_upsert's return
 * value. A failure other than out-of-memory, ccol_container_full for
 * example, must be reported as itself, never folded into a generic
 * ccol_not_enough_memory. The caller must hold cache->mutex. The helper
 * always returns with cache->mutex held, on success and on failure, and only
 * unlocks it briefly around its own entry_free() calls; see the comment at
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
    /* Unlock before the free and lock again before returning, so the helper's
     * "returns with cache->mutex held" contract holds whatever the outcome.
     * A segment's lock must not stay held across the _ccol_mem_free and
     * _ccol_cond_var_destroy calls for the freed entry; without this unlock
     * those calls would block every other concurrent getter and setter on
     * this cache for as long as they take. */
    _ccol_mutex_unlock(cache->mutex);
    entry_free(cache, e);
    _ccol_mutex_lock(cache->mutex);
    *err_out = ccol_not_enough_memory;
    return NULL;
  }
  memcpy(e->key, key_pair->ptr, key_pair->size);
  e->key_size = key_pair->size;

  /* ccol_key_already_present is a documented success outcome of
   * chmap_insert_elem, not a failure. The identical guard and comment in the
   * miss path of clrucache_get_full explain why. */
  ccol_retval_t ins = map_upsert(cache, key_pair, e);
  if (ins != ccol_success && ins != ccol_key_already_present) {
    _ccol_mutex_unlock(cache->mutex);
    entry_free(cache, e);
    _ccol_mutex_lock(cache->mutex);
    *err_out = ins;
    return NULL;
  }
  return e;
}

/*
 * Helper. Installs a value copy that the caller has already allocated into
 * an entry and makes the entry LIVE, evicting if it has to. It allocates
 * nothing and can't fail. Returns the entry's previous value (NULL for a
 * placeholder), which the caller frees after releasing cache->mutex. The
 * caller must hold cache->mutex.
 */
static void *entry_store_value(clru_shard *cache, clru_entry *entry,
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

ccol_retval_t clrucache_set_full(clru_cache cache, const cmap_pair *key_pair,
                                 const cmap_pair *val_pair) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return ccol_invalid_args;
  if (!key_pair || !key_pair->ptr || !key_pair->size || !val_pair ||
      !val_pair->ptr || val_pair->size == 0) {
    _clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  /* See the note in clrucache_get_full. The key picks one segment and every
   * step below stays inside it. */
  clru_shard *shard = shard_for(raw, key_pair);
  if (!shard) {
    _clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  /* The copy of the new value is made first, outside the mutex, so that the
   * only allocation of the set that can fail for this value fails before any
   * side effect: before the remote setter stores the value and before the
   * entry or the eviction order changes. A copy made after a successful
   * remote setter could fail while the remote store already holds the new
   * value, and the cache would then keep serving the old one as a hit. */
  void *new_val = _ccol_mem_alloc(shard->m_procs, val_pair->size);
  if (!new_val) {
    _clrucache_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }
  memcpy(new_val, val_pair->ptr, val_pair->size);

  _ccol_mutex_lock(shard->mutex);

  clru_entry *entry;

  /*
   * Loop until this thread owns the set slot for this key. One map_lookup
   * plus one create-or-wait isn't enough, because many setter threads can
   * block on the same in-progress operation and all wake up together once it
   * completes or fails. Without the loop the second thread to run would call
   * create_and_insert_placeholder for a key the first thread has already
   * inserted again, get ccol_key_already_present from chmap_insert_elem, and
   * wrongly return ccol_not_enough_memory to its caller. Looping back to
   * map_lookup finds the first thread's placeholder and waits on it, which
   * serialises the setters correctly.
   */
  for (;;) {
    entry = map_lookup(shard, key_pair);
    if (!entry) {
      ccol_retval_t create_err = ccol_not_enough_memory;
      entry = create_and_insert_placeholder(shard, key_pair, &create_err);
      if (!entry) {
        _ccol_mutex_unlock(shard->mutex);
        _ccol_mem_free(shard->m_procs, new_val);
        _clrucache_resolve_unpin(raw);
        return create_err;
      }
      break;
    }
    if (!entry->fetch_in_progress && !entry->set_in_progress) {
      break; /* The entry is live and this thread owns it */
    }
    /* A concurrent fetch or set is in progress; wait for it to finish */
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      _ccol_cond_var_wait(entry->cond, shard->mutex);
    }
    entry->waiters--;
    if (!entry->evicted) {
      break; /* The entry survived and this thread owns it */
    }
    /* Somebody cleaned the entry up while we waited. Loop back and check the
     * map again before creating a new placeholder, because another waiter may
     * create one first. */
    bool should_free = (entry->waiters == 0);
    if (should_free) {
      /* Unlock before the free, like every other entry_free call site in this
       * file. A segment's lock must not stay held across the _ccol_mem_free
       * and _ccol_cond_var_destroy calls for the freed entry; without this
       * unlock those calls would block every other concurrent getter and
       * setter on this cache for as long as they take. Locking again and
       * continuing the loop is safe: nothing touches `entry` again except the
       * next map_lookup() call, which assigns it a new value, and the pin this
       * function took at its start through _clrucache_resolve keeps `shard`
       * itself alive whether the mutex is held or not. */
      _ccol_mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      _ccol_mutex_lock(shard->mutex);
    }
  }

  /* This thread owns the set slot now */
  entry->set_in_progress = true;

  /* Without a remote setter the whole operation is a plain in-memory update,
   * so we never unlock the mutex, there's nothing to block on. We also don't
   * touch the LRU list before entry_store_value(), which handles both cases
   * correctly without help: !in_lru for a new entry or a placeholder, and
   * in_lru for an existing entry that moves to the front. That saves a
   * useless unlock and lock pair on every clru_set() call, which is the
   * common case for a cache with no remote source. */
  bool remote_ok = true;

  if (shard->remote_setter) {
    /* The set takes effect when the remote call returns, and only if it
     * succeeds. Until then a LIVE entry stays exactly as it is: at its old
     * place in the eviction order, and counted in the segment's size. So a
     * concurrent insert that needs room can evict it as the real least
     * recently used entry and never evicts some other key in its place, and
     * the segment never holds more entries than its capacity. See
     * evict_lru() for what such an eviction leaves behind. Shielding the
     * entry from eviction during the call looks safer, but once the set can
     * fail no serial order of the operations explains what that leaves.
     *
     * Hold a waiter reference so nothing frees the entry while this thread
     * is blocked in the remote call. */
    entry->waiters++;
    _ccol_mutex_unlock(shard->mutex);

    remote_ok = shard->remote_setter(key_pair, val_pair);

    _ccol_mutex_lock(shard->mutex);
    entry->waiters--;
  }

  ccol_retval_t retval = remote_ok ? ccol_success : ccol_unexpected_failure;

  /* entry->evicted is always false here, because set_in_progress keeps the
   * entry in the map for the whole call, and an eviction during the remote
   * call only turns a LIVE entry back into a placeholder (see evict_lru()).
   * So the entry is either LIVE (in_lru) or a placeholder without a value
   * (not in_lru), and a placeholder is either new or a LIVE entry that an
   * eviction took during the remote call.
   *
   * val_to_free is what this call frees once the mutex is released: the
   * entry's old value after a successful set (NULL for a placeholder), or the
   * unused copy after a failed remote setter. */
  void *val_to_free;
  if (remote_ok) {
    /* The copy already exists, so installing it can't fail, and the cache
     * agrees with the remote store as soon as the remote setter has
     * succeeded. */
    val_to_free = entry_store_value(shard, entry, new_val, val_pair->size);
  } else {
    val_to_free = new_val;
  }
  if (!remote_ok && !entry->in_lru) {
    /* The remote setter failed and the entry holds no value: either this call
     * created it, or an eviction took the key during the remote call. Either
     * way the key is absent from the cache once the set is undone, so we
     * remove the placeholder. A getter that waited on this set then looks the
     * key up again and takes the ordinary miss path, which runs the remote
     * getter if the cache has one.
     *
     * A LIVE entry needs nothing at all. The failed set never moved it, never
     * took it out of the segment's size and never touched its value, so the
     * cache is exactly as the concurrent operations alone left it, which is
     * what a set that never happened would leave. */
    cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
    chmap_delete_elem(shard->map, &kp);
    entry->evicted = true;
  }

  entry->set_in_progress = false;
#ifdef RUNNING_UNIT_TESTS
  _clru_test_maybe_delay_post_publish();
#endif
  _ccol_cond_var_broadcast(entry->cond);

  bool should_free = (entry->evicted && entry->waiters == 0);
  _ccol_mutex_unlock(shard->mutex);
  if (val_to_free) _ccol_mem_free(shard->m_procs, val_to_free);
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

/* The body of __clrucache_get_into(), entered with the segment's mutex held.
 * Every path that returns a result releases that mutex and the handle's pin
 * first.
 *
 * The only case that looks the key up again is a waiter that finds a
 * failed set. The entry point instantiates this body with again == NULL, and
 * there that case leaves through a call to clrucache_get_into_retry(),
 * because looping back to the lookup inside the entry point would keep more
 * values live across the whole function and cost the hit path a spill. The
 * cold function instantiates the body with again != NULL; there the body
 * reports the retry through *again, still holding the mutex and the pin, and
 * the cold function loops. */
static inline __attribute__((always_inline)) ccol_retval_t
clrucache_get_into_locked(struct clrucache *raw, clru_shard *shard,
                          const cmap_pair *key_pair, void *buf, size_t buf_size,
                          bool *again) {
  clru_entry *entry = map_lookup(shard, key_pair);

  if (!entry) {
    if (!shard->remote_getter) {
      _ccol_mutex_unlock(shard->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }

    entry = entry_alloc(shard);
    if (!entry) {
      _ccol_mutex_unlock(shard->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }

    entry->key = _ccol_mem_alloc(shard->m_procs, key_pair->size);
    if (!entry->key) {
      /* Unlock before the free. The identical comment in the miss path of
       * clrucache_get_full gives the details. */
      _ccol_mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }
    memcpy(entry->key, key_pair->ptr, key_pair->size);
    entry->key_size = key_pair->size;
    entry->fetch_in_progress = true;

    /* ccol_key_already_present is a documented success outcome of
     * chmap_insert_elem, not a failure. The identical guard and comment in
     * the miss path of clrucache_get_full have the full reasoning. */
    ccol_retval_t ins = map_upsert(shard, key_pair, entry);
    if (ins != ccol_success && ins != ccol_key_already_present) {
      _ccol_mutex_unlock(shard->mutex);
      entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return ins;
    }

    /* Hold a reference across the window without the mutex. See the
     * identical step in the miss path of clrucache_get_full, and the locking
     * discipline at the top of this file. */
    entry->waiters++;
    _ccol_mutex_unlock(shard->mutex);

    cmap_pair fetched = {};
    bool fetch_ok = shard->remote_getter(key_pair, &fetched);

    _ccol_mutex_lock(shard->mutex);
    entry->waiters--;
    entry->fetch_in_progress = false;

    if (fetch_ok && fetched.ptr && fetched.size > 0) {
      /* Reject a value whose size doesn't match the destination buffer
       * exactly. A larger value overflows buf, and a smaller one leaves the
       * tail of buf untouched, holding uninitialized or stale data from the
       * caller's stack or heap while the call still reports ccol_success, so
       * the caller of the type-inferred clru_get() macro gets a ValT that is
       * only partly filled. Caching a wrongly sized value would also make
       * every future __clrucache_get_into call for this key hit the same
       * problem, so any size mismatch counts as a complete fetch failure and
       * nothing gets cached. */
      if (fetched.size != buf_size) {
        _ccol_mem_free(shard->m_procs, fetched.ptr);
        cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
        chmap_delete_elem(shard->map, &kp);
        entry->evicted = true;
        entry->fetch_size_mismatch = true;
        entry->fetch_failed = true;
        _ccol_cond_var_broadcast(entry->cond);
        bool should_free = (entry->waiters == 0);
        _ccol_mutex_unlock(shard->mutex);
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
      _ccol_cond_var_broadcast(entry->cond);
      _ccol_mutex_unlock(shard->mutex);
      _clrucache_resolve_unpin(raw);
      return ccol_success;
    } else {
      if (fetched.ptr) _ccol_mem_free(shard->m_procs, fetched.ptr);
      cmap_pair kp = {.ptr = entry->key, .size = entry->key_size};
      chmap_delete_elem(shard->map, &kp);
      entry->evicted = true;
      entry->fetch_failed = true;
      _ccol_cond_var_broadcast(entry->cond);
      bool should_free = (entry->waiters == 0);
      _ccol_mutex_unlock(shard->mutex);
      if (should_free) entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return ccol_key_not_found;
    }
  }

  if (entry->fetch_in_progress || entry->set_in_progress) {
    entry->waiters++;
    while (entry->fetch_in_progress || entry->set_in_progress) {
      _ccol_cond_var_wait(entry->cond, shard->mutex);
    }
    entry->waiters--;

    if (!entry->value) {
      if (!entry->fetch_failed) {
        /* A set this call waited on failed and the key isn't in the cache:
         * either the set created the entry, or an eviction took the key while
         * the remote setter ran. Nobody asked the remote getter about this
         * key, so "not found" is not an answer this call is allowed to give.
         * Drop the reference and look the key up again. An absent key then
         * takes the miss path below, exactly like a get that ran after the
         * failed set; see the comment on fetch_failed. */
        if (entry->evicted && entry->waiters == 0) {
          /* Unlock around the free, like every other entry_free call site in
           * this file. The handle's pin keeps the segment alive while the
           * mutex isn't held. */
          _ccol_mutex_unlock(shard->mutex);
          entry_free(shard, entry);
          _ccol_mutex_lock(shard->mutex);
        }
        if (again) {
          *again = true;
          return ccol_success;
        }
        return clrucache_get_into_retry(raw, shard, key_pair, buf, buf_size);
      }
      /* The fetch this call coalesced onto failed, so there's nothing to give
       * back. The fetch may also have produced a value of the wrong size; see
       * the comment on fetch_size_mismatch. Either way every coalesced waiter
       * gets the same report as the thread that ran the remote getter, and we
       * don't downgrade it to the generic "not found" just because this
       * thread didn't run the fetch. */
      ccol_retval_t fail_r = entry->fetch_size_mismatch
                                 ? ccol_unexpected_failure
                                 : ccol_key_not_found;
      bool should_free = (entry->evicted && entry->waiters == 0);
      _ccol_mutex_unlock(shard->mutex);
      if (should_free) entry_free(shard, entry);
      _clrucache_resolve_unpin(raw);
      return fail_r;
    }
    /* entry->value isn't NULL, so the operation this call coalesced onto
     * succeeded. Fall through to the shared path below that hands the value
     * back, even when entry->evicted is now true. The wait branch of
     * clrucache_get_full has the same reasoning. */
  }

  /* Invariant: entry->value != NULL here always means one of two things.
   * Either the entry is still LIVE (in_lru), or an eviction removed it after
   * a value was stored in it successfully, and then it isn't part of the LRU
   * list and nothing may treat it as if it were. */
  _ccol_assert(entry->value != NULL);
  _ccol_assert(entry->evicted || entry->in_lru);

  /* Reject a size mismatch in either direction BEFORE promoting the entry's
   * LRU position; the fetch-path check above in this same function explains
   * why the sizes must be identical. Only an exact match may go into buf,
   * and a call we're about to reject must not also move the entry to the
   * front of the eviction order. */
  if (entry->value_size != buf_size) {
    bool should_free = (entry->evicted && entry->waiters == 0);
    _ccol_mutex_unlock(shard->mutex);
    if (should_free) entry_free(shard, entry);
    _clrucache_resolve_unpin(raw);
    return ccol_unexpected_failure;
  }
  if (!entry->evicted) lru_move_to_front(shard, entry);
  memcpy(buf, entry->value, entry->value_size);

  bool should_free = (entry->evicted && entry->waiters == 0);
  _ccol_mutex_unlock(shard->mutex);
  if (should_free) entry_free(shard, entry);
  _clrucache_resolve_unpin(raw);
  return ccol_success;
}

/* The cold instantiation of clrucache_get_into_locked(). It repeats the
 * lookup for as long as a waiter keeps finding a failed set; see the comment
 * on fetch_failed. */
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

  /* See the note in clrucache_get_full. The key picks one segment and every
   * step below stays inside it. */
  clru_shard *shard = shard_for(raw, key_pair);
  if (!shard) {
    _clrucache_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  _ccol_mutex_lock(shard->mutex);
  return clrucache_get_into_locked(raw, shard, key_pair, buf, buf_size, NULL);
}

/* ========================================================================== */
/*                         SIZE / CAPACITY QUERIES                            */
/* ========================================================================== */

size_t clrucache_size(clru_cache cache) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return 0;
  /* We add up the segment sizes, each under its own lock, so the total is a
   * snapshot rather than a value at one instant: it misses a concurrent
   * insert into a segment the loop has already counted. A total read under a
   * single lock would miss a concurrent insert during the read too.
   */
  size_t total = 0;
  for (size_t i = 0; i < raw->shard_count; i++) {
    clru_shard *sh = raw->shards[i];
    _ccol_mutex_lock(sh->mutex);
    total += sh->size;
    _ccol_mutex_unlock(sh->mutex);
  }
  _clrucache_resolve_unpin(raw);
  return total;
}

size_t clrucache_capacity(clru_cache cache) {
  struct clrucache *raw = _clrucache_resolve(cache);
  if (!raw) return 0;
  /* Fixed at creation and never written again, so no lock is needed. */
  size_t c = raw->capacity;
  _clrucache_resolve_unpin(raw);
  return c;
}
