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

#include <cthreadpool.h>
#include <cvector.h>
#include <errno.h>
#include <internal/catfork.h>
#include <internal/cdeadline.h>
#include <internal/cpintable.h>
#include <internal/test_helpers/common.h>
#include <internal/test_helpers/cthreadpool.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Initializes cond so that its timed waits measure deadlines on
// CLOCK_MONOTONIC. not_full is the one condition that a timed submit waits on,
// and its deadline comes from CLOCK_MONOTONIC too; a step of CLOCK_REALTIME
// would otherwise stretch the wait. Gives 0 or the first failing pthread
// status.
static int cthrpool_init_monotonic_cond_var(ccol__cond_var *cond) {
  ccol__cond_var_attr attr;
  int rv = ccol__cond_var_attr_init(attr);
  if (rv != 0) return rv;
  rv = ccol__cond_var_attr_setclock(attr, CLOCK_MONOTONIC);
  if (rv == 0) rv = ccol__cond_var_init_ca(*cond, attr);
  ccol__cond_var_attr_destroy(attr);
  return rv;
}

// One slot of the handle table. A cthrpool is (slot index << 32) | generation;
// the generation goes up on every reuse, so a stale handle never names a later
// pool and cthrpool__destroy can report a double destroy. ptr is NULL while the
// slot is free.
//
// torn_down is true from the moment a destroy clears in_use until the teardown
// is about to make ptr->mu unsafe to touch (destroyed, or freed on the
// foreign_since_fork path). The workers aren't joined yet in that window, so
// the fork handlers must still lock ptr->mu. It's never true together with
// in_use, and it's cleared under the table's write lock.
typedef struct {
  cthread_pool *ptr;
  uint32_t generation;
  bool in_use;
#if CCOL_FORK_SAFETY_REQUIRED
  bool torn_down;
#endif
} cthrpool_slot;

// The handle table. Only create, destroy, the fork handlers and test helpers
// take its rwlock; every other call resolves through the lock-free pin table.
// slots only grows, so an index stays valid for good; free_indices is a LIFO of
// released indices.
//
// lost_indices counts released slots whose index couldn't go back on the free
// list (the push failed to allocate). An acquire that finds the list empty
// scans for one of them. release_deferred is set when the exit destructor finds
// a pool still live; the destroy that releases the last slot then frees the
// table. Both fields are used only under the write lock.
static struct {
  ccol__rw_lock rwlock;
  ccol__once_flag once;
  cvec slots;
  cvec free_indices;
  size_t lost_indices;
  bool release_deferred;
} cthrpool_slot_table = {0};

#if CCOL_FORK_SAFETY_REQUIRED
static void cthrpool_atfork_prepare(void);
static void cthrpool_atfork_release(void);
static void cthrpool_atfork_child_release(void);
#endif

// Creates the handle table and registers the fork handlers; runs once on first
// use. The handlers are defined below struct cthread_pool, because they lock
// the mu of every live pool.
static void cthrpool_slot_table_init_globals(void) {
  if (ccol__rw_lock_init(cthrpool_slot_table.rwlock) != 0)
    ccol__fatal_err("cthrpool slot table: failed to initialize rwlock");
  cthrpool_slot_table.slots = cvector_create(sizeof(cthrpool_slot), NULL);
  if (!cthrpool_slot_table.slots)
    ccol__fatal_err("cthrpool slot table: failed to allocate slots vector");
  cthrpool_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!cthrpool_slot_table.free_indices)
    ccol__fatal_err(
        "cthrpool slot table: failed to allocate free-index vector");
#if CCOL_FORK_SAFETY_REQUIRED
  ccol__at_fork(cthrpool_atfork_prepare, cthrpool_atfork_release,
                cthrpool_atfork_child_release);
#endif
}

#if defined(CCOL__ATFORK_AT_LOAD)
// Registers the fork handlers at load time, on systems where a lazy
// registration from a thread is unsafe; see CCOL__ATFORK_AT_LOAD in
// common.h.
__attribute__((constructor)) static void cthrpool_atfork_at_load(void) {
  ccol__call_once(cthrpool_slot_table.once, cthrpool_slot_table_init_globals);
}
#endif

// Makes this module register its fork handlers now. Other modules of the
// library (chttpserver, for one) call it through internal/catfork.h before
// their own registration: pthread_atfork runs prepare handlers in reverse
// order, so their prepare handler then runs before ours, which avoids a lock
// order inversion between the two.
void cthrpool__ensure_atfork_registered_before_caller(void) {
  ccol__call_once(cthrpool_slot_table.once, cthrpool_slot_table_init_globals);
}

// One queued task. A callback task sets fn and on_complete, a future task sets
// fn_future and future; fn and fn_future share a union so the call needs no
// cast. next links the queue and the free list.
typedef struct cthrpool_task {
  union {
    void (*fn)(void *);
    void *(*fn_future)(void *);
  };
  void *arg;
  void (*on_complete)(void *, bool);
  cthrpool_future *future;
  struct cthrpool_task *next;
} cthrpool_task;

// A future, always allocated with plain malloc so its lifetime doesn't depend
// on any pool allocator. A submit gives it two references (caller and task),
// and every cthrpool_future_get in progress holds one more. The last reference
// out frees it.
struct cthrpool_future {
  ccol__mutex mu;
  ccol__cond_var cv;
  void *result;
  bool done;
  bool cancelled;
  int refcount;
};

// The hot half of the handle table: Handle to pointer, plus the pin that keeps
// a pool alive during a call. A resolve writes nothing that another thread
// reads, so submits from many threads don't contend on it.
static ccol_pintable cthrpool_pintable;

// One worker thread. The worker sets exited as its last write to the pool.
typedef struct cthrpool_worker_slot {
  ccol__thread_id id;
  atomic_bool exited;
} cthrpool_worker_slot;

// The pool. The queue and the free list of task nodes are intrusive lists, and
// mu guards everything here. queue_size, shutdown_drain and shutdown_immediate
// are written under mu with relaxed __atomic stores, because the refusal hint
// of a future try submit reads them without the lock.
//
// The free list keeps every node the recent traffic needed, up to
// task_free_list_cap (max(num_threads * 4, CTHRPOOL_TASK_FREE_LIST_CEILING)),
// so a deep queue doesn't cost an allocation per task. Its trim runs only at
// idle points (task_free_list_ran_dry, idle_points_since_trim), never on the
// submit or dequeue path, which would put another shared line in every critical
// section. The teardown must free the nodes on it.
//
// shutdown_complete is set, and shutdown_cv broadcast, once the call that does
// the shutdown has joined every worker; later shutdown calls wait for it.
// foreign_since_fork is set only by the child fork handler, for every pool
// inherited across fork(): Its workers don't exist in this process, so nothing
// may join them. shutdown_cv and the trim fields sit last, away from the lines
// that the submit and dequeue paths read.
struct cthread_pool {
  cthrpool_task *head;
  cthrpool_task *tail;
  size_t queue_size;
  size_t queue_cap;

  cthrpool_task *task_free_list;
  size_t task_free_list_size;
  size_t task_free_list_cap;

  ccol__mutex mu;
  ccol__cond_var not_empty;
  ccol__cond_var not_full;
  ccol__cond_var idle_cv;

  size_t active_count;

  bool shutdown_drain;
  bool shutdown_immediate;
  bool shutdown_started;
  bool shutdown_complete;

  cthrpool_worker_slot *threads;
  size_t num_threads;

  ccol_memmgmt_procs *m_procs;

  cthrpool self_handle;

#if CCOL_FORK_SAFETY_REQUIRED
  _Atomic bool foreign_since_fork;
#endif

  bool task_free_list_ran_dry;
  uint32_t idle_points_since_trim;

  ccol__cond_var shutdown_cv;
};

// cthrpool_lock_mu(p) locks the mu of pool p; every lock of a pool's mu goes
// through it. A test build also counts the locks each thread takes in
// cthrpool_mu_locks_taken, so a test can count the critical sections of one
// call; elsewhere it's the plain lock.
#ifdef RUNNING_UNIT_TESTS
static __thread unsigned long cthrpool_mu_locks_taken;
#define cthrpool_lock_mu(p) \
  (cthrpool_mu_locks_taken++, (void)ccol__mutex_lock((p)->mu))
#else
#define cthrpool_lock_mu(p) ccol__mutex_lock((p)->mu)
#endif

#if CCOL_FORK_SAFETY_REQUIRED
// The fork prepare handler: Takes the table's write lock and the mu of every
// slot that is in_use or torn_down, so the fork happens while no other thread
// holds any of them. fork() copies only the calling thread, so a lock another
// thread holds at that moment would stay locked in the child for good. A
// torn_down pool still has workers that can hold its mu. The default mutex type
// tracks no owner, so the release handlers may unlock from any thread, but a
// glibc rwlock checks the writer's thread ID, which differs in the child, so
// the child reinitializes it. Futures aren't covered: They have no registry to
// walk, and each of their critical sections is a few instructions long.
static void cthrpool_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  ccol__atfork_order_record(ccol__atfork_module_cthreadpool);
#endif
  ccol__rw_lock_wrlock(cthrpool_slot_table.rwlock);

  size_t n =
      cthrpool_slot_table.slots ? cvector_size(cthrpool_slot_table.slots) : 0;
  for (size_t i = 0; i < n; i++) {
    cthrpool_slot *slot =
        (cthrpool_slot *)cvector_at(cthrpool_slot_table.slots, i);
    if (!slot->in_use && !slot->torn_down) continue;
    ccol__mutex_lock(slot->ptr->mu);
  }
}

// Releases every lock that cthrpool_atfork_prepare took; is_child says which
// side of the fork runs it. In the child it also marks every in_use pool
// foreign_since_fork (its workers don't exist here), resets its pins (a parent
// thread that was inside a resolve can never unpin, and the destroy would wait
// for ever) and reinitializes its condition variables and the table's rwlock.
// A torn_down pool only needs its mu unlocked.
static void cthrpool_atfork_release_impl(bool is_child) {
  size_t n =
      cthrpool_slot_table.slots ? cvector_size(cthrpool_slot_table.slots) : 0;
  for (size_t i = 0; i < n; i++) {
    cthrpool_slot *slot =
        (cthrpool_slot *)cvector_at(cthrpool_slot_table.slots, i);
    if (!slot->in_use && !slot->torn_down) continue;
    cthread_pool *pool = slot->ptr;

    if (slot->in_use && is_child) {
      atomic_store(&pool->foreign_since_fork, true);
      ccol_pintable_reset_for(&cthrpool_pintable, pool->self_handle);

      if (ccol__cond_var_init(pool->not_empty) != 0)
        ccol__fatal_err("cthrpool atfork release: failed to reinit not_empty");
      if (cthrpool_init_monotonic_cond_var(&pool->not_full) != 0)
        ccol__fatal_err("cthrpool atfork release: failed to reinit not_full");
      if (ccol__cond_var_init(pool->idle_cv) != 0)
        ccol__fatal_err("cthrpool atfork release: failed to reinit idle_cv");
      if (ccol__cond_var_init(pool->shutdown_cv) != 0)
        ccol__fatal_err(
            "cthrpool atfork release: failed to reinit shutdown_cv");
    }

    ccol__mutex_unlock(pool->mu);
  }

  if (is_child) {
    if (ccol__rw_lock_reinit_in_child(cthrpool_slot_table.rwlock) != 0)
      ccol__fatal_err(
          "cthrpool atfork release: failed to reinit slot table rwlock");
  } else {
    ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
  }
}

// The parent's fork handler: Releases what cthrpool_atfork_prepare took.
static void cthrpool_atfork_release(void) {
  cthrpool_atfork_release_impl(false);
}

// The child's fork handler: Releases what cthrpool_atfork_prepare took and
// marks the inherited pools. It runs before fork() returns in the child, so
// before any code there can reach the shutdown or destroy of an inherited
// pool.
static void cthrpool_atfork_child_release(void) {
  cthrpool_atfork_release_impl(true);
}
#endif

static bool cthrpool_any_slot_live_locked(void);
static void cthrpool_release_slot_table_locked(void);
static void cthrpool_release_slot_table_if_deferred_locked(void);

// Waits until every caller that resolved pool before its slot was retired has
// unpinned, so the teardown never frees memory a caller still uses. It polls
// with a growing sleep because an unpin sends no wakeup, and it has no bound,
// since giving up would free memory in use. A pool whose handle was never
// published (failed create) has no slot to wait on.
static void cthrpool_wait_for_pins(cthread_pool *pool) {
  if (pool->self_handle == 0) return;
  uint32_t idx = (uint32_t)(pool->self_handle >> 32);
  long delay_ns = 1000;
  while (ccol_pintable_pins(&cthrpool_pintable, idx) > 0) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = delay_ns};
    nanosleep(&ts, NULL);
    if (delay_ns < 1000000L) delay_ns *= 2;
  }
}

// Resolves h to its pool and pins it, so a concurrent destroy waits until the
// caller is done with it. Every public call on a pool starts here. Gives NULL
// for 0, garbage, a free slot or a reused one. On success the caller must call
// cthrpool_resolve_unpin() exactly once. It takes no lock and makes no shared
// write, since every submit runs it.
static cthread_pool *cthrpool_resolve(cthrpool h) {
  return (cthread_pool *)ccol_pintable_pin(&cthrpool_pintable, h);
}

// Drops the pin that cthrpool_resolve took, when a public call is done with the
// pool. It touches nothing else: The pool can be freed right after, so nothing
// may read raw once the pin is gone.
static void cthrpool_resolve_unpin(cthread_pool *raw) {
  ccol_pintable_unpin(&cthrpool_pintable, raw->self_handle);
}

#ifdef RUNNING_UNIT_TESTS
// Test only: The next free list push fails like an allocation.
static _Atomic bool g_cthrpool_fail_next_free_index_push = false;

// Arms g_cthrpool_fail_next_free_index_push, so a test reaches the
// recovery of a lost slot index.
void cthrpool__force_next_free_index_push_failure_for_tests(void) {
  atomic_store(&g_cthrpool_fail_next_free_index_push, true);
}
#endif

// Puts idx back on the free list for the next create to reuse; the destroy and
// a failed acquire call it with the write lock held. A failed push is counted
// in lost_indices instead of stranding the slot for good.
static void cthrpool_free_index_release_locked(uint32_t idx) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_cthrpool_fail_next_free_index_push, false)) {
    cthrpool_slot_table.lost_indices++;
    return;
  }
#endif
  if (cvector_push_back(cthrpool_slot_table.free_indices, &idx) != ccol_success)
    cthrpool_slot_table.lost_indices++;
}

// Finds a released slot that no free list entry names (ptr NULL and not in_use)
// and puts its index in *out_idx, so a slot lost to a failed push gets reused
// instead of the table growing. The acquire calls it with the write lock held
// after finding the free list empty, which makes that state mean lost rather
// than free. A scan that finds nothing clears lost_indices.
static __attribute__((noinline)) bool cthrpool_reclaim_lost_index_locked(
    uint32_t *out_idx) {
  size_t slot_count = cvector_size(cthrpool_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    cthrpool_slot *slot =
        (cthrpool_slot *)cvector_at(cthrpool_slot_table.slots, i);
    if (slot->ptr == NULL && !slot->in_use) {
      *out_idx = (uint32_t)i;
      cthrpool_slot_table.lost_indices--;
      return true;
    }
  }
  cthrpool_slot_table.lost_indices = 0;
  return false;
}

// Gives pool a slot (a free one, a lost one or a new one), publishes it in the
// pin table and gives the handle; it's the last step of cthrpool_create_mp.
// Gives 0 when the table was released at unload, is full or can't grow.
// Generation 0 is skipped because handle 0 is CTHRPOOL_NULL. self_handle is
// written before the publish so a resolver finds it. A failed publish puts the
// slot back with both flags clear. Takes the write lock.
static cthrpool cthrpool_handle_slot_acquire(cthread_pool *pool) {
  ccol__call_once(cthrpool_slot_table.once, cthrpool_slot_table_init_globals);
  ccol__rw_lock_wrlock(cthrpool_slot_table.rwlock);
  if (!cthrpool_slot_table.slots) {
    ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
    return 0;
  }
  uint32_t idx;
  cthrpool_slot *slot;
  if (cvector_size(cthrpool_slot_table.free_indices) > 0) {
    cvector_pop_back(cthrpool_slot_table.free_indices, &idx);
    slot = (cthrpool_slot *)cvector_at(cthrpool_slot_table.slots, idx);
  } else if (cthrpool_slot_table.lost_indices > 0 &&
             cthrpool_reclaim_lost_index_locked(&idx)) {
    slot = (cthrpool_slot *)cvector_at(cthrpool_slot_table.slots, idx);
  } else {
    if (cvector_size(cthrpool_slot_table.slots) >= CCOL_PIN_MAX_SLOTS) {
      ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
      return 0;
    }
    cthrpool_slot fresh = {0};
    if (cvector_push_back(cthrpool_slot_table.slots, &fresh) != ccol_success) {
      ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
      return 0;
    }
    idx = (uint32_t)cvector_size(cthrpool_slot_table.slots) - 1;
    slot = (cthrpool_slot *)cvector_at(cthrpool_slot_table.slots, idx);
  }
  slot->generation++;
  if (slot->generation == 0) slot->generation++;
  cthrpool h = ((cthrpool)idx << 32) | (cthrpool)slot->generation;

  pool->self_handle = h;

  if (!ccol_pintable_publish(&cthrpool_pintable, idx, slot->generation, pool)) {
    pool->self_handle = 0;
    slot->ptr = NULL;
#if CCOL_FORK_SAFETY_REQUIRED
    slot->torn_down = false;
#endif
    cthrpool_free_index_release_locked(idx);
    ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
    return 0;
  }

  slot->ptr = pool;
  slot->in_use = true;
#if CCOL_FORK_SAFETY_REQUIRED
  slot->torn_down = false;
#endif
  ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
  return h;
}

// The thread-specific key that tells a worker which pool it serves. Each worker
// sets it once at start and never again, because a worker belongs to one pool
// for its whole life. It lets a task that waits on, shuts down or destroys its
// own pool be recognised: The join of the shutdown would otherwise get EDEADLK
// on the calling worker and carry on, and a destroy would free the pool under
// that worker. live tells whether the key was created.
static struct {
  ccol__thread_ls_key key;
  ccol__once_flag once;
  atomic_bool live;
} cthrpool_worker_key_bundle = {.once = CCOL__ONCE_INIT};

// Creates the worker key and sets live on success; the once of
// cthrpool_worker_key_bundle runs it on first need.
static void cthrpool_init_worker_key(void) {
  if (ccol__thread_ls_key_create(cthrpool_worker_key_bundle.key, NULL) == 0)
    atomic_store(&cthrpool_worker_key_bundle.live, true);
}

// Tells whether the worker key exists, creating it on the first call, so a
// create can refuse a pool whose workers couldn't mark themselves. Every other
// use of the key comes from a pool, and a pool exists only after this said
// true.
static bool cthrpool_worker_key_available(void) {
  ccol__call_once(cthrpool_worker_key_bundle.once, cthrpool_init_worker_key);
  return atomic_load(&cthrpool_worker_key_bundle.live);
}

// One pool whose discarded tasks the calling thread is completing. The frames
// live on the stack of cthrpool_discard_tasks; they form a stack because an
// on_complete callback there can shut down a second pool, whose discards then
// run nested.
typedef struct cthrpool_discard_frame {
  cthread_pool *pool;
  struct cthrpool_discard_frame *prev;
} cthrpool_discard_frame;

// The innermost discard frame of the calling thread, or NULL.
static __thread cthrpool_discard_frame *cthrpool_discarding_top = NULL;

// Tells whether the calling thread is a worker of pool, so a wait, shutdown,
// destroy or full-queue submit can refuse to wait for itself. That covers the
// pool's tasks and on_complete callbacks and the ran == false callbacks that
// this thread runs for pool (cthrpool_discarding_top). A thread with another
// pool's mark isn't one. A worker whose pthread_setspecific failed has no mark,
// so a thread without one is compared with the worker IDs, which are written
// before the handle is published. Exited workers are skipped, because glibc
// gives a joined thread's ID to the next thread that starts, and an inherited
// pool has no workers here. Every caller is a cold path.
static bool cthrpool_is_self_call(cthread_pool *pool) {
  for (const cthrpool_discard_frame *f = cthrpool_discarding_top; f;
       f = f->prev)
    if (f->pool == pool) return true;
  ccol__call_once(cthrpool_worker_key_bundle.once, cthrpool_init_worker_key);
  void *mark = ccol__thread_ls_get(cthrpool_worker_key_bundle.key);
  if (mark == (void *)pool) return true;
  if (mark != NULL) return false;
#if CCOL_FORK_SAFETY_REQUIRED
  if (atomic_load(&pool->foreign_since_fork)) return false;
#endif
  ccol__thread_id me = ccol__get_thread_id();
  for (size_t i = 0; i < pool->num_threads; i++) {
    if (ccol__thread_id_equal(pool->threads[i].id, me) &&
        !atomic_load_explicit(&pool->threads[i].exited, memory_order_acquire))
      return true;
  }
  return false;
}

// Frees the pool struct and the procs copy it owns, through the copy's own free
// function; the last step of a teardown or a failed create.
static void pool_free_self(cthread_pool *pool) {
  ccol_memmgmt_procs *mp = pool->m_procs;
  if (mp) {
    ccol_free free_fn = mp->free;
    free_fn(pool);
    free_fn(mp);
  } else {
    free(pool);
  }
}

// The cap on the task free list of a pool with few workers, in nodes (at most
// 1.25 MiB on LP64, and only after that many tasks were in flight at once).
#define CTHRPOOL_TASK_FREE_LIST_CEILING ((size_t)32768)

// How many idle points a trim waits for. A pool that keeps up with its producer
// goes idle between most tasks, and trimming each time would free nodes that
// the next burst allocates again.
#define CTHRPOOL_TASK_FREE_LIST_TRIM_IDLE_POINTS ((uint32_t)256)

// CTHRPOOL_TRIM_WINDOW() evaluates to the number of idle points between trims.
// A test build reads it from g_cthrpool_trim_window, so a test can shorten it
// through cthrpool__set_task_free_list_window_for_tests.
#ifdef RUNNING_UNIT_TESTS
static _Atomic uint32_t g_cthrpool_trim_window =
    CTHRPOOL_TASK_FREE_LIST_TRIM_IDLE_POINTS;
#define CTHRPOOL_TRIM_WINDOW() atomic_load(&g_cthrpool_trim_window)
// Sets g_cthrpool_trim_window, so a test reaches a trim with a few tasks; 0
// restores the shipped value.
void cthrpool__set_task_free_list_window_for_tests(uint32_t idle_points) {
  atomic_store(
      &g_cthrpool_trim_window,
      idle_points ? idle_points : CTHRPOOL_TASK_FREE_LIST_TRIM_IDLE_POINTS);
}
#else
#define CTHRPOOL_TRIM_WINDOW() CTHRPOOL_TASK_FREE_LIST_TRIM_IDLE_POINTS
#endif

// Trims the task free list at an idle point and unlocks pool->mu, which the
// caller holds (nothing is queued or running). At every trim window it gives
// half of what the list holds above num_threads * 4 back to the allocator,
// unless the list ran dry since the last trim. Nodes are unlinked under the
// lock and freed after it, since a custom free function may be slow. Out of
// line so the worker's completion path keeps its shape.
static __attribute__((noinline)) void cthrpool_idle_unlock_and_trim(
    cthread_pool *pool) {
  if (++pool->idle_points_since_trim < CTHRPOOL_TRIM_WINDOW()) {
    ccol__mutex_unlock(pool->mu);
    return;
  }
  pool->idle_points_since_trim = 0;
  bool ran_dry = pool->task_free_list_ran_dry;
  pool->task_free_list_ran_dry = false;
  size_t keep_min = pool->num_threads * 4;
  size_t size = pool->task_free_list_size;
  size_t excess = size > keep_min ? size - keep_min : 0;
  size_t to_free = ran_dry ? 0 : (excess + 1) / 2;
  cthrpool_task *freed = NULL;
  if (to_free > 0) {
    freed = pool->task_free_list;
    cthrpool_task *last = freed;
    for (size_t i = 1; i < to_free; i++) last = last->next;
    pool->task_free_list = last->next;
    last->next = NULL;
    pool->task_free_list_size -= to_free;
  }
  ccol__mutex_unlock(pool->mu);
  while (freed) {
    cthrpool_task *next = freed->next;
    ccol__mem_free(pool->m_procs, freed);
    freed = next;
  }
}

// Pushes a finished task's node onto the free list while it's under its cap, so
// the next submit doesn't allocate. The caller holds pool->mu. Gives false when
// the caller must free task itself, which it does only after unlocking.
// Separate from task_free so the worker can fold this into the critical section
// of its active_count decrement.
static bool task_release_locked(cthread_pool *pool, cthrpool_task *task) {
  bool recycled = pool->task_free_list_size < pool->task_free_list_cap;
  if (recycled) {
    task->next = pool->task_free_list;
    pool->task_free_list = task;
    pool->task_free_list_size++;
  }
  return recycled;
}

// Recycles a task node onto the free list, or frees it when the list is full;
// the discard path uses it for nodes no worker ran. Takes pool->mu.
static void task_free(cthread_pool *pool, cthrpool_task *task) {
  cthrpool_lock_mu(pool);
  bool recycled = task_release_locked(pool, task);
  ccol__mutex_unlock(pool->mu);

  if (!recycled) ccol__mem_free(pool->m_procs, task);
}

// Frees every node on the free list, so a teardown leaks none of the recycled
// nodes. It's called once no submit and no worker can touch the pool any more,
// so it takes no lock.
static void drain_task_free_list(cthread_pool *pool) {
  cthrpool_task *t = pool->task_free_list;
  pool->task_free_list = NULL;
  pool->task_free_list_size = 0;
  while (t) {
    cthrpool_task *next = t->next;
    ccol__mem_free(pool->m_procs, t);
    t = next;
  }
}

// Appends task to the queue for the workers to take; the caller holds
// pool->mu.
static void enqueue(cthread_pool *pool, cthrpool_task *task) {
  task->next = NULL;
  if (pool->tail) {
    pool->tail->next = task;
  } else {
    pool->head = task;
  }
  pool->tail = task;
  __atomic_store_n(&pool->queue_size, pool->queue_size + 1, __ATOMIC_RELAXED);
}

// Takes the head of a non-empty queue for a worker to run; the caller holds
// pool->mu.
static cthrpool_task *dequeue(cthread_pool *pool) {
  cthrpool_task *task = pool->head;
  pool->head = task->next;
  if (!pool->head) pool->tail = NULL;
  __atomic_store_n(&pool->queue_size, pool->queue_size - 1, __ATOMIC_RELAXED);
  return task;
}

// Test hooks that force a future allocation, a primitive init or a worker's
// thread create to fail. cthrpool_armed_fault holds the armed step, and
// cthrpool_worker_create_fault one more than the index of the failing worker
// (0 when nothing is armed).
#ifdef RUNNING_UNIT_TESTS
atomic_bool cthrpool__fail_next_future_alloc_for_tests;
atomic_long cthrpool__live_futures_for_tests;

static atomic_int cthrpool_armed_fault;
static atomic_size_t cthrpool_worker_create_fault;

// Arms cthrpool_armed_fault, so a test reaches the error path of one
// creation step.
void cthrpool__arm_fault_for_tests(cthrpool__fault step) {
  atomic_store(&cthrpool_armed_fault, (int)step);
}

// Arms cthrpool_worker_create_fault, so a test reaches a failed worker start;
// SIZE_MAX wraps it to 0, which disarms.
void cthrpool__fail_worker_create_at_for_tests(size_t index) {
  atomic_store(&cthrpool_worker_create_fault, index + 1);
}

// Tells whether the armed fault is step, and disarms it if so, so the forced
// failure hits exactly one thread once.
static bool cthrpool_fault_fires(cthrpool__fault step) {
  int expected = (int)step;
  return atomic_compare_exchange_strong(&cthrpool_armed_fault, &expected,
                                        (int)CTHRPOOL__FAULT_NONE);
}

// Tells whether the armed worker index is index, and disarms it if so.
static bool cthrpool_worker_create_fault_fires(size_t index) {
  size_t expected = index + 1;
  return atomic_compare_exchange_strong(&cthrpool_worker_create_fault,
                                        &expected, 0);
}

// CTHRPOOL_FAULT(step) and CTHRPOOL_WORKER_CREATE_FAULT(index) evaluate to true
// when a test armed a forced failure of that creation step or worker start, so
// the error paths can be tested. Without the test hooks they are constant
// false.
#define CTHRPOOL_FAULT(step) cthrpool_fault_fires(step)
#define CTHRPOOL_WORKER_CREATE_FAULT(index) \
  cthrpool_worker_create_fault_fires(index)
#else
#define CTHRPOOL_FAULT(step) false
#define CTHRPOOL_WORKER_CREATE_FAULT(index) false
#endif

// Allocated future blocks, partly built ones included.
#ifdef RUNNING_UNIT_TESTS
atomic_long cthrpool__future_blocks_for_tests;
#endif

// Allocates a zeroed future block, or gives NULL (also when a test fault
// fires). Every block goes back through future_block_free(), so a test build
// can count them.
static inline cthrpool_future *future_block_alloc(void) {
  if (CTHRPOOL_FAULT(CTHRPOOL__FAULT_FUTURE_ALLOC)) return NULL;
  cthrpool_future *f = (cthrpool_future *)calloc(1, sizeof(cthrpool_future));
#ifdef RUNNING_UNIT_TESTS
  if (f)
    atomic_fetch_add_explicit(&cthrpool__future_blocks_for_tests, 1,
                              memory_order_relaxed);
#endif
  return f;
}

// Frees a future block and counts it in a test build; the partner of
// future_block_alloc().
static inline void future_block_free(cthrpool_future *f) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_sub_explicit(&cthrpool__future_blocks_for_tests, 1,
                            memory_order_relaxed);
#endif
  free(f);
}

// Frees f when remaining is 0. It's the shared tail of every site that drops a
// future reference: Each decrements the count under f->mu, unlocks, and then
// calls this, and whichever thread sees 0 frees it.
static void future_destroy_if_unreferenced(cthrpool_future *f, int remaining) {
  if (remaining == 0) {
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_sub_explicit(&cthrpool__live_futures_for_tests, 1,
                              memory_order_relaxed);
#endif
    ccol__mutex_destroy(f->mu);
    ccol__cond_var_destroy(f->cv);
    future_block_free(f);
  }
}

// Drops one reference of f and frees it on the last one; cthrpool_future_free
// uses it. Locks f->mu itself, so the caller must not hold it.
static void future_deref(cthrpool_future *f) {
  ccol__mutex_lock(f->mu);
  int remaining = --f->refcount;
  ccol__mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
}

// Marks f done and cancelled, wakes every waiter and drops the task's
// reference. A shutdown uses it for the futures of discarded tasks.
static void future_cancel(cthrpool_future *f) {
  ccol__mutex_lock(f->mu);
  f->cancelled = true;
  f->done = true;
  ccol__cond_var_broadcast(f->cv);
  int remaining = --f->refcount;
  ccol__mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
}

// Takes the whole queue in one step, so an immediate shutdown or the teardown
// of an inherited pool can discard it; the caller holds pool->mu. The list must
// be discarded only after the unlock, because future->mu never nests under
// pool->mu.
static cthrpool_task *steal_queue(cthread_pool *pool) {
  cthrpool_task *list = pool->head;
  pool->head = NULL;
  pool->tail = NULL;
  __atomic_store_n(&pool->queue_size, 0, __ATOMIC_RELAXED);
  return list;
}

// Completes every task of a stolen list that no worker ran, so every accepted
// task still gets exactly one completion: Futures are cancelled and callback
// tasks get on_complete(arg, false). The caller doesn't hold pool->mu. While
// the callbacks run, a frame on cthrpool_discarding_top makes this thread a
// worker of pool, so a wait or shutdown of pool from a callback is a no-op and
// a destroy is fatal. recycle says whether nodes may go back on the free list
// or straight to m_procs (a pool nobody can submit to).
static void cthrpool_discard_tasks(cthread_pool *pool, cthrpool_task *list,
                                   bool recycle) {
  cthrpool_discard_frame frame = {pool, cthrpool_discarding_top};
  cthrpool_discarding_top = &frame;
  for (cthrpool_task *t = list; t;) {
    cthrpool_task *next = t->next;
    if (t->future)
      future_cancel(t->future);
    else if (t->on_complete)
      t->on_complete(t->arg, false);
    if (recycle)
      task_free(pool, t);
    else
      ccol__mem_free(pool->m_procs, t);
    t = next;
  }
  cthrpool_discarding_top = frame.prev;
}

// Computes the CLOCK_MONOTONIC deadline of a timed submit, timeout_us from now.
// It saturates instead of wrapping into the past. False when clock_gettime
// fails.
static bool make_abs_deadline(uint64_t timeout_us, struct timespec *abs_out) {
  return ccol_deadline_after_us(timeout_us, abs_out);
}

// Test hooks: Skip the worker mark, count running workers, and delay a worker's
// exit.
#ifdef RUNNING_UNIT_TESTS
atomic_bool cthrpool__skip_worker_mark_for_tests;

atomic_long cthrpool__running_workers_for_tests;
atomic_uint cthrpool__worker_exit_delay_us_for_tests;
#endif

// The body of one worker of pool (arg). It sets its key mark first (a failure
// is tolerated, since cthrpool_is_self_call falls back to the worker IDs), then
// takes tasks under pool->mu until an immediate shutdown, or a drain with an
// empty queue. A taken task signals not_full on a bounded queue. The node
// release goes in the same critical section as the active_count decrement, and
// the last task of an idle pool broadcasts idle_cv and may trim the free list.
// Marking its slot exited is the worker's last write to the pool.
static void *worker_thread_fn(void *arg) {
  cthread_pool *pool = (cthread_pool *)arg;

  ccol__call_once(cthrpool_worker_key_bundle.once, cthrpool_init_worker_key);
#ifdef RUNNING_UNIT_TESTS
  if (!atomic_load(&cthrpool__skip_worker_mark_for_tests))
#endif
    if (atomic_load(&cthrpool_worker_key_bundle.live))
      (void)ccol__thread_ls_set(cthrpool_worker_key_bundle.key, (void *)pool);

  for (;;) {
    cthrpool_lock_mu(pool);

    while (pool->queue_size == 0 && !pool->shutdown_immediate &&
           !pool->shutdown_drain) {
      ccol__cond_var_wait(pool->not_empty, pool->mu);
    }

    if (pool->shutdown_immediate ||
        (pool->shutdown_drain && pool->queue_size == 0)) {
      ccol__mutex_unlock(pool->mu);
      break;
    }

    cthrpool_task *task = dequeue(pool);
    pool->active_count++;

    if (pool->queue_cap > 0) {
      ccol__cond_var_signal(pool->not_full);
    }

    ccol__mutex_unlock(pool->mu);

    if (task->future) {
      void *result = task->fn_future(task->arg);
      ccol__mutex_lock(task->future->mu);
      task->future->result = result;
      task->future->done = true;
      ccol__cond_var_broadcast(task->future->cv);
      int remaining = --task->future->refcount;
      ccol__mutex_unlock(task->future->mu);
      future_destroy_if_unreferenced(task->future, remaining);
    } else {
      task->fn(task->arg);
      if (task->on_complete) {
        task->on_complete(task->arg, true);
      }
    }

    cthrpool_lock_mu(pool);
    bool recycled = task_release_locked(pool, task);
    pool->active_count--;
    if (pool->active_count == 0 && pool->queue_size == 0) {
      ccol__cond_var_broadcast(pool->idle_cv);
      cthrpool_idle_unlock_and_trim(pool);
    } else {
      ccol__mutex_unlock(pool->mu);
    }
    if (!recycled) ccol__mem_free(pool->m_procs, task);
  }

  ccol__thread_id me = ccol__get_thread_id();
  for (size_t i = 0; i < pool->num_threads; i++) {
    if (ccol__thread_id_equal(pool->threads[i].id, me)) {
      atomic_store_explicit(&pool->threads[i].exited, true,
                            memory_order_release);
      break;
    }
  }
#ifdef RUNNING_UNIT_TESTS
  unsigned delay_us = atomic_load(&cthrpool__worker_exit_delay_us_for_tests);
  if (delay_us) {
    struct timespec ts = {(time_t)(delay_us / 1000000u),
                          (long)(delay_us % 1000000u) * 1000L};
    nanosleep(&ts, NULL);
  }
  atomic_fetch_sub(&cthrpool__running_workers_for_tests, 1);
#endif
  return NULL;
}

// The idx that cthrpool_teardown_raw gets for a pool that never got a slot (the
// rollback of a failed create). No caller ever saw its handle, so there is no
// torn_down state to keep.
#define CTHRPOOL_TEARDOWN_NO_SLOT ((uint32_t)-1)

static void cthrpool_teardown_raw(cthread_pool *pool, uint32_t idx);

// Creates a pool of num_threads workers with the given queue capacity (0 for
// unbounded) and allocator, and gives its handle, or CTHRPOOL_NULL with
// *err_str set. It builds the pool, starts every worker and only then takes a
// slot, so an acquire failure has no published handle to worry about; that
// rollback must stop the running workers, which cthrpool_teardown_raw does. A
// failed thread create shuts down and joins the workers that did start. Needs
// the worker key (cthrpool_worker_key_bundle).
cthrpool cthrpool_create_mp(size_t num_threads, size_t queue_capacity,
                            ccol_memmgmt_procs *mprocs, char **err_str) {
  if (num_threads == 0) {
    if (err_str) *err_str = CCOL__ERR_STR("num_threads must be >= 1");
    return CTHRPOOL_NULL;
  }
  if (queue_capacity == ccol_invalid_size) {
    if (err_str)
      *err_str = CCOL__ERR_STR(
          "queue_capacity is ccol_invalid_size; pass 0 for an unbounded queue");
    return CTHRPOOL_NULL;
  }

  if (!ccol__verify_memmgmt_procs(mprocs, err_str)) return CTHRPOOL_NULL;

  if (!cthrpool_worker_key_available()) {
    if (err_str)
      *err_str = CCOL__ERR_STR(
          "the thread-local worker key is not available: it could not be "
          "created, or the library already released it at unload");
    return CTHRPOOL_NULL;
  }

  cthread_pool *pool =
      (cthread_pool *)(mprocs ? mprocs->calloc(1, sizeof(*pool))
                              : calloc(1, sizeof(*pool)));
  if (!pool) {
    if (err_str) *err_str = CCOL__ERR_STR("failed to allocate pool struct");
    return CTHRPOOL_NULL;
  }

  if (mprocs) {
    pool->m_procs = (ccol_memmgmt_procs *)mprocs->malloc(sizeof(*mprocs));
    if (!pool->m_procs) {
      mprocs->free(pool);
      if (err_str) *err_str = CCOL__ERR_STR("failed to allocate m_procs copy");
      return CTHRPOOL_NULL;
    }
    memcpy(pool->m_procs, mprocs, sizeof(*mprocs));
  }

  pool->queue_cap = queue_capacity;

  if (CTHRPOOL_FAULT(CTHRPOOL__FAULT_POOL_MUTEX) ||
      ccol__mutex_init(pool->mu) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("mutex init failed");
    pool_free_self(pool);
    return CTHRPOOL_NULL;
  }
  if (CTHRPOOL_FAULT(CTHRPOOL__FAULT_POOL_NOT_EMPTY) ||
      ccol__cond_var_init(pool->not_empty) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("cond_var init failed");
    ccol__mutex_destroy(pool->mu);
    pool_free_self(pool);
    return CTHRPOOL_NULL;
  }
  if (CTHRPOOL_FAULT(CTHRPOOL__FAULT_POOL_NOT_FULL) ||
      cthrpool_init_monotonic_cond_var(&pool->not_full) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("cond_var init failed");
    ccol__mutex_destroy(pool->mu);
    ccol__cond_var_destroy(pool->not_empty);
    pool_free_self(pool);
    return CTHRPOOL_NULL;
  }
  if (CTHRPOOL_FAULT(CTHRPOOL__FAULT_POOL_IDLE_CV) ||
      ccol__cond_var_init(pool->idle_cv) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("cond_var init failed");
    ccol__mutex_destroy(pool->mu);
    ccol__cond_var_destroy(pool->not_empty);
    ccol__cond_var_destroy(pool->not_full);
    pool_free_self(pool);
    return CTHRPOOL_NULL;
  }
  if (CTHRPOOL_FAULT(CTHRPOOL__FAULT_POOL_SHUTDOWN_CV) ||
      ccol__cond_var_init(pool->shutdown_cv) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("cond_var init failed");
    ccol__mutex_destroy(pool->mu);
    ccol__cond_var_destroy(pool->not_empty);
    ccol__cond_var_destroy(pool->not_full);
    ccol__cond_var_destroy(pool->idle_cv);
    pool_free_self(pool);
    return CTHRPOOL_NULL;
  }

#if CCOL_FORK_SAFETY_REQUIRED
  atomic_init(&pool->foreign_since_fork, false);
#endif

  pool->threads = (cthrpool_worker_slot *)ccol__mem_calloc(
      pool->m_procs, num_threads, sizeof(cthrpool_worker_slot));
  if (!pool->threads) {
    if (err_str) *err_str = CCOL__ERR_STR("failed to allocate threads array");
    ccol__mutex_destroy(pool->mu);
    ccol__cond_var_destroy(pool->not_empty);
    ccol__cond_var_destroy(pool->not_full);
    ccol__cond_var_destroy(pool->idle_cv);
    ccol__cond_var_destroy(pool->shutdown_cv);
    pool_free_self(pool);
    return CTHRPOOL_NULL;
  }

  pool->num_threads = num_threads;
  pool->task_free_list_cap = num_threads * 4 > CTHRPOOL_TASK_FREE_LIST_CEILING
                                 ? num_threads * 4
                                 : CTHRPOOL_TASK_FREE_LIST_CEILING;

  for (size_t i = 0; i < num_threads; i++) {
    if (CTHRPOOL_WORKER_CREATE_FAULT(i) ||
        ccol__thread_create(pool->threads[i].id, worker_thread_fn, pool) != 0) {
      cthrpool_lock_mu(pool);
      __atomic_store_n(&pool->shutdown_drain, true, __ATOMIC_RELAXED);
      pool->shutdown_started = true;
      ccol__cond_var_broadcast(pool->not_empty);
      ccol__mutex_unlock(pool->mu);
      for (size_t j = 0; j < i; j++) {
        ccol__thread_join(pool->threads[j].id);
      }
      if (err_str) *err_str = CCOL__ERR_STR("pthread_create failed");
      ccol__mem_free(pool->m_procs, pool->threads);
      ccol__mutex_destroy(pool->mu);
      ccol__cond_var_destroy(pool->not_empty);
      ccol__cond_var_destroy(pool->not_full);
      ccol__cond_var_destroy(pool->idle_cv);
      ccol__cond_var_destroy(pool->shutdown_cv);
      pool_free_self(pool);
      return CTHRPOOL_NULL;
    }
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&cthrpool__running_workers_for_tests, 1);
#endif
  }

  cthrpool h = cthrpool_handle_slot_acquire(pool);
  if (h == 0) {
    if (err_str)
      *err_str = CCOL__ERR_STR("failed to allocate cthrpool handle slot");
    cthrpool_teardown_raw(pool, CTHRPOOL_TEARDOWN_NO_SLOT);
    return CTHRPOOL_NULL;
  }

  return h;
}

// Test hooks: Make the next timed wait for room report ETIMEDOUT after a real
// wake, and record what the last timed wait of each thread did.
#ifdef RUNNING_UNIT_TESTS
atomic_bool cthrpool__timeout_after_wake_for_tests;
atomic_bool cthrpool__timeout_after_wake_waiting_for_tests;

static __thread cthrpool__timed_wait_record cthrpool_timed_wait_record;
#endif

// Checks for a shutdown and waits for room in the queue, for every submit. The
// caller holds pool->mu; on ccol_success it's still held and the queue has
// room, on any other result it's released. block is 0 for try, 1 to wait and 2
// to wait until abs_deadline. A waiting submit from one of pool's own workers
// gives ccol_container_full instead of waiting, since only the workers can free
// a slot and all of them could end up waiting here. After a failed timed wait
// the queue state decides, not the return code: The wait may have consumed a
// dequeue's signal as its deadline passed. Always inlined, since each submit
// passes a constant block.
static inline __attribute__((always_inline)) ccol_retval
submit_wait_for_room_locked(cthread_pool *pool, int block,
                            const struct timespec *abs_deadline) {
  if (pool->shutdown_drain || pool->shutdown_immediate) {
    ccol__mutex_unlock(pool->mu);
    return ccol_not_permitted;
  }

  if (pool->queue_cap > 0) {
    while (pool->queue_size >= pool->queue_cap && !pool->shutdown_drain &&
           !pool->shutdown_immediate) {
      if (block == 0) {
        ccol__mutex_unlock(pool->mu);
        return ccol_container_full;
      } else if (cthrpool_is_self_call(pool)) {
        ccol__mutex_unlock(pool->mu);
        return ccol_container_full;
      } else if (block == 1) {
        ccol__cond_var_wait(pool->not_full, pool->mu);
      } else {
        int rc;
#ifdef RUNNING_UNIT_TESTS
        if (!cthrpool_timed_wait_record.waited) {
          cthrpool_timed_wait_record.waited = true;
          cthrpool_timed_wait_record.deadline = *abs_deadline;
          clock_gettime(CLOCK_MONOTONIC,
                        &cthrpool_timed_wait_record.wait_started);
        }
        if (atomic_exchange(&cthrpool__timeout_after_wake_for_tests, false)) {
          atomic_store(&cthrpool__timeout_after_wake_waiting_for_tests, true);
          ccol__cond_var_wait(pool->not_full, pool->mu);
          rc = ETIMEDOUT;
        } else
#endif
          rc =
              ccol__cond_var_timedwait(pool->not_full, pool->mu, *abs_deadline);
        if (rc != 0 && pool->queue_size >= pool->queue_cap &&
            !pool->shutdown_drain && !pool->shutdown_immediate) {
#ifdef RUNNING_UNIT_TESTS
          cthrpool_timed_wait_record.timed_out = rc == ETIMEDOUT;
          clock_gettime(CLOCK_MONOTONIC,
                        &cthrpool_timed_wait_record.wait_ended);
#endif
          ccol__mutex_unlock(pool->mu);
          return rc == ETIMEDOUT ? ccol_timed_out : ccol_unexpected_failure;
        }
      }
    }
    if (pool->shutdown_drain || pool->shutdown_immediate) {
      ccol__mutex_unlock(pool->mu);
      return ccol_not_permitted;
    }
  }

  return ccol_success;
}

// Takes a node from the free list and enqueues the task in one pool->mu
// critical section. When the list is empty it allocates outside the lock and
// runs the checks again. A submitter that leaves after an allocation failure
// passes the not_full signal on, since it may have used up the one meant for
// another waiter. not_empty is signalled on every enqueue: Skipping it when no
// worker seems to wait makes a burst drain on fewer threads. A callback task
// passes fn and on_complete, a future task fn_future and future (allocated by
// the caller); the NULLs are constants, so each instantiation holds only its
// own code.
static inline __attribute__((always_inline)) ccol_retval
submit_pop_and_enqueue_body(cthread_pool *pool, void (*fn)(void *),
                            void *(*fn_future)(void *), void *arg,
                            void (*on_complete)(void *, bool),
                            cthrpool_future *future, int block,
                            const struct timespec *abs_deadline) {
  cthrpool_task *spare = NULL;
  cthrpool_lock_mu(pool);
  for (;;) {
    ccol_retval r = submit_wait_for_room_locked(pool, block, abs_deadline);
    if (r != ccol_success) {
      if (spare) ccol__mem_free(pool->m_procs, spare);
      return r;
    }
    cthrpool_task *task = pool->task_free_list;
    if (task) {
      pool->task_free_list = task->next;
      if (--pool->task_free_list_size == 0) pool->task_free_list_ran_dry = true;
    } else {
      pool->task_free_list_ran_dry = true;
      task = spare;
      spare = NULL;
    }
    if (!task) {
      ccol__mutex_unlock(pool->mu);
      spare = (cthrpool_task *)ccol__mem_calloc(pool->m_procs, 1,
                                                sizeof(cthrpool_task));
      if (!spare) {
        cthrpool_lock_mu(pool);
        ccol__cond_var_signal(pool->not_full);
        ccol__mutex_unlock(pool->mu);
        return ccol_not_enough_memory;
      }
      cthrpool_lock_mu(pool);
      continue;
    }
    memset(task, 0, sizeof(*task));
    if (future) {
      task->fn_future = fn_future;
      task->arg = arg;
      task->future = future;
    } else {
      task->fn = fn;
      task->arg = arg;
      task->on_complete = on_complete;
    }
    enqueue(pool, task);
    ccol__cond_var_signal(pool->not_empty);
    ccol__mutex_unlock(pool->mu);
    if (spare) ccol__mem_free(pool->m_procs, spare);
    return ccol_success;
  }
}

// Queues a callback task. Always inlined so each plain submit holds the whole
// path with no extra call.
static inline __attribute__((always_inline)) ccol_retval
submit_pop_and_enqueue(cthread_pool *pool, void (*fn)(void *), void *arg,
                       void (*on_complete)(void *, bool), int block,
                       const struct timespec *abs_deadline) {
  return submit_pop_and_enqueue_body(pool, fn, NULL, arg, on_complete, NULL,
                                     block, abs_deadline);
}

// Queues a future task. future must not be NULL; on failure the queue doesn't
// hold it and the caller keeps both its references.
static ccol_retval submit_future_pop_and_enqueue(
    cthread_pool *pool, void *(*fn_future)(void *), void *arg,
    cthrpool_future *future, int block, const struct timespec *abs_deadline) {
  return submit_pop_and_enqueue_body(pool, NULL, fn_future, arg, NULL, future,
                                     block, abs_deadline);
}

// Queues a callback task for the three plain submits: Resolves pool, checks fn,
// computes the deadline for block == 2 (after those checks and before any node
// is taken), submits and unpins.
static inline __attribute__((always_inline)) ccol_retval submit_generic(
    cthrpool pool, void (*fn)(void *), void *arg,
    void (*on_complete)(void *, bool), int block, uint64_t timeout_us) {
  cthread_pool *raw = cthrpool_resolve(pool);
  if (!raw) return ccol_invalid_args;
  if (!fn) {
    cthrpool_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  struct timespec deadline;
  const struct timespec *abs_deadline = NULL;
  if (block == 2) {
    if (!make_abs_deadline(timeout_us, &deadline)) {
      cthrpool_resolve_unpin(raw);
      return ccol_unexpected_failure;
    }
    abs_deadline = &deadline;
  }
  ccol_retval r =
      submit_pop_and_enqueue(raw, fn, arg, on_complete, block, abs_deadline);
  cthrpool_resolve_unpin(raw);
  return r;
}

// Queues fn(arg) with on_complete, waiting for room on a full bounded queue;
// the public blocking submit.
ccol_retval cthrpool_submit(cthrpool pool, void (*fn)(void *), void *arg,
                            void (*on_complete)(void *, bool)) {
  return submit_generic(pool, fn, arg, on_complete, 1, 0);
}

// Queues fn(arg) with on_complete, or gives ccol_container_full at once on a
// full bounded queue; the public submit that never blocks.
ccol_retval cthrpool_try_submit(cthrpool pool, void (*fn)(void *), void *arg,
                                void (*on_complete)(void *, bool)) {
  return submit_generic(pool, fn, arg, on_complete, 0, 0);
}

// Queues fn(arg) with on_complete, waiting at most timeout_us for room; the
// public timed submit. A timeout of 0 goes to cthrpool_try_submit() before any
// resolve, so that path pays for one resolve only.
ccol_retval cthrpool_timed_submit(cthrpool pool, void (*fn)(void *), void *arg,
                                  void (*on_complete)(void *, bool),
                                  uint64_t timeout_us) {
  if (timeout_us == 0) return cthrpool_try_submit(pool, fn, arg, on_complete);
  return submit_generic(pool, fn, arg, on_complete, 2, timeout_us);
}

// Allocates a submit's future with a refcount of 2 (caller and task), or gives
// NULL. A submit calls it before taking the pool lock, so the critical section
// allocates nothing.
static cthrpool_future *submit_future_alloc(void) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&cthrpool__fail_next_future_alloc_for_tests, false))
    return NULL;
#endif
  cthrpool_future *f = future_block_alloc();
  if (!f) return NULL;
  if (CTHRPOOL_FAULT(CTHRPOOL__FAULT_FUTURE_MUTEX) ||
      ccol__mutex_init(f->mu) != 0) {
    future_block_free(f);
    return NULL;
  }
  if (CTHRPOOL_FAULT(CTHRPOOL__FAULT_FUTURE_CV) ||
      ccol__cond_var_init(f->cv) != 0) {
    ccol__mutex_destroy(f->mu);
    future_block_free(f);
    return NULL;
  }
  f->refcount = 2;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&cthrpool__live_futures_for_tests, 1,
                            memory_order_relaxed);
#endif
  return f;
}

// Frees a future that no queue ever held, when its submit failed after
// allocating it.
static void submit_future_discard(cthrpool_future *f) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_sub_explicit(&cthrpool__live_futures_for_tests, 1,
                            memory_order_relaxed);
#endif
  ccol__mutex_destroy(f->mu);
  ccol__cond_var_destroy(f->cv);
  future_block_free(f);
}

// Predicts what a future try submit would get, from the pool state read without
// the lock: ccol_not_permitted after a shutdown started, ccol_container_full
// while a bounded queue is full, else ccol_success. A refused try then neither
// allocates nor locks. ccol_success is only a hint; the critical section checks
// again under the lock.
static ccol_retval submit_future_refusal_hint(cthread_pool *pool) {
  if (__atomic_load_n(&pool->shutdown_drain, __ATOMIC_RELAXED) ||
      __atomic_load_n(&pool->shutdown_immediate, __ATOMIC_RELAXED))
    return ccol_not_permitted;
  size_t cap = pool->queue_cap;
  if (cap > 0 && __atomic_load_n(&pool->queue_size, __ATOMIC_RELAXED) >= cap)
    return ccol_container_full;
  return ccol_success;
}

// Queues a future task for the three future submits (block as in
// submit_generic) and puts the future in *out. It sets *out to NULL first, so
// every failure path leaves it NULL; every caller has already checked that out
// isn't NULL. A try asks the refusal hint before it allocates the future.
static ccol_retval submit_future_generic(cthrpool pool, void *(*fn)(void *),
                                         void *arg, int block,
                                         uint64_t timeout_us,
                                         cthrpool_future **out) {
  *out = NULL;

  cthread_pool *raw = cthrpool_resolve(pool);
  if (!raw) return ccol_invalid_args;
  if (!fn) {
    cthrpool_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  struct timespec deadline;
  const struct timespec *abs_deadline = NULL;
  if (block == 2) {
    if (!make_abs_deadline(timeout_us, &deadline)) {
      cthrpool_resolve_unpin(raw);
      return ccol_unexpected_failure;
    }
    abs_deadline = &deadline;
  } else if (block == 0) {
    ccol_retval hint = submit_future_refusal_hint(raw);
    if (hint != ccol_success) {
      cthrpool_resolve_unpin(raw);
      return hint;
    }
  }

  cthrpool_future *f = submit_future_alloc();
  if (!f) {
    cthrpool_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }

  ccol_retval r =
      submit_future_pop_and_enqueue(raw, fn, arg, f, block, abs_deadline);
  if (r != ccol_success)
    submit_future_discard(f);
  else
    *out = f;
  cthrpool_resolve_unpin(raw);
  return r;
}

// Queues fn(arg) and gives its future, waiting for room on a full bounded
// queue; the public blocking future submit. NULL on failure, with the code in
// *rv_out when rv_out isn't NULL.
cthrpool_future *cthrpool_submit_future(cthrpool pool, void *(*fn)(void *),
                                        void *arg, ccol_retval *rv_out) {
  cthrpool_future *f;
  ccol_retval r = submit_future_generic(pool, fn, arg, 1, 0, &f);
  if (rv_out) *rv_out = r;
  return f;
}

// Queues fn(arg) and puts its future in *out, or fails at once on a full
// bounded queue; the public future submit that never blocks.
ccol_retval cthrpool_try_submit_future(cthrpool pool, void *(*fn)(void *),
                                       void *arg, cthrpool_future **out) {
  if (!out) return ccol_invalid_args;
  return submit_future_generic(pool, fn, arg, 0, 0, out);
}

// Queues fn(arg) and puts its future in *out, waiting at most timeout_us for
// room; the public timed future submit. A timeout of 0 is a try.
ccol_retval cthrpool_timed_submit_future(cthrpool pool, void *(*fn)(void *),
                                         void *arg, uint64_t timeout_us,
                                         cthrpool_future **out) {
  if (!out) return ccol_invalid_args;
  if (timeout_us == 0) return submit_future_generic(pool, fn, arg, 0, 0, out);
  return submit_future_generic(pool, fn, arg, 2, timeout_us, out);
}

// Blocks until f is done and gives its result (NULL when cancelled), so the
// caller can collect what the task computed. Each waiter holds its own
// reference for the whole call, taken and dropped under f->mu. Without it the
// other references can go while the thread waits (the completion broadcast
// wakes every waiter at once) and the future would be freed under it.
void *cthrpool_future_get(cthrpool_future *f) {
  if (!f) return NULL;
  ccol__mutex_lock(f->mu);
  f->refcount++;
  while (!f->done) {
    ccol__cond_var_wait(f->cv, f->mu);
  }
  void *result = f->result;
  int remaining = --f->refcount;
  ccol__mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
  return result;
}

// Tells whether f is done (finished or cancelled), read under f->mu, for a
// caller that polls instead of blocking.
bool cthrpool_future_done(cthrpool_future *f) {
  if (!f) return false;
  ccol__mutex_lock(f->mu);
  bool done = f->done;
  ccol__mutex_unlock(f->mu);
  return done;
}

// Tells whether f was cancelled, read under f->mu, so a caller can tell a
// cancelled task from one that returned NULL.
bool cthrpool_future_cancelled(cthrpool_future *f) {
  if (!f) return false;
  ccol__mutex_lock(f->mu);
  bool cancelled = f->cancelled;
  ccol__mutex_unlock(f->mu);
  return cancelled;
}

// Drops the caller's reference of f, which is how a caller releases a future;
// the last reference frees it.
void cthrpool_future_free(cthrpool_future *f) {
  if (!f) return;
  future_deref(f);
}

// Creates a future with no pool behind it, for a producer that isn't a
// cthrpool worker. It starts with two references, one for the caller and one
// for the producer. Gives NULL with *err_str set on failure.
cthrpool_future *cthrpool_future_create_detached(char **err_str) {
  cthrpool_future *f = future_block_alloc();
  if (!f) {
    if (err_str) *err_str = CCOL__ERR_STR("failed to allocate future");
    return NULL;
  }
  if (CTHRPOOL_FAULT(CTHRPOOL__FAULT_FUTURE_MUTEX) ||
      ccol__mutex_init(f->mu) != 0) {
    future_block_free(f);
    if (err_str) *err_str = CCOL__ERR_STR("future mutex init failed");
    return NULL;
  }
  if (CTHRPOOL_FAULT(CTHRPOOL__FAULT_FUTURE_CV) ||
      ccol__cond_var_init(f->cv) != 0) {
    ccol__mutex_destroy(f->mu);
    future_block_free(f);
    if (err_str) *err_str = CCOL__ERR_STR("future cond_var init failed");
    return NULL;
  }
  f->refcount = 2;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&cthrpool__live_futures_for_tests, 1,
                            memory_order_relaxed);
#endif
  return f;
}

// Stores result in a detached future, wakes every waiter and drops the
// producer's reference; it's how an outside producer completes such a future.
// ccol_not_permitted when f is already done.
ccol_retval cthrpool_future_fulfill(cthrpool_future *f, void *result) {
  if (!f) return ccol_invalid_args;
  ccol__mutex_lock(f->mu);
  if (f->done) {
    ccol__mutex_unlock(f->mu);
    return ccol_not_permitted;
  }
  f->result = result;
  f->done = true;
  ccol__cond_var_broadcast(f->cv);
  int remaining = --f->refcount;
  ccol__mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
  return ccol_success;
}

// Blocks until pool has nothing queued and nothing running, so a caller can
// wait for a batch without shutting down; it waits on idle_cv under mu. A
// self-call returns at once, since the calling task counts as active until it
// returns. A pool inherited across fork() counts as idle, because no thread
// here can ever finish its work.
void cthrpool_wait(cthrpool pool) {
  cthread_pool *raw = cthrpool_resolve(pool);
  if (!raw) return;
  if (cthrpool_is_self_call(raw)) {
    cthrpool_resolve_unpin(raw);
    return;
  }
  cthrpool_lock_mu(raw);
#if CCOL_FORK_SAFETY_REQUIRED
  if (!atomic_load(&raw->foreign_since_fork)) {
    while (raw->active_count > 0 || raw->queue_size > 0) {
      ccol__cond_var_wait(raw->idle_cv, raw->mu);
    }
  }
#else
  while (raw->active_count > 0 || raw->queue_size > 0) {
    ccol__cond_var_wait(raw->idle_cv, raw->mu);
  }
#endif
  ccol__mutex_unlock(raw->mu);
  cthrpool_resolve_unpin(raw);
}

// Sets shutdown_complete and wakes every shutdown call waiting for it, so a
// second shutdown call returns only once no worker is alive. The call that did
// the shutdown runs it last, after joining every worker.
static void cthrpool_mark_shutdown_complete(cthread_pool *pool) {
  cthrpool_lock_mu(pool);
  pool->shutdown_complete = true;
  ccol__cond_var_broadcast(pool->shutdown_cv);
  ccol__mutex_unlock(pool->mu);
}

// Runs a drain shutdown of pool and returns once every worker is joined; shared
// by cthrpool_shutdown_drain and cthrpool_teardown_raw. When another call
// already owns the shutdown it waits for shutdown_complete, because
// shutdown_started is set long before the workers are joined; the owner is
// never this thread, so that can't deadlock. Otherwise it sets the drain flag,
// wakes idle workers and blocked submitters, and joins every worker, except for
// a pool inherited across fork(), whose threads this process can't join.
static void cthrpool_shutdown_drain_internal(cthread_pool *pool) {
  cthrpool_lock_mu(pool);
  if (pool->shutdown_started) {
    while (!pool->shutdown_complete) {
      ccol__cond_var_wait(pool->shutdown_cv, pool->mu);
    }
    ccol__mutex_unlock(pool->mu);
    return;
  }
  __atomic_store_n(&pool->shutdown_drain, true, __ATOMIC_RELAXED);
  pool->shutdown_started = true;
  ccol__cond_var_broadcast(pool->not_empty);
  ccol__cond_var_broadcast(pool->not_full);
  ccol__mutex_unlock(pool->mu);

#if CCOL_FORK_SAFETY_REQUIRED
  if (!atomic_load(&pool->foreign_since_fork)) {
    for (size_t i = 0; i < pool->num_threads; i++) {
      ccol__thread_join(pool->threads[i].id);
    }
  }
#else
  for (size_t i = 0; i < pool->num_threads; i++) {
    ccol__thread_join(pool->threads[i].id);
  }
#endif

  cthrpool_mark_shutdown_complete(pool);
}

// Runs every queued task, then stops and joins the workers; the public graceful
// shutdown. A self-call is a full no-op and leaves every flag alone: The worker
// can't join itself, and a shutdown_started set here would stop a later
// shutdown from outside from ever joining that worker.
void cthrpool_shutdown_drain(cthrpool pool) {
  cthread_pool *raw = cthrpool_resolve(pool);
  if (!raw) return;
  if (cthrpool_is_self_call(raw)) {
    cthrpool_resolve_unpin(raw);
    return;
  }
  cthrpool_shutdown_drain_internal(raw);
  cthrpool_resolve_unpin(raw);
}

// Runs an immediate shutdown of pool and returns once every worker is joined.
// It steals the queue and completes the discarded tasks with mu unlocked, wakes
// workers and submitters, broadcasts idle_cv when nothing is active (no task is
// left to do it), and joins the workers. When a drain owns the shutdown it
// escalates it the same way and then waits for shutdown_complete instead.
static void cthrpool_shutdown_immediate_internal(cthread_pool *pool) {
  cthrpool_lock_mu(pool);
  if (pool->shutdown_started) {
    if (!pool->shutdown_immediate) {
      __atomic_store_n(&pool->shutdown_immediate, true, __ATOMIC_RELAXED);
      cthrpool_task *escalated = steal_queue(pool);
      ccol__mutex_unlock(pool->mu);
      cthrpool_discard_tasks(pool, escalated, true);
      cthrpool_lock_mu(pool);
      ccol__cond_var_broadcast(pool->not_empty);
      ccol__cond_var_broadcast(pool->not_full);
      if (pool->active_count == 0) {
        ccol__cond_var_broadcast(pool->idle_cv);
      }
    }
    while (!pool->shutdown_complete) {
      ccol__cond_var_wait(pool->shutdown_cv, pool->mu);
    }
    ccol__mutex_unlock(pool->mu);
    return;
  }
  __atomic_store_n(&pool->shutdown_immediate, true, __ATOMIC_RELAXED);
  pool->shutdown_started = true;

  cthrpool_task *discarded = steal_queue(pool);
  ccol__mutex_unlock(pool->mu);
  cthrpool_discard_tasks(pool, discarded, true);
  cthrpool_lock_mu(pool);

  ccol__cond_var_broadcast(pool->not_empty);
  ccol__cond_var_broadcast(pool->not_full);
  if (pool->active_count == 0) {
    ccol__cond_var_broadcast(pool->idle_cv);
  }
  ccol__mutex_unlock(pool->mu);

#if CCOL_FORK_SAFETY_REQUIRED
  if (!atomic_load(&pool->foreign_since_fork)) {
    for (size_t i = 0; i < pool->num_threads; i++) {
      ccol__thread_join(pool->threads[i].id);
    }
  }
#else
  for (size_t i = 0; i < pool->num_threads; i++) {
    ccol__thread_join(pool->threads[i].id);
  }
#endif

  cthrpool_mark_shutdown_complete(pool);
}

// Discards the queue and stops the workers after their current task; the public
// fast shutdown. A self-call is a no-op, as in cthrpool_shutdown_drain.
void cthrpool_shutdown_immediate(cthrpool pool) {
  cthread_pool *raw = cthrpool_resolve(pool);
  if (!raw) return;
  if (cthrpool_is_self_call(raw)) {
    cthrpool_resolve_unpin(raw);
    return;
  }
  cthrpool_shutdown_immediate_internal(raw);
  cthrpool_resolve_unpin(raw);
}

// Gives the number of queued tasks, read under mu, for monitoring; 0 for a bad
// handle.
size_t cthrpool_pending_count(cthrpool pool) {
  cthread_pool *raw = cthrpool_resolve(pool);
  if (!raw) return 0;
  cthrpool_lock_mu(raw);
  size_t n = raw->queue_size;
  ccol__mutex_unlock(raw->mu);
  cthrpool_resolve_unpin(raw);
  return n;
}

// Gives the number of running tasks, read under mu, for monitoring; 0 for a bad
// handle.
size_t cthrpool_active_count(cthrpool pool) {
  cthread_pool *raw = cthrpool_resolve(pool);
  if (!raw) return 0;
  cthrpool_lock_mu(raw);
  size_t n = raw->active_count;
  ccol__mutex_unlock(raw->mu);
  cthrpool_resolve_unpin(raw);
  return n;
}

#if CCOL_FORK_SAFETY_REQUIRED
// Clears torn_down of slot idx under the write lock, so the fork handler stops
// locking the pool's mu just before the teardown makes it unsafe to touch.
// No-op for CTHRPOOL_TEARDOWN_NO_SLOT.
static void cthrpool_teardown_clear_torn_down(uint32_t idx) {
  if (idx == CTHRPOOL_TEARDOWN_NO_SLOT) return;
  ccol__rw_lock_wrlock(cthrpool_slot_table.rwlock);
  cthrpool_slot *slot =
      (cthrpool_slot *)cvector_at(cthrpool_slot_table.slots, idx);
  slot->torn_down = false;
  ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
}
#endif

// Stops pool and frees it; idx is its slot, or CTHRPOOL_TEARDOWN_NO_SLOT.
// Shared by cthrpool__destroy and the rollback of a failed create. It runs a
// drain shutdown before it waits for pins, because only the shutdown's
// broadcast releases a pinned submitter blocked on a full queue; then it frees
// the free list, the thread array and the primitives, and clears torn_down
// before mu goes. The drain is idempotent under its own lock, so it's always
// called.
//
// A pool inherited across fork() is handled apart: Its primitives may be in use
// by parent threads that don't exist here (destroying a condition variable
// would hang on its waiter count), so only this process's private memory is
// freed. The pin wait still runs, for resolves by other threads of this
// process.
static void cthrpool_teardown_raw(cthread_pool *pool, uint32_t idx) {
#if !CCOL_FORK_SAFETY_REQUIRED
  (void)idx;
#endif
#if CCOL_FORK_SAFETY_REQUIRED
  if (atomic_load(&pool->foreign_since_fork)) {
    cthrpool_wait_for_pins(pool);
    cthrpool_lock_mu(pool);
    cthrpool_task *discarded = steal_queue(pool);
    ccol__mutex_unlock(pool->mu);
    cthrpool_discard_tasks(pool, discarded, false);
    drain_task_free_list(pool);

    ccol__mem_free(pool->m_procs, pool->threads);
    cthrpool_teardown_clear_torn_down(idx);
    pool_free_self(pool);
    return;
  }
#endif

  cthrpool_shutdown_drain_internal(pool);

  cthrpool_wait_for_pins(pool);

  drain_task_free_list(pool);

  ccol__mem_free(pool->m_procs, pool->threads);

#if CCOL_FORK_SAFETY_REQUIRED
  cthrpool_teardown_clear_torn_down(idx);
#endif
  ccol__mutex_destroy(pool->mu);
  ccol__cond_var_destroy(pool->not_empty);
  ccol__cond_var_destroy(pool->not_full);
  ccol__cond_var_destroy(pool->idle_cv);
  ccol__cond_var_destroy(pool->shutdown_cv);

  pool_free_self(pool);
}

// Destroys the pool behind a live handle and releases its slot; CTHRPOOL_NULL
// is a no-op. It looks the handle up and retires it in one write lock section,
// so a second destroy (concurrent or later) fails the lookup and ends in
// ccol__fatal_err, and so does a self-call. torn_down keeps the fork handler
// locking the pool's mu until the teardown clears it. The slot is released
// last, fetched again by index because a concurrent acquire can move the slots
// vector, and its generation is raised so the old handle never matches again.
// The destroy that frees the last slot also does a release that the exit
// destructor deferred.
void cthrpool__destroy(cthrpool pool) {
  if (!pool) return;

  ccol__call_once(cthrpool_slot_table.once, cthrpool_slot_table_init_globals);
  uint32_t idx = (uint32_t)(pool >> 32);
  uint32_t gen = (uint32_t)(pool & 0xFFFFFFFFu);
  ccol__rw_lock_wrlock(cthrpool_slot_table.rwlock);
  cthrpool_slot *slot = NULL;
  cthread_pool *raw = NULL;
  if (cthrpool_slot_table.slots &&
      idx < cvector_size(cthrpool_slot_table.slots)) {
    cthrpool_slot *s =
        (cthrpool_slot *)cvector_at(cthrpool_slot_table.slots, idx);
    if (s->in_use && s->generation == gen) {
      slot = s;
      raw = s->ptr;
    }
  }
  if (!raw) {
    ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
    ccol__fatal_err(
        "cthrpool_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a cthrpool handle)");
  }
  if (cthrpool_is_self_call(raw)) {
    ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
    ccol__fatal_err(
        "cthrpool_destroy: called from within a task (or its on_complete "
        "callback) running on this very pool's own worker thread; "
        "destroying it here would free the pool out from under that "
        "still-executing worker");
  }
  slot->in_use = false;
  ccol_pintable_retire(&cthrpool_pintable, idx);
#if CCOL_FORK_SAFETY_REQUIRED
  slot->torn_down = true;
#endif
  ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);

  cthrpool_teardown_raw(raw, idx);

  ccol__rw_lock_wrlock(cthrpool_slot_table.rwlock);
  cthrpool_slot *slot2 =
      (cthrpool_slot *)cvector_at(cthrpool_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++;
  cthrpool_free_index_release_locked(idx);
  cthrpool_release_slot_table_if_deferred_locked();
  ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS
// Gives the pool behind h for the test accessors, looked up under the read lock
// with no pin, so there's no unpin to forget.
cthread_pool *cthrpool__resolve_for_tests(cthrpool h) {
  ccol__call_once(cthrpool_slot_table.once, cthrpool_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol__rw_lock_rdlock(cthrpool_slot_table.rwlock);
  cthread_pool *raw = NULL;
  if (idx < cvector_size(cthrpool_slot_table.slots)) {
    cthrpool_slot *slot =
        (cthrpool_slot *)cvector_at(cthrpool_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
  return raw;
}

// Gives the number of indices on the free list, so a test can see that no slot
// is lost.
size_t cthrpool__free_index_count_for_tests(void) {
  ccol__call_once(cthrpool_slot_table.once, cthrpool_slot_table_init_globals);
  ccol__rw_lock_rdlock(cthrpool_slot_table.rwlock);
  size_t n = cvector_size(cthrpool_slot_table.free_indices);
  ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
  return n;
}

// Copies pool's shutdown flags out, read under mu, so a test can order two
// shutdown calls.
void cthrpool__shutdown_state_for_tests(struct cthread_pool *pool,
                                        bool *started, bool *immediate,
                                        bool *complete) {
  cthrpool_lock_mu(pool);
  if (started) *started = pool->shutdown_started;
  if (immediate) *immediate = pool->shutdown_immediate;
  if (complete) *complete = pool->shutdown_complete;
  ccol__mutex_unlock(pool->mu);
}

// Answers cthrpool_is_self_call() for pool on the calling thread, so a test can
// check it from a callback.
bool cthrpool__is_self_call_for_tests(struct cthread_pool *pool) {
  return cthrpool_is_self_call(pool);
}

// Gives the number of slots in the table, free ones included, so a test can see
// that slots are reused.
size_t cthrpool__slot_table_capacity_for_tests(void) {
  ccol__call_once(cthrpool_slot_table.once, cthrpool_slot_table_init_globals);
  ccol__rw_lock_rdlock(cthrpool_slot_table.rwlock);
  size_t n = cvector_size(cthrpool_slot_table.slots);
  ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
  return n;
}

// Gives the free list size, read under mu, so a test can check the recycling.
size_t cthrpool__task_free_list_size_for_tests(cthread_pool *pool) {
  cthrpool_lock_mu(pool);
  size_t n = pool->task_free_list_size;
  ccol__mutex_unlock(pool->mu);
  return n;
}

// Gives the free list cap, read under mu, so a test can check the bound.
size_t cthrpool__task_free_list_cap_for_tests(cthread_pool *pool) {
  cthrpool_lock_mu(pool);
  size_t n = pool->task_free_list_cap;
  ccol__mutex_unlock(pool->mu);
  return n;
}

// Gives the number of pins on pool's slot, read with no lock, so a test can see
// a concurrent resolve.
size_t cthrpool__pending_resolve_count_for_tests(cthread_pool *pool) {
  return ccol_pintable_pins_for(&cthrpool_pintable, pool->self_handle);
}

// Waits on not_full until abs, ignoring wakes, and gives the final status, so a
// test can tell which clock not_full uses.
int cthrpool__timedwait_not_full_for_tests(cthread_pool *pool,
                                           const struct timespec *abs) {
  cthrpool_lock_mu(pool);
  int rc = 0;
  do {
    rc = ccol__cond_var_timedwait(pool->not_full, pool->mu, *abs);
  } while (rc == 0);
  ccol__mutex_unlock(pool->mu);
  return rc;
}

// Gives the calling thread's count of pool locks, so a test can count the
// critical sections of one call.
unsigned long cthrpool__mu_locks_for_tests(void) {
  return cthrpool_mu_locks_taken;
}

// Gives the calling thread's timed wait record and clears it, so a test can
// check what a timed submit waited for.
cthrpool__timed_wait_record cthrpool__timed_wait_record_for_tests(void) {
  cthrpool__timed_wait_record r = cthrpool_timed_wait_record;
  memset(&cthrpool_timed_wait_record, 0, sizeof(cthrpool_timed_wait_record));
  return r;
}

// Gives f's refcount, read under its mutex, so a test can wait until a waiter
// holds its reference; -1 for NULL.
int cthrpool__future_refcount_for_tests(cthrpool_future *f) {
  if (!f) return -1;
  ccol__mutex_lock(f->mu);
  int refcount = f->refcount;
  ccol__mutex_unlock(f->mu);
  return refcount;
}

// Takes the write side of the table lock, so a test can hold it while another
// thread forks.
void cthrpool_test_wrlock_slot_table_for_tests(void) {
  ccol__call_once(cthrpool_slot_table.once, cthrpool_slot_table_init_globals);
  ccol__rw_lock_wrlock(cthrpool_slot_table.rwlock);
}

// Releases the write side of the table lock that the test took.
void cthrpool_test_wrunlock_slot_table_for_tests(void) {
  ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
}
#endif

// Tells whether any slot names a pool, so the table is never freed while a pool
// or its destroy may still use it; the caller holds the write lock. It reads
// ptr, not in_use: A destroy clears in_use first but still uses the table until
// its last step, and ptr stays set until then.
static bool cthrpool_any_slot_live_locked(void) {
  size_t slot_count = cvector_size(cthrpool_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    cthrpool_slot *slot =
        (cthrpool_slot *)cvector_at(cthrpool_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

// Frees the table when the exit destructor deferred that and no slot is live
// any more; the caller holds the write lock. Out of line so the destroy path
// keeps its shape.
static __attribute__((noinline)) void
cthrpool_release_slot_table_if_deferred_locked(void) {
  if (cthrpool_slot_table.release_deferred &&
      !cthrpool_any_slot_live_locked()) {
    cthrpool_release_slot_table_locked();
  }
}

// Frees the table vectors and the pin table and deletes the worker key; the
// caller holds the write lock and found no live slot. Each vector becomes NULL,
// so a later call sees the release. The rwlock isn't destroyed, since the
// caller holds it. Any create after this is refused.
static void cthrpool_release_slot_table_locked(void) {
  cvector_destroy(cthrpool_slot_table.slots);
  cvector_destroy(cthrpool_slot_table.free_indices);
  ccol_pintable_dispose(&cthrpool_pintable);
  cthrpool_slot_table.release_deferred = false;
  if (atomic_exchange(&cthrpool_worker_key_bundle.live, false))
    ccol__thread_ls_key_delete(cthrpool_worker_key_bundle.key);
}

// Frees the table at process exit or unload so a leak checker doesn't report
// it, or sets release_deferred when a pool is still live, so the destroy that
// frees the last slot does it (destructor order across files isn't ours to
// choose). The call_once is needed because a process may never have created a
// pool. The static rwlock is never destroyed.
__attribute__((destructor)) static void cleanup_cthrpool_slot_table(void) {
  ccol__call_once(cthrpool_slot_table.once, cthrpool_slot_table_init_globals);
  if (!cthrpool_slot_table.slots) return;
  ccol__rw_lock_wrlock(cthrpool_slot_table.rwlock);
  if (cthrpool_any_slot_live_locked()) {
    cthrpool_slot_table.release_deferred = true;
    ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
    return;
  }
  cthrpool_release_slot_table_locked();
  ccol__rw_lock_unlock(cthrpool_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS
// Runs the exit destructor now, so a test can reach the deferred release.
void cthrpool__run_exit_cleanup_for_tests(void) {
  cleanup_cthrpool_slot_table();
}

// Tells whether the worker key exists, so a test can see the release delete
// it.
bool cthrpool__worker_key_live_for_tests(void) {
  return atomic_load(&cthrpool_worker_key_bundle.live);
}
#endif
